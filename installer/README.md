# Installer

WiX Toolset project goes here (`ClearSpeak.wxs`): installs the engine, copies the driver
package, runs `pnputil /add-driver ClearSpeak.inf /install`. Milestone 1 is test-signed only;
production needs attestation/WHQL signing.
