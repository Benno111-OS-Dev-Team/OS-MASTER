param()

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

$x64Iso = Join-Path $repoRoot "image\os8-x86_64.iso"
$arm64Kernel = Join-Path $repoRoot "build\kernel\unixos.elf"

function Write-Header {
    Write-Host ""
    Write-Host "OS8 launcher" -ForegroundColor Cyan
    Write-Host "Repository: $repoRoot" -ForegroundColor DarkGray
    Write-Host ""
    Write-Host "1. x86_64 UEFI (Recommended)"
    Write-Host "2. x86_64 BIOS"
    Write-Host "3. ARM64 GUI"
    Write-Host "4. ARM64 Text"
    Write-Host "Q. Quit"
    Write-Host ""
}

function Convert-ToWslPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$WindowsPath
    )

    $normalized = $WindowsPath -replace "\\", "/"
    if ($normalized -match '^([A-Za-z]):/(.*)$') {
        $drive = $matches[1].ToLowerInvariant()
        $rest = $matches[2]
        return "/mnt/$drive/$rest"
    }

    throw "Could not convert Windows path to WSL path: $WindowsPath"
}

function Resolve-LauncherMode {
    $nativeQemuX64 = Get-Command qemu-system-x86_64 -ErrorAction SilentlyContinue
    $nativeQemuArm64 = Get-Command qemu-system-aarch64 -ErrorAction SilentlyContinue

    if ($nativeQemuX64 -or $nativeQemuArm64) {
        return @{
            Type = "native"
        }
    }

    $wsl = Get-Command wsl.exe -ErrorAction SilentlyContinue
    if ($wsl) {
        return @{
            Type = "wsl"
            Command = $wsl.Source
        }
    }

    throw "Could not find QEMU in PATH, and WSL is not available. Install QEMU or WSL, then try again."
}

function Require-File {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        throw $Message
    }
}

function Resolve-NativeUefiFirmware {
    $candidates = @(
        $env:QEMU_UEFI_FIRMWARE,
        "C:\Program Files\qemu\share\edk2-x86_64-code.fd",
        "C:\Program Files\qemu\share\OVMF_CODE.fd",
        "C:\Program Files\qemu\share\OVMF_CODE_4M.fd"
    ) | Where-Object { $_ }

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) {
            return $candidate
        }
    }

    throw "Could not find OVMF firmware for native QEMU. Set QEMU_UEFI_FIRMWARE or install OVMF with QEMU."
}

function Resolve-WslUefiFirmware {
    param(
        [Parameter(Mandatory = $true)]
        [string]$WslCommand
    )

    $result = & $WslCommand bash -lc "if [ -f /usr/share/OVMF/OVMF_CODE.fd ]; then printf '%s' /usr/share/OVMF/OVMF_CODE.fd; elif [ -f /usr/share/OVMF/OVMF_CODE_4M.fd ]; then printf '%s' /usr/share/OVMF/OVMF_CODE_4M.fd; fi"
    if (-not $result) {
        throw "Could not find OVMF firmware in WSL. Install the 'ovmf' package in WSL."
    }

    return $result.Trim()
}

function Invoke-NativeCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Command,
        [Parameter(Mandatory = $true)]
        [string[]]$Arguments
    )

    Write-Host ""
    Write-Host "Running: $Command $($Arguments -join ' ')" -ForegroundColor Green
    Write-Host ""

    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Command failed with exit code $LASTEXITCODE."
    }
}

function Invoke-WslCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string]$WslCommand,
        [Parameter(Mandatory = $true)]
        [string]$Script
    )

    Write-Host ""
    Write-Host "Running in WSL: $Script" -ForegroundColor Green
    Write-Host ""

    & $WslCommand bash -lc $Script
    if ($LASTEXITCODE -ne 0) {
        throw "WSL command failed with exit code $LASTEXITCODE."
    }
}

