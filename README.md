# Star Wars: The Clone Wars Native Port Workspace

This workspace is being used to reverse engineer the original Xbox release into a compilable, modifiable, and extendable native Windows port.

## How it works

The game's original x86 code runs natively on Windows; nothing is emulated.

- `native_port/tools/xbe2exe` converts `extracted_iso/default.xbe` into a 32-bit Windows executable, `clone_wars.exe`, at the XBE's original base address `0x10000`.
- `native_port/runtime` builds `cw_runtime.dll`, which provides:
  - **Xbox kernel:** a high-level emulation of the kernel exports (files, threads, sync, memory, crypto).
  - **Graphics:** Direct3D 8 (Xbox) functions replaced with Direct3D 9, including NV2A texture unswizzling.
  - **Input:** XInput gamepads plus keyboard and mouse.
  - **Audio:** a silent DirectSound replacement.
  - **Setup:** patches for the Xbox `fs:` thread-block accesses, plus small game-specific hooks (`runtime/HleXapi.cpp`).

The real title screen, mission select, difficulty menu and the first level (Geonosis) run using the original assets.

## Build and run

```powershell
cmake -S native_port -B native_port/build-x86 -G "Visual Studio 18 2026" -A Win32
cmake --build native_port/build-x86 --config Debug
native_port/build-x86/bin/clone_wars.exe
```

The build must be 32-bit (`-A Win32`). By default the game data is read from `extracted_iso/`; override it with `CW_GAME_ROOT`. Save data goes to `native_port/build-x86/bin/hdd/`.

## Controls

The game window must be focused. Xbox controllers also work through XInput.

| Xbox | Keyboard / mouse |
|---|---|
| D-pad / left stick | Arrow keys / WASD |
| A | Enter, left mouse button |
| B | Esc, Backspace, right mouse button |
| X / Y | X / Y |
| Start / Back | Space / Tab |
| Left / right trigger | Q / E |

The original menus have no pointer support, so a mouse click acts as A on the highlighted item.

## Known limitations

- Audio is silent.
- FMV movies (XMV) are skipped.
- Online and system link are disabled.
- Xbox vertex shaders (NV2A microcode) and pixel shaders (register combiners) are translated to HLSL at runtime; a few rare texture modes are approximated.

## Tests

```powershell
python tests/run_tests.py                       # all cases
python tests/run_tests.py thule_academy_starts  # one case
```

Each case in `tests/cases.json` boots the game with scripted input against a fresh copy of `tests/fixtures/hdd`, then checks memory values sampled with `CW_WATCH` and fails on any hardware exception. Logs and screenshots go to `tests/results/<case>/`.

## Debugging

These environment variables affect `clone_wars.exe`:

- `CW_SCREENSHOT_FRAMES=600,1200`: saves `screenshot_<frame>.bmp` files. `CW_SCREENSHOT_EVERY=120` saves one every 120 frames.
- `CW_INPUT_SCRIPT=10000:start,11400:a,15000:lup:3000`: presses a button at a millisecond offset for 150 ms, or for the optional hold time. Buttons are `start`, `back`, `a`, `b`, `x`, `y`, `black`, `white`, `lt`, `rt`, `up`, `down`, `left`, `right` (d-pad) and `lup`, `ldown`, `lleft`, `lright` (left stick).
  - Thule Moon Academy: `10000:start,13000:a,15500:right,16500:a,20500:left,21200:left,22500:down,23500:a,26000:a,28000:a,30000:start,32000:start`
- `CW_WATCH=41b884:f,41b888,5a2448:s`: logs memory values every `CW_WATCH_MS` (default 1000) ms. Types are `i` (int32, default), `f` (float), `b` (byte), `h` (int16) and `s` (string).
- `CW_TRACE=2d551a,...`: logs registers at the given addresses.
- `CW_WATCHDOG_MS=5000`: periodically dumps thread stacks.
- `CW_EXIT_MS=60000`: exits after the given time. `CW_LOG_PATH` and `CW_HDD_ROOT` move the log file and the hard disk folder.
- `CW_DISABLE_HOOKS=XOnlineReadCachedRecord,...`: skips the named hooks, to compare against the original game code.
- `CW_WIDESCREEN=0`: use the original 4:3 mode (widescreen 16:9 is the default, using the game's own widescreen support).
- `CW_FPS=n`: software frame cap instead of vsync (`CW_FPS=0` = unlimited, for fast automated tests).
- `CW_SHADERLOG=1`: writes translated shaders to `vs_<n>.hlsl` / `ps_<n>.hlsl`.
- `CW_DRAWLOG_FRAME=1500`: logs every draw call of one frame.
- `CW_WIREFRAME=1`, `CW_DEBUG_SOLID=1`, `CW_PS_OUTPUT=t0`: rendering debug views.

The window title shows the current FPS and frame number.

The log is written to `bin/cw_runtime.log`; it can be read while the game is running.

Export the Ghidra analysis index:

```powershell
$env:JAVA_HOME = 'C:\Program Files\Microsoft\jdk-21.0.12.101-hotspot'
$ghidra = "$env:USERPROFILE\tools\ghidra\ghidra_12.1.4_PUBLIC"
$env:Path = "$env:JAVA_HOME\bin;$ghidra\support;$env:Path"
analyzeHeadless ghidra_projects clone_wars -process default.xbe -noanalysis -scriptPath tools/ghidra_scripts -postScript ExportCloneWarsAnalysis.java analysis_exports/default_xbe
```

Decompile game functions (or list references to an address) without opening Ghidra, using the same environment:

```powershell
analyzeHeadless ghidra_projects clone_wars -process default.xbe -noanalysis -readOnly -scriptPath tools/ghidra_scripts -postScript DecompileFunctions.java out.c 00077340 refs:0041b884 callers:000dea10
```

To change game behaviour, hook a game function from `runtime/HleXapi.cpp` with `hle::hookFunction(address, replacement, name)`. You can then rewrite engine functions in C++ one at a time.
