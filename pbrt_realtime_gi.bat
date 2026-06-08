@echo off
setlocal
REM PBRT realtime GI example - Bistro scene.

set "ROOT=%~dp0"
set "EXEDIR=%ROOT%build\windows-vs2022\bin\Release"
set "EXE=%EXEDIR%\pbrt_realtime_gi.exe"
set "SCENE=D:\models\pbrt-v4-scenes\bistro\bistro_cafe.pbrt"
set "CMAKE=%ROOT%tools\.packman\cmake\bin\cmake.exe"
set "BUILD_DIR=%ROOT%build\windows-vs2022"

if not exist "%BUILD_DIR%\CMakeCache.txt" (
    echo [INFO] Configuring VS2022 solution...
    call "%ROOT%setup_vs2022.bat"
    if errorlevel 1 exit /b 1
)

if exist "%BUILD_DIR%\CMakeCache.txt" (
    echo [INFO] Refreshing CMake project files...
    "%CMAKE%" -S "%ROOT%." -B "%BUILD_DIR%"
    if errorlevel 1 exit /b 1
)

if not exist "%EXE%" (
    echo [INFO] pbrt_realtime_gi.exe not found. Building first...
    "%CMAKE%" --build "%BUILD_DIR%" --config Release --target PBRTRealtimeGI -- /m:1
    if errorlevel 1 (
        echo [ERROR] Build failed.
        pause
        exit /b 1
    )
)

if not exist "%EXE%" (
    echo [ERROR] pbrt_realtime_gi.exe still missing:
    echo         %EXE%
    pause
    exit /b 1
)

if not exist "%SCENE%" (
    echo [ERROR] Scene file not found:
    echo         %SCENE%
    pause
    exit /b 1
)

echo Starting PBRT Realtime GI example...
echo Scene: %SCENE%
echo.

start "PBRT Realtime GI - Bistro" /D "%EXEDIR%" "%EXE%" --scene "%SCENE%"

endlocal
exit /b 0
