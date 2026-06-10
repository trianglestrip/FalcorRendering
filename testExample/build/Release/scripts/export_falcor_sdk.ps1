param(
    [string]$Preset = "windows-vs2022",
    [string]$Config = "Release",
    [string]$SdkDir = "",
    [switch]$SkipDependencyPull
)

$ErrorActionPreference = "Stop"

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$CMake = Join-Path $RepoRoot "tools\.packman\cmake\bin\cmake.exe"
$BuildDir = Join-Path $RepoRoot "build\$Preset"
$RuntimeDir = Join-Path $BuildDir "bin\$Config"

if (!(Get-Command pwsh.exe -ErrorAction SilentlyContinue)) {
    $PowerShell = (Get-Command powershell.exe).Source
    $ShimDir = Join-Path $RepoRoot "build\shims"
    $PwshShim = Join-Path $ShimDir "pwsh.exe"

    New-Item -ItemType Directory -Force -Path $ShimDir | Out-Null
    if (!(Test-Path $PwshShim)) {
        Copy-Item -LiteralPath $PowerShell -Destination $PwshShim -Force
    }

    $env:PATH = "$ShimDir;$env:PATH"
}

if ([string]::IsNullOrWhiteSpace($SdkDir)) {
    $SdkDir = Join-Path $RepoRoot "sdk\$Config"
} elseif (![System.IO.Path]::IsPathRooted($SdkDir)) {
    $SdkDir = Join-Path $RepoRoot $SdkDir
}

$SdkDir = [System.IO.Path]::GetFullPath($SdkDir)

if (!(Test-Path $CMake)) {
    if ($SkipDependencyPull) {
        throw "Missing CMake from Packman: $CMake"
    }

    $Packman = Join-Path $RepoRoot "tools\packman\packman.cmd"
    $Dependencies = Join-Path $RepoRoot "dependencies.xml"

    if (!(Test-Path $Packman)) {
        throw "Missing Packman bootstrap script: $Packman"
    }
    if (!(Test-Path $Dependencies)) {
        throw "Missing dependency manifest: $Dependencies"
    }

    & $Packman pull $Dependencies --platform windows-x86_64
}

& $CMake --preset $Preset -DCMAKE_VS_GLOBALS="VcpkgEnabled=false"
& $CMake --build $BuildDir --config $Config --target Mogwai
& $CMake --install $BuildDir --config $Config --prefix $SdkDir

if (!(Test-Path $RuntimeDir)) {
    throw "Missing runtime directory: $RuntimeDir"
}

New-Item -ItemType Directory -Force -Path (Join-Path $SdkDir "bin") | Out-Null

Get-ChildItem -Path $RuntimeDir -File | Where-Object {
    $_.Extension -in ".dll", ".json", ".bat", ".ps1"
} | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $SdkDir "bin") -Force
}

foreach ($dir in @("D3D12", "shaders", "data", "scripts", "plugins", "python")) {
    $src = Join-Path $RuntimeDir $dir
    if (Test-Path $src) {
        Copy-Item -LiteralPath $src -Destination (Join-Path $SdkDir "bin") -Recurse -Force
    }
}

$required = @(
    "bin\Falcor.dll",
    "lib\Falcor.lib",
    "include\Falcor\Falcor.h",
    "include\Falcor\Core\API\Device.h",
    "include\Falcor\Scene\Scene.h",
    "cmake\FalcorConfig.cmake",
    "bin\settings.json",
    "bin\shaders",
    "bin\plugins\plugins.json"
)

foreach ($item in $required) {
    $path = Join-Path $SdkDir $item
    if (!(Test-Path $path)) {
        throw "Falcor SDK export is incomplete. Missing: $item"
    }
}

Write-Host "Falcor SDK exported to $SdkDir"
