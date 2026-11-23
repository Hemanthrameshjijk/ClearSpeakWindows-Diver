# ClearSpeak driver

`sysvad/` is Microsoft's SYSVAD sample (MIT, github.com/microsoft/Windows-driver-samples, audio/sysvad),
trimmed to one speaker and one microphone endpoint, with these ClearSpeak changes:

- `TabletAudioSample/minipairs.h`: only `SpeakerMiniports` and `MicInMiniports` are registered.
- `TabletAudioSample/ComponentizedAudioSample.inx`: hardware ID `Root\ClearSpeak`, provider/names "ClearSpeak Speaker", "ClearSpeak Microphone".
- `EndpointsCommon/clearspeakpipe.{h,cpp}` (new): control device `\\.\ClearSpeakPipe` plus a non-paged ring.
- `EndpointsCommon/minwavertstream.cpp`: capture `WriteBytes` reads the pipe instead of generating a sine; render
  `ReadBytes` of the speaker endpoint copies what apps play into the speaker ring (`FeedSpeakerRing`).
- `EndpointsCommon/speakerwavtable.h`: "ClearSpeak Speaker" only offers 48 kHz 16-bit PCM (stereo; mono too on the
  host/loopback pins), so the speaker ring needs no kernel resampling.
- `adapter.cpp`: creates/removes the pipe in `DriverEntry`/`DriverUnload`.

Output is `ClearSpeak.sys` (renamed from ClearNAI on 17 Nov 2026; Release x64 rebuilt, test-signed with "CN=ClearSpeak Test Signing", pipe unit tests pass; not yet re-tested on a PC or VM). The install script removes a device left by the old ClearNAI driver (`RootClearNAI`). See `docs/SIGNING.md`.

## Speaker read-back (`IOCTL_CLEARSPEAK_READ`, branch `feature/speaker-read`)

The engine reads what call apps play into "ClearSpeak Speaker" with `IOCTL_CLEARSPEAK_READ` (0x802, `FILE_READ_ACCESS`):
PCM16 / mono / 48 kHz, returns at once with whatever is buffered (possibly 0 bytes), up to the output length (ask for
<= 100 ms = 9600 bytes). Source: the render stream's position update averages the channels into a second 1 s
non-paged ring (own spin lock). Render never waits: a full ring drops its oldest samples (`SpkOverflowDrops`); with no
READ for 250 ms the reader counts as gone and the ring is kept empty, so a new reader starts with current audio (its
first READ returns 0 bytes). One render stream feeds the ring at a time (normally the shared-mode engine's system
stream); another stream (offload/exclusive) takes over only after the first stops or stalls 50 ms. Endpoint volume /
mute are not applied (the topology volume node only stores the level). `IOCTL_CLEARSPEAK_STATS` gained five appended
counters (`Spk*`); a caller passing the old 80-byte struct still gets exactly that. Unit tests cover it (2289 checks);
`pipetest --speaker` plays a 440 Hz tone into the speaker and reads it back (needs the driver: VM only, not yet run).
Built Debug/Release x64 in `C:\b\sysvad_spk` with 0 warnings; not installed, signed or tested on a VM yet.

## Build (VS 2026 + WDK 28000.2526 + SDK 10.0.28000)

Copy `sysvad/` to a short path (e.g. `C:\b\sysvad`; the tracking files break past MAX_PATH), then with the 64-bit MSBuild
(`C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe`):

```
msbuild EndpointsCommon\EndpointsCommon.vcxproj     /p:Configuration=Debug /p:Platform=x64
msbuild TabletAudioSample\TabletAudioSample.vcxproj /p:Configuration=Debug /p:Platform=x64
```

## Install (VM only, test-signing on)

`bcdedit /set testsigning on`, reboot, then in an admin shell from the build output folder:
`pnputil /add-driver ComponentizedAudioSample.inf /install` and create the device with
`devcon install ComponentizedAudioSample.inf Root\ClearSpeak`. Then run `clearspeak-engine`.

Status: builds, installs and loads in a Hyper-V VM (`tools/vm`), both endpoints appear, and a 440 Hz tone sent through
`\\.\ClearSpeakPipe` is recorded back from "ClearSpeak Microphone" at the right level with ~100% of its energy at 440 Hz
(`engine/src/bin/pipetest.rs`). Also verified in the VM: two apps recording at once both get a clean tone, and when the engine stops
writing for 1.5 s the microphone goes silent and the tone resumes afterwards. Scenarios: the local test-VM scripts (not in this repo).
All three scenarios, plus a device-remove / package-delete / reinstall cycle, also pass with Driver Verifier (standard flags +
special pool) enabled on the driver, with no bugchecks (the local test-VM scripts (not in this repo), `reinstall-in-vm.ps1`, `unload-in-vm.ps1`).
Soak (2 min, 1198 windows of 100 ms): every window at the right level with a clean tone, 0 phase jumps, clock drift -0.4 ppm
(`run-pipetest.ps1 -Scenario soak|soakrt`). Earlier runs showed glitches in the first seconds; the driver's own counters
(`IOCTL_CLEARSPEAK_STATS`, printed by pipetest) showed an 84 ms stall of the *test writer*, which the driver handled as designed
(underrun -> rebuffer). The driver now keeps a 30 ms jitter margin and, after running dry, outputs silence until it is rebuilt.

Logic tests that need no VM: `tools/pipe_unittest` compiles `clearspeakpipe.cpp` against kernel stand-ins and checks 2289 cases
(8 sample rates x 4 bit depths x 3 channel counts, underrun/rebuffer, engine timeout, latency clamp, ring wrap, IOCTL validation,
dispatch isolation, cleanup, counters, plus the speaker ring: formats, resampling, overflow, stale reader, ownership): `g++ -std=c++17 -Wno-unknown-pragmas -Wno-multichar -Ishim test.cpp && ./a.exe`.

Final VM run on the rebuffering driver with counters (Driver Verifier on): 2-minute soak 1199/1199 clean windows, 0 jumps,
whole-recording tone share 100%; two apps both 100%; engine-restart gap: 1 engine-timeout counted, silence 1.4 s, tone resumed.

Not yet tested: a real softphone, recording below 48 kHz in a real app (only the unit tests cover
resampling), the real microphone through `clearspeak-engine`, and the driver's actual unload path (`DriverUnload` /
`ClearSpeakPipe_Cleanup`): the service stays RUNNING after the device is removed and `sc stop` did not stop it, so unload only
happens at reboot/shutdown and has not been observed. The `MaxWriteGapMs` counter is a lifetime maximum, so it includes idle time
between test launches (pipetest shows n/a above 60 s).

Note: the INF needs a signed catalog, which the Debug build doesn't produce. Generate it with
`Inf2Cat /driver:<pkg> /os:10_X64` and sign the .cat with the test certificate (see the local test-VM scripts (not in this repo) for the VM flow).
Run Inf2Cat on a folder holding only the INF, .sys and DLL: extra files (test programs) make it fail with an empty .cat.
