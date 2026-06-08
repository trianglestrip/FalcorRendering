@echo off
setlocal
REM PBRT DXR RTGI sample - Bistro scene.

set "ROOT=%~dp0"
set "EXEDIR=%ROOT%build\windows-vs2022\bin\Release"
set "EXE=%EXEDIR%\pbrt_rtgi.exe"
set "SCENE=D:\models\pbrt-v4-scenes\bistro\bistro_cafe.pbrt"
set "CMAKE=%ROOT%tools\.packman\cmake\bin\cmake.exe"
set "BUILD_DIR=%ROOT%build\windows-vs2022"

if not exist "%BUILD_DIR%\CMakeCache.txt" (
    echo [INFO] Configuring VS2022 solution...
    call "%ROOT%setup_vs2022.bat"
    if errorlevel 1 exit /b 1
)

echo [INFO] Refreshing CMake project files...
"%CMAKE%" -S "%ROOT%." -B "%BUILD_DIR%"
if errorlevel 1 exit /b 1

if not exist "%EXE%" (
    echo [INFO] pbrt_rtgi.exe not found. Building first...
    "%CMAKE%" --build "%BUILD_DIR%" --config Release --target PBRTRTGI -- /m:1
    if errorlevel 1 (
        echo [ERROR] Build failed.
        pause
        exit /b 1
    )
)

if not exist "%EXE%" (
    echo [ERROR] pbrt_rtgi.exe still missing:
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

echo Starting PBRT DXR RTGI sample...
echo Scene: %SCENE%
echo.

start "PBRT DXR RTGI - Bistro" /D "%EXEDIR%" "%EXE%" --scene "%SCENE%"

endlocal
exit /b 0
