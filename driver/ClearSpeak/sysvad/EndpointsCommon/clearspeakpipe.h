/*++

ClearSpeak engine -> driver audio pipe.

The user-mode engine opens \\.\ClearSpeakPipe and writes PCM16 / mono / 48 kHz
with IOCTL_CLEARSPEAK_WRITE. The samples land in a non-paged ring; each capture
stream of "ClearSpeak Microphone" reads from it (own cursor, so several apps can
record at once). When the engine is absent the capture stream gets silence.

The reverse direction: what apps play into "ClearSpeak Speaker" is copied (as
PCM16 / mono / 48 kHz) into a second non-paged ring, the speaker ring, which the
engine drains with IOCTL_CLEARSPEAK_READ (never blocks; may return 0 bytes).

--*/

#ifndef _CLEARSPEAK_PIPE_H_
#define _CLEARSPEAK_PIPE_H_

#include <ntddk.h>

#define CLEARSPEAK_PIPE_RATE          48000

// Max bytes accepted per IOCTL (100 ms of PCM16 mono).
#define CLEARSPEAK_PIPE_MAX_WRITE     (CLEARSPEAK_PIPE_RATE / 10 * 2)

#define IOCTL_CLEARSPEAK_WRITE \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_WRITE_DATA)

// Reads what apps played into "ClearSpeak Speaker": output buffer receives PCM16 LE / mono / 48 kHz,
// Information = bytes copied (even, possibly 0, at most the output length). Never blocks. Input unused.
#define IOCTL_CLEARSPEAK_READ     CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_READ_ACCESS)

// Diagnostics, readable with IOCTL_CLEARSPEAK_STATS (counters run from driver load; take a before/after delta).
#define IOCTL_CLEARSPEAK_STATS     CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _CLEARSPEAK_PIPE_STATS
{
    ULONGLONG   WriteCalls;         // accepted IOCTL_CLEARSPEAK_WRITE calls
    ULONGLONG   SamplesWritten;     // source samples accepted
    ULONGLONG   ReadCalls;          // capture-stream reads (one per DMA position update)
    ULONGLONG   Syncs;              // a reader (re)started at the jitter margin
    ULONGLONG   Underruns;          // a read ran out of data
    ULONGLONG   Jumps;              // a playing reader was too far behind and skipped ahead
    ULONGLONG   Timeouts;           // a playing reader found the engine silent for 250 ms
    ULONGLONG   MaxWriteGapMs;      // longest pause between two writes
    ULONGLONG   MinBacklog;         // smallest queued margin seen by a playing reader, in samples (all-ones = none)
    ULONGLONG   MaxBacklog;         // largest queued margin seen by a playing reader, in samples
    // ---- appended (speaker ring). Fields above keep their offsets; a caller passing the old
    // 80-byte buffer gets exactly the old struct.
    ULONGLONG   SpkSamplesWritten;  // 48 kHz mono samples rendered into the speaker ring
    ULONGLONG   SpkSamplesRead;     // samples returned by IOCTL_CLEARSPEAK_READ
    ULONGLONG   SpkOverflowDrops;   // oldest samples dropped because the ring was full (reader too slow)
    ULONGLONG   SpkReadCalls;       // IOCTL_CLEARSPEAK_READ calls
    ULONGLONG   SpkReaderResets;    // ring emptied because no READ came for 250 ms (reader gone)
} CLEARSPEAK_PIPE_STATS;

// Size of the stats struct before the speaker counters were appended: the smallest STATS buffer accepted.
#define CLEARSPEAK_PIPE_STATS_V1_SIZE   (10 * sizeof(ULONGLONG))

// Per capture-stream read position, in 48 kHz source samples.
typedef struct _CLEARSPEAK_PIPE_CURSOR
{
    ULONGLONG   Pos;        // next source sample to consume
    ULONG       Frac;       // 16.16 fractional position between source samples
    BOOLEAN     Started;
    BOOLEAN     Rebuffer;   // ran dry: output silence until PREFILL samples are queued again
} CLEARSPEAK_PIPE_CURSOR;

#ifdef __cplusplus
extern "C" {
#endif

// Creates the ring and the control device, and hooks the driver's IRP table.
// Must be called after PcInitializeAdapterDriver.
NTSTATUS ClearSpeakPipe_Init(_In_ PDRIVER_OBJECT DriverObject);

VOID ClearSpeakPipe_Cleanup(VOID);

VOID ClearSpeakPipe_ResetCursor(_Out_ CLEARSPEAK_PIPE_CURSOR *Cursor);

// Fills Dst with FrameCount frames in the stream's format (PCM, 16/24/32 bit
// container, any channel count, any rate; nearest-sample resampling).
// Writes silence when the engine is not feeding the pipe. IRQL <= DISPATCH.
VOID ClearSpeakPipe_ReadFrames(
    _Inout_ CLEARSPEAK_PIPE_CURSOR *Cursor,
    _Out_writes_bytes_(FrameCount * Channels * (BitsPerSample / 8)) PUCHAR Dst,
    _In_ ULONG FrameCount,
    _In_ ULONG Channels,
    _In_ ULONG BitsPerSample,
    _In_ ULONG SampleRate);

// Per render-stream state for feeding the speaker ring.
typedef struct _CLEARSPEAK_SPK_WRITER
{
    ULONG       Phase;      // resampling accumulator (rates other than 48 kHz; nearest-sample)
} CLEARSPEAK_SPK_WRITER;

// Sample formats a render stream can hand to ClearSpeakPipe_SpeakerWrite.
#define CLEARSPEAK_SPK_FMT_PCM      0
#define CLEARSPEAK_SPK_FMT_FLOAT    1

VOID ClearSpeakPipe_ResetSpeakerWriter(_Out_ CLEARSPEAK_SPK_WRITER *Writer);

// Copies FrameCount rendered frames (PCM 16/24/32-bit or 32-bit float, any channel count) into the speaker
// ring as mono PCM16 48 kHz (channels averaged). Only one render stream feeds the ring at a time: Owner
// identifies the stream; another stream's data is ignored until the owner releases or stalls for 50 ms.
// Never blocks: a full ring drops its oldest samples. IRQL <= DISPATCH.
VOID ClearSpeakPipe_SpeakerWrite(
    _In_ PVOID Owner,
    _Inout_ CLEARSPEAK_SPK_WRITER *Writer,
    _In_reads_bytes_(FrameCount * Channels * (BitsPerSample / 8)) const UCHAR *Src,
    _In_ ULONG FrameCount,
    _In_ ULONG Channels,
    _In_ ULONG BitsPerSample,
    _In_ ULONG SampleFormat,
    _In_ ULONG SampleRate);

// A render stream stopped or is closing: let another stream feed the speaker ring. IRQL <= DISPATCH.
VOID ClearSpeakPipe_SpeakerRelease(_In_ PVOID Owner);

#ifdef __cplusplus
}
#endif

#endif // _CLEARSPEAK_PIPE_H_
