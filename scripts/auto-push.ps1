param(
    [Parameter(Mandatory = $true)]
    [string]$Message
)

$ErrorActionPreference = "Stop"

function Resolve-GitExe {
    $fromPath = Get-Command git -ErrorAction SilentlyContinue
    if ($fromPath) {
        return $fromPath.Source
    }

    $candidates = @(
        "C:\Program Files\Git\cmd\git.exe",
        "C:\Program Files\Git\bin\git.exe",
        "$env:LOCALAPPDATA\Programs\Git\cmd\git.exe"
    )

    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate)) {
            return $candidate
        }
    }

    $desktopRoots = @(
        "$env:LOCALAPPDATA\GitHubDesktop",
        "$env:LOCALAPPDATA\Programs\GitHub Desktop"
    )

    foreach ($root in $desktopRoots) {
        if (-not (Test-Path -LiteralPath $root)) {
            continue
        }
        $match = Get-ChildItem -Path $root -Recurse -Filter git.exe -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -like "*\resources\app\git\cmd\git.exe" } |
            Select-Object -First 1
        if ($match) {
            return $match.FullName
        }
    }

    throw "git.exe was not found. Install Git or GitHub Desktop, or add Git to PATH."
}

function Invoke-Git {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Args,
        [switch]$AllowFailure
    )

    $output = & $script:GitExe @Args 2>&1
    $exitCode = $LASTEXITCODE

    if (-not $AllowFailure -and $exitCode -ne 0) {
        if ($output) {
            $output | Write-Host
        }
        throw "git $($Args -join ' ') failed with exit code $exitCode."
    }

    return @{
        Output = $output
        ExitCode = $exitCode
    }
}

$script:GitExe = Resolve-GitExe

$repoCheck = Invoke-Git -Args @("rev-parse", "--show-toplevel")
$repoRoot = ($repoCheck.Output | Select-Object -First 1).Trim()
if (-not $repoRoot) {
    throw "Could not resolve repository root."
}

Set-Location -LiteralPath $repoRoot

$status = Invoke-Git -Args @("status", "--porcelain")
if (-not $status.Output) {
    Write-Host "No changes to commit."
    exit 0
}

$branch = Invoke-Git -Args @("rev-parse", "--abbrev-ref", "HEAD")
$branchName = ($branch.Output | Select-Object -First 1).Trim()
if (-not $branchName -or $branchName -eq "HEAD") {
    throw "Detached HEAD detected. Check out a branch before running auto-push."
}

$name = Invoke-Git -Args @("config", "--get", "user.name") -AllowFailure
$email = Invoke-Git -Args @("config", "--get", "user.email") -AllowFailure
if ($name.ExitCode -ne 0 -or $email.ExitCode -ne 0) {
    throw "Git user.name/user.email are not configured. Run 'git config user.name ...' and 'git config user.email ...' first."
}

Invoke-Git -Args @("add", "-A") | Out-Null

$commit = Invoke-Git -Args @("commit", "-m", $Message) -AllowFailure
if ($commit.ExitCode -ne 0) {
    $joined = ($commit.Output | Out-String)
    if ($joined -match "nothing to commit") {
        Write-Host "Nothing to commit after staging."
        exit 0
    }
    throw "Commit failed."
}

$push = Invoke-Git -Args @("push", "origin", "HEAD") -AllowFailure
if ($push.ExitCode -ne 0) {
    $joined = ($push.Output | Out-String)
    if ($joined -match "Authentication failed|could not read Username|Permission denied|403|403 Forbidden") {
        throw "Push failed because credentials or remote permissions are missing."
    }
    if ($joined -match "rejected|non-fast-forward|fetch first") {
        throw "Push was rejected because the remote branch moved. Pull/rebase and retry."
    }
    throw "Push failed."
}

Write-Host "Pushed successfully on branch $branchName."
