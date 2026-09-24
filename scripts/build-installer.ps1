$ErrorActionPreference = "Continue"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

cmake -S . -B build -G "Visual Studio 17 2022" -A x64 *> $null
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
cmake --build build --config Release -- /m /nologo /v:m
if ($LASTEXITCODE -ne 0) { throw "build failed" }

$ver = (Select-String -Path CMakeLists.txt -Pattern 'project\(aes67bridge VERSION ([0-9.]+)').Matches[0].Groups[1].Value

$iscc = @(
  "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
  "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
  "$env:ProgramFiles\Inno Setup 6\ISCC.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { throw "Inno Setup 6 (ISCC.exe) not found" }

& $iscc "/DAppVersion=$ver" installer\AES67Bridge.iss
if ($LASTEXITCODE -ne 0) { throw "ISCC failed" }
Get-Item "dist\AES67Bridge-Setup-$ver.exe"
