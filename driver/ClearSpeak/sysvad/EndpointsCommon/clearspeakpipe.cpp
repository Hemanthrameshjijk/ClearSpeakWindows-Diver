/*++

ClearSpeak engine -> driver audio pipe. See clearspeakpipe.h.

Locking: one spin lock guards the ring and its write position. Writers
(the engine's IOCTL) run at PASSIVE_LEVEL, readers (capture stream position
update) at DISPATCH_LEVEL, so everything that touches the ring is non-paged
and takes the lock with KeAcquireSpinLock.

The speaker ring (render -> engine) uses the same scheme with its own spin
lock, g_SpkLock: the render stream's position update writes into it at
DISPATCH_LEVEL, IOCTL_CLEARSPEAK_READ drains it at PASSIVE_LEVEL. The two locks
are only held together by IOCTL_CLEARSPEAK_STATS (g_PipeLock first).

--*/

#include <sysvad.h>
#include <wdmsec.h>
#include "clearspeakpipe.h"

#define CLEARSPEAK_POOLTAG        'ianC'

#define RING_SAMPLES            CLEARSPEAK_PIPE_RATE          // 1 s of mono PCM16
#define MAX_LATENCY_SAMPLES     (CLEARSPEAK_PIPE_RATE / 10)   // jump forward past 100 ms of backlog
#define PREFILL_SAMPLES         (CLEARSPEAK_PIPE_RATE * 3 / 100)  // jitter margin: start readers 30 ms behind the writer
#define ENGINE_TIMEOUT_100NS    (2500000LL)                 // 250 ms without writes = engine gone

#define SPK_RING_SAMPLES        CLEARSPEAK_PIPE_RATE          // 1 s of mono PCM16
#define SPK_READER_TIMEOUT_100NS (2500000LL)                // 250 ms without READ = reader gone
#define SPK_OWNER_STALL_100NS   (500000LL)                  // 50 ms without data = feeding stream gone

static const GUID ClearSpeakControlClassGuid =
    { 0x6b1b2f6e, 0x4f13, 0x4b8c, { 0x9d, 0x5a, 0x7c, 0x1e, 0x2a, 0x9f, 0x3d, 0x41 } };

static KSPIN_LOCK       g_PipeLock;
static SHORT           *g_Ring = NULL;
static ULONGLONG        g_WritePos = 0;         // absolute count of source samples written
static LONGLONG         g_LastWriteTime = 0;    // KeQueryInterruptTime of the last write
static PDEVICE_OBJECT   g_ControlDevice = NULL;
static CLEARSPEAK_PIPE_STATS g_Stats;             // guarded by g_PipeLock
static PDRIVER_DISPATCH g_SavedDispatch[IRP_MJ_MAXIMUM_FUNCTION + 1];

// Speaker ring, all guarded by g_SpkLock. Positions are absolute sample counts; the
// buffered audio is [g_SpkReadPos, g_SpkWritePos), never more than SPK_RING_SAMPLES.
static KSPIN_LOCK       g_SpkLock;
static SHORT           *g_SpkRing = NULL;
static ULONGLONG        g_SpkWritePos = 0;
static ULONGLONG        g_SpkReadPos = 0;
static LONGLONG         g_SpkLastReadTime = 0;  // KeQueryInterruptTime of the last READ
static BOOLEAN          g_SpkReaderActive = FALSE;
static PVOID            g_SpkOwner = NULL;      // render stream currently feeding the ring
static LONGLONG         g_SpkLastFeedTime = 0;  // KeQueryInterruptTime of the owner's last data
static ULONGLONG        g_SpkWritten = 0;       // counters, copied into the STATS tail
static ULONGLONG        g_SpkRead = 0;
static ULONGLONG        g_SpkDrops = 0;
static ULONGLONG        g_SpkReadCalls = 0;
static ULONGLONG        g_SpkResets = 0;

DRIVER_DISPATCH ClearSpeakDispatch;

#pragma code_seg()

static NTSTATUS CompleteIrp(_In_ PIRP Irp, _In_ NTSTATUS Status, _In_ ULONG_PTR Info)
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Info;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

