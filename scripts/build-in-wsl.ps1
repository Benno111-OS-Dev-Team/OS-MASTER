param(
    [string]$Arch = "x86_64",
    [string]$Target = "kernel",
    [string]$Distro,
    [switch]$SetupToolchain
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
        return $true
    }

    return $false
}

function Normalize-UnixScripts {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root
    )

    $files = Get-ChildItem -Path $Root -Recurse -File -Include *.sh
    $updated = 0

    foreach ($file in $files) {
        if (Convert-FileToLf -Path $file.FullName) {
            $updated++
        }
    }

    if ($updated -gt 0) {
        Write-Host "Normalized LF line endings for $updated shell script(s)." -ForegroundColor DarkYellow
    }
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
    param(
        [string]$ChosenDistro
    )

    if ($ChosenDistro) {
        return @("wsl.exe", "-d", $ChosenDistro, "--")
    }

    return @("wsl.exe", "--")
}

$distros = Get-InstalledWslDistros
if ($distros.Count -eq 0) {
    throw @"
WSL is enabled but no Linux distribution is installed yet.

Install one first, for example:
  wsl --install -d Ubuntu

Then rerun:
  powershell -ExecutionPolicy Bypass -File .\scripts\build-in-wsl.ps1 -Arch $Arch -Target $Target
"@
}

if ($Distro -and ($distros -notcontains $Distro)) {
    throw "Requested WSL distro '$Distro' is not installed. Installed distros: $($distros -join ', ')"
}

$wslRepoRoot = Convert-ToWslPath $repoRoot
$wslPrefix = Get-WslPrefix $Distro

Normalize-UnixScripts -Root $repoRoot

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

if ($SetupToolchain) {
    $setupScript = "cd '$wslRepoRoot' && chmod +x ./scripts/setup-toolchain-linux.sh && ./scripts/setup-toolchain-linux.sh"
    Write-Host ""
    Write-Host "Bootstrapping Linux toolchain inside WSL..." -ForegroundColor Cyan
    Write-Host ""
    Invoke-WslBash -Prefix $wslPrefix -Script $setupScript
}

$buildScript = "cd '$wslRepoRoot' && make -f Makefile.multiarch ARCH=$Arch $Target"

Write-Host ""
Write-Host "Building OS8 inside WSL..." -ForegroundColor Cyan
Write-Host "Repo:   $repoRoot" -ForegroundColor DarkGray
Write-Host "Arch:   $Arch" -ForegroundColor DarkGray
Write-Host "Target: $Target" -ForegroundColor DarkGray
if ($Distro) {
    Write-Host "Distro: $Distro" -ForegroundColor DarkGray
}
Write-Host ""

Invoke-WslBash -Prefix $wslPrefix -Script $buildScript
