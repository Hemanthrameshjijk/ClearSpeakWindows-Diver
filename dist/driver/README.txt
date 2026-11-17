ClearSpeak virtual microphone driver - TEST build (not installed by the ClearSpeak installer)

What it is: a virtual audio device with two endpoints, "ClearSpeak Microphone" and a speaker. ClearSpeak sends its cleaned microphone
audio to it, and other apps pick "ClearSpeak Microphone" as their microphone. If it is not installed, ClearSpeak works as before
(it uses the VB-CABLE route).

Why it is not installed automatically: it is signed with a self-made test certificate. Windows only loads such a driver in "test
mode" (Secure Boot off, usually Memory Integrity off), and installing it means trusting that certificate on the PC. Use a test PC.

To install:   open PowerShell as Administrator in this folder, then   .\Install-ClearSpeak-Driver.ps1     (add -DryRun to preview)
To remove:    .\Uninstall-ClearSpeak-Driver.ps1
Needs devcon.exe from the Windows Driver Kit (the script finds it, or pass -DevconPath).

For a driver that loads on normal PCs it must be signed through Microsoft (attestation signing): not done.
