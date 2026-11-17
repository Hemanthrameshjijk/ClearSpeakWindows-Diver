; ClearSpeak virtual audio driver: TEST installer (one .exe, no terminal).
; For test PCs only: the driver is signed with a self-made test certificate and loads only in Windows test mode.
; Build:  makensis installer\ClearSpeak-Driver-Test.nsi     (output: installer\out\ClearSpeak-Driver-Test-Setup.exe)
; Silent: ClearSpeak-Driver-Test-Setup.exe /S              (skips the question boxes; still needs test mode)
Unicode true
!include "MUI2.nsh"
!include "LogicLib.nsh"

!define APP      "ClearSpeak Driver (Test)"
!define VERSION  "1.0.0.0"
!define HWID     "Root\ClearSpeak"
!define CERT_THUMB "FFF1785B7ABC3112EE4B6415D7C516DA90E6FC6D"
!define DEVCON   "C:\Program Files (x86)\Windows Kits\10\Tools\10.0.28000.0\x64\devcon.exe"
!define UNINSTKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\ClearSpeakDriverTest"

Name "${APP}"
OutFile "out\ClearSpeak-Driver-Test-Setup.exe"
InstallDir "$PROGRAMFILES64\ClearSpeak Driver (Test)"
RequestExecutionLevel admin
ShowInstDetails show
ShowUninstDetails show
BrandingText "ClearSpeak test driver ${VERSION}"

!define MUI_ABORTWARNING
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Var Out

; Runs a command, prints its output in the details window, sets $0 to the exit code.
!macro RunLogged label cmd
    DetailPrint "${label}"
    nsExec::ExecToStack '${cmd}'
    Pop $0
    Pop $Out
    DetailPrint "$Out"
!macroend

Function .onInit
    SetRegView 64
    ; The driver loads only in test mode: check before changing anything.
    nsExec::ExecToStack '"$SYSDIR\cmd.exe" /c bcdedit /enum {current} | findstr /i /c:"testsigning" | findstr /i /c:"Yes"'
    Pop $0
    Pop $Out
    ${If} $0 != 0
        MessageBox MB_OK|MB_ICONSTOP "Windows test mode is OFF, so Windows would refuse this driver.$\r$\n$\r$\nTo use this test build: turn Secure Boot off in the BIOS/UEFI, turn Memory Integrity off (Windows Security > Device security > Core isolation), run  bcdedit /set testsigning on  as administrator, restart, then run this installer again.$\r$\n$\r$\nNothing was changed."
        Abort
    ${EndIf}
    ${IfNot} ${Silent}
        MessageBox MB_YESNO|MB_ICONEXCLAMATION "This installs a TEST-signed ClearSpeak driver and adds the ClearSpeak TEST certificate to this PC's Trusted Root and Trusted Publishers stores. That lowers this PC's security until you uninstall it.$\r$\n$\r$\nA faulty kernel driver can blue-screen the PC. Use a test machine only.$\r$\n$\r$\nContinue?" IDYES +2
        Abort
    ${EndIf}
FunctionEnd

Section "Install"
    SetRegView 64
    SetOutPath "$INSTDIR\driver"
    File "..\dist\driver\ClearSpeak.sys"
    File "..\dist\driver\ComponentizedAudioSample.inf"
    File "..\dist\driver\clearspeak.cat"
    File "..\dist\driver\ClearSpeak.cer"
    File "..\dist\driver\Uninstall-ClearSpeak-Driver.ps1"
    SetOutPath "$INSTDIR\tools"
    File "${DEVCON}"

    ; Trust the test certificate so Windows accepts the package.
    !insertmacro RunLogged "Trusting the ClearSpeak test certificate (Trusted Root)..." '"$SYSDIR\certutil.exe" -addstore -f Root "$INSTDIR\driver\ClearSpeak.cer"'
    ${If} $0 != 0
        Goto failed
    ${EndIf}
    !insertmacro RunLogged "Trusting the ClearSpeak test certificate (Trusted Publishers)..." '"$SYSDIR\certutil.exe" -addstore -f TrustedPublisher "$INSTDIR\driver\ClearSpeak.cer"'
    ${If} $0 != 0
        Goto failed
    ${EndIf}

    ; Add the package to the driver store, then create the device the driver attaches to.
    !insertmacro RunLogged "Adding the driver package..." '"$SYSDIR\pnputil.exe" /add-driver "$INSTDIR\driver\ComponentizedAudioSample.inf" /install'
    ${If} $0 != 0
    ${AndIf} $0 != 3010
        Goto failed
    ${EndIf}
    !insertmacro RunLogged "Creating the ClearSpeak device..." '"$INSTDIR\tools\devcon.exe" install "$INSTDIR\driver\ComponentizedAudioSample.inf" ${HWID}'
    ${If} $0 != 0
    ${AndIf} $0 != 1
        Goto failed
    ${EndIf}

    Sleep 6000
    nsExec::ExecToStack '"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -Command "Get-PnpDevice -Class AudioEndpoint -ErrorAction SilentlyContinue | Where-Object FriendlyName -match ''ClearSpeak'' | ForEach-Object { $$_.FriendlyName + '' : '' + $$_.Status }"'
    Pop $0
    Pop $Out
    DetailPrint "Audio endpoints from the driver:"
    DetailPrint "$Out"
    ${If} $Out == ""
        DetailPrint "No ClearSpeak endpoints listed yet. Open Settings > System > Sound; if they are missing, restart Windows."
    ${EndIf}

    ; Uninstaller + Apps list entry
    WriteUninstaller "$INSTDIR\Uninstall.exe"
    WriteRegStr HKLM "${UNINSTKEY}" "DisplayName" "${APP}"
    WriteRegStr HKLM "${UNINSTKEY}" "DisplayVersion" "${VERSION}"
    WriteRegStr HKLM "${UNINSTKEY}" "Publisher" "ClearSpeak"
    WriteRegStr HKLM "${UNINSTKEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
    WriteRegDWORD HKLM "${UNINSTKEY}" "NoModify" 1
    WriteRegDWORD HKLM "${UNINSTKEY}" "NoRepair" 1
    DetailPrint "Done. In your call app choose 'ClearSpeak Microphone' as the microphone."
    Return

failed:
    DetailPrint "Install failed (code $0). See the lines above."
    MessageBox MB_OK|MB_ICONSTOP "The driver install failed (code $0). The messages above in the install window say which step. You can run the uninstaller to clean up."
    SetErrorLevel 2
    Abort
SectionEnd

Section "Uninstall"
    SetRegView 64
    DetailPrint "Removing the ClearSpeak device and driver package..."
    nsExec::ExecToStack '"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "$INSTDIR\driver\Uninstall-ClearSpeak-Driver.ps1" -DevconPath "$INSTDIR\tools\devcon.exe"'
    Pop $0
    Pop $Out
    DetailPrint "$Out"
    Delete "$INSTDIR\driver\*.*"
    Delete "$INSTDIR\tools\*.*"
    RMDir "$INSTDIR\driver"
    RMDir "$INSTDIR\tools"
    Delete "$INSTDIR\Uninstall.exe"
    RMDir "$INSTDIR"
    DeleteRegKey HKLM "${UNINSTKEY}"
    DetailPrint "Done. Restart Windows to unload the driver completely."
SectionEnd
