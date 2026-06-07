param(
    [string]$ProductName = "os-master-freebsd",
    [string]$FreebsdRelease = "14.4-RELEASE",
    [string]$FreebsdArch = "amd64",
    [string]$FreebsdImageBasename = "dvd1.iso",
    [string]$BuildDir = "build/freebsd",
    [string]$OutputDir = "image",
    [string]$WslCacheRoot = "~/.cache/os-master-freebsd",
    [switch]$SkipDependencyInstall
)

$ErrorActionPreference = "Stop"

function Require-Command {
    param([string]$Name)
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "Required command not found: $Name"
    }
}

Require-Command "wsl.exe"

$repoWindowsPath = (Get-Location).Path
$repoWslPath = (wsl.exe -e wslpath -a $repoWindowsPath).Trim()
$cacheKey = "$FreebsdRelease-$FreebsdArch-$FreebsdImageBasename"

if ($WslCacheRoot.StartsWith("~/")) {
    $wslCacheRootForShell = "`$HOME/" + $WslCacheRoot.Substring(2)
} else {
    $wslCacheRootForShell = $WslCacheRoot
}

if (-not $SkipDependencyInstall) {
    Write-Host "[SETUP] Installing local WSL build dependencies..."
    Write-Host "[SETUP] WSL may prompt for your sudo password."
    $installCommand = @"
set -e
if command -v sudo >/dev/null 2>&1; then
  sudo apt-get update
  sudo apt-get install -y curl xz-utils xorriso make file python3
else
  apt-get update
  apt-get install -y curl xz-utils xorriso make file python3
fi
"@
    wsl.exe -e sh -lc $installCommand
    if ($LASTEXITCODE -ne 0) {
        throw "WSL dependency installation failed with exit code $LASTEXITCODE"
    }
}

$wslCommand = @"
set -euo pipefail
wsl_cache_path="$wslCacheRootForShell/$cacheKey"
cd '$repoWslPath'
PRODUCT_NAME='$ProductName' \
FREEBSD_RELEASE='$FreebsdRelease' \
FREEBSD_ARCH='$FreebsdArch' \
FREEBSD_IMAGE_BASENAME='$FreebsdImageBasename' \
BUILD_DIR='$BuildDir' \
OUTPUT_DIR='$OutputDir' \
FREEBSD_CACHE_DIR="`$wsl_cache_path" \
bash ./scripts/fetch-freebsd-release.sh
"@

Write-Host "[BUILD] Running FreeBSD image build in WSL..."
Write-Host "[BUILD] Reusing cached source media from $WslCacheRoot/$cacheKey when available."
wsl.exe -e sh -lc $wslCommand
if ($LASTEXITCODE -ne 0) {
    throw "WSL build failed with exit code $LASTEXITCODE"
}

Write-Host "[DONE] Local build completed."
