param(
    [ValidateSet('build', 'flash', 'monitor')]
    [string]$Action = 'build',
    [string]$Port = 'COM20',
    [ValidateSet('display', 'camera', 'sign')]
    [string]$Profile = 'display'
)

$projectRoot = Split-Path -Parent $PSScriptRoot
$idfPath = 'E:\XUEXI\ESP_IDF_5.5.4\v5.5.4\esp-idf'
$python = Join-Path $projectRoot '.idf-build-env\Scripts\python.exe'
if ($Profile -eq 'camera') {
    $firmwareRoot = Join-Path $projectRoot 'firmware\p4_camera_probe'
    $buildDir = 'build_camera'
    $sdkconfig = Join-Path $firmwareRoot 'sdkconfig.camera'
} elseif ($Profile -eq 'sign') {
    $firmwareRoot = Join-Path $projectRoot 'firmware\p4_main'
    $buildDir = 'build_sign'
    $sdkconfig = Join-Path $firmwareRoot 'sdkconfig.sign'
} else {
    $firmwareRoot = Join-Path $projectRoot 'firmware\p4_main'
    $buildDir = 'build_display'
    $sdkconfig = Join-Path $firmwareRoot 'sdkconfig.display'
}
$toolPaths = @(
    'C:\Espressif\tools\cmake\3.30.2\bin',
    'C:\Espressif\tools\ninja\1.12.1',
    'C:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin',
    'C:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin'
)

if (-not (Test-Path -LiteralPath $idfPath)) { throw "ESP-IDF v5.5.4 not found: $idfPath" }
if (-not (Test-Path -LiteralPath $python)) { throw "Project build Python not found: $python" }

$env:IDF_PATH = $idfPath
# ESP-Hosted uses this variable to select its IDF-major/minor Kconfig template.
$env:ESP_IDF_VERSION = '5.5'
$env:IDF_TOOLS_PATH = 'C:\Espressif\tools'
$env:IDF_PYTHON_ENV_PATH = Join-Path $projectRoot '.idf-build-env'
$env:ESP_ROM_ELF_DIR = 'C:\Espressif\tools\esp-rom-elfs\20241011'
$env:PATH = ($toolPaths -join ';') + ';' + $env:PATH

Set-Location $firmwareRoot
$idfArgs = @('-B', $buildDir, '-D', "SDKCONFIG=$sdkconfig", '-D', 'IDF_TARGET=esp32p4')
if ($Action -eq 'flash' -or $Action -eq 'monitor') { $idfArgs += @('-p', $Port) }
$idfArgs += $Action

& $python "$idfPath\tools\idf.py" @idfArgs
exit $LASTEXITCODE
