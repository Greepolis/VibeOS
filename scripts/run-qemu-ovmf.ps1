param(
    [string]$BuildDir = "build",
    [string]$EfiRoot = "",
    [string]$ExpectToken = "BOOT_OK",
    [int]$Timeout = 25,
    [string]$SerialLog = "artifacts/qemu-ovmf-serial.log",
    [string]$ErrorLog = "artifacts/qemu-ovmf-stderr.log",
    [string]$SummaryLog = "artifacts/qemu-ovmf-summary.json",
    [string]$OvmfCode = "",
    [string]$OvmfVars = "",
    [switch]$Strict = $false
)

$ErrorActionPreference = "Stop"

function Find-OvmfPair {
    param(
        [string]$CodeOverride,
        [string]$VarsOverride
    )
    if ($CodeOverride -and $VarsOverride -and (Test-Path $CodeOverride) -and (Test-Path $VarsOverride)) {
        return @{ code = $CodeOverride; vars = $VarsOverride }
    }

    $codeCandidates = @(
        "C:\Program Files\qemu\share\edk2-x86_64-code.fd",
        "C:\Program Files\qemu\share\OVMF_CODE.fd",
        "C:\Program Files\qemu\share\OVMF_CODE_4M.fd"
    )
    $varsCandidates = @(
        "C:\Program Files\qemu\share\edk2-i386-vars.fd",
        "C:\Program Files\qemu\share\OVMF_VARS.fd",
        "C:\Program Files\qemu\share\OVMF_VARS_4M.fd"
    )

    $codePath = $null
    $varsPath = $null
    foreach ($p in $codeCandidates) {
        if (Test-Path $p) {
            $codePath = $p
            break
        }
    }
    foreach ($p in $varsCandidates) {
        if (Test-Path $p) {
            $varsPath = $p
            break
        }
    }
    if ($codePath -and $varsPath) {
        return @{ code = $codePath; vars = $varsPath }
    }
    return $null
}

function Exit-WithSummary {
    param(
        [string]$Status,
        [string]$Reason,
        [int]$Code
    )
    $summary = @{
        status = $Status
        reason = $Reason
        expect_token = $ExpectToken
        build_dir = $BuildDir
        efi_root = $resolvedEfiRoot
        serial_log = $SerialLog
        error_log = $ErrorLog
    }
    $summary | ConvertTo-Json -Depth 5 | Set-Content -Path $SummaryLog -Encoding UTF8
    exit $Code
}

New-Item -ItemType Directory -Force (Split-Path -Path $SerialLog -Parent) | Out-Null
New-Item -ItemType Directory -Force (Split-Path -Path $ErrorLog -Parent) | Out-Null
New-Item -ItemType Directory -Force (Split-Path -Path $SummaryLog -Parent) | Out-Null

$resolvedEfiRoot = if ($EfiRoot) {
    $EfiRoot
} else {
    Join-Path $BuildDir "artifacts\efi_root"
}

if (-not (Get-Command qemu-system-x86_64 -ErrorAction SilentlyContinue)) {
    if ($Strict) {
        Exit-WithSummary -Status "infra_error" -Reason "qemu_missing" -Code 1
    }
    Exit-WithSummary -Status "infra_warning" -Reason "qemu_missing" -Code 0
}

if (-not (Test-Path $resolvedEfiRoot)) {
    Exit-WithSummary -Status "infra_error" -Reason "efi_root_missing" -Code 1
}

$bootx64 = Join-Path $resolvedEfiRoot "EFI\BOOT\BOOTX64.EFI"
$kernelPayload = Join-Path $resolvedEfiRoot "EFI\BOOT\VIBEOSKR.ELF"
if (-not (Test-Path $bootx64) -or -not (Test-Path $kernelPayload)) {
    Exit-WithSummary -Status "infra_error" -Reason "efi_payload_missing" -Code 1
}

$ovmf = Find-OvmfPair -CodeOverride $OvmfCode -VarsOverride $OvmfVars
if (-not $ovmf) {
    if ($Strict) {
        Exit-WithSummary -Status "infra_error" -Reason "ovmf_missing" -Code 1
    }
    Exit-WithSummary -Status "infra_warning" -Reason "ovmf_missing" -Code 0
}

$runtimeVars = Join-Path (Split-Path $SummaryLog -Parent) "qemu-ovmf-vars-runtime.fd"
Copy-Item -LiteralPath $ovmf.vars -Destination $runtimeVars -Force

"" | Set-Content $SerialLog -Encoding ASCII
"" | Set-Content $ErrorLog -Encoding ASCII

$qemuArgs = @(
    "-machine", "q35",
    "-m", "512M",
    "-display", "none",
    "-monitor", "none",
    "-serial", "file:$SerialLog",
    "-drive", "if=pflash,format=raw,readonly=on,file=$($ovmf.code)",
    "-drive", "if=pflash,format=raw,file=$runtimeVars",
    "-drive", "format=raw,file=fat:rw:$resolvedEfiRoot",
    "-no-reboot",
    "-no-shutdown"
)

$proc = Start-Process -FilePath "qemu-system-x86_64" -ArgumentList $qemuArgs -PassThru -NoNewWindow -RedirectStandardError $ErrorLog
$completed = $proc | Wait-Process -Timeout $Timeout -ErrorAction SilentlyContinue

if ($completed -eq $null) {
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    Exit-WithSummary -Status "timeout" -Reason "timeout_waiting_for_boot_token" -Code 2
}

$serialText = ""
if (Test-Path $SerialLog) {
    $serialText = Get-Content $SerialLog -Raw -ErrorAction SilentlyContinue
}
if ($serialText -match $ExpectToken) {
    Exit-WithSummary -Status "pass" -Reason "boot_token_found" -Code 0
}

Exit-WithSummary -Status "fail" -Reason ("boot_token_missing_rc_{0}" -f $proc.ExitCode) -Code 1
