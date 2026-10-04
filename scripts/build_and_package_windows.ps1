# Windows 完整构建和打包脚本
# 一键构建静态库、动态库并打包成发行版本

param(
    # CR-085: 留空时由 package_windows.ps1 从根 CMakeLists.txt 解析
    [string]$Version = "",
    [string]$BuildType = "Release",
    [string]$Generator = "",
    [string]$Architecture = "x64",
    [switch]$BuildStatic = $true,
    [switch]$BuildShared = $true,
    [switch]$BuildTests = $false,
    [switch]$BuildExamples = $false,
    [string]$BuildDir = "build_windows",
    [string]$OutputDir = "dist",
    # 传递给 package_windows.ps1 的打包架构标识；留空时使用环境变量回退。
    [string]$Arch = ""
)

$ErrorActionPreference = "Stop"

Write-Host "========================================" -ForegroundColor Magenta
Write-Host "Kairo Windows Build and Package" -ForegroundColor Magenta
Write-Host "========================================" -ForegroundColor Magenta
Write-Host ""

# Get script directory
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

# Step 1: Build
Write-Host "Step 1/2: Building libraries..." -ForegroundColor Cyan
& "$ScriptDir\build_windows.ps1" `
    -BuildType $BuildType `
    -Generator $Generator `
    -Architecture $Architecture `
    -BuildStatic:$BuildStatic `
    -BuildShared:$BuildShared `
    -BuildTests:$BuildTests `
    -BuildExamples:$BuildExamples `
    -OutputDir $BuildDir

if ($LASTEXITCODE -ne 0) {
    Write-Host "Error: Build failed" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "Step 2/2: Packaging release..." -ForegroundColor Cyan
& "$ScriptDir\package_windows.ps1" `
    -Version $Version `
    -BuildDir $BuildDir `
    -OutputDir $OutputDir `
    -Arch $Arch `
    -IncludeStatic:$BuildStatic `
    -IncludeShared:$BuildShared

if ($LASTEXITCODE -ne 0) {
    Write-Host "Error: Packaging failed" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "========================================" -ForegroundColor Green
Write-Host "All done!" -ForegroundColor Green
Write-Host "========================================" -ForegroundColor Green
