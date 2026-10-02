# Builds everything and stages a release in work\release (no game files: players supply their own disc).
#
#   work\release\AcademyLauncher\              the installed layout (also zipped as the portable download)
#   work\release\AcademyLauncher-<v>-portable.zip
#   work\release\AcademyLauncherSetup-<v>.exe  (when Inno Setup 6 is installed)
#   work\release\SHA256SUMS.txt
#
# Usage: powershell -File tools\release\package.ps1 [-SkipBuild] [-RequireInstaller]
param([switch]$SkipBuild, [switch]$RequireInstaller)
$ErrorActionPreference = "Stop"
$repo = Resolve-Path "$PSScriptRoot\..\.."
$version = (Get-Content "$repo\VERSION" -TotalCount 1).Trim()
$out = "$repo\work\release"
$stage = "$out\AcademyLauncher"
$bin = "$repo\native_port\build-x86\bin"

if (-not $SkipBuild) {
    cmake -S "$repo\native_port" -B "$repo\native_port\build-x86" -A Win32
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
    cmake --build "$repo\native_port\build-x86" --config Release --target cw_runtime xbe2exe clone_wars
    if ($LASTEXITCODE -ne 0) { throw "native build failed" }
    & "$repo\launcher\build.ps1"
}

if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force "$stage\runtime", "$stage\mods" | Out-Null
Copy-Item "$repo\launcher\out\AcademyLauncher.exe", "$repo\launcher\out\academy-tool.exe", "$repo\launcher\defaults.ini", "$repo\VERSION" $stage
Copy-Item "$repo\docs\player-guide.md" "$stage\README.md"
Copy-Item "$bin\cw_runtime.dll", "$bin\xbe2exe.exe" "$stage\runtime"
Copy-Item "$repo\mods\presets.json" "$stage\mods"

# The mods the presets use: their data, edits and loose files, and the built plugin (not its source).
$presets = Get-Content "$repo\mods\presets.json" -Raw | ConvertFrom-Json
$mods = $presets.presets | ForEach-Object { $_.mods } | Sort-Object -Unique
foreach ($mod in $mods) {
    $source = "$repo\mods\$mod"
    $target = "$stage\mods\$mod"
    New-Item -ItemType Directory -Force $target | Out-Null
    foreach ($folder in "data", "files") {
        if (Test-Path "$source\$folder") { Copy-Item -Recurse "$source\$folder" $target }
    }
    if (Test-Path "$source\edits.json") { Copy-Item "$source\edits.json" $target }
    if (Test-Path "$source\plugin") {
        New-Item -ItemType Directory -Force "$target\plugin" | Out-Null
        Copy-Item "$bin\mods\$mod.dll" "$target\plugin"
    }
}

# Allow-list: anything else (game data, symbols, build junk) must not ship.
$allowed = '\.(exe|dll|json|ini|md|xbt|txt)$|\\VERSION$'
$blocked = @(Get-ChildItem -Recurse -File $stage | Where-Object { $_.FullName -notmatch $allowed -or $_.Name -match '^(default\.xbe|data\.zwp)$' })
if ($blocked.Count -gt 0) { throw "files outside the release allow-list: $($blocked.FullName -join ', ')" }

# The staged launcher must find everything it needs (presets, mods, plugins).
& "$stage\academy-tool.exe" presets
if ($LASTEXITCODE -ne 0) { throw "the staged launcher cannot read its presets" }

Compress-Archive -Path "$stage\*" -DestinationPath "$out\AcademyLauncher-$version-portable.zip"

$iscc = @("${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe", "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe") |
    Where-Object { Test-Path $_ } | Select-Object -First 1
if ($iscc) {
    & $iscc /Q "/DAppVersion=$version" "/DStageDir=$stage" "/DOutputDir=$out" "$PSScriptRoot\installer.iss"
    if ($LASTEXITCODE -ne 0) { throw "Inno Setup failed" }
} elseif ($RequireInstaller) {
    throw "Inno Setup 6 (ISCC.exe) not found"
} else {
    Write-Warning "Inno Setup 6 not found: skipping the installer (the portable zip is ready)"
}

Push-Location $out
Get-ChildItem -File | Where-Object { $_.Name -ne "SHA256SUMS.txt" } | ForEach-Object {
    "{0}  {1}" -f (Get-FileHash $_.Name -Algorithm SHA256).Hash.ToLower(), $_.Name
} | Set-Content -Encoding ascii SHA256SUMS.txt
Pop-Location
Get-Content "$out\SHA256SUMS.txt"
Write-Host "release $version staged in $out"
