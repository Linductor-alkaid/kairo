# Windows 打包脚本
# 将构建好的库打包成发行版本

param(
    # CR-085: 留空时从根 CMakeLists.txt 的 project(VERSION) 解析（单一来源）
    [string]$Version = "",
    [string]$BuildDir = "build_windows",
    [string]$OutputDir = "dist",
    # 打包名中的架构标识（x64/arm64）；留空时回退到 $env:PROCESSOR_ARCHITECTURE。
    [string]$Arch = "",
    [switch]$IncludeStatic = $true,
    [switch]$IncludeShared = $true
)

$ErrorActionPreference = "Stop"

# CR-085: 版本单一来源——在首个横幅输出前解析，横幅需要显示它
if (-not $Version) {
    $CMakeListsPath = Join-Path (Split-Path -Parent $PSScriptRoot) "CMakeLists.txt"
    $VersionMatch = Select-String -Path $CMakeListsPath -Pattern '^project\(kairo\s+[^\)]*VERSION\s+([0-9][0-9.]*)\)' |
        Select-Object -First 1
    if ($VersionMatch) {
        $Version = $VersionMatch.Matches[0].Groups[1].Value
    }
}
if (-not $Version) {
    Write-Host "Error: unable to resolve version from CMakeLists.txt and no -Version given" -ForegroundColor Red
    exit 1
}

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "Kairo Windows Package Script" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "Version: $Version" -ForegroundColor Yellow
Write-Host "Build Dir: $BuildDir" -ForegroundColor Yellow
Write-Host "Output Dir: $OutputDir" -ForegroundColor Yellow
Write-Host "Include Static: $IncludeStatic" -ForegroundColor Yellow
Write-Host "Include Shared: $IncludeShared" -ForegroundColor Yellow
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

# Get project root directory
# CR-081: scripts/ 直接位于仓库根下，只需上跳一级；旧版连跳两级导致
# 包内 README/LICENSE/CHANGELOG 静默缺失（Test-Path 恒假）。
$ProjectRoot = Split-Path -Parent $PSScriptRoot

# Create output directories
if (-not $Arch) { $Arch = $env:PROCESSOR_ARCHITECTURE }
$PackageName = "kairo-${Version}-windows-${Arch}"
$PackageDir = Join-Path $OutputDir $PackageName
$PackageDirStatic = Join-Path $PackageDir "static"
$PackageDirShared = Join-Path $PackageDir "shared"

if (Test-Path $PackageDir) {
    Write-Host "Cleaning old package directory..." -ForegroundColor Yellow
    Remove-Item -Recurse -Force $PackageDir
}

New-Item -ItemType Directory -Path $PackageDir -Force | Out-Null
New-Item -ItemType Directory -Path $PackageDirStatic -Force | Out-Null
New-Item -ItemType Directory -Path $PackageDirShared -Force | Out-Null

Write-Host "Starting packaging..." -ForegroundColor Yellow

# Copy static library
if ($IncludeStatic) {
    $StaticInstallDir = Join-Path $BuildDir "static\install"
    if (Test-Path $StaticInstallDir) {
        Write-Host "Copying static library files..." -ForegroundColor Yellow
        Copy-Item -Recurse -Path "$StaticInstallDir\*" -Destination $PackageDirStatic -Force
        
        # Verify key files
        $libFile = Get-ChildItem -Path $PackageDirStatic -Filter "kairo.lib" -Recurse | Select-Object -First 1
        if (-not $libFile) {
            Write-Host "Warning: kairo.lib not found" -ForegroundColor Yellow
        } else {
            Write-Host "  Found: $($libFile.FullName)" -ForegroundColor Green
        }
    } else {
        Write-Host "Warning: Static library install directory does not exist: $StaticInstallDir" -ForegroundColor Yellow
    }
}

# Copy shared library
if ($IncludeShared) {
    $SharedInstallDir = Join-Path $BuildDir "shared\install"
    if (Test-Path $SharedInstallDir) {
        Write-Host "Copying shared library files..." -ForegroundColor Yellow
        Copy-Item -Recurse -Path "$SharedInstallDir\*" -Destination $PackageDirShared -Force
        
        # Verify key files
        $dllFile = Get-ChildItem -Path $PackageDirShared -Filter "kairo.dll" -Recurse | Select-Object -First 1
        $libFile = Get-ChildItem -Path $PackageDirShared -Filter "kairo.lib" -Recurse | Select-Object -First 1
        
        if (-not $dllFile) {
            Write-Host "Warning: kairo.dll not found" -ForegroundColor Yellow
        } else {
            Write-Host "  Found: $($dllFile.FullName)" -ForegroundColor Green
        }
        
        if (-not $libFile) {
            Write-Host "Warning: kairo.lib (import library) not found" -ForegroundColor Yellow
        } else {
            Write-Host "  Found: $($libFile.FullName)" -ForegroundColor Green
        }
    } else {
        Write-Host "Warning: Shared library install directory does not exist: $SharedInstallDir" -ForegroundColor Yellow
    }
}

