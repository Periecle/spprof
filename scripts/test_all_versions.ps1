# test_all_versions.ps1
# Script to test spprof across multiple Python versions on Windows
#
# Usage:
#   .\scripts\test_all_versions.ps1           # Run all versions
#   .\scripts\test_all_versions.ps1 -Setup    # Create venvs only
#   .\scripts\test_all_versions.ps1 -Clean    # Remove all venvs
#   .\scripts\test_all_versions.ps1 -Versions 3.11,3.12  # Test specific versions

param(
    [switch]$Setup,
    [switch]$Clean,
    [switch]$Verbose,
    [string[]]$Versions = @("3.9", "3.10", "3.11", "3.12", "3.13", "3.14")
)

$ErrorActionPreference = "Continue"
# Get project root - handle both direct invocation and script invocation
if ($PSScriptRoot) {
    $ProjectRoot = Split-Path -Parent $PSScriptRoot
} else {
    $ProjectRoot = Get-Location
}

$VenvsDir = Join-Path $ProjectRoot ".venvs"
$Results = @{}

function Write-Header {
    param([string]$Text)
    Write-Host ""
    Write-Host ("=" * 70) -ForegroundColor Cyan
    Write-Host $Text -ForegroundColor Cyan
    Write-Host ("=" * 70) -ForegroundColor Cyan
}

function Write-Status {
    param([string]$Text, [string]$Color = "White")
    Write-Host "  $Text" -ForegroundColor $Color
}

function Test-PythonVersion {
    param([string]$Version)
    $result = py -$Version --version 2>&1
    return $LASTEXITCODE -eq 0
}

function Get-VenvPath {
    param([string]$Version)
    return Join-Path $VenvsDir "py$($Version -replace '\.', '')"
}

function New-Venv {
    param([string]$Version)
    
    $venvPath = Get-VenvPath $Version
    
    if (Test-Path $venvPath) {
        Write-Status "Venv for Python $Version already exists" "Yellow"
        return $true
    }
    
    Write-Status "Creating venv for Python $Version..."
    py -$Version -m venv $venvPath 2>&1 | Out-Null
    
    if ($LASTEXITCODE -ne 0) {
        Write-Status "Failed to create venv for Python $Version" "Red"
        return $false
    }
    
    # Install dependencies
    $pip = Join-Path $venvPath "Scripts\pip.exe"
    & $pip install --upgrade pip -q 2>&1 | Out-Null
    & $pip install setuptools wheel pytest pytest-timeout -q 2>&1 | Out-Null
    
    Write-Status "Venv created successfully" "Green"
    return $true
}

function Remove-Venv {
    param([string]$Version)
    
    $venvPath = Get-VenvPath $Version
    
    if (Test-Path $venvPath) {
        Write-Status "Removing venv for Python $Version..."
        Remove-Item -Path $venvPath -Recurse -Force
        Write-Status "Removed" "Green"
    } else {
        Write-Status "No venv found for Python $Version" "Yellow"
    }
}

function Build-And-Test {
    param([string]$Version)
    
    $venvPath = Get-VenvPath $Version
    $python = Join-Path $venvPath "Scripts\python.exe"
    $pip = Join-Path $venvPath "Scripts\pip.exe"
    
    if (-not (Test-Path $python)) {
        Write-Status "Venv not found. Creating..." "Yellow"
        if (-not (New-Venv $Version)) {
            return @{
                Version = $Version
                Status = "FAILED"
                Error = "Could not create venv"
                Passed = 0
                Failed = 0
                Skipped = 0
            }
        }
    }
    
    # Clean build directory
    $buildDir = Join-Path $ProjectRoot "build"
    if (Test-Path $buildDir) {
        Remove-Item -Path $buildDir -Recurse -Force -ErrorAction SilentlyContinue
    }
    
    # Build the extension
    Write-Status "Building extension..."
    Push-Location $ProjectRoot
    $buildOutput = & $pip install -e . --no-build-isolation -q 2>&1
    $buildSuccess = $LASTEXITCODE -eq 0
    Pop-Location
    
    if (-not $buildSuccess) {
        Write-Status "Build failed!" "Red"
        if ($Verbose) {
            Write-Host $buildOutput -ForegroundColor Red
        }
        return @{
            Version = $Version
            Status = "BUILD FAILED"
            Error = "Extension build failed"
            Passed = 0
            Failed = 0
            Skipped = 0
        }
    }
    Write-Status "Build successful" "Green"
    
    # Run tests
    Write-Status "Running tests..."
    Push-Location $ProjectRoot
    $testOutput = & $python -m pytest tests/test_profiler.py -v --tb=short 2>&1
    $testExitCode = $LASTEXITCODE
    Pop-Location
    
    # Parse test results
    $passed = 0
    $failed = 0
    $skipped = 0
    
    foreach ($line in $testOutput) {
        if ($line -match "(\d+) passed") { $passed = [int]$Matches[1] }
        if ($line -match "(\d+) failed") { $failed = [int]$Matches[1] }
        if ($line -match "(\d+) skipped") { $skipped = [int]$Matches[1] }
    }
    
    $status = if ($testExitCode -eq 0) { "PASSED" } else { "FAILED" }
    $color = if ($testExitCode -eq 0) { "Green" } else { "Red" }
    
    Write-Status "Tests: $passed passed, $failed failed, $skipped skipped" $color
    
    if ($Verbose -or $testExitCode -ne 0) {
        Write-Host ""
        foreach ($line in $testOutput) {
            if ($line -match "PASSED|FAILED|ERROR|::") {
                $lineColor = if ($line -match "PASSED") { "Green" } 
                            elseif ($line -match "FAILED|ERROR") { "Red" }
                            else { "Gray" }
                Write-Host "    $line" -ForegroundColor $lineColor
            }
        }
    }
    
    return @{
        Version = $Version
        Status = $status
        Error = $null
        Passed = $passed
        Failed = $failed
        Skipped = $skipped
    }
}

