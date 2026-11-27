# Agent notes — HueLeaf E-Ink DevKit

## Toolchain
- ESP-IDF **v5.5.5** is installed **natively on Windows**, not in WSL:
  - IDF source: `D:\Codes\espressif\.espressif\v5.5.5\esp-idf`
  - tools / python venv: `C:\Espressif\tools`
  - target chip: `esp32c5`
- Do not migrate this project to WSL, containers, or another toolchain: the compiler, python venv and
  openocd are Windows-native and already installed.

## Build
DSH starts every shell with `-NoProfile`, so the IDF environment is not active by default.

```powershell
.\build.ps1                                   # builds examples\3.7einkDvb
.\build.ps1 -Project examples\<name>          # another example
```

Equivalent one-liner:

```powershell
. 'C:\Espressif\tools\Microsoft.v5.5.5.PowerShell_profile.ps1'; idf.py -C examples\3.7einkDvb build
```

Artifacts land in `examples\3.7einkDvb\build\` (`3.7einkDvb.bin`, `bootloader\bootloader.bin`,
`partition_table\partition-table.bin`). A successful build ends with `Project build complete.`

## Auto build loop (agent verification)
Agent shells run in an isolated Linux workspace that cannot reach the Windows toolchain.
To compile after edits without asking the user to run anything manually:

1. Ask the user to start the watcher once in a normal PowerShell window and keep it open:
   `powershell -ExecutionPolicy Bypass -File .\build-watch.ps1`
2. It rebuilds 3 s after anything under `examples\3.7einkDvb\main` (or the project's
   `CMakeLists.txt`, `sdkconfig`, `sdkconfig.defaults`, `partitions.csv`) changes.
3. Outputs: `.esp-build\last.log` (full log), `.esp-build\status.txt`
   (`BUILDING <ts>`, then `OK exit=0 ...` or `FAILED exit=<code> ...`),
   `.esp-build\heartbeat.txt` (refreshed every 10 s, proves the watcher is alive),
   `.esp-build\history.log`.
4. Poll `status.txt` from the workspace after editing; a new `OK/FAILED` line means the build
   finished. `.esp-build/` is gitignored.

Flash / monitor (needs the board on a COM port; PID/VID is in the dev docs):

```powershell
. 'C:\Espressif\tools\Microsoft.v5.5.5.PowerShell_profile.ps1'; idf.py -C examples\3.7einkDvb -p COM3 flash monitor
```

## Permission mode
`idf.py build` cannot run under the DSH `workspace-write` sandbox: DSH blocks named pipes on Windows and
Python's `asyncio` subprocess layer needs one, so the build dies with `PermissionError: [WinError 5]`
before cmake starts. This workspace must run with **danger-full-access**.

## Paths
Keep the repository path ASCII-only. `kconfgen` decodes `config.env` with the system code page (GBK on this
machine) and fails with `UnicodeDecodeError` when the project path contains non-ASCII characters.