# Copy documentation and license
Write-Host "Copying documentation files..." -ForegroundColor Yellow
$DocsToCopy = @(
    "README.md",
    "LICENSE",
    "CHANGELOG.md"
)

foreach ($doc in $DocsToCopy) {
    $srcPath = Join-Path $ProjectRoot $doc
    if (Test-Path $srcPath) {
        Copy-Item -Path $srcPath -Destination $PackageDir -Force
        Write-Host "  Copied: $doc" -ForegroundColor Green
    }
}

# Create usage guide
$UsageGuide = @"
# Kairo Windows Distribution Package Usage Guide

## Version Information
- Version: $Version
- Platform: Windows
- Architecture: $Arch

## Directory Structure

### Static Library (static/)
- \`lib/kairo.lib\` - Static library file
- \`include/kairo/\` - Header files directory
- \`lib/cmake/kairo/\` - CMake configuration files (for find_package)

### Shared Library (shared/)
- \`bin/kairo.dll\` - Shared library file (required at runtime)
- \`lib/kairo.lib\` - Import library file (for linking)
- \`include/kairo/\` - Header files directory
- \`lib/cmake/kairo/\` - CMake configuration files (for find_package)

## Usage

### Using Static Library

\`\`\`cmake
find_package(kairo REQUIRED)
target_link_libraries(your_target PRIVATE kairo::kairo)
\`\`\`

Make sure to set the path when configuring CMake:
\`\`\`bash
cmake -DCMAKE_PREFIX_PATH=path/to/kairo-$Version-windows-$Arch/static
\`\`\`

### Using Shared Library

\`\`\`cmake
find_package(kairo REQUIRED)
target_link_libraries(your_target PRIVATE kairo::kairo)
\`\`\`

Make sure to set the path when configuring CMake:
\`\`\`bash
cmake -DCMAKE_PREFIX_PATH=path/to/kairo-$Version-windows-$Arch/shared
\`\`\`

**Note**: When using shared library, ensure \`kairo.dll\` is available at runtime:
- Copy \`kairo.dll\` to the executable directory
- Or add the directory containing \`kairo.dll\` to PATH environment variable

## System Requirements

- Windows 10 or higher
- Visual Studio 2019 or higher (MSVC 14.0+)
- CMake 3.16 or higher
- C++20 support

## More Information

Please refer to README.md and documents in docs/ directory.
"@

$UsageGuidePath = Join-Path $PackageDir "USAGE.md"
Set-Content -Path $UsageGuidePath -Value $UsageGuide -Encoding UTF8
Write-Host "  Created: USAGE.md" -ForegroundColor Green

# Create zip package
Write-Host ""
Write-Host "Creating zip package..." -ForegroundColor Yellow
$ZipPath = Join-Path $OutputDir "${PackageName}.zip"

if (Test-Path $ZipPath) {
    Remove-Item -Force $ZipPath
}

# 使用 .NET 压缩功能
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory($PackageDir, $ZipPath)

Write-Host ""
Write-Host "========================================" -ForegroundColor Green
Write-Host "Packaging completed!" -ForegroundColor Green
Write-Host "========================================" -ForegroundColor Green
Write-Host ""
Write-Host "Package directory: $PackageDir" -ForegroundColor Cyan
Write-Host "Zip package: $ZipPath" -ForegroundColor Cyan
Write-Host ""

# Display package content summary
Write-Host "Package content summary:" -ForegroundColor Yellow
if ($IncludeStatic) {
    $staticLibs = Get-ChildItem -Path $PackageDirStatic -Filter "*.lib" -Recurse
    $staticHeaders = Get-ChildItem -Path $PackageDirStatic -Filter "*.hpp" -Recurse
    Write-Host "  Static library: $($staticLibs.Count) .lib files, $($staticHeaders.Count) header files" -ForegroundColor Cyan
}
if ($IncludeShared) {
    $sharedDlls = Get-ChildItem -Path $PackageDirShared -Filter "*.dll" -Recurse
    $sharedLibs = Get-ChildItem -Path $PackageDirShared -Filter "*.lib" -Recurse
    $sharedHeaders = Get-ChildItem -Path $PackageDirShared -Filter "*.hpp" -Recurse
    Write-Host "  Shared library: $($sharedDlls.Count) .dll files, $($sharedLibs.Count) .lib files, $($sharedHeaders.Count) header files" -ForegroundColor Cyan
}
