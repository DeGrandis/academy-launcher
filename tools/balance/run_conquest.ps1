# Conquest balance runs: split-screen Conquest with two computer players (one per team) and player 1 idle at their base
# as an observer, sped up with CW_TIME_SCALE. The conquest_stats plugin logs per-side telemetry; each game ends when
# an HQ falls or after -Minutes of game time. Logs go to work/balance/<label>/<map>_<n>/log.txt; summarize them with
# python tools/balance/summarize.py work/balance/<label>.
#
#   powershell tools/balance/run_conquest.ps1 -Label baseline -Games 3 -Parallel 3
#   -Mods: the mods under test (default: the "full" preset); conquest_stats is added.
#   -ObserverTeam: 1 = player 1 joins team 1, 2 = team 2, alternate (default) = by game number.
#   -Env NAME=value,...: extra environment settings for every game.
param(
    [string]$Label = "baseline",
    [string[]]$Maps = @("multi8", "multi12", "multi10", "multi6"),
    [int]$Games = 3,
    [int]$Parallel = 12,     # games at once (at most one per physical core)
    [double]$Scale = 4,
    [int]$Fps = 30,          # game frames per game second (the Xbox original ran at 30)
    [int]$RenderEvery = 10,  # draw only every nth frame (the simulation runs every frame)
    [int]$Minutes = 20,
    [string[]]$Mods = @("unlock_all", "academy_vehicles", "academy_powerups", "academy_waves", "mp_solo", "ai_players", "cis_spiders",
        "cis_bosses", "rep_vehicles"),
    [string]$ObserverTeam = "alternate",
    [string[]]$Env = @()  # extra NAME=value settings for every game
)
$ErrorActionPreference = "Stop"
$repo = Resolve-Path "$PSScriptRoot\..\.."
$exe = "$repo\native_port\build-x86\bin\clone_wars.exe"
# Position of each Conquest map in the split-screen map list (multiplayerbuttons.cfg).
$mapIndex = @{ multi8 = 8; multi12 = 9; multi10 = 10; multi6 = 11 }

$allMods = @($Mods) + "conquest_stats"
python "$repo\tools\build_mod.py" @allMods | Select-Object -Last 1
$modRoot = "$repo\work\mod_root\$($allMods -join '+')"

function Route([int]$index, [int]$observerTeam) {
    $rights = (0..($index - 1) | ForEach-Object { "$(11000 + $_ * 350):right" }) -join ","
    $after = 11000 + $index * 350 + 500
    $t = [Math]::Max($after, 14500) - 14500
    $s = @(
        "2700:start", "4250:a", "7000:right", "7500:a", $rights,
        "$(14500 + $t):down", "$(15000 + $t):a", "$(16500 + $t):a",
        # panel 2: computer player, defaults to team 2
        "$(18000 + $t):black", "$(19000 + $t):start", "$(20000 + $t):a", "$(21000 + $t):a", "$(22000 + $t):a",
        # panel 3: computer player, defaults to team 1
        "$(23000 + $t):black", "$(24000 + $t):start", "$(25000 + $t):a", "$(26000 + $t):a", "$(27000 + $t):a",
        # back to player 1 (panel 4 is skipped), then profile, team, vehicle, start
        "$(28000 + $t):black", "$(28700 + $t):black", "$(29500 + $t):a"
    )
    if ($observerTeam -eq 2) { $s += "$(30300 + $t):right" }
    $s += @("$(31000 + $t):a", "$(32500 + $t):a", "$(34000 + $t):start")
    return ($s -join ",")
}

$jobs = @()
foreach ($map in $Maps) {
    for ($n = 1; $n -le $Games; $n++) {
        $team = if ($ObserverTeam -eq "alternate") { if ($n % 2 -eq 1) { 1 } else { 2 } } else { [int]$ObserverTeam }
        $jobs += [pscustomobject]@{ Map = $map; N = $n; Team = $team }
    }
}

# Each game gets a physical core of its own (both of its hardware threads): games sharing a core run far slower.
$cores = [Math]::Max(1, [Environment]::ProcessorCount / 2)
$running = @{}  # core -> process
foreach ($job in $jobs) {
    $core = -1
    while ($core -lt 0) {
        for ($c = 0; $c -lt [Math]::Min($Parallel, $cores); $c++) {
            if (-not $running.ContainsKey($c) -or $running[$c].HasExited) { $core = $c; break }
        }
        if ($core -lt 0) { Start-Sleep -Milliseconds 500 }
    }
    $dir = "$repo\work\balance\$Label\$($job.Map)_$($job.N)"
    if (Test-Path $dir) { [System.IO.Directory]::Delete($dir, $true) }
    New-Item -ItemType Directory -Force $dir | Out-Null
    Copy-Item "$repo\tests\fixtures\hdd" "$dir\hdd" -Recurse -Force
    Copy-Item "$repo\extracted_iso\gameicon.xpr" "$dir\hdd\E\UDATA\4c410004\TitleImage.xbx"
    Copy-Item "$repo\extracted_iso\saveicon.xpr" "$dir\hdd\E\UDATA\4c410004\SaveImage.xbx"
    Get-ChildItem Env:CW_* | ForEach-Object { Remove-Item "Env:$($_.Name)" }
    # Cheap frames: 640x480, and only every RenderEvery-th frame drawn, so many games keep up with the time scale.
    $env:CW_MOD_ROOT = $modRoot; $env:CW_AUDIO = "0"; $env:CW_FPS = "$Fps"; $env:CW_TIME_SCALE = "$Scale"
    $env:CW_RESOLUTION = "640x480"; $env:CW_RENDER_SCALE = "1"; $env:CW_RENDER_EVERY = "$RenderEvery"
    # The plugin ends the game at -Minutes after the match starts; CW_EXIT_MS is only a backstop (menus, a hang).
    $env:CW_STATS_MINUTES = "$Minutes"; $env:CW_EXIT_MS = "$(($Minutes * 60 + 240) * 1000)"; $env:CW_STATS_EXIT = "1"
    $env:CW_HDD_ROOT = "$dir\hdd"; $env:CW_LOG_PATH = "$dir\log.txt"
    $env:CW_INPUT_SCRIPT = Route $mapIndex[$job.Map] $job.Team
    foreach ($setting in $Env) { $name, $value = $setting -split "=", 2; Set-Item "Env:$name" $value }
    Set-Content "$dir\observer_team.txt" $job.Team
    Set-Content "$dir\scale.txt" $Scale
    $process = Start-Process $exe -WorkingDirectory $dir -PassThru
    $process.ProcessorAffinity = [IntPtr](3L -shl (2 * $core))
    $running[$core] = $process
    "started $($job.Map) game $($job.N) (observer on team $($job.Team))"
}
$running.Values | ForEach-Object { $_.WaitForExit() }
"done: work\balance\$Label"
