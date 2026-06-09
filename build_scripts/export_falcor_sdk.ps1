param(
    [string]$Preset = "windows-vs2022",
    [string]$Config = "Release",
    [string]$SdkDir = ""
)

$ErrorActionPreference = "Stop"

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$CMake = Join-Path $RepoRoot "tools\.packman\cmake\bin\cmake.exe"
$BuildDir = Join-Path $RepoRoot "build\$Preset"
$RuntimeDir = Join-Path $BuildDir "bin\$Config"
$FalcorLibDir = Join-Path $BuildDir "Source\Falcor\$Config"

if ([string]::IsNullOrWhiteSpace($SdkDir)) {
    $SdkDir = Join-Path $RepoRoot "FalcorSDK"
} elseif (![System.IO.Path]::IsPathRooted($SdkDir)) {
    $SdkDir = Join-Path $RepoRoot $SdkDir
}
$SdkDir = [System.IO.Path]::GetFullPath($SdkDir)

Write-Host "=== Exporting Falcor SDK ==="
Write-Host "Config:     $Config"
Write-Host "Build dir:  $BuildDir"
Write-Host "SDK output: $SdkDir"
Write-Host ""

# Validate paths
if (!(Test-Path $RuntimeDir)) {
    throw "Missing runtime directory: $RuntimeDir. Build Falcor first!"
}
if (!(Test-Path (Join-Path $FalcorLibDir "Falcor.lib"))) {
    throw "Missing Falcor.lib. Build Falcor target first!"
}
if (!(Test-Path $CMake)) {
    throw "Missing CMake: $CMake"
}

# Clean existing SDK output
if (Test-Path $SdkDir) {
    Write-Host "Cleaning existing SDK directory..."
    Remove-Item -Path $SdkDir -Recurse -Force
}

Write-Host "Creating SDK directory structure..."
New-Item -ItemType Directory -Force -Path (Join-Path $SdkDir "bin")  | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $SdkDir "lib")  | Out-Null

# ── Step 1: Copy Falcor headers ──────────────────────────────────────
Write-Host "Copying Falcor headers..."
$headerSrc = Join-Path $RepoRoot "Source\Falcor"
$headerDst = Join-Path $SdkDir "include\Falcor"
Get-ChildItem -Path $headerSrc -Recurse -Include "*.h", "*.slangh" | ForEach-Object {
    $relPath = $_.FullName.Substring($headerSrc.Length + 1)
    $target = Join-Path $headerDst $relPath
    $targetDir = Split-Path $target -Parent
    if (!(Test-Path $targetDir)) { New-Item -ItemType Directory -Force -Path $targetDir | Out-Null }
    Copy-Item -LiteralPath $_.FullName -Destination $target -Force
}

# ── Step 2: Copy external headers (minimal) ─────────────────────────
Write-Host "Copying external headers (fmt, pybind11, imgui, vulkan-headers, etc.)..."

$externalHeaders = @(
    @{src="external\fmt\include"; dst="include\external\fmt"}
    @{src="external\pybind11\include"; dst="include\external\pybind11"}
    @{src="external\vulkan-headers\include"; dst="include\external\vulkan-headers"}
    @{src="external\imgui"; dst="include\external\imgui"; filter="*.h"}
    @{src="external\imgui_addons"; dst="include\external\imgui_addons"; filter="*.h"}
    @{src="external\include"; dst="include\external\include"}
    @{src="external\mikktspace"; dst="include\external\mikktspace"; filter="*.h"}
)

foreach ($item in $externalHeaders) {
    $src = Join-Path $RepoRoot $item.src
    $dst = Join-Path $SdkDir $item.dst
    $filter = if ($item.ContainsKey("filter")) { $item.filter } else { "*" }

    if (Test-Path $src) {
        New-Item -ItemType Directory -Force -Path $dst | Out-Null
        if ($filter -eq "*") {
            Copy-Item -Path "$src\*" -Destination $dst -Recurse -Force -ErrorAction SilentlyContinue
        } else {
            Get-ChildItem -Path $src -Recurse -Include $filter | ForEach-Object {
                $relPath = $_.FullName.Substring($src.Length + 1)
                $target = Join-Path $dst $relPath
                $targetDir = Split-Path $target -Parent
                if (!(Test-Path $targetDir)) { New-Item -ItemType Directory -Force -Path $targetDir | Out-Null }
                Copy-Item -LiteralPath $_.FullName -Destination $target -Force
            }
        }
    }
}

# Copy slang headers
$slangInclude = Join-Path $RepoRoot "external\packman\slang\include"
if (Test-Path $slangInclude) {
    $dst = Join-Path $SdkDir "include\external\slang"
    New-Item -ItemType Directory -Force -Path $dst | Out-Null
    Copy-Item -Path "$slangInclude\*" -Destination $dst -Recurse -Force
}

