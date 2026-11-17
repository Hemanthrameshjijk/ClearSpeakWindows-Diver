#Requires -RunAsAdministrator
<#
 Removes the ClearSpeak test driver and the ClearSpeak TEST certificate from this PC's trust stores. Run elevated.
   .\Uninstall-ClearSpeak-Driver.ps1            removes it
   .\Uninstall-ClearSpeak-Driver.ps1 -DryRun    only shows what it would do
 Afterwards you can turn test mode off:  bcdedit /set testsigning off  (and re-enable Secure Boot / Memory Integrity), then restart.
#>
param([switch] $DryRun, [string] $DevconPath)
$ErrorActionPreference = 'Continue'
# The ClearSpeak test certificate (ClearSpeak.cer) and the one from before the rename (ClearNAI.cer).
$thumbs = @('ACE3B58B11A2DEAB6AD061EA7B8B173FE173EE7C', 'FFF1785B7ABC3112EE4B6415D7C516DA90E6FC6D')
function Run($text, [scriptblock] $action) { if ($DryRun) { Write-Host "  [dry run] $text" } else { Write-Host "  -> $text"; & $action } }

if (-not $DevconPath) {
    $DevconPath = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\Tools" -Recurse -Filter devcon.exe -ErrorAction SilentlyContinue |
        Where-Object FullName -match '\\x64\\' | Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
}
if ($DevconPath -and (Test-Path $DevconPath)) {
    Run 'remove the ClearSpeak device' { & $DevconPath remove 'Root\ClearSpeak' }
    Run 'remove the device from before the rename (ClearNAI), if any' { & $DevconPath remove 'Root\ClearNAI' }
} else {
    Write-Host 'devcon.exe not found: remove the device "ClearSpeak Virtual Audio Device" in Device Manager (Sound, video and game controllers) instead.'
}
$oem = (pnputil /enum-drivers | Out-String) -split "(?m)^\s*$" | Where-Object { $_ -match 'componentizedaudiosample' } |
    ForEach-Object { if ($_ -match 'Published Name:\s+(oem\d+\.inf)') { $Matches[1] } }
foreach ($o in $oem) { Run "delete driver package $o" { pnputil /delete-driver $o /uninstall } }
foreach ($store in 'Root', 'TrustedPublisher') {
    Run "remove the ClearSpeak test certificate from $store" {
        $c = Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object { $thumbs -contains $_.Thumbprint }
        if ($c) { $c | Remove-Item } else { Write-Host '     (not present)' }
    }
}
Write-Host 'Done. Restart Windows to unload the driver completely.'

