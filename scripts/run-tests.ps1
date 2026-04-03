param(
    [string]$BuildDir = "build",
    [string]$Profile = "agent"
)

$ErrorActionPreference = "Stop"

$artifactsDir = "artifacts"
New-Item -ItemType Directory -Force $artifactsDir | Out-Null

$runId = (Get-Date).ToString("yyyyMMdd-HHmmss")
$summaryPath = Join-Path $artifactsDir "test-summary.json"
$logPath = Join-Path $artifactsDir "kernel-tests.log"
New-Item -ItemType File -Force $logPath | Out-Null
Add-Content -Path $logPath -Value ("run_id={0}" -f $runId)

$start = Get-Date
$status = "pass"
$failedTests = @()
$suite = "kernel-core-host"
$gitRevision = "unknown"

try {
    $gitRevision = (git rev-parse --short HEAD).Trim()
} catch {
}

try {
    if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
        throw "INFRA_CMAKE_MISSING: cmake command not found in PATH"
    }

    cmake -S . -B $BuildDir -DVIBEOS_BUILD_TESTS=ON | Tee-Object -FilePath $logPath
    cmake --build $BuildDir | Tee-Object -FilePath $logPath -Append
    & ".\$BuildDir\vibeos_kernel_tests.exe" | Tee-Object -FilePath $logPath -Append
    if ($LASTEXITCODE -ne 0) {
        throw "kernel tests returned non-zero exit code"
    }
} catch {
    $status = "fail"
    if ($_.Exception.Message -like "INFRA_*") {
        $status = "infra_error"
    }
    $failedTests += @{
        id = "KERNEL.CORE.HOST.001"
        title = "Kernel core host tests failed"
        layer = "L0"
        subsystem = "kernel-core"
        severity = "high"
        symptom = $_.Exception.Message
        likely_causes = @(
            "regression in core module contracts",
            "host build flags mismatch",
            "unexpected API break in kernel core"
        )
        suggested_entrypoints = @(
            "kernel/core/",
            "kernel/mm/",
            "kernel/sched/",
            "kernel/ipc/",
            "tests/kernel/"
        )
        artifacts = @(
            $logPath
        )
    }
}

$durationMs = [int]((Get-Date) - $start).TotalMilliseconds

$summary = @{
    run_id = $runId
    git_revision = $gitRevision
    suite = $suite
    profile = $Profile
    status = $status
    duration_ms = $durationMs
    failed_tests = $failedTests
}

$summary | ConvertTo-Json -Depth 8 | Set-Content -Path $summaryPath -Encoding UTF8

if ($status -ne "pass") {
    if ($status -eq "infra_error") {
        Write-Host "[INFRA] KERNEL.CORE.HOST.001 kernel-core Kernel test infrastructure error"
    } else {
        Write-Host "[FAIL] KERNEL.CORE.HOST.001 kernel-core Kernel core host tests failed"
    }
    Write-Host "  next: inspect $logPath and core kernel modules"
    exit 1
}

Write-Host "[PASS] KERNEL.CORE.HOST.000 kernel-core Host kernel test suite passed"
exit 0
