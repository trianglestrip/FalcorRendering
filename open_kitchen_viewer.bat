@echo off
setlocal

REM Open PBRT Offline Renderer in interactive GUI mode.
REM   open_kitchen_viewer.bat           default scene with preview
REM   open_kitchen_viewer.bat [args]    pass custom args to PBRTOfflineRenderer.exe

set "ROOT=%~dp0"
set "EXE=%ROOT%build\windows-vs2022\bin\Release\PBRTOfflineRenderer.exe"
set "SCENE=D:\models\pbrt-v4-scenes\zero-day\frame35.pbrt"

if not exist "%EXE%" (
    echo [INFO] PBRTOfflineRenderer.exe not found. Building...
    call "%ROOT%build_pbrt_renderer.bat"
    if errorlevel 1 (
        echo [ERROR] Build failed.
        pause
        exit /b 1
    )
)

if "%~1"=="" (
    if not exist "%SCENE%" (
        echo [ERROR] Default scene not found:
        echo         %SCENE%
        pause
        exit /b 1
    )
    echo Opening PBRT Viewer with default scene...
    echo Scene: %SCENE%
    echo.
    start "" "%EXE%" --preview --scene "%SCENE%"
) else (
    start "" "%EXE%" %*
)

endlocal
exit /b 0
