@echo off
setlocal
REM Interactive PBRT viewer launcher.
REM   pbrt_viewer.bat           open default scene in realtime preview mode
REM   pbrt_viewer.bat [args]    pass custom args to pbrt_viewer.exe
REM Default launch prewarms local shader/scene caches when the executable,
REM shader sources, renderer sources, git revision, or default scene changes.

set "ROOT=%~dp0"
set "EXEDIR=%ROOT%build\windows-vs2022\bin\Release"
set "EXE=%EXEDIR%\pbrt_viewer.exe"
set "DEFAULT_SCENE=D:\models\pbrt-v4-scenes\barcelona-pavilion\pavilion-day.pbrt"
set "CACHE_DIR=%EXEDIR%\.pbrt_viewer_cache"
set "WARMUP_STAMP_FILE=%CACHE_DIR%\warmup.stamp"

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

    if not exist "%CACHE_DIR%" mkdir "%CACHE_DIR%" >nul 2>nul
    for /f "usebackq delims=" %%S in (`powershell -NoProfile -ExecutionPolicy Bypass -Command "$root=(Resolve-Path '%ROOT%').Path; $exe=(Resolve-Path '%EXE%').Path; $scene=(Resolve-Path '%DEFAULT_SCENE%').Path; $paths=@($exe,$scene, (Join-Path $root 'Source\Samples\PBRTOfflineRenderer'), (Join-Path $root 'Source\RenderPasses\FilamentFX'), (Join-Path $root 'Source\RenderPasses\FilamentAOPass'), (Join-Path $root 'Source\RenderPasses\DeferredAOPass'), (Join-Path $root 'Source\plugins\importers\PBRTImporter')); $latest=0L; foreach($p in $paths){ if(Test-Path $p){ $items=if((Get-Item $p).PSIsContainer){ Get-ChildItem $p -Recurse -File -Include *.slang,*.slangh,*.cpp,*.h,*.bat,*.cmake,CMakeLists.txt } else { Get-Item $p }; foreach($i in $items){ if($i.LastWriteTimeUtc.Ticks -gt $latest){ $latest=$i.LastWriteTimeUtc.Ticks } } } }; $git=(& git -C $root rev-parse HEAD 2>$null); if(-not $git){ $git='nogit' }; & git -C $root diff --quiet 2>$null; $dirty=if($LASTEXITCODE -eq 0){'clean'}else{'dirty'}; $stamp='v2|' + $git + '|' + $dirty + '|' + $latest; Write-Output $stamp"`) do set "WARMUP_STAMP=%%S"

    set "OLD_WARMUP_STAMP="
    if exist "%WARMUP_STAMP_FILE%" set /p OLD_WARMUP_STAMP=<"%WARMUP_STAMP_FILE%"
    if not "%WARMUP_STAMP%"=="%OLD_WARMUP_STAMP%" (
        echo [INFO] PBRT viewer cache warmup required.
        pushd "%EXEDIR%"
        "%EXE%" --warmup-cache --fast-materials --scene "%DEFAULT_SCENE%"
        if errorlevel 1 (
            popd
            echo [WARN] Cache warmup failed. Starting viewer anyway.
        ) else (
            popd
            >"%WARMUP_STAMP_FILE%" echo %WARMUP_STAMP%
            echo [INFO] PBRT viewer cache warmup complete.
        )
    ) else (
        echo [INFO] PBRT viewer cache warmup is current.
    )
    start "PBRT Viewer" /D "%EXEDIR%" "%EXE%" --preview --fast-materials --scene "%DEFAULT_SCENE%"
) else (
    start "PBRT Viewer" /D "%EXEDIR%" "%EXE%" %*
)

endlocal
exit /b 0
