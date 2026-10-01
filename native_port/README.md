# Clone Wars Native Port Scaffold

This is the first native Windows port target. It does not run the game yet; it creates a compilable and extendable host executable that can discover the extracted Xbox game files and grow a replacement platform layer around the decompiled game code.

Current milestone:

- start a native Windows executable
- initialize a placeholder Xbox platform shim
- map Xbox-style `D:\` paths to the extracted ISO directory
- verify `default.xbe` and `data.zwp` are available
- model the discovered XBE entrypoint creating its primary startup thread
- parse `D:\config.ini` into a native editable configuration object
- inspect the `D:\data.zwp` archive header and identify the first compressed stream
- decompress the first `D:\data.zwp` zlib stream and preview its embedded `DataBase()` text
- scan `D:\data.zwp` for early zlib streams, including interface/menu database data
- decode `ifsMain` menu labels from the interface database and display them as a native menu preview
- open a native Windows window that shows current boot/menu milestone status

Current startup facts from Ghidra:

- XBE entrypoint: `00160eba`
- primary `CreateThread` start address: `00160e46`
- primary startup thread calls into `0001b580` before `XapiBootToDash`
- secondary thread candidate: `002241e0`, created by `00224100`

Build from the repo root:

```powershell
cmake -S native_port -B native_port/build-vs -G "Visual Studio 18 2026" -A x64 -DCMAKE_TOOLCHAIN_FILE="$env:USERPROFILE/tools/vcpkg/scripts/buildsystems/vcpkg.cmake"
cmake --build native_port/build-vs --config Debug
native_port/build-vs/Debug/clone_wars_native.exe --game-root extracted_iso
```

Use `--headless` for command-line validation without opening the window:

```powershell
native_port/build-vs/Debug/clone_wars_native.exe --game-root extracted_iso --headless --verbose
```
