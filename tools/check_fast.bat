@echo off
rem check_fast.bat - the quick check layer: build the sim-only tree (cmake-build-sim,
rem LR_SIM_ONLY) and run the ctest label "fast". Prints only the outcome; the
rem full logs stay in cmake-build-sim\check_build.log and check_test.log.
rem
rem   tools\check_fast.bat                 build + fast tests
rem   tools\check_fast.bat full            build + all sim tests (fast + full)
rem   tools\check_fast.bat build           build only
rem   tools\check_fast.bat -R lr_sim   extra args go to ctest (rerun one test by name)
rem   tools\check_fast.bat rerun           rerun only the tests that failed last time
setlocal enabledelayedexpansion
cd /d "%~dp0.."
set "ROOT=%CD%"
set "BUILD=%ROOT%\cmake-build-sim"
set "TPRESET=sim-fast"
set "MODE=test"
set "CTEST_EXTRA="
if /i "%~1"=="full"  (set "TPRESET=sim-full" & shift)
if /i "%~1"=="build" (set "MODE=build" & shift)
if /i "%~1"=="rerun" (set "CTEST_EXTRA=--rerun-failed" & shift)
:args
if "%~1"=="" goto args_done
set "CTEST_EXTRA=!CTEST_EXTRA! %1"
shift
goto args
:args_done

rem --- toolchain: MSVC env for clang-cl's linker, CLion's bundled cmake/ninja if none on PATH
if not defined VCToolsInstallDir (
    set "VSDEV=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat"
    if not exist "!VSDEV!" (echo check_fast: VsDevCmd.bat not found & exit /b 2)
    call "!VSDEV!" -arch=x64 -host_arch=x64 >nul 2>&1
)
where cmake >nul 2>&1
if errorlevel 1 (
    for /d %%D in ("C:\Program Files\JetBrains\CLion*") do (
        if exist "%%D\bin\cmake\win\x64\bin\cmake.exe" set "PATH=%%D\bin\cmake\win\x64\bin;%%D\bin\ninja\win\x64;!PATH!"
    )
)
where cmake >nul 2>&1
if errorlevel 1 (echo check_fast: cmake not found & exit /b 2)

if not exist "%BUILD%\CMakeCache.txt" (
    cmake --preset sim > "%ROOT%\cmake-build-sim-configure.log" 2>&1
    if errorlevel 1 (
        echo CONFIGURE FAILED, log: %ROOT%\cmake-build-sim-configure.log
        powershell -NoProfile -Command "Get-Content '%ROOT%\cmake-build-sim-configure.log' | Select-String -Pattern 'error' -SimpleMatch | Select-Object -First 20 | ForEach-Object { $_.Line }"
        exit /b 1
    )
    move /y "%ROOT%\cmake-build-sim-configure.log" "%BUILD%\check_configure.log" >nul
)

set "T0=%time%"
cmake --build --preset sim > "%BUILD%\check_build.log" 2>&1
if errorlevel 1 (
    echo BUILD FAILED, full log: %BUILD%\check_build.log
    powershell -NoProfile -Command "Get-Content '%BUILD%\check_build.log' | Select-String -Pattern 'error' | Select-Object -First 20 | ForEach-Object { $_.Line }"
    exit /b 1
)
echo build ok  (%T0% - %time%)
if "%MODE%"=="build" exit /b 0

ctest --preset %TPRESET% %CTEST_EXTRA% > "%BUILD%\check_test.log" 2>&1
set "RC=%errorlevel%"
powershell -NoProfile -Command "$l = Get-Content '%BUILD%\check_test.log'; $l | Select-String -Pattern 'tests passed|\*\*\*Failed|\*\*\*Timeout|Exception|  FAIL ' | Select-Object -First 20 | ForEach-Object { $_.Line }; $l | Select-String -Pattern 'Total Test time' | ForEach-Object { $_.Line }"
if not "%RC%"=="0" (
    echo TESTS FAILED, full log: %BUILD%\check_test.log
    echo rerun failed:  tools\check_fast.bat rerun     one by name:  tools\check_fast.bat -R ^<name^>
    exit /b 1
)
exit /b 0
