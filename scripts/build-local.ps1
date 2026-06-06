param(
    [string]$ProductName = "os-master-freebsd",
    [string]$FreebsdRelease = "14.4-RELEASE",
    [string]$FreebsdArch = "amd64",
    [string]$FreebsdImageBasename = "dvd1.iso",
    [string]$BuildDir = "build/freebsd",
    [string]$OutputDir = "image",
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
cd '$repoWslPath'
PRODUCT_NAME='$ProductName' \
FREEBSD_RELEASE='$FreebsdRelease' \
FREEBSD_ARCH='$FreebsdArch' \
FREEBSD_IMAGE_BASENAME='$FreebsdImageBasename' \
BUILD_DIR='$BuildDir' \
OUTPUT_DIR='$OutputDir' \
bash ./scripts/fetch-freebsd-release.sh
"@

Write-Host "[BUILD] Running FreeBSD image build in WSL..."
wsl.exe -e sh -lc $wslCommand
if ($LASTEXITCODE -ne 0) {
    throw "WSL build failed with exit code $LASTEXITCODE"
}

Write-Host "[DONE] Local build completed."