static NTSTATUS HandleWrite(_In_ PIRP Irp, _In_ PIO_STACK_LOCATION Stack)
{
    ULONG bytes = Stack->Parameters.DeviceIoControl.InputBufferLength;
    const SHORT *src = (const SHORT *)Irp->AssociatedIrp.SystemBuffer;

    if (src == NULL || bytes == 0 || (bytes & 1) != 0 || bytes > CLEARSPEAK_PIPE_MAX_WRITE)
    {
        return STATUS_INVALID_PARAMETER;
    }

    ULONG samples = bytes / sizeof(SHORT);
    KIRQL irql;
    KeAcquireSpinLock(&g_PipeLock, &irql);
    if (g_Ring == NULL)
    {
        KeReleaseSpinLock(&g_PipeLock, irql);
        return STATUS_DEVICE_NOT_READY;
    }
    for (ULONG i = 0; i < samples; i++)
    {
        g_Ring[(g_WritePos + i) % RING_SAMPLES] = src[i];
    }
    g_WritePos += samples;
    {
        LONGLONG now = (LONGLONG)KeQueryInterruptTime();
        if (g_LastWriteTime != 0)
        {
            ULONGLONG gapMs = (ULONGLONG)((now - g_LastWriteTime) / 10000);
            if (gapMs > g_Stats.MaxWriteGapMs)
            {
                g_Stats.MaxWriteGapMs = gapMs;
            }
        }
        g_LastWriteTime = now;
    }
    g_Stats.WriteCalls++;
    g_Stats.SamplesWritten += samples;
    KeReleaseSpinLock(&g_PipeLock, irql);
    return STATUS_SUCCESS;
}

// True when no READ came for 250 ms (or none ever). Caller holds g_SpkLock.
static BOOLEAN SpkReaderStale(_In_ LONGLONG Now)
{
    return !g_SpkReaderActive || (Now - g_SpkLastReadTime) >= SPK_READER_TIMEOUT_100NS;
}

