# ClearSpeak architecture

```
real mic -> [engine: WASAPI capture -> (DSP/ONNX, later) -> DeviceIoControl]
         -> \\.\ClearSpeakPipe (driver control device, IOCTL_CLEARSPEAK_WRITE)
         -> non-paged ring in the driver -> capture miniport -> "ClearSpeak Microphone"
app picks "ClearSpeak Microphone" as its input.

call app plays into "ClearSpeak Speaker" -> render miniport -> speaker ring (driver, 1 s)
         -> IOCTL_CLEARSPEAK_READ -> [engine: noise removal] -> real headset (WASAPI render)
```

- **Driver**: one KMDF/PortCls device, SYSVAD-derived, two endpoints (Speaker, Microphone). A thin pipe only.
- **Engine**: user mode, so a crash never takes the OS down. Rust: the WASAPI, ring-buffer and
  model code already exists in Rust (`../Clearnaiwindows-main/crates`), Rust gives memory safety in
  the real-time and shared-memory paths, and `ort` provides ONNX Runtime (broader operator coverage
  than tract). The driver stays C: kernel-mode Rust is not production-ready.
- **Wire contract**: PCM16, mono, 48 kHz, up to 100 ms per `IOCTL_CLEARSPEAK_WRITE` (`driver/ClearSpeak/sysvad/EndpointsCommon/clearspeakpipe.h`).
  A named shared-memory section was rejected: it is pageable (unsafe to read at DISPATCH_LEVEL), and a Local\ section is
  invisible to the kernel. Each capture stream keeps its own read cursor, converts to the stream format and emits
  silence when the engine stops writing for 250 ms.
- **Speaker direction**: `IOCTL_CLEARSPEAK_READ` returns PCM16 / mono / 48 kHz without blocking (0 bytes when empty).
  The speaker endpoint only offers 48 kHz 16-bit formats, so the kernel only averages channels. Overflow drops the
  oldest audio; no READ for 250 ms empties the ring so a new reader never gets stale audio. Speaker counters are
  appended to `IOCTL_CLEARSPEAK_STATS` (old 80-byte callers unaffected).
- **Milestone 1**: passthrough only. No USB bus layer, no model training, test-signing only.
- **Benchmark**: `tools/benchmark` cross-correlates raw-mic and ClearSpeak-mic recordings of a clap.
