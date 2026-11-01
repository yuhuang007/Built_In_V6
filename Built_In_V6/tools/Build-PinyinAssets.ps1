param([Parameter(Mandatory=$true)][string]$Zig)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$vendor = Join-Path $root 'firmware/p4_main/components/p4_pinyin/vendor'
$out = Join-Path $root 'logs/pinyin-host'
New-Item -ItemType Directory -Path $out -Force | Out-Null
$sources = @(Get-ChildItem (Join-Path $vendor 'src') -Filter '*.cpp' | ForEach-Object FullName)
$flags = @('c++', '-target', 'x86-windows-gnu', '-std=c++11', '-O2',
    '-DP4_PINYIN_READ_ONLY=1', '-I'+(Join-Path $vendor 'include'), '-I'+(Join-Path $PSScriptRoot 'pinyin_host'))
& $Zig @flags '-DP4_PINYIN_BUILD_MODEL=1' (Join-Path $PSScriptRoot 'pinyin_dictbuilder.cpp') @sources '-o' (Join-Path $out 'dictbuilder.exe')
if ($LASTEXITCODE) { throw '32-bit dictionary builder failed' }
& (Join-Path $out 'dictbuilder.exe') (Join-Path $root 'assets/pinyin/source/rawdict_utf16_65105_freq.txt') (Join-Path $root 'assets/pinyin/source/valid_utf16.txt') (Join-Path $root 'assets/pinyin/P4PYV1.DAT')
if ($LASTEXITCODE) { throw 'Dictionary generation failed' }
& $Zig @flags ('-I'+(Split-Path -Parent $vendor)) (Join-Path $PSScriptRoot 'pinyin_engine_probe.cpp') (Join-Path (Split-Path -Parent $vendor) 'p4_pinyin.cpp') @sources '-o' (Join-Path $out 'probe.exe')
if ($LASTEXITCODE) { throw '32-bit runtime probe build failed' }
& (Join-Path $out 'probe.exe') (Join-Path $root 'assets/pinyin/P4PYV1.DAT')
if ($LASTEXITCODE) { throw '32-bit runtime probe failed' }
Get-FileHash (Join-Path $root 'assets/pinyin/P4PYV1.DAT') -Algorithm SHA256 | Select-Object Hash
& (Join-Path $root '.idf-build-env/Scripts/python.exe') (Join-Path $PSScriptRoot 'pinyin_assets.py')
if ($LASTEXITCODE) { throw 'Metadata generation failed' }
