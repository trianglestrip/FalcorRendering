@echo off
setlocal
REM Interactive PBRT viewer launcher.
REM   pbrt_viewer.bat           open default scene in realtime preview mode
REM   pbrt_viewer.bat [args]    pass custom args to pbrt_viewer.exe

set "ROOT=%~dp0"
set "EXEDIR=%ROOT%build\windows-vs2022\bin\Release"
set "EXE=%EXEDIR%\pbrt_viewer.exe"
set "DEFAULT_SCENE=D:\models\pbrt-v4-scenes\barcelona-pavilion\pavilion-day.pbrt"

if not exist "%EXE%" (
    echo [INFO] pbrt_viewer.exe not found. Building first...
    call "%ROOT%build_pbrt_renderer.bat"
    if errorlevel 1 (
        echo [ERROR] Build failed.
        pause
        exit /b 1
    )
)

if not exist "%EXE%" (
    echo [ERROR] pbrt_viewer.exe still missing:
    echo         %EXE%
    pause
    exit /b 1
)

if "%~1"=="" (
    if not exist "%DEFAULT_SCENE%" (
        echo [ERROR] Default scene not found:
        echo         %DEFAULT_SCENE%
        echo Pass a scene explicitly, for example:
        echo         pbrt_viewer.bat --preview --scene "D:\path\scene.pbrt"
        pause
        exit /b 1
    )
    start "PBRT Viewer" /D "%EXEDIR%" "%EXE%" --preview --scene "%DEFAULT_SCENE%"
) else (
    start "PBRT Viewer" /D "%EXEDIR%" "%EXE%" %*
)

endlocal
exit /b 0
