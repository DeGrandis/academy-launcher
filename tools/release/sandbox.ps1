# Tries a staged release the way a friend would, in Windows Sandbox: a clean, throwaway Windows with nothing
# installed, discarded when closed. This PC is not touched.
#
# Needs the "Windows Sandbox" feature (Windows 11/10 Pro): Settings > System > Optional features > More Windows
# features > Windows Sandbox, then restart. Run tools\release\package.ps1 first.
#
# Inside the sandbox, the desktop has:
#   release\   work\release, read-only (run AcademyLauncherSetup-*.exe, or unzip the portable zip)
#   iso\       original_iso, read-only (choose the .iso there in the launcher's setup)
param([string]$Iso = "")
$ErrorActionPreference = "Stop"
$repo = Resolve-Path "$PSScriptRoot\..\.."
$release = "$repo\work\release"
if (-not (Test-Path $release)) { throw "no staged release: run tools\release\package.ps1 first" }
if (-not $Iso) { $Iso = "$repo\original_iso" }
$installer = Get-ChildItem "$release\AcademyLauncherSetup-*.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
$logon = if ($installer) { "<LogonCommand><Command>C:\Users\WDAGUtilityAccount\Desktop\release\$($installer.Name)</Command></LogonCommand>" } else { "" }
$config = @"
<Configuration>
  <MappedFolders>
    <MappedFolder><HostFolder>$release</HostFolder><SandboxFolder>C:\Users\WDAGUtilityAccount\Desktop\release</SandboxFolder><ReadOnly>true</ReadOnly></MappedFolder>
    <MappedFolder><HostFolder>$Iso</HostFolder><SandboxFolder>C:\Users\WDAGUtilityAccount\Desktop\iso</SandboxFolder><ReadOnly>true</ReadOnly></MappedFolder>
  </MappedFolders>
  <vGPU>Enable</vGPU>
  <Networking>Enable</Networking>
  $logon
</Configuration>
"@
$file = "$repo\work\academy-sandbox.wsb"
Set-Content -Path $file -Value $config -Encoding utf8
Write-Host "starting Windows Sandbox with $file"
Start-Process $file