# Main script
Write-Header "spprof Multi-Version Test Script"
Write-Host "Project: $ProjectRoot"
Write-Host "Venvs directory: $VenvsDir"
Write-Host "Versions to test: $($Versions -join ', ')"

# Create venvs directory if it doesn't exist
if (-not (Test-Path $VenvsDir)) {
    New-Item -ItemType Directory -Path $VenvsDir -Force | Out-Null
}

# Clean mode
if ($Clean) {
    Write-Header "Cleaning Virtual Environments"
    foreach ($version in $Versions) {
        Remove-Venv $version
    }
    Write-Host ""
    Write-Host "Cleanup complete!" -ForegroundColor Green
    exit 0
}

# Setup mode
if ($Setup) {
    Write-Header "Setting Up Virtual Environments"
    foreach ($version in $Versions) {
        Write-Host ""
        Write-Host "Python $version" -ForegroundColor Yellow
        if (Test-PythonVersion $version) {
            New-Venv $version | Out-Null
        } else {
            Write-Status "Python $version not available on this system" "Red"
        }
    }
    Write-Host ""
    Write-Host "Setup complete!" -ForegroundColor Green
    exit 0
}

# Test mode (default)
Write-Header "Running Tests Across All Python Versions"

foreach ($version in $Versions) {
    Write-Host ""
    Write-Host "Python $version" -ForegroundColor Yellow
    Write-Host ("-" * 40) -ForegroundColor Gray
    
    if (-not (Test-PythonVersion $version)) {
        Write-Status "Python $version not available on this system" "Red"
        $Results[$version] = @{
            Version = $version
            Status = "SKIPPED"
            Error = "Python not available"
            Passed = 0
            Failed = 0
            Skipped = 0
        }
        continue
    }
    
    $Results[$version] = Build-And-Test $version
}

# Summary
Write-Header "Test Results Summary"

$totalPassed = 0
$totalFailed = 0
$totalSkipped = 0
$versionsOk = 0
$versionsFailed = 0

foreach ($version in $Versions) {
    $result = $Results[$version]
    $statusColor = switch ($result.Status) {
        "PASSED" { "Green" }
        "FAILED" { "Red" }
        "BUILD FAILED" { "Red" }
        "SKIPPED" { "Yellow" }
        default { "White" }
    }
    
    $line = "  Python {0,-6} : {1,-12}" -f $version, $result.Status
    if ($result.Passed -gt 0 -or $result.Failed -gt 0) {
        $line += " ({0} passed, {1} failed, {2} skipped)" -f $result.Passed, $result.Failed, $result.Skipped
    }
    
    Write-Host $line -ForegroundColor $statusColor
    
    $totalPassed += $result.Passed
    $totalFailed += $result.Failed
    $totalSkipped += $result.Skipped
    
    if ($result.Status -eq "PASSED") { $versionsOk++ }
    elseif ($result.Status -in @("FAILED", "BUILD FAILED")) { $versionsFailed++ }
}

Write-Host ""
Write-Host ("-" * 50) -ForegroundColor Gray
Write-Host "  Total: $totalPassed passed, $totalFailed failed, $totalSkipped skipped" -ForegroundColor White
Write-Host "  Versions: $versionsOk OK, $versionsFailed failed" -ForegroundColor White
Write-Host ""

if ($versionsFailed -gt 0) {
    Write-Host "Some tests failed!" -ForegroundColor Red
    exit 1
} else {
    Write-Host "All tests passed!" -ForegroundColor Green
    exit 0
}

