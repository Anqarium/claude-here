# Sağ tık paketlerini kaldırır ve %LOCALAPPDATA%\ClaudeHere klasörünü siler.
$ErrorActionPreference = 'Stop'
$base = Join-Path $env:LOCALAPPDATA 'ClaudeHere'

foreach ($name in 'ClaudeHere.Terminal', 'ClaudeHere.Desktop') {
  $pkg = Get-AppxPackage -Name $name
  if ($pkg) { Remove-AppxPackage -Package $pkg.PackageFullName; Write-Host "Kaldırıldı: $name" }
}

Get-Process dllhost -ErrorAction SilentlyContinue | Where-Object {
  try { $_.Modules.FileName -like "$base\*" } catch { $false }
} | Stop-Process -Force

if (Test-Path $base) { Remove-Item $base -Recurse -Force }
