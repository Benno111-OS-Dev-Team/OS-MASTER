param(
    [ValidateSet("x86_64")]
    [string]$Arch = "x86_64",

    [int]$BootSeconds = 25,

    [int]$MemoryMb = 4096,

    [int]$Jobs = 1,

    [int]$PauseSeconds = 5,

    [int]$MaxCycles = 0,

    [switch]$IncludeInstaller,

    [switch]$SkipBios,

    [switch]$SkipUefi,

    [switch]$StopOnFailure
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

$diagRoot = Join-Path $repoRoot "diagnostics"
$loopRoot = Join-Path $diagRoot "stress-loop"
$runLog = Join-Path $loopRoot "loop.log"
$stateFile = Join-Path $loopRoot "latest.txt"

New-Item -ItemType Directory -Force -Path $loopRoot | Out-Null

function Write-LoopLog {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    $line = "[{0}] {1}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss"), $Message
    Add-Content -LiteralPath $runLog -Value $line
    Write-Host $line
}

$iteration = 0

while ($true) {
    $iteration++
    Write-LoopLog "Starting stress cycle $iteration"

    $args = @(
        "-ExecutionPolicy", "Bypass",
        "-File", (Join-Path $repoRoot "scripts\build-and-stress-iso.ps1"),
        "-Arch", $Arch,
        "-Iterations", "1",
        "-BootSeconds", [string]$BootSeconds,
        "-MemoryMb", [string]$MemoryMb,
        "-Jobs", [string]$Jobs
    )

    if ($IncludeInstaller) { $args += "-IncludeInstaller" }
    if ($SkipBios) { $args += "-SkipBios" }
    if ($SkipUefi) { $args += "-SkipUefi" }

    & powershell @args
    $exitCode = $LASTEXITCODE

    $latestDiag = Get-ChildItem -LiteralPath $diagRoot -Directory -Filter "iso-stress-*" -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1

    if ($latestDiag) {
        Set-Content -LiteralPath $stateFile -Value $latestDiag.FullName
        Write-LoopLog "Latest diagnostics: $($latestDiag.FullName)"
    }

    if ($exitCode -eq 0) {
        Write-LoopLog "Stress cycle $iteration completed successfully"
    } else {
        Write-LoopLog "Stress cycle $iteration failed with exit code $exitCode"
        if ($StopOnFailure) {
            throw "Stopping on failure from cycle $iteration"
        }
    }

    if ($MaxCycles -gt 0 -and $iteration -ge $MaxCycles) {
        Write-LoopLog "Reached MaxCycles=$MaxCycles, stopping loop"
        break
    }

    Write-LoopLog "Sleeping $PauseSeconds seconds before next cycle"
    Start-Sleep -Seconds $PauseSeconds
}
