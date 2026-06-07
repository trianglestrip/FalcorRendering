@echo off
setlocal

REM Open PBRT Offline Renderer in interactive GUI mode.
REM   open_kitchen_viewer.bat           default scene with preview
REM   open_kitchen_viewer.bat [args]    pass custom args to PBRTOfflineRenderer.exe

set "ROOT=%~dp0"
set "EXE=%ROOT%build\windows-vs2022\bin\Release\PBRTOfflineRenderer.exe"
set "EXEDIR=%ROOT%build\windows-vs2022\bin\Release"
set "SCENE=D:\models\pbrt-v4-scenes\zero-day\frame35.pbrt"

REM Always rebuild to pick up latest changes
echo [INFO] Building PBRTOfflineRenderer...
call "%ROOT%build_pbrt_renderer.bat"
if errorlevel 1 (
    echo [ERROR] Build failed.
    pause
    exit /b 1
)

if not exist "%EXE%" (
    echo [ERROR] PBRTOfflineRenderer.exe not found after build:
    echo         %EXE%
    pause
    exit /b 1
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
    start "" /D "%EXEDIR%" "%EXE%" --preview --scene "%SCENE%"
) else (
    start "" /D "%EXEDIR%" "%EXE%" %*
)

endlocal
exit /b 0
