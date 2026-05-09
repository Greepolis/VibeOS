param(
    [string]$BuildDir = "build",
    [string]$EfiRoot = "",
    [string]$ExpectToken = "BOOT_OK",
    [int]$Timeout = 60,
    [string]$SerialLog = "artifacts/qemu-ovmf-serial.log",
    [string]$ErrorLog = "artifacts/qemu-ovmf-stderr.log",
    [string]$SummaryLog = "artifacts/qemu-ovmf-summary.json",
    [string]$OvmfCode = "",
    [string]$OvmfVars = "",
    [switch]$Strict = $false
)

$ErrorActionPreference = "Stop"
$TailLines = 30

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
        "C:\Program Files\qemu\share\edk2-x86_64-vars.fd",
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
        [int]$Code,
        [string]$ProfileUsed = "none",
        [string]$LastBootPhase = "none",
        [int]$QemuRc = 0,
        [int]$Attempts = 0,
        [string]$SerialTail = "",
        [string]$ErrorTail = ""
    )
    $summary = @{
        status = $Status
        reason = $Reason
        qemu_rc = $QemuRc
        timeout_sec = $Timeout
        expect_token = $ExpectToken
        profile_used = $ProfileUsed
        last_boot_phase = $LastBootPhase
        attempts = $Attempts
        build_dir = $BuildDir
        efi_root = $resolvedEfiRoot
        serial_log = $SerialLog
        error_log = $ErrorLog
        ovmf_code = if ($ovmf) { $ovmf.code } else { "unknown" }
        ovmf_vars_template = if ($ovmf) { $ovmf.vars } else { "unknown" }
        serial_tail = $SerialTail
        error_tail = $ErrorTail
    }
    $summary | ConvertTo-Json -Depth 5 | Set-Content -Path $SummaryLog -Encoding UTF8
    exit $Code
}

function Get-LastBootPhase {
    param(
        [string]$SerialText
    )

    $phase = "none"
    if ($SerialText -match "BL_FS_OK") {
        $phase = "BL_FS_OK"
    }
    if ($SerialText -match "BL_PLAN_OK") {
        $phase = "BL_PLAN_OK"
    }
    if ($SerialText -match "BL_HANDOFF_OK|BL_HANDOFF_START") {
        $phase = "BL_HANDOFF_OK"
    }
    if ($SerialText -match $ExpectToken) {
        $phase = "BOOT_OK"
    }
    return $phase
}

function Get-PhaseScore {
    param(
        [string]$Phase
    )
    switch ($Phase) {
        "BOOT_OK" { return 4 }
        "BL_HANDOFF_OK" { return 3 }
        "BL_PLAN_OK" { return 2 }
        "BL_FS_OK" { return 1 }
        default { return 0 }
    }
}

function Get-TailInline {
    param(
        [string]$Path,
        [int]$Lines = 30
    )
    if (-not (Test-Path $Path)) {
        return ""
    }
    $tail = Get-Content -Path $Path -Tail $Lines -ErrorAction SilentlyContinue
    if (-not $tail) {
        return ""
    }
    return (($tail | ForEach-Object { $_ -replace "`r", " " }) -join " | ")
}

