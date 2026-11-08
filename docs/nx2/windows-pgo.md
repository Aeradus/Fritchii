# Windows: a PGO build of the CLI with clang-cl

The emulator's speed in heavy scenes is set by its GPU threads (command processing, buffer and
texture binding), which are ordinary C++ and gain a lot from profile-guided optimization (PGO).
`tools/windows-pgo.cmd` builds `eden-cli.exe` with Visual Studio's clang-cl, profiled on the
game you play. Release CI has its own PGO builds (MSYS2 clang with `eglantine.profdata`,
`.github/workflows/release-binaries.yml`); this is the local route with Visual Studio only.

## Steps

1. `tools\windows-pgo.cmd gen` builds the instrumented `build-pgo-gen\bin\eden-cli.exe`.
2. Play with it for about 10 minutes, with `LLVM_PROFILE_FILE=<repo>\build\pgo\raw\%p.profraw`
   set and the usual `user` folder next to the exe (a junction to an existing one works). It
   runs slower while it records. Close the game window normally: the profile is written on exit,
   not when the process is killed or crashes. Vary the play (towns, open field, interiors,
   menus); the optimizer favours the code that ran.
3. `tools\windows-pgo.cmd merge`, then `tools\windows-pgo.cmd use` builds the optimized
   `build-pgo-use\bin\eden-cli.exe`.

The first boot of each new exe recompiles the AMD driver's pipeline cache (several minutes).
Keep the profile with the source it was made from: functions changed since are compiled without
profile data (`-Wno-profile-instr-out-of-date` behaviour; warnings are off with `-w`).
Only the CLI is built: the bundled Qt needs the Visual Studio 2026 STL to link.

## Result

The Legend of Zelda: Ocarina of Time, Kakariko, standing still, 60 s each, Ryzen 5 5700X3D,
Radeon RX 9070 XT, AMD FSR 4 at 1440p, profile from about 10 minutes of play:

| Build | Mean fps | 5% low | Draws/s on the GPU back thread |
|---|---|---|---|
| MSVC 14.44 `/O2` (the usual Release build) | 26.6 | 23.4 | ~71,000 |
| clang-cl 19 + PGO | 35.0 | 31.2 | ~90,000 |

Part of the gain is clang itself; no plain clang-cl build was measured.