# Copy RTXDI headers
$rtxdiInclude = Join-Path $RepoRoot "external\packman\rtxdi\include"
if (Test-Path $rtxdiInclude) {
    $dst = Join-Path $SdkDir "include\external\rtxdi"
    New-Item -ItemType Directory -Force -Path $dst | Out-Null
    Copy-Item -Path "$rtxdiInclude\*" -Destination $dst -Recurse -Force
}

# ── Step 3: Copy Falcor.lib ──────────────────────────────────────────
Write-Host "Copying Falcor.lib..."
Copy-Item -Path (Join-Path $FalcorLibDir "Falcor.lib") -Destination (Join-Path $SdkDir "lib") -Force

# ── Step 4: Copy runtime DLLs (no pdb/ilk/exp) ───────────────────────
Write-Host "Copying runtime DLLs..."
Get-ChildItem -Path $RuntimeDir -File | Where-Object {
    $_.Extension -in ".dll", ".json", ".bat", ".ps1"
} | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $SdkDir "bin") -Force
}

# ── Step 5: Copy runtime directories (no pdb) ────────────────────────
Write-Host "Copying runtime directories..."
foreach ($dir in @("D3D12", "shaders", "data", "scripts", "plugins", "python", "pythondist", "usd")) {
    $src = Join-Path $RuntimeDir $dir
    if (Test-Path $src) {
        $dst = Join-Path $SdkDir "bin"
        Copy-Item -LiteralPath $src -Destination $dst -Recurse -Force
    }
}

# ── Step 6: Copy CMake config ────────────────────────────────────────
Write-Host "Copying CMake config..."
$cmakeConfigSrc = Join-Path $BuildDir "FalcorConfig.cmake"
$cmakeTargetsSrc = Join-Path $BuildDir "cmake\FalcorTargets.cmake"
$cmakeDst = Join-Path $SdkDir "cmake"
New-Item -ItemType Directory -Force -Path $cmakeDst | Out-Null

if (Test-Path $cmakeConfigSrc) {
    Copy-Item -LiteralPath $cmakeConfigSrc -Destination $cmakeDst -Force
}
if (Test-Path $cmakeTargetsSrc) {
    Copy-Item -LiteralPath $cmakeTargetsSrc -Destination $cmakeDst -Force
}

# ── Step 7: Remove any stray .pdb/.ilk/.exp files ────────────────────
Write-Host "Removing debug files (.pdb, .ilk, .exp)..."
Get-ChildItem -Path $SdkDir -Recurse -Include "*.pdb", "*.ilk", "*.exp", "*.iobj", "*.ipdb" -ErrorAction SilentlyContinue | ForEach-Object {
    Remove-Item -LiteralPath $_.FullName -Force -ErrorAction SilentlyContinue
}

# ── Verification ──────────────────────────────────────────────────────
Write-Host ""
Write-Host "=== Verification ==="
$checkFalcorDll = Join-Path $SdkDir "bin\Falcor.dll"
$checkFalcorLib = Join-Path $SdkDir "lib\Falcor.lib"
$checkFalcorH   = Join-Path $SdkDir "include\Falcor\Falcor.h"
$checkDeviceH   = Join-Path $SdkDir "include\Falcor\Core\API\Device.h"
$checkSettings  = Join-Path $SdkDir "bin\settings.json"
$checkShaders   = Join-Path $SdkDir "bin\shaders"
$checkPlugins   = Join-Path $SdkDir "bin\plugins\plugins.json"

$allOk = $true
if (!(Test-Path $checkFalcorDll)) { Write-Warning "Missing: Falcor.dll"; $allOk = $false }
if (!(Test-Path $checkFalcorLib)) { Write-Warning "Missing: Falcor.lib"; $allOk = $false }
if (!(Test-Path $checkFalcorH))   { Write-Warning "Missing: Falcor.h"; $allOk = $false }
if (!(Test-Path $checkDeviceH))   { Write-Warning "Missing: Device.h"; $allOk = $false }
if (!(Test-Path $checkSettings))  { Write-Warning "Missing: settings.json"; $allOk = $false }
if (!(Test-Path $checkShaders))   { Write-Warning "Missing: shaders/"; $allOk = $false }
if (!(Test-Path $checkPlugins))   { Write-Warning "Missing: plugins.json"; $allOk = $false }

if ($allOk) {
    # Calculate total size
    $totalSize = (Get-ChildItem -Path $SdkDir -Recurse -File | Measure-Object -Property Length -Sum).Sum
    $sizeMB = [math]::Round($totalSize / 1MB, 1)
    $fileCount = (Get-ChildItem -Path $SdkDir -Recurse -File | Measure-Object).Count

    Write-Host "✓ Falcor SDK export complete!"
    Write-Host "  Location: $SdkDir"
    Write-Host "  Files:    $fileCount"
    Write-Host "  Size:     $sizeMB MB"
    Write-Host ""
    Write-Host "SDK structure:"
    Get-ChildItem -Path $SdkDir -Depth 1 | ForEach-Object {
        $name = if ($_.PSIsContainer) { "$($_.Name)/" } else { $_.Name }
        Write-Host "  $name"
    }
} else {
    Write-Warning "SDK export incomplete. Check warnings above."
}
