# Derlenen DLL'i %LOCALAPPDATA%\ClaudeHere altına kopyalar ve her menü komutunu
# ayrı bir sparse paket olarak kaydeder. Developer Mode açık olmalı (imzasız kayıt).
# Tekrar çalıştırmak güncellemedir: eski paketler kaldırılıp yeniden kaydedilir.
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$build = Join-Path $root 'build'
$base = Join-Path $env:LOCALAPPDATA 'ClaudeHere'
$version = '1.0.0.0'

if (-not (Test-Path (Join-Path $build 'ClaudeHere.dll'))) { & (Join-Path $root 'build.ps1') }

Add-Type -AssemblyName System.Drawing

function Find-DesktopExe {
  $pkg = Get-AppxPackage -Name Claude | Where-Object PackageFamilyName -eq 'Claude_pzs8sxrjxfjjc' | Select-Object -First 1
  if ($pkg) { $exe = Join-Path $pkg.InstallLocation 'app\Claude.exe'; if (Test-Path $exe) { return $exe } }
  return $null
}

function Find-ClaudeCli {
  $p = Join-Path $env:USERPROFILE '.local\bin\claude.exe'
  if (Test-Path $p) { return $p }
  $c = Get-Command claude.exe -ErrorAction SilentlyContinue
  if ($c) { return $c.Source }
  return $null
}

# Paket logoları (App extensions sayfasında görünür) exe'nin ikonundan üretilir.
function Write-Logos([string]$exe, [string]$assets) {
  New-Item -ItemType Directory -Force $assets | Out-Null
  $icon = try { [System.Drawing.Icon]::ExtractIcon($exe, 0, 256) } catch { $null }
  if (-not $icon) { $icon = [System.Drawing.Icon]::ExtractAssociatedIcon($exe) }
  $src = $icon.ToBitmap()
  foreach ($logo in @{ 'StoreLogo.png' = 50; 'Square44x44Logo.png' = 44; 'Square150x150Logo.png' = 150 }.GetEnumerator()) {
    $bmp = New-Object System.Drawing.Bitmap $logo.Value, $logo.Value
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.DrawImage($src, 0, 0, $logo.Value, $logo.Value)
    $g.Dispose()
    $bmp.Save((Join-Path $assets $logo.Key), [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
  }
  $src.Dispose(); $icon.Dispose()
}

# Paket adları (App extensions sayfası) kurulum anındaki Windows arayüz diline göre;
# menü yazılarını ise DLL her açılışta kendisi seçer.
$tr = (Get-UICulture).TwoLetterISOLanguageName -eq 'tr'
$commands = @(
  @{ Name = 'ClaudeHere.Terminal'; Folder = 'Terminal'; Verb = 'ClaudeCode'
     Display = $(if ($tr) { 'Claude Code (sağ tık)' } else { 'Claude Code (context menu)' })
     Clsid = '0f55b8ab-4947-4eab-b1f7-4e8c0db27ac9'; Exe = Find-ClaudeCli },
  @{ Name = 'ClaudeHere.Desktop'; Folder = 'Desktop'; Verb = 'ClaudeDesktop'
     Display = $(if ($tr) { 'Claude Masaüstü (sağ tık)' } else { 'Claude Desktop (context menu)' })
     Clsid = '59847620-2a77-4b45-ad83-ef512e443b3b'; Exe = Find-DesktopExe }
)

# DLL'i tutan surrogate (dllhost) süreçlerini kapat; yoksa kopyalama kilitli dosyaya çarpar.
Get-Process dllhost -ErrorAction SilentlyContinue | Where-Object {
  try { $_.Modules.FileName -like "$base\*" } catch { $false }
} | Stop-Process -Force

$template = Get-Content (Join-Path $root 'package\AppxManifest.template.xml') -Raw -Encoding utf8

foreach ($c in $commands) {
  $old = Get-AppxPackage -Name $c.Name
  if ($old) { Remove-AppxPackage -Package $old.PackageFullName }

  if (-not $c.Exe) {
    Write-Warning "$($c.Display): hedef uygulama bulunamadı, atlanıyor."
    continue
  }

  $dir = Join-Path $base $c.Folder
  $pkg = Join-Path $dir 'package'
  New-Item -ItemType Directory -Force $pkg | Out-Null
  Copy-Item (Join-Path $build 'ClaudeHere.dll'), (Join-Path $build 'ClaudeHereStub.exe') $dir -Force
  Write-Logos $c.Exe (Join-Path $pkg 'Assets')

  $manifest = $template.Replace('{{NAME}}', $c.Name).Replace('{{DISPLAY}}', $c.Display).
    Replace('{{VERB}}', $c.Verb).Replace('{{CLSID}}', $c.Clsid).Replace('{{VERSION}}', $version)
  $manifestPath = Join-Path $pkg 'AppxManifest.xml'
  [IO.File]::WriteAllText($manifestPath, $manifest, (New-Object System.Text.UTF8Encoding $false))

  Add-AppxPackage -Register $manifestPath -ExternalLocation $dir
  Write-Host "Kaydedildi: $($c.Name)"
}
