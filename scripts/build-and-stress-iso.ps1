param(
    [ValidateSet("x86_64")]
    [string]$Arch = "x86_64",

    [int]$Iterations = 3,

    [int]$BootSeconds = 25,

    [int]$MemoryMb = 4096,

    [int]$Jobs = 1,

    [switch]$IncludeInstaller,

    [switch]$SkipBios,

    [switch]$SkipUefi
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

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

function Require-Command {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if (-not $cmd) {
        throw "Required command not found: $Name"
    }

    return $cmd.Source
}

function Write-Section {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    Write-Host ""
    Write-Host $Message -ForegroundColor Cyan
}

function New-Utf8NoBomFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [string]$Content
    )

    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $Content.Replace("`r`n", "`n"), $utf8NoBom)
}

$null = Require-Command "wsl.exe"

$timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
$diagRoot = Join-Path $repoRoot "diagnostics"
$diagDir = Join-Path $diagRoot "iso-stress-$timestamp"
New-Item -ItemType Directory -Force -Path $diagDir | Out-Null

$wslRepoRoot = Convert-ToWslPath $repoRoot
$wslDiagDir = Convert-ToWslPath $diagDir
$includeInstallerFlag = if ($IncludeInstaller) { "1" } else { "0" }
$runBiosFlag = if ($SkipBios) { "0" } else { "1" }
$runUefiFlag = if ($SkipUefi) { "0" } else { "1" }

$helperScriptWindows = Join-Path $diagDir "run-build-and-stress.sh"
$helperScriptWsl = Convert-ToWslPath $helperScriptWindows

$helperScript = @'
#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR='__ROOT_DIR__'
DIAG_DIR='__DIAG_DIR__'
ARCH='__ARCH__'
ITERATIONS='__ITERATIONS__'
BOOT_SECONDS='__BOOT_SECONDS__'
MEMORY_MB='__MEMORY_MB__'
MAKE_JOBS='__MAKE_JOBS__'
INCLUDE_INSTALLER='__INCLUDE_INSTALLER__'
RUN_BIOS='__RUN_BIOS__'
RUN_UEFI='__RUN_UEFI__'

LF_SCRIPT_DIR="\$ROOT_DIR/scripts/.lf-run"
BUILD_DIR="\$ROOT_DIR/build/\$ARCH"
IMAGE_DIR="\$ROOT_DIR/image"
BUILD_LOG="\$DIAG_DIR/build.log"
ISO_LOG="\$DIAG_DIR/iso-build.log"
ENV_LOG="\$DIAG_DIR/environment.txt"
SUMMARY_FILE="\$DIAG_DIR/summary.txt"
MANIFEST_FILE="\$DIAG_DIR/manifest.txt"
ISO_CONTENTS_FILE="\$DIAG_DIR/iso-contents.txt"
KEYWORD_FILE="\$DIAG_DIR/keyword-hits.txt"
TOOL_VERSIONS_FILE="\$DIAG_DIR/tool-versions.txt"

mkdir -p "\$DIAG_DIR"
mkdir -p "\$LF_SCRIPT_DIR"

log() {
    printf '[ISO-STRESS] %s\n' "\$1"
}

resolve_ovmf() {
    if [ -f /usr/share/OVMF/OVMF_CODE.fd ]; then
        printf '%s\n' /usr/share/OVMF/OVMF_CODE.fd
        return 0
    fi
    if [ -f /usr/share/OVMF/OVMF_CODE_4M.fd ]; then
        printf '%s\n' /usr/share/OVMF/OVMF_CODE_4M.fd
        return 0
    fi
    return 1
}

resolve_ovmf_vars() {
    if [ -f /usr/share/OVMF/OVMF_VARS.fd ]; then
        printf '%s\n' /usr/share/OVMF/OVMF_VARS.fd
        return 0
    fi
    if [ -f /usr/share/OVMF/OVMF_VARS_4M.fd ]; then
        printf '%s\n' /usr/share/OVMF/OVMF_VARS_4M.fd
        return 0
    fi
    return 1
}