function Start-X64Uefi {
    param(
        [Parameter(Mandatory = $true)]
        [hashtable]$Launcher
    )

    Require-File $x64Iso "Missing x86_64 ISO: $x64Iso. Build the ISO first."

    if ($Launcher.Type -eq "native") {
        $fw = Resolve-NativeUefiFirmware
        Invoke-NativeCommand "qemu-system-x86_64" @(
            "-M", "q35",
            "-cpu", "qemu64",
            "-m", "4G",
            "-nographic",
            "-serial", "mon:stdio",
            "-bios", $fw,
            "-cdrom", $x64Iso
        )
        return
    }

    $wslIso = Convert-ToWslPath $x64Iso
    $fw = Resolve-WslUefiFirmware $Launcher.Command
    Invoke-WslCommand $Launcher.Command "qemu-system-x86_64 -M q35 -cpu qemu64 -m 4G -nographic -serial mon:stdio -bios '$fw' -cdrom '$wslIso'"
}

function Start-X64Bios {
    param(
        [Parameter(Mandatory = $true)]
        [hashtable]$Launcher
    )

    Require-File $x64Iso "Missing x86_64 ISO: $x64Iso. Build the ISO first."

    if ($Launcher.Type -eq "native") {
        Invoke-NativeCommand "qemu-system-x86_64" @(
            "-M", "q35",
            "-cpu", "qemu64",
            "-m", "4G",
            "-nographic",
            "-serial", "mon:stdio",
            "-cdrom", $x64Iso
        )
        return
    }

    $wslIso = Convert-ToWslPath $x64Iso
    Invoke-WslCommand $Launcher.Command "qemu-system-x86_64 -M q35 -cpu qemu64 -m 4G -nographic -serial mon:stdio -cdrom '$wslIso'"
}

function Start-Arm64Gui {
    param(
        [Parameter(Mandatory = $true)]
        [hashtable]$Launcher
    )

    Require-File $arm64Kernel "Missing ARM64 kernel: $arm64Kernel. Build the ARM64 kernel first."

    if ($Launcher.Type -eq "native") {
        Invoke-NativeCommand "qemu-system-aarch64" @(
            "-M", "virt,gic-version=3",
            "-cpu", "max",
            "-m", "512M",
            "-global", "virtio-mmio.force-legacy=false",
            "-device", "ramfb",
            "-device", "virtio-keyboard-device",
            "-device", "virtio-tablet-device",
            "-device", "virtio-net-device,netdev=net0",
            "-netdev", "user,id=net0",
            "-serial", "stdio",
            "-kernel", $arm64Kernel
        )
        return
    }

    $wslKernel = Convert-ToWslPath $arm64Kernel
    Invoke-WslCommand $Launcher.Command "qemu-system-aarch64 -M virt,gic-version=3 -cpu max -m 512M -global virtio-mmio.force-legacy=false -device ramfb -device virtio-keyboard-device -device virtio-tablet-device -device virtio-net-device,netdev=net0 -netdev user,id=net0 -serial stdio -kernel '$wslKernel'"
}

function Start-Arm64Text {
    param(
        [Parameter(Mandatory = $true)]
        [hashtable]$Launcher
    )

    Require-File $arm64Kernel "Missing ARM64 kernel: $arm64Kernel. Build the ARM64 kernel first."

    if ($Launcher.Type -eq "native") {
        Invoke-NativeCommand "qemu-system-aarch64" @(
            "-M", "virt,gic-version=3",
            "-cpu", "max",
            "-m", "4G",
            "-nographic",
            "-kernel", $arm64Kernel
        )
        return
    }

    $wslKernel = Convert-ToWslPath $arm64Kernel
    Invoke-WslCommand $Launcher.Command "qemu-system-aarch64 -M virt,gic-version=3 -cpu max -m 4G -nographic -kernel '$wslKernel'"
}

$launcher = Resolve-LauncherMode

Write-Header
$choice = Read-Host "Choose a launch mode"

switch ($choice.ToUpperInvariant()) {
    "1" { Start-X64Uefi $launcher }
    "2" { Start-X64Bios $launcher }
    "3" { Start-Arm64Gui $launcher }
    "4" { Start-Arm64Text $launcher }
    "Q" { exit 0 }
    default { throw "Unknown option: $choice" }
}

pause


