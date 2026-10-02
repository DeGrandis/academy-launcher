# Release guide

How Academy Launcher is built, published and updated. Players only ever download the launcher; the game comes from
their own disc image and is never part of a release.

## Pieces

| Piece | Source | Built by |
| --- | --- | --- |
| Runtime (`cw_runtime.dll`), `xbe2exe.exe`, plugin DLLs | `native_port/`, `mods/*/plugin/` | CMake (Win32, static CRT) |
| Launcher (`AcademyLauncher.exe`) and `academy-tool.exe` | `launcher/` | `launcher/build.ps1` (Roslyn `csc`, .NET Framework 4.8) |
| Presets and mod data | `mods/presets.json`, `mods/<name>/` | copied as they are |
| Installer | `tools/release/installer.iss` | Inno Setup 6 |
| Relay | `server/relay/` | Docker |

`tools/release/package.ps1` builds all of it and stages `work/release/`:

- `AcademyLauncher/`: the installed layout (launcher, `runtime/`, `mods/`, `defaults.ini`, `VERSION`, `README.md`).
- `AcademyLauncher-<version>-portable.zip`, and `AcademyLauncherSetup-<version>.exe` when Inno Setup is installed.
- `SHA256SUMS.txt`: the launcher checks a downloaded update against it.

The script refuses to stage anything outside an allow-list (`.exe .dll .json .ini .md .xbt .txt`, `VERSION`), so game
files cannot slip in.

## On the player's PC

1. The installer puts the launcher in `%LOCALAPPDATA%\Programs\Academy Launcher` (no administrator rights).
2. On first start the launcher imports the game from the player's ISO (XDVDFS reader) or extracted folder into
   `%LOCALAPPDATA%\AcademyLauncher\game`. It checks `default.xbe` against the supported release's SHA-256.
3. `xbe2exe` converts the player's `default.xbe` into `play\clone_wars.exe`, next to a copy of `cw_runtime.dll`. This is
   redone whenever the runtime, `xbe2exe` or the game changes.
4. Playing a preset builds its mod root in `modroots\<preset>` with the C# port of `tools/build_mod.py`. It is rebuilt
   only when the mods, the launcher version or the game's `data.zwp` change.
5. The launcher starts the game with the player's settings as `CW_*` environment variables.

## Versions and online compatibility

- `VERSION` (repository root) is the version. CMake bakes it into the runtime log, and the launcher shows it and
  compares it with releases.
- Online, every game sends a build fingerprint: the first 8 hex digits of a SHA-256 over the version and every input
  of the preset's mods (data, edits, loose files, plugin DLLs). Games with different fingerprints ignore each other.
  The launcher checks the host or relay room before joining and tells the player to update. Any release that changes
  the online preset's mods needs everyone to update.
- `academy-tool presets` prints each preset's fingerprint.

## Publishing a release

1. Bump `VERSION` (e.g. `0.2.0`) and commit.
2. Tag and push: `git tag v0.2.0` then `git push origin main v0.2.0`.
3. `.github/workflows/release.yml` checks the tag matches `VERSION`, writes `update_repo` (this repository) and the
   relay address into `defaults.ini`, builds, packages with the installer, and publishes a GitHub release with the
   setup, the portable zip and `SHA256SUMS.txt`. It also pushes the relay image to
   `ghcr.io/<owner>/academy-relay:<tag>`.
4. Launchers on the stable channel see the new release at their next start and offer **Update now**. That downloads
   the setup, checks its SHA-256, and runs it with `/SILENT /UPDATE=1`. Setup closes the launcher, replaces its files
   (not the player's game, mod roots, saves or settings), and starts it again.

A tag with a suffix (`v0.2.0-beta.1`) makes a pre-release, which only launchers set to the **Beta** channel install.
To roll back, publish a new version with the old content. Players can also install any older setup from the Releases
page.

## One-time setup

- Create the GitHub repository and push (keep it private until ready; releases of a private repository are not
  visible to players' launchers).
- Set the repository variable `RELAY_ADDRESS` (Settings > Secrets and variables > Actions > Variables), e.g.
  `relay.example.com:3074`. Without it, releases keep the placeholder in `launcher/defaults.ini`.
- Optional: a self-hosted Windows runner labelled `has-game`, with the environment variable `CW_GAME_ROOT` pointing
  at an extracted game, and the repository variable `HAS_GAME_RUNNER=true`. CI then also runs some regression tests.

## Relay server

The relay (`server/relay/relay.py`, standard library only) forwards datagrams between the games in a room (join code).
It never parses game data. Rooms hold one build fingerprint; a room is closed 30 s after its last game leaves.

On the server (e.g. an EC2 instance with Docker):

1. Copy `server/relay/` there (or `git clone` the repository), then `docker compose up -d --build` in that folder.
   Or use the published image: `docker run -d --restart unless-stopped -p 3074:3074/udp ghcr.io/<owner>/academy-relay:latest`.
2. Open **UDP 3074** inbound in the instance's security group.
3. Point a DNS name at the instance (an A record, e.g. `relay.example.com`).
4. Check from your PC: `python server/relay/test_relay.py relay.example.com 3074`.
5. Logs: `docker compose logs -f` (rooms opening and closing, players joining).

Settings are environment variables in `docker-compose.yml`: `RELAY_PORT`, `RELAY_EXPIRE`, `RELAY_MAX_ROOMS`,
`RELAY_MAX_MEMBERS`, `RELAY_LOG`.

## Testing a release without another PC

- `academy-tool.exe` runs the launcher's steps from a command line: `extract`, `verify`, `presets`, `build`, `query`.
- `ACADEMY_DATA=<folder>` makes the launcher use a throwaway data folder instead of `%LOCALAPPDATA%\AcademyLauncher`.
- `tools/release/sandbox.ps1` opens Windows Sandbox (a clean, disposable Windows) with `work/release` and the ISO
  folder mapped read-only. It starts the installer, so you see exactly what a friend sees. Windows Sandbox is an
  optional Windows feature; turn it on once in "Turn Windows features on or off".
