@echo off
REM Build PBRTOfflineRenderer - minimal incremental compile by default.
REM   build_pbrt_renderer.bat          incremental only stale targets
REM   build_pbrt_renderer.bat full     Falcor then Lib then App
REM   build_pbrt_renderer.bat clean    clean then incremental
setlocal

set ROOT=%~dp0
set CMAKE=%ROOT%tools\.packman\cmake\bin\cmake.exe
set BUILD_DIR=%ROOT%build\windows-vs2022
set MODE=%~1
if "%MODE%"=="" set MODE=incremental

if not exist "%BUILD_DIR%\CMakeCache.txt" (
    echo [INFO] Configuring VS2022 solution...
    call "%ROOT%setup_vs2022.bat"
    if errorlevel 1 exit /b 1
)

if /I "%MODE%"=="clean" (
    echo [INFO] Cleaning PBRTOfflineRenderer and FilamentPostProcessLib
    "%CMAKE%" --build "%BUILD_DIR%" --config Release --target PBRTOfflineRenderer -- /t:Clean /m:1
    "%CMAKE%" --build "%BUILD_DIR%" --config Release --target FilamentPostProcessLib -- /t:Clean /m:1
)

if /I "%MODE%"=="full" goto do_full
goto do_incremental

:do_full
echo [INFO] Full chain build m:1
"%CMAKE%" --build "%BUILD_DIR%" --config Release --target Falcor -- /m:1
if errorlevel 1 exit /b 1
"%CMAKE%" --build "%BUILD_DIR%" --config Release --target FilamentPostProcessLib -- /m:1
if errorlevel 1 exit /b 1
"%CMAKE%" --build "%BUILD_DIR%" --config Release --target PBRTOfflineRenderer -- /m:1
if errorlevel 1 exit /b 1
goto done

:do_incremental
echo [INFO] Incremental build PBRTOfflineRenderer m:1
"%CMAKE%" --build "%BUILD_DIR%" --config Release --target PBRTOfflineRenderer -- /m:1
if errorlevel 1 (
    echo [ERROR] Build failed. Try: build_pbrt_renderer.bat clean
    echo [HINT] Do not use -j or high /m on full solution.
    exit /b 1
)

:done
if not exist "%BUILD_DIR%\bin\Release\pbrt_viewer.exe" (
    echo [ERROR] Output missing.
    exit /b 1
)
echo [OK] %BUILD_DIR%\bin\Release\pbrt_viewer.exe
exit /b 0
