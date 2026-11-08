@echo off
rem Profile-guided (PGO) build of the CLI (eden-cli.exe) on Windows with Visual Studio's clang-cl.
rem See docs/nx2/windows-pgo.md.
rem   tools\windows-pgo.cmd gen    instrumented build in build-pgo-gen\ (records a profile on exit)
rem   tools\windows-pgo.cmd merge  merges build\pgo\raw\*.profraw into build\pgo\profile.profdata
rem   tools\windows-pgo.cmd use    optimized build in build-pgo-use\ from that profile
rem Needs Visual Studio 2022 with "C++ Clang tools for Windows", CMake, Ninja, the Vulkan SDK and
rem Python 3 on PATH. VSDIR, LLVMDIR and JOBS override the defaults below.
setlocal
set "ROOT=%~dp0.."
if "%VSDIR%"=="" set "VSDIR=C:\Program Files\Microsoft Visual Studio\2022\Community"
if "%LLVMDIR%"=="" set "LLVMDIR=%VSDIR%\VC\Tools\Llvm\x64"
if "%JOBS%"=="" set "JOBS=16"
set "PROFDIR=%ROOT%\build\pgo"

if /i "%~1"=="merge" (
  "%LLVMDIR%\bin\llvm-profdata.exe" merge -o "%PROFDIR%\profile.profdata" "%PROFDIR%\raw\*.profraw"
  exit /b
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=%LLVMDIR%\bin;%PATH%"
rem lld-link finds clang's profile runtime (clang_rt.profile) through LIB.
for /d %%v in ("%LLVMDIR%\lib\clang\*") do set "LIB=%%v\lib\windows;%LIB%"
if /i "%~1"=="gen" (
  set "DIR=build-pgo-gen"
  set "PGO=-w -fprofile-generate"
) else if /i "%~1"=="use" (
  set "DIR=build-pgo-use"
  set "PGO=-w -fprofile-use=%PROFDIR:\=/%/profile.profdata"
) else (
  echo usage: windows-pgo.cmd gen^|merge^|use
  exit /b 1
)
cd /d "%ROOT%" || exit /b 1
if exist %DIR%\CMakeCache.txt del %DIR%\CMakeCache.txt
cmake -S . -B %DIR% -G Ninja -DCMAKE_BUILD_TYPE=Release -DYUZU_TESTS=OFF -DENABLE_WERROR=OFF ^
  -DENABLE_UPDATE_CHECKER=OFF -DENABLE_QT=OFF -DYUZU_CMD=ON ^
  -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_LINKER=lld-link ^
  "-DCMAKE_C_FLAGS_INIT=%PGO%" "-DCMAKE_CXX_FLAGS_INIT=%PGO%" || exit /b 1
cmake --build %DIR% --parallel %JOBS% --target yuzu-cmd