static NTSTATUS HandleRead(_In_ PIRP Irp, _In_ PIO_STACK_LOCATION Stack, _Out_ ULONG_PTR *Info)
{
    ULONG outLen = Stack->Parameters.DeviceIoControl.OutputBufferLength;
    SHORT *dst = (SHORT *)Irp->AssociatedIrp.SystemBuffer;

    *Info = 0;
    if (dst == NULL && outLen != 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    ULONG want = outLen / sizeof(SHORT);
    KIRQL irql;
    KeAcquireSpinLock(&g_SpkLock, &irql);
    if (g_SpkRing == NULL)
    {
        KeReleaseSpinLock(&g_SpkLock, irql);
        return STATUS_DEVICE_NOT_READY;
    }

    LONGLONG now = (LONGLONG)KeQueryInterruptTime();
    g_SpkReadCalls++;
    if (SpkReaderStale(now))
    {
        // A (new) reader after a gap: drop anything left so it starts with current audio.
        if (g_SpkReaderActive)
        {
            g_SpkResets++;
        }
        g_SpkReadPos = g_SpkWritePos;
        g_SpkReaderActive = TRUE;
    }
    g_SpkLastReadTime = now;

    ULONGLONG avail = g_SpkWritePos - g_SpkReadPos;
    ULONG n = (avail < want) ? (ULONG)avail : want;
    for (ULONG i = 0; i < n; i++)
    {
        dst[i] = g_SpkRing[(g_SpkReadPos + i) % SPK_RING_SAMPLES];
    }
    g_SpkReadPos += n;
    g_SpkRead += n;
    KeReleaseSpinLock(&g_SpkLock, irql);

    *Info = (ULONG_PTR)n * sizeof(SHORT);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS ClearSpeakDispatch(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    UCHAR major = stack->MajorFunction;

    if (DeviceObject == g_ControlDevice)
    {
        switch (major)
        {
        case IRP_MJ_CREATE:
        case IRP_MJ_CLEANUP:
        case IRP_MJ_CLOSE:
            return CompleteIrp(Irp, STATUS_SUCCESS, 0);
        case IRP_MJ_DEVICE_CONTROL:
            if (stack->Parameters.DeviceIoControl.IoControlCode == IOCTL_CLEARSPEAK_WRITE)
            {
                NTSTATUS st = HandleWrite(Irp, stack);
                return CompleteIrp(Irp, st, NT_SUCCESS(st) ? stack->Parameters.DeviceIoControl.InputBufferLength : 0);
            }
            if (stack->Parameters.DeviceIoControl.IoControlCode == IOCTL_CLEARSPEAK_READ)
            {
                ULONG_PTR info;
                NTSTATUS st = HandleRead(Irp, stack, &info);
                return CompleteIrp(Irp, st, info);
            }
            if (stack->Parameters.DeviceIoControl.IoControlCode == IOCTL_CLEARSPEAK_STATS)
            {
                // Older callers pass the original 10-counter struct: they get exactly that much back.
                ULONG outLen = stack->Parameters.DeviceIoControl.OutputBufferLength;
                if (Irp->AssociatedIrp.SystemBuffer == NULL || outLen < CLEARSPEAK_PIPE_STATS_V1_SIZE)
                {
                    return CompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
                }
                ULONG copy = (outLen < sizeof(CLEARSPEAK_PIPE_STATS)) ? outLen : (ULONG)sizeof(CLEARSPEAK_PIPE_STATS);
                CLEARSPEAK_PIPE_STATS snap;
                KIRQL irql, spkIrql;
                KeAcquireSpinLock(&g_PipeLock, &irql);
                snap = g_Stats;
                KeAcquireSpinLock(&g_SpkLock, &spkIrql);
                snap.SpkSamplesWritten = g_SpkWritten;
                snap.SpkSamplesRead = g_SpkRead;
                snap.SpkOverflowDrops = g_SpkDrops;
                snap.SpkReadCalls = g_SpkReadCalls;
                snap.SpkReaderResets = g_SpkResets;
                KeReleaseSpinLock(&g_SpkLock, spkIrql);
                KeReleaseSpinLock(&g_PipeLock, irql);
                RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, &snap, copy);
                return CompleteIrp(Irp, STATUS_SUCCESS, copy);
            }
            return CompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
        default:
            return CompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
        }
    }

    // Anything else belongs to the PortCls function device: pass it on untouched.
    if (major <= IRP_MJ_MAXIMUM_FUNCTION && g_SavedDispatch[major] != NULL)
    {
        return g_SavedDispatch[major](DeviceObject, Irp);
    }
    return CompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
}

VOID ClearSpeakPipe_ResetCursor(_Out_ CLEARSPEAK_PIPE_CURSOR *Cursor)
{
    Cursor->Pos = 0;
    Cursor->Frac = 0;
    Cursor->Started = FALSE;
    Cursor->Rebuffer = FALSE;
}

VOID ClearSpeakPipe_ReadFrames(
    _Inout_ CLEARSPEAK_PIPE_CURSOR *Cursor,
    _Out_writes_bytes_(FrameCount * Channels * (BitsPerSample / 8)) PUCHAR Dst,
    _In_ ULONG FrameCount,
    _In_ ULONG Channels,
    _In_ ULONG BitsPerSample,
    _In_ ULONG SampleRate)
{
    ULONG bytesPerSample = BitsPerSample / 8;
    ULONG frameBytes = Channels * bytesPerSample;
    BOOLEAN supported = (BitsPerSample == 8 || BitsPerSample == 16 || BitsPerSample == 24 || BitsPerSample == 32) &&
                        Channels > 0 && SampleRate > 0;

    // Default: silence. 8-bit PCM is unsigned, so its silence is 0x80.
    if (frameBytes == 0)
    {
        return;
    }
    RtlFillMemory(Dst, (SIZE_T)FrameCount * frameBytes, BitsPerSample == 8 ? 0x80 : 0x00);
    if (!supported)
    {
        return;
    }

    ULONG step = (ULONG)(((ULONGLONG)CLEARSPEAK_PIPE_RATE << 16) / SampleRate);

    KIRQL irql;
    KeAcquireSpinLock(&g_PipeLock, &irql);

    BOOLEAN live = (g_Ring != NULL) &&
                   ((LONGLONG)KeQueryInterruptTime() - g_LastWriteTime) < ENGINE_TIMEOUT_100NS;
    g_Stats.ReadCalls++;
    if (!live)
    {
        if (Cursor->Started && g_Ring != NULL)
        {
            g_Stats.Timeouts++;
        }
        Cursor->Started = FALSE;
        KeReleaseSpinLock(&g_PipeLock, irql);
        return;
    }

    ULONGLONG wpos = g_WritePos;
    if (!Cursor->Started || Cursor->Pos > wpos || (wpos - Cursor->Pos) > MAX_LATENCY_SAMPLES)
    {
        if (Cursor->Started)
        {
            g_Stats.Jumps++;
        }
        g_Stats.Syncs++;
        Cursor->Pos = (wpos > PREFILL_SAMPLES) ? (wpos - PREFILL_SAMPLES) : 0;
        Cursor->Frac = 0;
        Cursor->Started = TRUE;
        Cursor->Rebuffer = FALSE;
    }

    if (!Cursor->Rebuffer)
    {
        ULONGLONG backlog = wpos - Cursor->Pos;
        if (backlog < g_Stats.MinBacklog)
        {
            g_Stats.MinBacklog = backlog;
        }
        if (backlog > g_Stats.MaxBacklog)
        {
            g_Stats.MaxBacklog = backlog;
        }
    }

    // After running dry, wait until the jitter margin is back instead of trickling samples out as the writer
    // delivers them (which keeps the reader at zero margin and makes every late burst an audible gap).
    if (Cursor->Rebuffer)
    {
        if ((wpos - Cursor->Pos) < PREFILL_SAMPLES)
        {
            KeReleaseSpinLock(&g_PipeLock, irql);
            return;     // silence
        }
        Cursor->Rebuffer = FALSE;
    }

    for (ULONG f = 0; f < FrameCount; f++)
    {
        if (Cursor->Pos >= wpos)
        {
            Cursor->Rebuffer = TRUE;    // underrun: the rest stays silent until the margin is rebuilt
            g_Stats.Underruns++;
            break;
        }

        SHORT s = g_Ring[Cursor->Pos % RING_SAMPLES];
        PUCHAR out = Dst + (SIZE_T)f * frameBytes;
        for (ULONG c = 0; c < Channels; c++)
        {
            switch (BitsPerSample)
            {
            case 8:
                *out = (UCHAR)((s >> 8) + 128);
                break;
            case 16:
                *(SHORT *)out = s;
                break;
            case 24:
                out[0] = 0;
                out[1] = (UCHAR)(s & 0xFF);
                out[2] = (UCHAR)((s >> 8) & 0xFF);
                break;
            case 32:
                *(LONG *)out = ((LONG)s) << 16;
                break;
            }
            out += bytesPerSample;
        }

        Cursor->Frac += step;
        Cursor->Pos += Cursor->Frac >> 16;
        Cursor->Frac &= 0xFFFF;
    }

    KeReleaseSpinLock(&g_PipeLock, irql);
}

VOID ClearSpeakPipe_ResetSpeakerWriter(_Out_ CLEARSPEAK_SPK_WRITER *Writer)
{
    Writer->Phase = 0;
}

// IEEE-754 single (raw bits) -> PCM16, truncating, clamped. Integer only: no FPU state needed in the DPC.
static LONG FloatBitsToS16(_In_ ULONG Bits)
{
    LONG exp = (LONG)((Bits >> 23) & 0xFF);
    LONG v;
    if (exp == 0xFF)
    {
        v = (Bits & 0x7FFFFF) ? 0 : 32768;      // NaN -> 0, Inf -> clamp
    }
    else if (exp == 0)
    {
        v = 0;                                  // zero / denormal
    }
    else
    {
        // value * 32768 = mant * 2^(exp - 127 - 23 + 15)
        ULONG mant = (Bits & 0x7FFFFF) | 0x800000;
        LONG sh = exp - 135;
        if (sh >= 0)
        {
            v = 32768;
        }
        else if (sh <= -24)
        {
            v = 0;
        }
        else
        {
            v = (LONG)(mant >> (ULONG)(-sh));
            if (v > 32768)
            {
                v = 32768;
            }
        }
    }
    if (Bits & 0x80000000)
    {
        return -v;                              // down to -32768
    }
    return (v > 32767) ? 32767 : v;
}

// One frame -> mono PCM16 (average of the channels; 24/32-bit keep their top 16 bits).
static SHORT FrameToMono(_In_ const UCHAR *Frame, _In_ ULONG Channels, _In_ ULONG BitsPerSample, _In_ ULONG SampleFormat)
{
    LONG sum = 0;
    for (ULONG c = 0; c < Channels; c++)
    {
        LONG s;
        if (SampleFormat == CLEARSPEAK_SPK_FMT_FLOAT)
        {
            ULONG bits = (ULONG)Frame[0] | ((ULONG)Frame[1] << 8) | ((ULONG)Frame[2] << 16) | ((ULONG)Frame[3] << 24);
            s = FloatBitsToS16(bits);
        }
        else
        {
            ULONG hi = BitsPerSample / 8 - 2;   // offset of the top two bytes: 0 (16), 1 (24), 2 (32)
            s = (LONG)(SHORT)(USHORT)((ULONG)Frame[hi] | ((ULONG)Frame[hi + 1] << 8));
        }
        Frame += BitsPerSample / 8;
        sum += s;
    }
    return (SHORT)(sum / (LONG)Channels);
}

VOID ClearSpeakPipe_SpeakerWrite(
    _In_ PVOID Owner,
    _Inout_ CLEARSPEAK_SPK_WRITER *Writer,
    _In_reads_bytes_(FrameCount * Channels * (BitsPerSample / 8)) const UCHAR *Src,
    _In_ ULONG FrameCount,
    _In_ ULONG Channels,
    _In_ ULONG BitsPerSample,
    _In_ ULONG SampleFormat,
    _In_ ULONG SampleRate)
{
    BOOLEAN supported = Channels > 0 && SampleRate > 0 && Src != NULL &&
        ((SampleFormat == CLEARSPEAK_SPK_FMT_PCM && (BitsPerSample == 16 || BitsPerSample == 24 || BitsPerSample == 32)) ||
         (SampleFormat == CLEARSPEAK_SPK_FMT_FLOAT && BitsPerSample == 32));
    if (!supported || FrameCount == 0)
    {
        return;
    }
    ULONG frameBytes = Channels * (BitsPerSample / 8);

    KIRQL irql;
    KeAcquireSpinLock(&g_SpkLock, &irql);
    if (g_SpkRing == NULL)
    {
        KeReleaseSpinLock(&g_SpkLock, irql);
        return;
    }

    LONGLONG now = (LONGLONG)KeQueryInterruptTime();
    if (g_SpkOwner != NULL && g_SpkOwner != Owner && (now - g_SpkLastFeedTime) < SPK_OWNER_STALL_100NS)
    {
        KeReleaseSpinLock(&g_SpkLock, irql);    // another render stream is feeding the ring
        return;
    }
    g_SpkOwner = Owner;
    g_SpkLastFeedTime = now;

    // Nearest-sample rate conversion to 48 kHz (a no-op at 48 kHz: one output per input frame).
    ULONGLONG start = g_SpkWritePos;
    for (ULONG f = 0; f < FrameCount; f++)
    {
        Writer->Phase += CLEARSPEAK_PIPE_RATE;
        if (Writer->Phase < SampleRate)
        {
            continue;
        }
        SHORT m = FrameToMono(Src + (SIZE_T)f * frameBytes, Channels, BitsPerSample, SampleFormat);
        while (Writer->Phase >= SampleRate)
        {
            g_SpkRing[g_SpkWritePos % SPK_RING_SAMPLES] = m;
            g_SpkWritePos++;
            Writer->Phase -= SampleRate;
        }
    }
    g_SpkWritten += g_SpkWritePos - start;

    if (SpkReaderStale(now))
    {
        // Nobody reading: keep the ring empty so a returning reader only gets current audio.
        if (g_SpkReaderActive)
        {
            g_SpkResets++;
            g_SpkReaderActive = FALSE;
        }
        g_SpkReadPos = g_SpkWritePos;
    }
    else if (g_SpkWritePos - g_SpkReadPos > SPK_RING_SAMPLES)
    {
        // Reader too slow: the oldest samples were overwritten.
        g_SpkDrops += g_SpkWritePos - g_SpkReadPos - SPK_RING_SAMPLES;
        g_SpkReadPos = g_SpkWritePos - SPK_RING_SAMPLES;
    }
    KeReleaseSpinLock(&g_SpkLock, irql);
}

VOID ClearSpeakPipe_SpeakerRelease(_In_ PVOID Owner)
{
    KIRQL irql;
    KeAcquireSpinLock(&g_SpkLock, &irql);
    if (g_SpkOwner == Owner)
    {
        g_SpkOwner = NULL;
    }
    KeReleaseSpinLock(&g_SpkLock, irql);
}

#pragma code_seg("PAGE")

NTSTATUS ClearSpeakPipe_Init(_In_ PDRIVER_OBJECT DriverObject)
{
    PAGED_CODE();

    NTSTATUS status;
    UNICODE_STRING devName = RTL_CONSTANT_STRING(L"\\Device\\ClearSpeakPipe");
    UNICODE_STRING linkName = RTL_CONSTANT_STRING(L"\\DosDevices\\ClearSpeakPipe");
    // System and Administrators: all access. Authenticated users: read/write (engine runs unelevated).
    UNICODE_STRING sddl = RTL_CONSTANT_STRING(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;AU)");

    KeInitializeSpinLock(&g_PipeLock);
    g_WritePos = 0;
    g_LastWriteTime = 0;
    RtlZeroMemory(&g_Stats, sizeof(g_Stats));
    g_Stats.MinBacklog = (ULONGLONG)-1;     // "none seen yet"

    KeInitializeSpinLock(&g_SpkLock);
    g_SpkWritePos = 0;
    g_SpkReadPos = 0;
    g_SpkLastReadTime = 0;
    g_SpkReaderActive = FALSE;
    g_SpkOwner = NULL;
    g_SpkLastFeedTime = 0;
    g_SpkWritten = g_SpkRead = g_SpkDrops = g_SpkReadCalls = g_SpkResets = 0;

    g_Ring = (SHORT *)ExAllocatePool2(POOL_FLAG_NON_PAGED, RING_SAMPLES * sizeof(SHORT), CLEARSPEAK_POOLTAG);
    if (g_Ring == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    g_SpkRing = (SHORT *)ExAllocatePool2(POOL_FLAG_NON_PAGED, SPK_RING_SAMPLES * sizeof(SHORT), CLEARSPEAK_POOLTAG);
    if (g_SpkRing == NULL)
    {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Fail;
    }

    status = IoCreateDeviceSecure(
        DriverObject, 0, &devName, FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN, FALSE,
        &sddl, &ClearSpeakControlClassGuid, &g_ControlDevice);
    if (!NT_SUCCESS(status))
    {
        goto Fail;
    }
    g_ControlDevice->Flags |= DO_BUFFERED_IO;

    status = IoCreateSymbolicLink(&linkName, &devName);
    if (!NT_SUCCESS(status))
    {
        IoDeleteDevice(g_ControlDevice);
        g_ControlDevice = NULL;
        goto Fail;
    }

    // Take over every dispatch entry PortCls installed, except PnP/Power/WMI
    // (which never reach a non-PnP control device), so a stray IRP sent to our
    // device can never reach PortCls with a device object it does not own.
    for (ULONG i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
    {
        g_SavedDispatch[i] = DriverObject->MajorFunction[i];
        if (i != IRP_MJ_PNP && i != IRP_MJ_POWER && i != IRP_MJ_SYSTEM_CONTROL)
        {
            DriverObject->MajorFunction[i] = ClearSpeakDispatch;
        }
    }

    g_ControlDevice->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;

Fail:
    ExFreePoolWithTag(g_Ring, CLEARSPEAK_POOLTAG);
    g_Ring = NULL;
    if (g_SpkRing != NULL)
    {
        ExFreePoolWithTag(g_SpkRing, CLEARSPEAK_POOLTAG);
        g_SpkRing = NULL;
    }
    return status;
}

VOID ClearSpeakPipe_Cleanup(VOID)
{
    PAGED_CODE();

    UNICODE_STRING linkName = RTL_CONSTANT_STRING(L"\\DosDevices\\ClearSpeakPipe");

    if (g_ControlDevice != NULL)
    {
        IoDeleteSymbolicLink(&linkName);
        IoDeleteDevice(g_ControlDevice);
        g_ControlDevice = NULL;
    }

    // Detach the ring under the lock so a late reader sees "engine gone".
    KIRQL irql;
    KeAcquireSpinLock(&g_PipeLock, &irql);
    SHORT *ring = g_Ring;
    g_Ring = NULL;
    KeReleaseSpinLock(&g_PipeLock, irql);
    if (ring != NULL)
    {
        ExFreePoolWithTag(ring, CLEARSPEAK_POOLTAG);
    }

    KeAcquireSpinLock(&g_SpkLock, &irql);
    SHORT *spkRing = g_SpkRing;
    g_SpkRing = NULL;
    g_SpkOwner = NULL;
    KeReleaseSpinLock(&g_SpkLock, irql);
    if (spkRing != NULL)
    {
        ExFreePoolWithTag(spkRing, CLEARSPEAK_POOLTAG);
    }
}