normalize_script() {
    local src="\$1"
    local dst="\$2"
    tr -d '\r' < "\$src" > "\$dst"
    chmod +x "\$dst"
}

patch_script_refs() {
    local path="\$1"
    sed -i "s#^ROOT_DIR=.*#ROOT_DIR=\"\$ROOT_DIR\"#" "\$path"
    sed -i 's#${ROOT_DIR}/scripts/update-os-boot-manager.sh#${ROOT_DIR}/scripts/.lf-run/update-os-boot-manager.sh#g' "\$path"
    sed -i 's#${ROOT_DIR}/scripts/export-kernel-raw-parts.sh#${ROOT_DIR}/scripts/.lf-run/export-kernel-raw-parts.sh#g' "\$path"
    sed -i 's#${ROOT_DIR}/scripts/build-install-boot-files.sh#${ROOT_DIR}/scripts/.lf-run/build-install-boot-files.sh#g' "\$path"
    sed -i 's#${ROOT_DIR}/scripts/create-system-image.sh#${ROOT_DIR}/scripts/.lf-run/create-system-image.sh#g' "\$path"
}

write_environment_report() {
    {
        echo "timestamp_utc=\$(date -u +'%Y-%m-%dT%H:%M:%SZ')"
        echo "pwd=\$(pwd)"
        echo "root_dir=\$ROOT_DIR"
        echo "diag_dir=\$DIAG_DIR"
        echo "arch=\$ARCH"
        echo "iterations=\$ITERATIONS"
        echo "boot_seconds=\$BOOT_SECONDS"
        echo "memory_mb=\$MEMORY_MB"
        echo "make_jobs=\$MAKE_JOBS"
        echo "include_installer=\$INCLUDE_INSTALLER"
        echo "run_bios=\$RUN_BIOS"
        echo "run_uefi=\$RUN_UEFI"
        echo "uname=\$(uname -a)"
        echo "git_head=\$(git -C "\$ROOT_DIR" rev-parse HEAD)"
        echo "git_branch=\$(git -C "\$ROOT_DIR" branch --show-current || true)"
    } > "\$ENV_LOG"
}

write_tool_versions() {
    {
        clang --version | head -1
        ld.lld --version | head -1
        llvm-ar --version | head -1
        llvm-objcopy --version | head -1
        llvm-objdump --version | head -1
        nasm -v | head -1
        qemu-system-x86_64 --version | head -1
        qemu-system-aarch64 --version | head -1
        xorriso -version | head -1
        python3 --version
        git --version
        if command -v 7z >/dev/null 2>&1; then
            7z | head -2
        fi
    } > "\$TOOL_VERSIONS_FILE" 2>&1
}

prepare_lf_scripts() {
    normalize_script "\$ROOT_DIR/scripts/update-os-boot-manager.sh" "\$LF_SCRIPT_DIR/update-os-boot-manager.sh"
    normalize_script "\$ROOT_DIR/scripts/export-kernel-raw-parts.sh" "\$LF_SCRIPT_DIR/export-kernel-raw-parts.sh"
    normalize_script "\$ROOT_DIR/scripts/build-install-boot-files.sh" "\$LF_SCRIPT_DIR/build-install-boot-files.sh"
    normalize_script "\$ROOT_DIR/scripts/create-system-image.sh" "\$LF_SCRIPT_DIR/create-system-image.sh"
    normalize_script "\$ROOT_DIR/scripts/create-x86_64-iso.sh" "\$LF_SCRIPT_DIR/create-x86_64-iso.sh"

    patch_script_refs "\$LF_SCRIPT_DIR/build-install-boot-files.sh"
    patch_script_refs "\$LF_SCRIPT_DIR/create-system-image.sh"
    patch_script_refs "\$LF_SCRIPT_DIR/create-x86_64-iso.sh"
}

build_kernel() {
    log "Building \$ARCH kernel"
    make -C "\$ROOT_DIR" -f Makefile.multiarch ARCH="\$ARCH" kernel -j"\$MAKE_JOBS" > "\$BUILD_LOG" 2>&1
}

