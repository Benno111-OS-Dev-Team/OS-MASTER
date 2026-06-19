param()

$ErrorActionPreference = "Stop"

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$repoRoot = Split-Path -Parent $PSScriptRoot
$launcherScript = Join-Path $PSScriptRoot "run-desktop.ps1"
$wslBuildScript = Join-Path $PSScriptRoot "build-in-wsl.ps1"

function Test-CommandExists {
    param(
        [Parameter(Mandatory = $true)]
        [string]$CommandName
    )

    return $null -ne (Get-Command $CommandName -ErrorAction SilentlyContinue)
}

function Get-ArtifactState {
    $x64Iso = Join-Path $repoRoot "image\os8-x86_64.iso"
    $arm64Kernel = Join-Path $repoRoot "build\arm64\kernel\os-arm64.elf"
    $wslLines = @()

    if (Test-CommandExists "wsl.exe") {
        $wslLines = @(
            & wsl.exe -l -q 2>$null |
                ForEach-Object { ($_ -replace "`0", "").Trim() } |
                Where-Object { $_ }
        )
    }

    return @{
        X64IsoPath = $x64Iso
        X64IsoReady = Test-Path -LiteralPath $x64Iso
        Arm64KernelPath = $arm64Kernel
        Arm64KernelReady = Test-Path -LiteralPath $arm64Kernel
        NativeQemu = (Test-CommandExists "qemu-system-x86_64") -or
            (Test-CommandExists "qemu-system-aarch64")
        WslAvailable = Test-CommandExists "wsl.exe"
        WslBuildHelperReady = Test-Path -LiteralPath $wslBuildScript
        WslDistros = $wslLines
    }
}

function Show-Message {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Text,
        [System.Windows.Forms.MessageBoxIcon]$Icon = [System.Windows.Forms.MessageBoxIcon]::Information
    )

    [System.Windows.Forms.MessageBox]::Show(
        $Text,
        "OS8 Windows Launcher",
        [System.Windows.Forms.MessageBoxButtons]::OK,
        $Icon
    ) | Out-Null
}

function Start-LauncherMode {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Mode
    )

    Start-Process powershell.exe -WorkingDirectory $repoRoot -ArgumentList @(
        "-NoLogo",
        "-ExecutionPolicy", "Bypass",
        "-File", "`"$launcherScript`"",
        "-Mode", $Mode,
        "-NoPause"
    )
}

function Start-WslBuildCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Arch,
        [Parameter(Mandatory = $true)]
        [string]$Target,
        [switch]$SetupToolchain
    )

    if (-not (Test-Path -LiteralPath $wslBuildScript)) {
        Show-Message "Could not find scripts/build-in-wsl.ps1. Update the repo launcher scripts first." ([System.Windows.Forms.MessageBoxIcon]::Warning)
        return
    }

    if (-not (Test-CommandExists "wsl.exe")) {
        Show-Message "WSL is not available on this machine yet. Install WSL first, then try again." ([System.Windows.Forms.MessageBoxIcon]::Warning)
        return
    }

    $args = @(
        "-NoExit",
        "-ExecutionPolicy", "Bypass",
        "-File", "`"$wslBuildScript`"",
        "-Arch", $Arch,
        "-Target", $Target
    )

    if ($SetupToolchain) {
        $args += "-SetupToolchain"
    }

    Start-Process powershell.exe -WorkingDirectory $repoRoot -ArgumentList $args
}

function New-ActionButton {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Text,
        [Parameter(Mandatory = $true)]
        [int]$X,
        [Parameter(Mandatory = $true)]
        [int]$Y,
        [Parameter(Mandatory = $true)]
        [scriptblock]$OnClick
    )

    $button = New-Object System.Windows.Forms.Button
    $button.Text = $Text
    $button.Location = New-Object System.Drawing.Point($X, $Y)
    $button.Size = New-Object System.Drawing.Size(180, 38)
    $button.FlatStyle = [System.Windows.Forms.FlatStyle]::System
    $button.Add_Click($OnClick)
    return $button
}

$state = Get-ArtifactState

$form = New-Object System.Windows.Forms.Form
$form.Text = "OS8 Windows Launcher"
$form.StartPosition = [System.Windows.Forms.FormStartPosition]::CenterScreen
$form.ClientSize = New-Object System.Drawing.Size(720, 450)
$form.FormBorderStyle = [System.Windows.Forms.FormBorderStyle]::FixedDialog
$form.MaximizeBox = $false
$form.BackColor = [System.Drawing.Color]::FromArgb(245, 247, 250)
$form.Font = New-Object System.Drawing.Font("Segoe UI", 10)

