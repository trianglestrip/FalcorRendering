# Generate sample .fnanite assets with NaniteBuilder and validate with NaniteLoader.
# Run from the repository root:
#   powershell -ExecutionPolicy Bypass -File scripts\build_nanite_assets.ps1

param(
    [string]$Configuration = "Release",
    [string]$Preset = "windows-vs2022"
)

$ErrorActionPreference = "Stop"

function Resolve-RepoRoot {
    $root = Split-Path -Parent $PSScriptRoot
    if (-not (Test-Path (Join-Path $root "CMakeLists.txt"))) {
        throw "Could not locate repository root from $PSScriptRoot"
    }
    return $root
}

function Resolve-NaniteTool {
    param(
        [string]$Root,
        [string]$Name,
        [string]$Configuration,
        [string]$Preset
    )

    $candidates = @(
        (Join-Path $Root "build\$Preset\bin\$Configuration\$Name.exe"),
        (Join-Path $Root "build\$Preset\bin\$Name.exe")
    )

    foreach ($path in $candidates) {
        if (Test-Path $path) {
            return $path
        }
    }

    throw @"
$Name.exe not found. Build the Nanite tools first:

  cmake --preset $Preset
  cmake --build build\$Preset --config $Configuration --target NaniteBuilder NaniteLoader
"@
}

function Invoke-External {
    param(
        [string]$FilePath,
        [string[]]$Arguments
    )

    Write-Host "> $FilePath $($Arguments -join ' ')"
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $FilePath"
    }
}

$repoRoot = Resolve-RepoRoot
Set-Location $repoRoot

$builderPath = Resolve-NaniteTool -Root $repoRoot -Name "NaniteBuilder" -Configuration $Configuration -Preset $Preset
$loaderPath = Resolve-NaniteTool -Root $repoRoot -Name "NaniteLoader" -Configuration $Configuration -Preset $Preset

$outputDir = Join-Path $repoRoot "data\nanite"
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null

$assets = @(
    @{
        Input  = "data\framework\meshes\cube.obj"
        Output = "data\nanite\cube.fnanite"
        Debug  = "data\nanite\cube.fnanite.json"
    }
)

foreach ($asset in $assets) {
    $inputPath = Join-Path $repoRoot $asset.Input
    if (-not (Test-Path $inputPath)) {
        throw "Input mesh not found: $inputPath"
    }

    Invoke-External -FilePath $builderPath -Arguments @(
        "--input", $inputPath,
        "--output", (Join-Path $repoRoot $asset.Output),
        "--debug-json", (Join-Path $repoRoot $asset.Debug)
    )

    Invoke-External -FilePath $loaderPath -Arguments @(
        "--input", (Join-Path $repoRoot $asset.Output),
        "--list-meshes",
        "--list-clusters"
    )
}

Write-Host ""
Write-Host "Nanite sample assets generated under data\nanite"
