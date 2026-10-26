param(
    [string]$Config = 'Release',
    [switch]$NoGui
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$exe = Join-Path $root "build-release\Black_Recomp.exe"

if (-not (Test-Path $exe)) {
    Write-Warning "Executável $exe não encontrado. Compile com build-local.cmd antes de testar."
    exit 1
}

Write-Host "Iniciando Black Port..."
Set-Location $root
& $exe
