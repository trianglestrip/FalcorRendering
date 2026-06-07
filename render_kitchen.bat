@echo off
REM PBRT Offline Renderer - Kitchen Scene
REM Launch the renderer with the kitchen PBRT scene

set "ROOT=%~dp0"
set "EXE=%ROOT%build\windows-vs2022\bin\Release\pbrt_viewer.exe"
set "SCENE=D:\gitProject\VLR_WF\models\kitchen\scene-v4.pbrt"
set "FALLBACK_SCENE=D:\models\pbrt-v4-scenes\barcelona-pavilion\pavilion-day.pbrt"
set "OUTPUT=%ROOT%docs\development\debug_images\render_kitchen_output.png"

if not exist "%EXE%" (
    echo [INFO] pbrt_viewer.exe not found, building...
    call "%~dp0build_pbrt_renderer.bat"
    if errorlevel 1 (
        echo [ERROR] Build failed.
        pause
        exit /b 1
    )
)

if not exist "%SCENE%" (
    echo [WARN] Kitchen scene file not found:
    echo        %SCENE%
    if exist "%FALLBACK_SCENE%" (
        echo [INFO] Falling back to:
        echo        %FALLBACK_SCENE%
        set "SCENE=%FALLBACK_SCENE%"
        set "OUTPUT=%ROOT%docs\development\debug_images\render_barcelona_output.png"
    ) else (
        echo [ERROR] Fallback scene file not found:
        echo         %FALLBACK_SCENE%
        pause
        exit /b 1
    )
)

if not exist "%ROOT%docs\development\debug_images" mkdir "%ROOT%docs\development\debug_images"

echo Starting PBRT Offline Renderer...
echo Scene : %SCENE%
echo Output: %OUTPUT%
echo.

"%EXE%" --headless --preview --single-frame --scene "%SCENE%" --output "%OUTPUT%" --enable-shadows
if errorlevel 1 (
    if exist "%OUTPUT%" (
        echo.
        echo Render process returned a non-zero code, but output exists:
        echo %OUTPUT%
        exit /b 0
    )
    echo.
    echo Render failed.
    pause
    exit /b 1
)

echo.
echo Render complete: %OUTPUT%
exit /b 0