build_iso() {
    local iso_name
    iso_name='os8-x86_64.iso'
    if [ "\$INCLUDE_INSTALLER" = '1' ]; then
        iso_name='os8-x86_64-installer.iso'
    fi

    log "Building \$iso_name"
    INCLUDE_INSTALLER="\$INCLUDE_INSTALLER" ISO_NAME="\$iso_name" \
        bash "\$LF_SCRIPT_DIR/create-x86_64-iso.sh" "\$BUILD_DIR" "\$IMAGE_DIR" > "\$ISO_LOG" 2>&1

    ISO_PATH="\$IMAGE_DIR/\$iso_name"
    if [ ! -f "\$ISO_PATH" ]; then
        echo "[ERROR] Expected ISO not found: \$ISO_PATH" >&2
        exit 1
    fi
}

collect_iso_diagnostics() {
    sha256sum "\$ISO_PATH" > "\$DIAG_DIR/iso.sha256"
    ls -lh "\$ISO_PATH" > "\$DIAG_DIR/iso-size.txt"
    file "\$ISO_PATH" > "\$DIAG_DIR/iso-file.txt"
    xorriso -indev "\$ISO_PATH" -find / -type f -exec lsdl > "\$ISO_CONTENTS_FILE" 2>&1
}

run_qemu_case() {
    local mode="\$1"
    local ovmf_path="\${2:-}"
    local ovmf_vars_path="\${3:-}"
    local args=()
    local qemu_log
    local serial_log
    local exit_file
    local cmd_file
    local vars_copy
    local rc

    args=(-M q35 -cpu qemu64 -m "\$MEMORY_MB" -nographic -serial mon:stdio -no-reboot -no-shutdown -d guest_errors -cdrom "\$ISO_PATH")

    for iter in \$(seq 1 "\$ITERATIONS"); do
        qemu_log="\$DIAG_DIR/qemu-\$mode-\$iter-debug.log"
        serial_log="\$DIAG_DIR/qemu-\$mode-\$iter-serial.log"
        exit_file="\$DIAG_DIR/qemu-\$mode-\$iter-exit.txt"
        cmd_file="\$DIAG_DIR/qemu-\$mode-\$iter-command.txt"
        vars_copy="\$DIAG_DIR/qemu-\$mode-vars-\$iter.fd"

        if [ "\$mode" = 'uefi' ]; then
            cp "\$ovmf_vars_path" "\$vars_copy"
            args=(-M q35 -cpu qemu64 -m "\$MEMORY_MB" -nographic -serial mon:stdio -no-reboot -no-shutdown -d guest_errors -drive "if=pflash,format=raw,readonly=on,file=\$ovmf_path" -drive "if=pflash,format=raw,file=\$vars_copy" -cdrom "\$ISO_PATH")
        fi

        {
            printf 'timeout --signal=TERM %ss qemu-system-x86_64 ' "\$BOOT_SECONDS"
            printf '%q ' "\${args[@]}"
            printf '%q\n' -D "\$qemu_log"
        } > "\$cmd_file"

        set +e
        timeout --signal=TERM "\${BOOT_SECONDS}s" qemu-system-x86_64 "\${args[@]}" -D "\$qemu_log" > "\$serial_log" 2>&1
        rc=\$?
        set -e

        {
            echo "mode=\$mode"
            echo "iteration=\$iter"
            echo "exit_code=\$rc"
            if [ "\$rc" -eq 124 ] || [ "\$rc" -eq 143 ]; then
                echo "status=timeout"
            elif [ "\$rc" -eq 0 ]; then
                echo "status=clean-exit"
            else
                echo "status=nonzero-exit"
            fi
        } > "\$exit_file"
    done
}

collect_keyword_hits() {
    if ! grep -R -n -i -E 'panic|fault|assert|exception|error|fail|oops' "\$DIAG_DIR"/qemu-*-serial.log "\$DIAG_DIR"/qemu-*-debug.log "\$BUILD_LOG" "\$ISO_LOG" > "\$KEYWORD_FILE" 2>/dev/null; then
        : > "\$KEYWORD_FILE"
    fi
}