$title = New-Object System.Windows.Forms.Label
$title.Text = "OS8 for Windows"
$title.Font = New-Object System.Drawing.Font("Segoe UI Semibold", 19)
$title.Location = New-Object System.Drawing.Point(24, 20)
$title.Size = New-Object System.Drawing.Size(300, 40)
$form.Controls.Add($title)

$subtitle = New-Object System.Windows.Forms.Label
$subtitle.Text = "Launch the current OS8 images from Windows with visible graphics and build missing artifacts without leaving the desktop."
$subtitle.Location = New-Object System.Drawing.Point(28, 62)
$subtitle.Size = New-Object System.Drawing.Size(650, 42)
$subtitle.ForeColor = [System.Drawing.Color]::FromArgb(70, 70, 70)
$form.Controls.Add($subtitle)

$status = New-Object System.Windows.Forms.Label
$status.Location = New-Object System.Drawing.Point(28, 112)
$status.Size = New-Object System.Drawing.Size(660, 100)
$status.BorderStyle = [System.Windows.Forms.BorderStyle]::FixedSingle
$status.BackColor = [System.Drawing.Color]::White
$status.Padding = New-Object System.Windows.Forms.Padding(10)
$status.Text = @(
    "Status",
    "x86_64 ISO: " + ($(if ($state.X64IsoReady) { "ready" } else { "missing" })) + "  -  " + $state.X64IsoPath,
    "ARM64 kernel: " + ($(if ($state.Arm64KernelReady) { "ready" } else { "missing" })) + "  -  " + $state.Arm64KernelPath,
    "Emulator support: " + ($(if ($state.NativeQemu) { "native QEMU found" } elseif ($state.WslAvailable) { "WSL fallback available" } else { "QEMU/WSL missing" })),
    "WSL build support: " + ($(if (-not $state.WslAvailable) { "WSL unavailable" } elseif ($state.WslDistros.Count -eq 0) { "no distro installed" } else { "ready via " + ($state.WslDistros -join ", ") }))
) -join [Environment]::NewLine
$form.Controls.Add($status)

$launchGroup = New-Object System.Windows.Forms.GroupBox
$launchGroup.Text = "Launch OS8"
$launchGroup.Location = New-Object System.Drawing.Point(24, 228)
$launchGroup.Size = New-Object System.Drawing.Size(390, 180)
$form.Controls.Add($launchGroup)

$buildGroup = New-Object System.Windows.Forms.GroupBox
$buildGroup.Text = "Build Through WSL"
$buildGroup.Location = New-Object System.Drawing.Point(430, 228)
$buildGroup.Size = New-Object System.Drawing.Size(260, 180)
$form.Controls.Add($buildGroup)

$launchGroup.Controls.Add((New-ActionButton "x86_64 UEFI" 18 34 { Start-LauncherMode "x64-uefi" }))
$launchGroup.Controls.Add((New-ActionButton "x86_64 BIOS" 198 34 { Start-LauncherMode "x64-bios" }))
$launchGroup.Controls.Add((New-ActionButton "ARM64 GUI" 18 86 { Start-LauncherMode "arm64-gui" }))
$launchGroup.Controls.Add((New-ActionButton "ARM64 Text" 198 86 { Start-LauncherMode "arm64-text" }))

$buildGroup.Controls.Add((New-ActionButton "Build x86_64 ISO" 36 22 { Start-WslBuildCommand -Arch "x86_64" -Target "image" }))
$buildGroup.Controls.Add((New-ActionButton "Build ARM64 Kernel" 36 68 { Start-WslBuildCommand -Arch "arm64" -Target "kernel" }))
$buildGroup.Controls.Add((New-ActionButton "Setup WSL Toolchain" 36 114 { Start-WslBuildCommand -Arch "x86_64" -Target "kernel" -SetupToolchain }))

$tips = New-Object System.Windows.Forms.Label
$tips.Location = New-Object System.Drawing.Point(28, 415)
$tips.Size = New-Object System.Drawing.Size(660, 22)
$tips.ForeColor = [System.Drawing.Color]::FromArgb(80, 80, 80)
$tips.Text = "Tip: launch actions open graphics in QEMU, and build actions open a WSL-backed PowerShell session with the compile log."
$form.Controls.Add($tips)

[void]$form.ShowDialog()