function Test-EfiBootloaderHeader {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if (-not (Test-Path $Path)) {
        return @{ ok = $false; reason = "missing_file" }
    }

    $stream = $null
    $reader = $null
    try {
        $stream = [System.IO.File]::OpenRead($Path)
        if ($stream.Length -lt 128) {
            return @{ ok = $false; reason = "too_small" }
        }
        $reader = New-Object System.IO.BinaryReader($stream)

        $m0 = $reader.ReadByte()
        $m1 = $reader.ReadByte()
        if ($m0 -ne 0x4D -or $m1 -ne 0x5A) {
            return @{ ok = $false; reason = "missing_mz" }
        }

        $stream.Position = 0x3C
        $e_lfanew = $reader.ReadUInt32()
        if (($e_lfanew + 0x18 + 0x46) -gt $stream.Length) {
            return @{ ok = $false; reason = "pe_offset_out_of_range" }
        }

        $stream.Position = $e_lfanew
        $sig = $reader.ReadBytes(4)
        if ($sig.Length -ne 4 -or $sig[0] -ne 0x50 -or $sig[1] -ne 0x45 -or $sig[2] -ne 0x00 -or $sig[3] -ne 0x00) {
            return @{ ok = $false; reason = "missing_pe_signature" }
        }

        $stream.Position = $e_lfanew + 0x18
        $magic = $reader.ReadUInt16()
        if ($magic -ne 0x20B) {
            return @{ ok = $false; reason = ("optional_magic_0x{0:x}" -f $magic) }
        }

        $stream.Position = $e_lfanew + 0x18 + 0x44
        $subsystem = $reader.ReadUInt16()
        if ($subsystem -ne 10) {
            return @{ ok = $false; reason = ("subsystem_{0}" -f $subsystem) }
        }

        return @{ ok = $true; reason = "ok" }
    } catch {
        return @{ ok = $false; reason = ("exception_{0}" -f $_.Exception.Message.Replace(' ', '_')) }
    } finally {
        if ($reader) {
            $reader.Close()
        } elseif ($stream) {
            $stream.Close()
        }
    }
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

$efiValidation = Test-EfiBootloaderHeader -Path $bootx64
if (-not $efiValidation.ok) {
    Exit-WithSummary -Status "infra_error" -Reason ("efi_header_invalid_{0}" -f $efiValidation.reason) -Code 1
}

$ovmf = Find-OvmfPair -CodeOverride $OvmfCode -VarsOverride $OvmfVars
if (-not $ovmf) {
    if ($Strict) {
        Exit-WithSummary -Status "infra_error" -Reason "ovmf_missing" -Code 1
    }
    Exit-WithSummary -Status "infra_warning" -Reason "ovmf_missing" -Code 0
}

$summaryDir = Split-Path $SummaryLog -Parent
$fatEfiRoot = (Resolve-Path $resolvedEfiRoot).Path -replace "\\", "/"
$profiles = @(
    @{
        name = "primary"
        machine = "q35"
        driveArgs = @(
            "-drive", "if=none,id=esp,format=raw,file=fat:rw:$fatEfiRoot",
            "-device", "virtio-blk-pci,drive=esp,bootindex=1"
        )
    },
    @{
        name = "compat"
        machine = "pc"
        driveArgs = @(
            "-drive", "if=ide,index=0,media=disk,format=raw,file=fat:rw:$fatEfiRoot"
        )
    }
)

$attemptResults = @()
$attemptCount = 0
$best = $null
$bestScore = -1
$seenTimeout = $false

foreach ($profile in $profiles) {
    $attemptCount += 1
    $name = $profile.name
    $machine = $profile.machine
    $driveArgs = $profile.driveArgs

    $runtimeVars = Join-Path $summaryDir ("qemu-ovmf-vars-runtime-{0}.fd" -f $name)
    $attemptSerial = "qemu-ovmf-serial-{0}.log" -f $name
    $attemptError = "qemu-ovmf-err-{0}.log" -f $name
    Copy-Item -LiteralPath $ovmf.vars -Destination $runtimeVars -Force
    "" | Set-Content $attemptSerial -Encoding ASCII
    "" | Set-Content $attemptError -Encoding ASCII

    $qemuArgs = @(
        "-machine", $machine,
        "-m", "512M",
        "-display", "none",
        "-monitor", "none",
        "-serial", "file:$attemptSerial",
        "-drive", "if=pflash,format=raw,readonly=on,file=$($ovmf.code)",
        "-drive", "if=pflash,format=raw,file=$runtimeVars"
    ) + $driveArgs + @(
        "-net", "none",
        "-no-reboot",
        "-no-shutdown"
    )

    $proc = Start-Process -FilePath "qemu-system-x86_64" -ArgumentList $qemuArgs -PassThru -NoNewWindow -RedirectStandardError $attemptError
    $completed = $proc | Wait-Process -Timeout $Timeout -ErrorAction SilentlyContinue
    $timedOut = $completed -eq $null
    if ($timedOut) {
        $seenTimeout = $true
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    }

    $serialText = ""
    if (Test-Path $attemptSerial) {
        $serialText = Get-Content $attemptSerial -Raw -ErrorAction SilentlyContinue
    }
    $phase = Get-LastBootPhase -SerialText $serialText
    $score = Get-PhaseScore -Phase $phase
    $tokenFound = $serialText -match $ExpectToken
    $exitCode = if ($timedOut) { 124 } else { $proc.ExitCode }
    $serialTail = Get-TailInline -Path $attemptSerial -Lines $TailLines
    $errorTail = Get-TailInline -Path $attemptError -Lines $TailLines

    $result = @{
        name = $name
        machine = $machine
        phase = $phase
        score = $score
        tokenFound = $tokenFound
        timedOut = $timedOut
        exitCode = $exitCode
        serialLog = $attemptSerial
        errorLog = $attemptError
        serialTail = $serialTail
        errorTail = $errorTail
    }
    $attemptResults += $result

    if ($score -gt $bestScore) {
        $bestScore = $score
        $best = $result
    }

    if ($tokenFound) {
        Copy-Item -LiteralPath $attemptSerial -Destination $SerialLog -Force
        Copy-Item -LiteralPath $attemptError -Destination $ErrorLog -Force
        Exit-WithSummary -Status "pass" -Reason "boot_token_found" -Code 0 -ProfileUsed $name -LastBootPhase "BOOT_OK" -QemuRc $exitCode -Attempts $attemptCount -SerialTail $serialTail -ErrorTail $errorTail
    }
}

if (-not $best -and $attemptResults.Count -gt 0) {
    $best = $attemptResults[-1]
}

if ($best) {
    Copy-Item -LiteralPath $best.serialLog -Destination $SerialLog -Force
    Copy-Item -LiteralPath $best.errorLog -Destination $ErrorLog -Force
}

if ($seenTimeout) {
    Exit-WithSummary -Status "timeout" -Reason "timeout_waiting_for_boot_token" -Code 2 -ProfileUsed $best.name -LastBootPhase $best.phase -QemuRc $best.exitCode -Attempts $attemptCount -SerialTail $best.serialTail -ErrorTail $best.errorTail
}

Exit-WithSummary -Status "fail" -Reason ("boot_token_missing_rc_{0}" -f $best.exitCode) -Code 1 -ProfileUsed $best.name -LastBootPhase $best.phase -QemuRc $best.exitCode -Attempts $attemptCount -SerialTail $best.serialTail -ErrorTail $best.errorTail