write_summary() {
    {
        echo "OS8 ISO Build + Stress Summary"
        echo ""
        echo "ISO: \$ISO_PATH"
        echo "SHA256: \$(cut -d' ' -f1 "\$DIAG_DIR/iso.sha256")"
        echo "Build log: \$BUILD_LOG"
        echo "ISO build log: \$ISO_LOG"
        echo "Environment: \$ENV_LOG"
        echo "Tool versions: \$TOOL_VERSIONS_FILE"
        echo "ISO contents: \$ISO_CONTENTS_FILE"
        echo "Keyword hits: \$KEYWORD_FILE"
        echo ""
        echo "QEMU runs:"
        for file in "\$DIAG_DIR"/qemu-*-exit.txt; do
            [ -f "\$file" ] || continue
            mode=\$(grep '^mode=' "\$file" | cut -d= -f2)
            iteration=\$(grep '^iteration=' "\$file" | cut -d= -f2)
            exit_code=\$(grep '^exit_code=' "\$file" | cut -d= -f2)
            status=\$(grep '^status=' "\$file" | cut -d= -f2)
            echo "- \$mode iteration \$iteration: \$status (exit \$exit_code)"
        done
        echo ""
        echo "Tail of serial logs:"
        for file in "\$DIAG_DIR"/qemu-*-serial.log; do
            [ -f "\$file" ] || continue
            echo ""
            echo "==> \$(basename "\$file") <=="
            tail -n 20 "\$file"
        done
    } > "\$SUMMARY_FILE"
}

write_manifest() {
    find "\$DIAG_DIR" -maxdepth 1 -type f | sort > "\$MANIFEST_FILE"
}

main() {
    cd "\$ROOT_DIR"
    write_environment_report
    write_tool_versions
    prepare_lf_scripts
    build_kernel
    build_iso
    collect_iso_diagnostics

    if [ "\$RUN_BIOS" = '1' ]; then
        log "Running BIOS stress loop"
        run_qemu_case bios
    fi

    if [ "\$RUN_UEFI" = '1' ]; then
        log "Running UEFI stress loop"
        OVMF_PATH="\$(resolve_ovmf)"
        OVMF_VARS_PATH="\$(resolve_ovmf_vars)"
        run_qemu_case uefi "\$OVMF_PATH" "\$OVMF_VARS_PATH"
    fi

    collect_keyword_hits
    write_summary
    write_manifest
    cat "\$SUMMARY_FILE"
}

main "\$@"
'@

$helperScript = $helperScript.Replace('__ROOT_DIR__', $wslRepoRoot)
$helperScript = $helperScript.Replace('__DIAG_DIR__', $wslDiagDir)
$helperScript = $helperScript.Replace('__ARCH__', $Arch)
$helperScript = $helperScript.Replace('__ITERATIONS__', [string]$Iterations)
$helperScript = $helperScript.Replace('__BOOT_SECONDS__', [string]$BootSeconds)
$helperScript = $helperScript.Replace('__MEMORY_MB__', [string]$MemoryMb)
$helperScript = $helperScript.Replace('__MAKE_JOBS__', [string]$Jobs)
$helperScript = $helperScript.Replace('__INCLUDE_INSTALLER__', $includeInstallerFlag)
$helperScript = $helperScript.Replace('__RUN_BIOS__', $runBiosFlag)
$helperScript = $helperScript.Replace('__RUN_UEFI__', $runUefiFlag)
$helperScript = $helperScript.Replace('\$', '$')

New-Utf8NoBomFile -Path $helperScriptWindows -Content $helperScript

Write-Section "Running ISO build and stress test"
& wsl.exe bash -lc "chmod +x '$helperScriptWsl' && '$helperScriptWsl'"
if ($LASTEXITCODE -ne 0) {
    throw "WSL build/stress run failed with exit code $LASTEXITCODE. Diagnostics: $diagDir"
}

Write-Section "Diagnostics bundle"
Write-Host $diagDir -ForegroundColor Green

$summaryPath = Join-Path $diagDir "summary.txt"
if (Test-Path -LiteralPath $summaryPath) {
    Write-Section "Summary"
    Get-Content $summaryPath
}
