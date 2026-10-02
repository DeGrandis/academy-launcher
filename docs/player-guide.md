# Academy Launcher: player guide

Academy Launcher runs **Star Wars: The Clone Wars** (original Xbox, 2002) on a Windows PC, with mods for Thule Moon
Academy and Conquest, AI players, and online play with friends.

You need your own copy of the game: a disc image (`.iso`) made from your own disc (the North American release). The
launcher does not include the game, and it does not download it.

## Install

1. Download `AcademyLauncherSetup-<version>.exe` from the Releases page and run it.
2. Windows may say "Windows protected your PC" because the installer is not signed. Click **More info**, then
   **Run anyway**.
3. The installer needs no administrator rights. It installs for your Windows account only.

You need Windows 10 or 11 and a graphics card with DirectX 9 (any card from the last 15 years). Nothing else needs
installing.

## First start

The launcher asks for your game:

- **Choose disc image (.iso)**: pick your `.iso` file. The launcher copies the game files out of it (about 1.4 GB),
  which takes a minute.
- **Choose extracted folder**: if you already have the game files in a folder (with `default.xbe` and `data.zwp`).

The launcher checks the game is the supported release, then gets it ready. You only do this once.

## Playing

On the **Play** tab, pick a mode and press **PLAY**:

- **Academy+**: Thule Moon Academy with extra vehicles (the gunship flies), the Thule Power mode (a powerup in the
  middle that changes every 60 seconds) and continuous enemy waves.
- **Conquest and Online**: Conquest with AI players and new vehicles for both sides. In the lobby, press **RB** (or
  **R** on the keyboard) to move to an empty slot and make it an AI player, then set its team.
- **Original game**: no mods.

The first time you play a mode, the launcher builds its mods (a few seconds).

A controller (Xbox or any XInput pad) works like the original. The **Controls** tab lists the keyboard keys. In the
game, **F11** toggles fullscreen and **F9** changes the resolution.

## Playing online

Everyone needs the same launcher version. Online games use the **Conquest and Online** mode.

Set **Your name** on the **Online** tab: it is the name other players see in the lobby (it starts as your Windows
user name). Your save profile keeps its own name.

**With a join code (easiest, no router setup):**

1. The host opens the **Online** tab, picks **Join code** and presses **Host**. The launcher shows a 6-letter code;
   send it to your friends.
2. In the game, the host goes to **Network Play > System Link > Create Game**.
3. Friends type the code on the **Online** tab and press **Join**. In the game they go to **Network Play > System Link >
   Search for Games** and pick the host's game.

**Direct (IP address):** the host's router must forward **UDP port 3074** to the host's PC. Friends pick **Direct**
and type the host's public IP address.

**Same network (LAN):** for players in the same house. Everyone picks **Same network**.

## Updates

The launcher checks for updates when it starts. When a new version is out, a yellow bar appears: click **Update now**.
Your game files, saves and settings stay as they are.

## Where things are

- The launcher: `%LOCALAPPDATA%\Programs\Academy Launcher`
- Your game files, saves, settings and logs: `%LOCALAPPDATA%\AcademyLauncher` (**Settings > Open saves and logs**)

## Problems

- **The game does not start or closes**: the log is `cw_runtime.log` in the saves and logs folder. Send it to whoever
  gave you the launcher.
- **"different release or region"**: only the North American release works.
- **Mods look wrong after an update**: **Settings > Rebuild mods**.
- **Can't find the host's game**: check the code, check the host is on **Create Game**, and that you both have the
  same launcher version.

## Uninstall

Use **Settings > Apps > Installed apps > Academy Launcher > Uninstall** in Windows. It asks whether to delete your game
files and saves too.
