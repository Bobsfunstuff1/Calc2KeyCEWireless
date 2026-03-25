param(
    [string]$OutputDir = "dist"
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$stageDir = Join-Path $repoRoot "$OutputDir\calculator"
$zipPath = Join-Path $repoRoot "$OutputDir\Calc2KeyCE-Calculator.zip"

$files = @(
    (Join-Path $repoRoot 'Calc2KeyCE.Calc\bin\Calc2Key.8xp'),
    (Join-Path $repoRoot 'README.md')
)

foreach ($file in $files) {
    if (-not (Test-Path $file)) {
        throw "Required calculator artifact not found: $file"
    }
}

New-Item -ItemType Directory -Force -Path $stageDir | Out-Null
Remove-Item -Recurse -Force (Join-Path $stageDir '*') -ErrorAction SilentlyContinue

Copy-Item (Join-Path $repoRoot 'Calc2KeyCE.Calc\bin\Calc2Key.8xp') $stageDir -Force
Copy-Item (Join-Path $repoRoot 'README.md') $stageDir -Force

if (Test-Path $zipPath) {
    Remove-Item $zipPath -Force
}

Compress-Archive -Path (Join-Path $stageDir '*') -DestinationPath $zipPath
Write-Host "Created $zipPath"
