param(
    [string]$FirmwareDir,
    [string]$Device,
    [int]$ImageSizeMb = 512,
    [string]$Distro
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

function Convert-FileToLf {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $content = [System.IO.File]::ReadAllText($Path)
    $normalized = $content -replace "`r`n", "`n"
    $normalized = $normalized -replace "`r", "`n"
    if ($normalized -ne $content) {
        [System.IO.File]::WriteAllText($Path, $normalized, (New-Object System.Text.UTF8Encoding($false)))
    }
}

function Normalize-UnixScripts {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root
    )

    Get-ChildItem -Path $Root -Recurse -File -Include *.sh | ForEach-Object {
        Convert-FileToLf -Path $_.FullName
    }
}

function Convert-ToWslPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$PathValue
    )

    if ($PathValue -match '^/mnt/[a-z]/') {
        return $PathValue
    }

    $fullPath = [System.IO.Path]::GetFullPath($PathValue)
    $normalized = $fullPath -replace "\\", "/"
    if ($normalized -match '^([A-Za-z]):/(.*)$') {
        return "/mnt/$($matches[1].ToLowerInvariant())/$($matches[2])"
    }

    throw "Could not convert path to WSL format: $PathValue"
}

function Get-InstalledWslDistros {
    $lines = & wsl.exe -l -q 2>$null
    if ($LASTEXITCODE -ne 0) {
        return @()
    }

    return @(
        $lines |
            ForEach-Object { ($_ -replace "`0", "").Trim() } |
            Where-Object { $_ }
    )
}

function Get-WslPrefix {
    param([string]$ChosenDistro)

    if ($ChosenDistro) {
        return @("wsl.exe", "-d", $ChosenDistro, "--")
    }

    return @("wsl.exe", "--")
}

function Invoke-WslBash {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Prefix,
        [Parameter(Mandatory = $true)]
        [string]$Script
    )

    $command = @($Prefix + @("bash", "-lc", $Script))
    & $command[0] $command[1..($command.Length - 1)]
    if ($LASTEXITCODE -ne 0) {
        throw "WSL command failed with exit code $LASTEXITCODE."
    }
}

function Quote-ForBash {
    param([string]$Value)

    $singleQuote = "'"
    $escaped = $Value.Replace($singleQuote, $singleQuote + '"' + $singleQuote + '"' + $singleQuote)
    return $singleQuote + $escaped + $singleQuote
}

$distros = Get-InstalledWslDistros
if ($distros.Count -eq 0) {
    throw @"
WSL is enabled but no Linux distribution is installed yet.

Install one first, for example:
  wsl --install -d Ubuntu
"@
}

if ($Distro -and ($distros -notcontains $Distro)) {
    throw "Requested WSL distro '$Distro' is not installed. Installed distros: $($distros -join ', ')"
}

if ($ImageSizeMb -lt 128) {
    throw "ImageSizeMb must be at least 128."
}

Normalize-UnixScripts -Root $repoRoot

$wslRepoRoot = Convert-ToWslPath -PathValue $repoRoot
$wslPrefix = Get-WslPrefix -ChosenDistro $Distro

Write-Host ""
Write-Host "Building ARM64 kernel inside WSL..." -ForegroundColor Cyan
Invoke-WslBash -Prefix $wslPrefix -Script "cd $(Quote-ForBash $wslRepoRoot) && make -f Makefile.multiarch ARCH=arm64 kernel"

$scriptParts = @(
    "cd $(Quote-ForBash $wslRepoRoot)",
    "bash ./scripts/create-rpi-usb-image.sh --image-size-mb $ImageSizeMb"
)

if ($FirmwareDir) {
    if (-not (Test-Path -LiteralPath $FirmwareDir)) {
        throw "Firmware directory not found: $FirmwareDir"
    }

    $wslFirmwareDir = Convert-ToWslPath -PathValue $FirmwareDir
    $scriptParts[-1] += " --firmware-dir $(Quote-ForBash $wslFirmwareDir)"
}

if ($Device) {
    $scriptParts[-1] += " --device $(Quote-ForBash $Device)"
}

Write-Host ""
Write-Host "Creating Raspberry Pi USB image..." -ForegroundColor Cyan
if ($FirmwareDir) {
    Write-Host "Firmware source: $FirmwareDir" -ForegroundColor DarkGray
} else {
    Write-Host "Firmware source: auto-download latest pftf/RPi4 release" -ForegroundColor DarkGray
}
if ($Device) {
    Write-Host "Target device: $Device" -ForegroundColor DarkGray
}
Write-Host ""

Invoke-WslBash -Prefix $wslPrefix -Script ($scriptParts -join " && ")

Write-Host ""
Write-Host "Raspberry Pi boot media flow finished." -ForegroundColor Green
