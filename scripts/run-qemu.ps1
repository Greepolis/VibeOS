param(
    [string]$BuildDir = "build",
    [string]$ImagePath = "artifacts/vibeos_boot.img",
    [int]$Timeout = 30,
    [string]$SerialLog = "artifacts/qemu-serial.log",
    [string]$ExitLog = "artifacts/qemu-exit.txt",
    [string]$ExpectToken = "",
    [switch]$Debug = $false
)

$ErrorActionPreference = "Continue"

# Ensure artifacts directory exists
$artifactsDir = Split-Path -Path $SerialLog -Parent
New-Item -ItemType Directory -Force $artifactsDir | Out-Null

# Resolve image path
$imagePath = if ([System.IO.Path]::IsPathRooted($ImagePath)) {
    $ImagePath
} else {
    Join-Path (Get-Location) $ImagePath
}

Write-Host "[QEMU] Starting VibeOS boot in emulator..."
Write-Host "  Image: $imagePath"
Write-Host "  Timeout: ${Timeout}s"
Write-Host "  Serial log: $SerialLog"

# Check if image exists
if (-not (Test-Path $imagePath)) {
    Write-Host "[ERROR] Boot image not found: $imagePath"
    Write-Host "  Build the image first with: cmake --build $BuildDir --target vibeos_image"
    @{
        status = "infra_error"
        error = "Boot image not found"
        image_path = $imagePath
    } | ConvertTo-Json | Set-Content $ExitLog
    exit 1
}

# Check if QEMU is available
if (-not (Get-Command qemu-system-x86_64 -ErrorAction SilentlyContinue)) {
    Write-Host "[WARNING] qemu-system-x86_64 not found in PATH"
    Write-Host "  Install QEMU to run emulator tests."
    @{
        status = "infra_warning"
        error = "QEMU not installed"
    } | ConvertTo-Json | Set-Content $ExitLog
    exit 0
}

$start = Get-Date

# Configure QEMU arguments
# M1-focused: minimal configuration, serial output only
$qemuArgs = @(
    "-machine", "q35",
    "-m", "512M",
    "-cpu", "host",
    "-enable-kvm",  # Use hardware acceleration if available; will be ignored on non-KVM systems
    "-nographic",
    "-monitor", "none",           # Disable QEMU monitor (prevents stdio conflicts)
    "-serial", "file:${SerialLog}",
    "-kernel", $imagePath,
    "-append", "console=ttyS0",
    "-display", "none"
)

# Add debug flags if requested
if ($Debug) {
    $qemuArgs += @("-S", "-gdb", "tcp::1234")
    Write-Host "[DEBUG] GDB stub enabled on tcp::1234"
}

# Clear serial log
"" | Set-Content $SerialLog -Encoding ASCII -Force

# Run QEMU with timeout
$job = Start-Process -FilePath "qemu-system-x86_64" -ArgumentList $qemuArgs -PassThru -NoNewWindow
$jobId = $job.Id

Write-Host "[QEMU] Process started (PID: $jobId)"

# Wait for completion or timeout
$completed = $job | Wait-Process -Timeout $Timeout -ErrorAction SilentlyContinue

if ($completed -eq $null) {
    Write-Host "[TIMEOUT] QEMU did not complete within ${Timeout}s, terminating..."
    Stop-Process -Id $jobId -Force -ErrorAction SilentlyContinue
    $duration = [int]((Get-Date) - $start).TotalMilliseconds
    
    @{
        status = "timeout"
        pid = $jobId
        duration_ms = $duration
        timeout_s = $Timeout
        serial_log = $SerialLog
    } | ConvertTo-Json | Set-Content $ExitLog
    
    exit 2
}

$exitCode = $job.ExitCode
$duration = [int]((Get-Date) - $start).TotalMilliseconds

Write-Host "[QEMU] Completed with exit code: $exitCode (duration: ${duration}ms)"

# Read and display serial output
if (Test-Path $SerialLog) {
    $serialContent = Get-Content $SerialLog -Raw -ErrorAction SilentlyContinue
    if ([string]::IsNullOrWhiteSpace($serialContent)) {
        Write-Host "[INFO] No serial output captured"
    } else {
        Write-Host "[QEMU SERIAL OUTPUT]"
        Write-Host "---"
        Write-Host $serialContent
        Write-Host "---"
        
        # Check for expected token if specified
        if ($ExpectToken -and $serialContent -match $ExpectToken) {
            Write-Host "[QEMU] Token '$ExpectToken' found in serial output"
        } elseif ($ExpectToken) {
            Write-Host "[QEMU] WARNING: Expected token '$ExpectToken' not found"
        }
    }
} else {
    Write-Host "[WARNING] Serial log not generated"
}

# Record exit status
@{
    status = if ($exitCode -eq 0) { "pass" } else { "fail" }
    exit_code = $exitCode
    pid = $jobId
    duration_ms = $duration
    serial_log = $SerialLog
    image_path = $imagePath
    expected_token = $ExpectToken
} | ConvertTo-Json | Set-Content $ExitLog

Write-Host "[QEMU] Exit log written to $ExitLog"

if ($ExpectToken -and $serialContent -notmatch $ExpectToken) {
    exit 1
}
exit $exitCode
