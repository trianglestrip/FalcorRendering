@echo off
setlocal enabledelayedexpansion
:: ============================================================
:: Build minimal Falcor.dll for MiniFalcorSDK
:: Sources are in the parent FalcorSDKBuild directory
:: ============================================================
set SCRIPT_DIR=%~dp0
set SCRIPT_DIR=%SCRIPT_DIR:~0,-1%
set FALCOR_SRC=%SCRIPT_DIR%\..
set BUILD_DIR=%SCRIPT_DIR%\build
set OUT_BIN=%SCRIPT_DIR%\bin
set OUT_LIB=%SCRIPT_DIR%\lib
set OUT_INCLUDE=%SCRIPT_DIR%\include

echo ============================================================
echo  Building Minimal Falcor.dll for MiniFalcorSDK
echo  Source: %FALCOR_SRC%
echo  Build:  %BUILD_DIR%
echo ============================================================

:: ── Step 1: Configure CMake ──────────────────────────────────
echo.
echo [1/4] Configuring CMake (minimal flags)...

cmake -B "%BUILD_DIR%" -S "%FALCOR_SRC%" ^
    -G "Visual Studio 17 2022" ^
    -G "Visual Studio 17 2022" -A x64 ^
    -DFALCOR_ENABLE_USD=OFF ^
    -DFALCOR_ENABLE_NANITE=OFF ^
    -DFALCOR_ENABLE_SDFS=OFF ^
    -DFALCOR_ENABLE_DIFFREND=OFF ^
    -DFALCOR_ENABLE_ANIMATION=OFF ^
    -DFALCOR_ENABLE_CURVES=OFF ^
    -DFALCOR_ENABLE_VOLUMES=OFF ^
    -DFALCOR_ENABLE_DISPLACEMENT=OFF ^
    -DFALCOR_ENABLE_MERL=OFF ^
    -DFALCOR_ENABLE_RGL=OFF ^
    -DFALCOR_ENABLE_HAIRCLOTH=OFF ^
    -DFALCOR_ENABLE_PBRT_MAT=ON ^
    -DFALCOR_ENABLE_PROFILER=ON
if %ERRORLEVEL% neq 0 (
    echo ERROR: CMake configuration failed!
    exit /b 1
)

:: ── Step 2: Build Falcor.dll only ────────────────────────────
echo.
echo [2/4] Building Falcor.dll (Release, minimal config)...

cmake --build "%BUILD_DIR%" --config Release --target Falcor -- /m
if %ERRORLEVEL% neq 0 (
    echo ERROR: Build failed!
    exit /b 1
)

:: ── Step 3: Copy outputs to MiniFalcorSDK ────────────────────
echo.
echo [3/4] Copying outputs to MiniFalcorSDK...

:: Find the actual output directory (Release subfolder for multi-config generators)
set SRC_BIN=%BUILD_DIR%\bin\Release

if not exist "%SRC_BIN%\Falcor.dll" (
    echo ERROR: Falcor.dll not found at %SRC_BIN%
    exit /b 1
)

:: Copy DLL and LIB
copy /Y "%SRC_BIN%\Falcor.dll" "%OUT_BIN%\" >nul
copy /Y "%SRC_BIN%\Falcor.lib" "%OUT_LIB%\" >nul
echo   Copied Falcor.dll and Falcor.lib

:: Copy shaders
if exist "%SRC_BIN%\shaders" (
    robocopy "%SRC_BIN%\shaders" "%OUT_BIN%\shaders" /E /NFL /NDL /NJH /NJS >nul
    echo   Copied shaders/
)

:: Copy additional runtime DLLs needed by Falcor.dll
for %%f in (
    slang.dll slang-glslang.dll slang-llvm.dll slang-rt.dll
    dxcompiler.dll dxil.dll
    assimp-vc143-mt.dll
    FreeImage.dll FreeImagePlus.dll
    Half-2_4.dll Iex-2_4.dll IexMath-2_4.dll IlmImf-2_4.dll IlmImfUtil-2_4.dll IlmThread-2_4.dll Imath-2_4.dll
    tbb.dll tbbmalloc.dll tbbmalloc_proxy.dll
    zlib.dll lz4.dll blosc.dll
    python3.dll python310.dll
    gfx.dll
    WinPixEventRuntime.dll
    cudart64_110.dll
) do (
    if exist "%SRC_BIN%\%%f" (
        copy /Y "%SRC_BIN%\%%f" "%OUT_BIN%\" >nul 2>nul
    )
)
echo   Copied runtime DLLs

:: Copy data
if exist "%SRC_BIN%\data" (
    robocopy "%SRC_BIN%\data" "%OUT_BIN%\data" /E /NFL /NDL /NJH /NJS >nul
    echo   Copied data/
)

:: ── Step 4: Install headers ──────────────────────────────────
echo.
echo [4/4] Installing headers...

:: Copy main Falcor headers
robocopy "%FALCOR_SRC%\Source\Falcor" "%OUT_INCLUDE%\Falcor" *.h *.slangh /S /NFL /NDL /NJH /NJS >nul

:: Copy external headers
robocopy "%FALCOR_SRC%\external\fmt"        "%OUT_INCLUDE%\external\fmt"        *.h *.hpp *.inl /S /NFL /NDL /NJH /NJS >nul
robocopy "%FALCOR_SRC%\external\pybind11"   "%OUT_INCLUDE%\external\pybind11"   *.h *.hpp /S /NFL /NDL /NJH /NJS >nul
robocopy "%FALCOR_SRC%\external\vulkan-headers" "%OUT_INCLUDE%\external\vulkan-headers" *.h *.hpp /S /NFL /NDL /NJH /NJS >nul
robocopy "%FALCOR_SRC%\external\imgui"      "%OUT_INCLUDE%\external\imgui"      *.h /S /NFL /NDL /NJH /NJS >nul
robocopy "%FALCOR_SRC%\external\imgui_addons" "%OUT_INCLUDE%\external\imgui_addons" *.h *.hpp /S /NFL /NDL /NJH /NJS >nul
robocopy "%FALCOR_SRC%\external\include"    "%OUT_INCLUDE%\external\include"    *.h *.hpp *.inl /S /NFL /NDL /NJH /NJS >nul
robocopy "%FALCOR_SRC%\external\mikktspace" "%OUT_INCLUDE%\external\mikktspace" *.h /S /NFL /NDL /NJH /NJS >nul

if exist "%FALCOR_SRC%\external\packman\deps\include" (
    robocopy "%FALCOR_SRC%\external\packman\deps\include" "%OUT_INCLUDE%\external\packman\deps\include" /S /NFL /NDL /NJH /NJS >nul
)
if exist "%FALCOR_SRC%\external\packman\slang\include" (
    robocopy "%FALCOR_SRC%\external\packman\slang\include" "%OUT_INCLUDE%\external\packman\slang\include" /S /NFL /NDL /NJH /NJS >nul
)

echo.
echo ============================================================
echo  MiniFalcorSDK built successfully!
echo  Output: %SCRIPT_DIR%
echo   bin\     - Falcor.dll + shaders + runtime DLLs
echo   lib\     - Falcor.lib
echo   include\ - Headers
echo ============================================================
