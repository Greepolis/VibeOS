param(
    [string]$BuildDir = "build",
    [string]$Profile = "agent",
    [string]$Generator = "Ninja"
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
    try {
        cmake -S . -B $BuildDir -G $Generator -DVIBEOS_BUILD_TESTS=ON | Tee-Object -FilePath $logPath
        cmake --build $BuildDir | Tee-Object -FilePath $logPath -Append
        & ".\$BuildDir\vibeos_kernel_tests.exe" | Tee-Object -FilePath $logPath -Append
        if ($LASTEXITCODE -ne 0) {
            throw "kernel tests returned non-zero exit code"
        }
    } catch {
        Add-Content -Path $logPath -Value ("cmake_path_failed={0}" -f $_.Exception.Message)
        if (-not (Get-Command gcc -ErrorAction SilentlyContinue)) {
            throw "INFRA_GCC_MISSING: gcc command not found for manual fallback"
        }
        $manualExe = Join-Path $BuildDir "vibeos_kernel_tests_manual.exe"
        $gccArgs = @(
            "-std=c11", "-Wall", "-Wextra", "-Wpedantic",
            "-Iinclude",
            "tests/kernel/kernel_tests.c",
            "kernel/core/kmain.c",
            "kernel/mm/pmm.c",
            "kernel/mm/vm.c",
            "kernel/object/handle_table.c",
            "kernel/proc/process.c",
            "kernel/sched/scheduler.c",
            "kernel/ipc/event.c",
            "kernel/ipc/channel.c",
            "kernel/ipc/handle_transfer.c",
            "kernel/ipc/waitset.c",
            "kernel/core/interrupts.c",
            "kernel/core/policy.c",
            "kernel/core/syscall_policy.c",
            "kernel/core/security.c",
            "kernel/core/syscall.c",
            "kernel/time/timer.c",
            "kernel/arch/x86_64/trap.c",
            "kernel/arch/x86_64/idt.c",
            "kernel/arch/x86_64/boot_stub.c",
            "user/init/init_system.c",
            "user/servicemgr/service_manager.c",
            "user/servicemgr/service_ipc.c",
            "user/devmgr/device_manager.c",
            "user/devmgr/driver_host.c",
            "user/drivers/driver_framework.c",
            "user/fs/vfs_service.c",
            "user/fs/vfs_ops.c",
            "user/net/network_service.c",
            "user/net/socket.c",
            "user/lib/user_api.c",
            "boot/bootloader_stub.c",
            "-o", $manualExe
        )
        New-Item -ItemType Directory -Force $BuildDir | Out-Null
        & gcc @gccArgs | Tee-Object -FilePath $logPath -Append
        if ($LASTEXITCODE -ne 0) {
            throw "manual gcc kernel tests build failed"
        }
        & $manualExe | Tee-Object -FilePath $logPath -Append
        if ($LASTEXITCODE -ne 0) {
            throw "manual gcc kernel tests execution failed"
        }
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
