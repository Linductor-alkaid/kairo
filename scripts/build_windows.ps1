# Windows 构建脚本
# 用于构建 kairo 库的静态库和动态库

param(
    [string]$BuildType = "Release",
    # 生成器留空 = 使用 CMake 默认（自动跟随 runner/本机安装的 VS 版本）。
    # GitHub windows-latest 镜像已升级到 Visual Studio 18 2026，钉死 VS17 2022
    # 会在新镜像上报 "could not find any instance of Visual Studio"。
    [string]$Generator = "",
    [string]$Architecture = "x64",
    [switch]$BuildStatic = $true,
    [switch]$BuildShared = $true,
    [switch]$BuildTests = $false,
    [switch]$BuildExamples = $false,
    [string]$OutputDir = "build_windows"
)

$ErrorActionPreference = "Stop"

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "Kairo Windows Build Script" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "Build Type: $BuildType" -ForegroundColor Yellow
Write-Host "Generator: $Generator" -ForegroundColor Yellow
Write-Host "Architecture: $Architecture" -ForegroundColor Yellow
Write-Host "Build Static: $BuildStatic" -ForegroundColor Yellow
Write-Host "Build Shared: $BuildShared" -ForegroundColor Yellow
Write-Host "Output Dir: $OutputDir" -ForegroundColor Yellow
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

# Check CMake
$cmakePath = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $cmakePath) {
    Write-Host "Error: CMake not found. Please ensure CMake is installed and in PATH" -ForegroundColor Red
    exit 1
}

Write-Host "Found CMake: $($cmakePath.Source)" -ForegroundColor Green
Write-Host "CMake version:" -NoNewline
& cmake --version | Select-Object -First 1


# Build static library
if ($BuildStatic) {
    Write-Host ""
    Write-Host "========================================" -ForegroundColor Cyan
    Write-Host "Building Static Library" -ForegroundColor Cyan
    Write-Host "========================================" -ForegroundColor Cyan
    
    $StaticBuildDir = Join-Path $OutputDir "static"
    
    # Configure
    Write-Host "Configuring static library build..." -ForegroundColor Yellow
    $CmakeArgs = @("-B", $StaticBuildDir)
    if ($Generator) { $CmakeArgs += @("-G", $Generator, "-A", $Architecture) }
    # CR-082: 尊重 -BuildTests/-BuildExamples 开关（旧版硬编码 OFF 无视入参）
    $CmakeArgs += @(
        "-DCMAKE_BUILD_TYPE=$BuildType",
        "-DKAIRO_BUILD_SHARED=OFF",
        "-DKAIRO_BUILD_TESTS=$(if ($BuildTests) { 'ON' } else { 'OFF' })",
        "-DKAIRO_BUILD_EXAMPLES=$(if ($BuildExamples) { 'ON' } else { 'OFF' })",
        "-DCMAKE_INSTALL_PREFIX=$StaticBuildDir\install"
    )
    & cmake @CmakeArgs
    
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Error: CMake configuration failed" -ForegroundColor Red
        exit 1
    }
    
    # Build
    Write-Host "Building static library..." -ForegroundColor Yellow
    & cmake --build $StaticBuildDir --config $BuildType
    
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Error: Build failed" -ForegroundColor Red
        exit 1
    }
    
    # Install
    Write-Host "Installing static library..." -ForegroundColor Yellow
    & cmake --install $StaticBuildDir --config $BuildType
    
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Error: Installation failed" -ForegroundColor Red
        exit 1
    }
    
    Write-Host "Static library build completed!" -ForegroundColor Green
}

# Build shared library
if ($BuildShared) {
    Write-Host ""
    Write-Host "========================================" -ForegroundColor Cyan
    Write-Host "Building Shared Library" -ForegroundColor Cyan
    Write-Host "========================================" -ForegroundColor Cyan
    
    $SharedBuildDir = Join-Path $OutputDir "shared"
    
    # Configure
    Write-Host "Configuring shared library build..." -ForegroundColor Yellow
    $CmakeArgs = @("-B", $SharedBuildDir)
    if ($Generator) { $CmakeArgs += @("-G", $Generator, "-A", $Architecture) }
    # CR-082: 同静态库块——开关透传
    $CmakeArgs += @(
        "-DCMAKE_BUILD_TYPE=$BuildType",
        "-DKAIRO_BUILD_SHARED=ON",
        "-DKAIRO_BUILD_TESTS=$(if ($BuildTests) { 'ON' } else { 'OFF' })",
        "-DKAIRO_BUILD_EXAMPLES=$(if ($BuildExamples) { 'ON' } else { 'OFF' })",
        "-DCMAKE_INSTALL_PREFIX=$SharedBuildDir\install"
    )
    & cmake @CmakeArgs
    
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Error: CMake configuration failed" -ForegroundColor Red
        exit 1
    }
    
    # Build
    Write-Host "Building shared library..." -ForegroundColor Yellow
    & cmake --build $SharedBuildDir --config $BuildType
    
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Error: Build failed" -ForegroundColor Red
        exit 1
    }
    
    # Install
    Write-Host "Installing shared library..." -ForegroundColor Yellow
    & cmake --install $SharedBuildDir --config $BuildType
    
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Error: Installation failed" -ForegroundColor Red
        exit 1
    }
    
    Write-Host "Shared library build completed!" -ForegroundColor Green
}

Write-Host ""
Write-Host "========================================" -ForegroundColor Green
Write-Host "Build completed!" -ForegroundColor Green
Write-Host "========================================" -ForegroundColor Green
Write-Host ""
Write-Host "Build artifacts location:" -ForegroundColor Yellow
if ($BuildStatic) {
    Write-Host "  Static library: $OutputDir\static\install" -ForegroundColor Cyan
}
if ($BuildShared) {
    Write-Host "  Shared library: $OutputDir\shared\install" -ForegroundColor Cyan
}
