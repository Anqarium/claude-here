# ClaudeHere.dll ve ClaudeHereStub.exe'yi MSVC ile build/ klasörüne derler.
# Gerekenler: Visual Studio 2022 (C++ araçları) ve Windows 11 SDK.
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$out = Join-Path $root 'build'
New-Item -ItemType Directory -Force $out | Out-Null

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'MSVC (Visual Studio C++ araçları) bulunamadı.' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'

$flags = '/nologo /utf-8 /std:c++17 /O2 /MT /EHsc /W4 /DUNICODE /D_UNICODE'
$cmd = @(
  "`"$vcvars`" >nul",
  "cd /d `"$out`"",
  "cl $flags /LD `"$root\src\ClaudeHere.cpp`" /Fe:ClaudeHere.dll /link /DEF:`"$root\src\ClaudeHere.def`"",
  "cl $flags `"$root\src\Stub.cpp`" /Fe:ClaudeHereStub.exe /link /SUBSYSTEM:WINDOWS user32.lib"
) -join ' && '
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "Derleme başarısız (çıkış kodu $LASTEXITCODE)." }
Get-ChildItem $out -Include *.dll, *.exe -Recurse | Select-Object Name, Length
