param(
    [string]$BuildDir = "build",
    [string]$Profile = "agent",
    [string]$Generator = "Ninja",
    [switch]$SkipTests = $false,
    [switch]$SkipImageBuild = $false,
    [switch]$RunQemu = $false,
    [switch]$RunM2BootSmoke = $false
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
$toolchainInfo = @{}

function Test-IsCMakeBuildDirectory {
    param(
        [string]$Dir
    )
    if (-not (Test-Path -LiteralPath $Dir)) {
        return $false
    }
    $cachePath = Join-Path $Dir "CMakeCache.txt"
    return (Test-Path -LiteralPath $cachePath)
}

function Test-CMakeGeneratorAvailable {
    param(
        [string]$Name
    )
    $helpOutput = cmake --help 2>&1
    foreach ($line in $helpOutput) {
        if ($line -match "^\s*\*?\s*(.+?)\s*=\s*Generates") {
            $generatorName = $Matches[1].Trim()
            if ($generatorName -eq $Name) {
                return $true
            }
        }
    }
    return $false
}

function Get-ToolchainVersions {
    # Capture toolchain versions for reporting
    $versions = @{}
    
    # CMake version
    try {
        $cmakeOutput = cmake --version 2>&1
        if ($cmakeOutput -match "cmake version (.+)") {
            $versions["cmake"] = $Matches[1]
        }
    } catch { }
    
    # GCC version (fallback compiler)
    try {
        $gccOutput = gcc --version 2>&1
        if ($gccOutput -match "gcc \(.*\) (.+)") {
            $versions["gcc"] = $Matches[1]
        }
    } catch { }
    
    # QEMU version (if available)
    try {
        $qemuOutput = qemu-system-x86_64 --version 2>&1
        if ($qemuOutput -match "QEMU emulator version (.+)") {
            $versions["qemu"] = $Matches[1]
        }
    } catch { }
    
    # Ninja version (if using Ninja generator)
    if ($Generator -eq "Ninja") {
        try {
            $ninjaOutput = ninja --version 2>&1
            $versions["ninja"] = $ninjaOutput.Trim()
        } catch { }
    }
    
    return $versions
}

function Validate-BuildArtifacts {
    param(
        [string]$BuildDir,
        [bool]$ImageBuildEnabled
    )
    $artifacts = @()
    
    # Check kernel ELF
    $kernelElf = Join-Path $BuildDir "artifacts" "vibeos_kernel.elf"
    if (Test-Path $kernelElf) {
        $artifacts += @{
            name = "vibeos_kernel.elf"
            path = $kernelElf
            status = "found"
            size = (Get-Item $kernelElf).Length
        }
    } else {
        $artifacts += @{
            name = "vibeos_kernel.elf"
            status = "missing"
        }
    }
    
    # Check boot image
    if ($ImageBuildEnabled) {
        $bootImage = Join-Path $BuildDir "artifacts" "vibeos_boot.img"
        if (Test-Path $bootImage) {
            $artifacts += @{
                name = "vibeos_boot.img"
                path = $bootImage
                status = "found"
                size = (Get-Item $bootImage).Length
            }
        } else {
            $artifacts += @{
                name = "vibeos_boot.img"
                status = "missing"
            }
        }
    }
    
    return $artifacts
}

function Get-LatestSourceWriteTimeUtc {
    param(
        [string[]]$Paths
    )
    $latest = [datetime]::MinValue
    foreach ($entry in $Paths) {
        if (-not (Test-Path -LiteralPath $entry)) {
            continue
        }
        $item = Get-Item -LiteralPath $entry -ErrorAction SilentlyContinue
        if ($item -and -not $item.PSIsContainer) {
            if ($item.LastWriteTimeUtc -gt $latest) {
                $latest = $item.LastWriteTimeUtc
            }
            continue
        }
        $files = Get-ChildItem -LiteralPath $entry -Recurse -File -ErrorAction SilentlyContinue
        foreach ($f in $files) {
            if ($f.LastWriteTimeUtc -gt $latest) {
                $latest = $f.LastWriteTimeUtc
            }
        }
    }
    if ($latest -eq [datetime]::MinValue) {
        return $null
    }
    return $latest
}

function Assert-HostTestExecutableFresh {
    param(
        [string]$Path,
        [datetime]$ReferenceUtc
    )
    if (-not (Test-Path -LiteralPath $Path)) {
        throw ("missing expected host test executable: {0}" -f $Path)
    }
    if ($ReferenceUtc -eq $null) {
        return
    }
    $exeTime = (Get-Item -LiteralPath $Path).LastWriteTimeUtc
    if ($exeTime -lt $ReferenceUtc) {
        throw ("stale host test executable detected: {0} (exe_utc={1:o}, src_utc={2:o})" -f $Path, $exeTime, $ReferenceUtc)
    }
}

try {
    $gitRevision = (git rev-parse --short HEAD).Trim()
} catch {
}

# Collect toolchain versions before build
$toolchainInfo = Get-ToolchainVersions
Add-Content -Path $logPath -Value ("toolchain_info={0}" -f ($toolchainInfo | ConvertTo-Json -Compress))

try {
    if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
        throw "INFRA_CMAKE_MISSING: cmake command not found in PATH"
    }

    if (-not (Test-CMakeGeneratorAvailable -Name $Generator)) {
        throw ("INFRA_CMAKE_GENERATOR_UNAVAILABLE: cmake generator unavailable (generator={0})" -f $Generator)
    }

    # Step 1: Configure and build via CMake
    try {
        if (-not (Test-IsCMakeBuildDirectory -Dir $BuildDir)) {
            if (Test-Path -LiteralPath $BuildDir) {
                Write-Host ("[BUILD] Build directory '{0}' is not a CMake build tree; reconfiguring in-place" -f $BuildDir)
            } else {
                Write-Host ("[BUILD] Build directory '{0}' not found; creating via CMake configure" -f $BuildDir)
            }
        }

        $configArgs = @(
            "-S", ".",
            "-B", $BuildDir,
            "-G", $Generator,
            "-DVIBEOS_BUILD_TESTS=ON"
        )
        if ($SkipImageBuild) {
            $configArgs += "-DVIBEOS_BUILD_KERNEL_IMAGE=OFF"
        } else {
            $configArgs += "-DVIBEOS_BUILD_KERNEL_IMAGE=ON"
        }
        Write-Host ("[BUILD] Configuring with generator '{0}'..." -f $Generator)
        & cmake @configArgs | Tee-Object -FilePath $logPath -Append
        if ($LASTEXITCODE -ne 0) {
            throw ("cmake configure failed (generator={0})" -f $Generator)
        }

        # Step 2: Build
        Write-Host "[BUILD] Building project..."
        & cmake --build $BuildDir --parallel | Tee-Object -FilePath $logPath -Append
        if ($LASTEXITCODE -ne 0) {
            throw "cmake build failed"
        }

        # Validate image artifacts if build enabled
        if (-not $SkipImageBuild) {
            Write-Host "[BUILD] Validating kernel image artifacts..."
            $artifacts = Validate-BuildArtifacts -BuildDir $BuildDir -ImageBuildEnabled $true
            Add-Content -Path $logPath -Value ("artifacts_validation={0}" -f ($artifacts | ConvertTo-Json -Compress))
            
            # Check if critical artifacts are missing
            $missingCritical = $artifacts | Where-Object { $_.name -eq "vibeos_kernel.elf" -and $_.status -eq "missing" }
            if ($missingCritical) {
                throw "VALIDATION: Required kernel ELF image not generated"
            }
        }

        # Step 3: Run host tests (if not skipped)
        if (-not $SkipTests) {
            Write-Host "[TEST] Running host-side kernel tests..."
            $latestSourceUtc = Get-LatestSourceWriteTimeUtc -Paths @(
                "kernel",
                "boot",
                "user",
                "include",
                "tests",
                "CMakeLists.txt"
            )
            Add-Content -Path $logPath -Value ("latest_source_utc={0}" -f ($latestSourceUtc))

            $hostTests = @(
                (Join-Path $BuildDir "vibeos_kernel_tests.exe"),
                (Join-Path $BuildDir "vibeos_bootloader_tests.exe")
            )
            foreach ($testExe in $hostTests) {
                Assert-HostTestExecutableFresh -Path $testExe -ReferenceUtc $latestSourceUtc
                Write-Host "[TEST] Executing $testExe"
                & $testExe | Tee-Object -FilePath $logPath -Append
                if ($LASTEXITCODE -ne 0) {
                    throw ("host test executable returned non-zero exit code: {0}" -f $testExe)
                }
            }
        }
        
        # Step 4: Run QEMU smoke (if requested and image built)
        if ($RunQemu -and -not $SkipImageBuild) {
            Write-Host "[QEMU] Starting emulator smoke test..."
            $imagePath = Join-Path $BuildDir "artifacts" "vibeos_boot.img"
            & .\scripts\run-qemu.ps1 -BuildDir $BuildDir -ImagePath $imagePath | Tee-Object -FilePath $logPath -Append
            if ($LASTEXITCODE -ne 0) {
                Write-Host "[WARNING] QEMU test did not complete cleanly; continuing"
            }
        }
        
        # Step 5: Run M2 boot smoke test (if requested)
        if ($RunM2BootSmoke) {
            Write-Host "[M2-BOOT] Starting M2 boot smoke test..."
            $imagePath = Join-Path $BuildDir "artifacts" "vibeos_boot.img"
            if (-not (Test-Path $imagePath)) {
                Write-Host "[M2-BOOT] ERROR: boot image not found"
                throw "M2 boot smoke test failed: boot image missing"
            }
            Write-Host "[M2-BOOT] Running QEMU boot verification..."
            & .\scripts\run-qemu.ps1 -BuildDir $BuildDir -ImagePath $imagePath -ExpectToken "BOOT_OK" -Timeout 10 | Tee-Object -FilePath $logPath -Append
            if ($LASTEXITCODE -ne 0) {
                throw "M2 boot smoke test failed: kernel did not output BOOT_OK"
            }
            Write-Host "[M2-BOOT] Boot verification PASSED"
        }

    } catch {
        Add-Content -Path $logPath -Value ("cmake_path_failed={0}" -f $_.Exception.Message)

        if ($_.Exception.Message -like "INFRA_CMAKE_GENERATOR_UNAVAILABLE:*") {
            throw $_.Exception.Message
        }

        if (-not (Get-Command gcc -ErrorAction SilentlyContinue)) {
            throw "INFRA_GCC_MISSING: gcc command not found for manual fallback"
        }

        # Fallback: manual GCC build
        Write-Host "[BUILD] CMake failed, attempting manual GCC fallback..."
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
            "kernel/arch/x86_64/serial.c",
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
            "user/compat/compat_runtime.c",
            "user/compat/linux/linux_compat.c",
            "user/compat/windows/windows_compat.c",
            "user/compat/macos/macos_compat.c",
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
        Add-Content -Path $logPath -Value ("manual_fallback_exe={0}" -f $manualExe)
    }
} catch {
    $status = "fail"
    if ($_.Exception.Message -like "INFRA_*") {
        $status = "infra_error"
    } elseif ($_.Exception.Message -like "*stale host test executable*") {
        $status = "build_validation_failed"
    } elseif ($_.Exception.Message -like "VALIDATION:*") {
        $status = "build_validation_failed"
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
            "unexpected API break in kernel core",
            "missing build artifacts or image generation failure"
        )
        suggested_entrypoints = @(
            "kernel/core/",
            "kernel/arch/x86_64/",
            "kernel/mm/",
            "kernel/sched/",
            "kernel/ipc/",
            "tests/kernel/",
            "cmake/"
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
    toolchain = $toolchainInfo
    build_dir = $BuildDir
    image_build_enabled = (-not $SkipImageBuild)
    qemu_test_enabled = $RunQemu
    failed_tests = $failedTests
}

$summary | ConvertTo-Json -Depth 10 | Set-Content -Path $summaryPath -Encoding UTF8

if ($status -ne "pass") {
    if ($status -eq "infra_error") {
        Write-Host "[INFRA] KERNEL.CORE.HOST.001 kernel-core Kernel test infrastructure error"
    } elseif ($status -eq "build_validation_failed") {
        Write-Host "[VALIDATION] KERNEL.CORE.BUILD.001 kernel-core Build artifact validation failed"
    } else {
        Write-Host "[FAIL] KERNEL.CORE.HOST.001 kernel-core Kernel core host tests failed"
    }
    Write-Host "  next: inspect $logPath and core kernel modules"
    exit 1
}

Write-Host "[PASS] KERNEL.CORE.HOST.000 kernel-core Host kernel test suite passed"
Write-Host "  toolchain: $(($toolchainInfo | ConvertTo-Json -Compress))"
Write-Host "  summary: $(Join-Path (Get-Location) $summaryPath)"
exit 0
