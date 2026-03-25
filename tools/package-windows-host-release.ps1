param(
    [string]$Configuration = "Release",
    [string]$OutputDir = "dist"
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $repoRoot "Calc2KeyCE.BridgeHostWin\build\$Configuration"
$stageDir = Join-Path $repoRoot "$OutputDir\windows-host"
$zipPath = Join-Path $repoRoot "$OutputDir\Calc2KeyCE-WindowsHost-$Configuration.zip"

if (-not (Test-Path $buildDir)) {
    throw "Build output not found: $buildDir"
}

New-Item -ItemType Directory -Force -Path $stageDir | Out-Null
Get-ChildItem $stageDir -Force -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue

Get-ChildItem $buildDir -File | Where-Object { $_.Extension -in @('.exe', '.dll') } | ForEach-Object {
    Copy-Item $_.FullName $stageDir -Force
}
Copy-Item (Join-Path $repoRoot 'Launch-Calc2KeyBridge.ps1') $stageDir -Force
Copy-Item (Join-Path $repoRoot 'Launch-Calc2KeyBridge.bat') $stageDir -Force
Copy-Item (Join-Path $repoRoot 'README.md') $stageDir -Force

if (Test-Path $zipPath) {
    Remove-Item $zipPath -Force
}

Compress-Archive -Path (Join-Path $stageDir '*') -DestinationPath $zipPath
Write-Host "Created $zipPath"
