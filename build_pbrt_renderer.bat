@echo off
REM Reliable build for PBRTOfflineRenderer (avoids PCH/link races from parallel MSBuild).
setlocal

set ROOT=%~dp0
set CMAKE=%ROOT%tools\.packman\cmake\bin\cmake.exe
set BUILD_DIR=%ROOT%build\windows-vs2022

if not exist "%BUILD_DIR%\CMakeCache.txt" (
    echo [INFO] Configuring VS2022 solution...
    call "%ROOT%setup_vs2022.bat"
    if errorlevel 1 (
        echo [ERROR] CMake configure failed.
        exit /b 1
    )
)

echo [INFO] Building Falcor (single-threaded)...
"%CMAKE%" --build "%BUILD_DIR%" --config Release --target Falcor -- /m:1
if errorlevel 1 (
    echo [ERROR] Falcor build failed.
    exit /b 1
)

echo [INFO] Building FilamentPostProcessLib...
"%CMAKE%" --build "%BUILD_DIR%" --config Release --target FilamentPostProcessLib -- /m:1
if errorlevel 1 (
    echo [ERROR] FilamentPostProcessLib build failed.
    exit /b 1
)

echo [INFO] Building PBRTOfflineRenderer...
"%CMAKE%" --build "%BUILD_DIR%" --config Release --target PBRTOfflineRenderer -- /m:1
if errorlevel 1 (
    echo [ERROR] PBRTOfflineRenderer build failed.
    exit /b 1
)

echo [OK] %BUILD_DIR%\bin\Release\PBRTOfflineRenderer.exe
exit /b 0
