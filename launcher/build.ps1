# Builds AcademyLauncher.exe (the player-facing app) and academy-tool.exe (the same operations from a command line)
# into launcher\out with the C# compiler that comes with Visual Studio / Build Tools. Targets .NET Framework 4.8, which
# Windows 10 and 11 include, so players install nothing.
param([string]$Out = "$PSScriptRoot\out")
$ErrorActionPreference = "Stop"

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$csc = $null
if (Test-Path $vswhere) {
    $vs = & $vswhere -latest -prerelease -products * -property installationPath
    if ($vs) { $csc = Join-Path $vs "MSBuild\Current\Bin\Roslyn\csc.exe" }
}
if (-not $csc -or -not (Test-Path $csc)) { throw "csc.exe not found: install Visual Studio or Build Tools with MSBuild" }

$framework = "$env:WINDIR\Microsoft.NET\Framework\v4.0.30319"
$references = "System.dll", "System.Core.dll", "System.Drawing.dll", "System.Windows.Forms.dll", "System.IO.Compression.dll",
    "System.Web.Extensions.dll" | ForEach-Object { "/reference:$framework\$_" }
$sources = Get-ChildItem "$PSScriptRoot\src" -Recurse -Filter *.cs | ForEach-Object { $_.FullName }
$common = @("/nologo", "/noconfig", "/nostdlib+", "/reference:$framework\mscorlib.dll", "/optimize+", "/deterministic",
    "/platform:anycpu", "/langversion:latest", "/warnaserror+", "/nowarn:1701,1702") + $references
New-Item -ItemType Directory -Force $Out | Out-Null

& $csc @common /target:winexe "/out:$Out\AcademyLauncher.exe" "/win32manifest:$PSScriptRoot\app.manifest" "/win32icon:$PSScriptRoot\academy.ico" $sources
if ($LASTEXITCODE -ne 0) { throw "AcademyLauncher.exe failed to build" }
& $csc @common /target:exe /define:TOOL "/out:$Out\academy-tool.exe" $sources
if ($LASTEXITCODE -ne 0) { throw "academy-tool.exe failed to build" }
Write-Host "built $Out\AcademyLauncher.exe and $Out\academy-tool.exe"
