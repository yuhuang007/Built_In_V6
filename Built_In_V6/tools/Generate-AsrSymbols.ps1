# Local converter and its npm cache stay on E:, never install project tools on C:.
# Install if absent:
# npm --cache .font-tools/npm-cache --prefix .font-tools install --no-audit --no-fund --ignore-scripts lv_font_conv@1.5.3
$projectRoot = Split-Path -Parent $PSScriptRoot
$converter = Join-Path $projectRoot '.font-tools/node_modules/lv_font_conv/lv_font_conv.js'
$source = Join-Path $projectRoot 'firmware/p4_main/managed_components/lvgl__lvgl/scripts/generators/built_in_font/SourceHanSansSC-Normal.otf'
$output = Join-Path $projectRoot 'firmware/p4_main/main/font_asr_symbols.c'
if (!(Test-Path -LiteralPath $converter)) { throw 'Install the E:-local lv_font_conv@1.5.3 first.' }
# Codepoints avoid PowerShell/native-command quoting of typographic quote glyphs.
$codepoints = @(
    0x00B7,0x2013,0x2014,0x2018,0x2019,0x201C,0x201D,0x2026,
    0x3001,0x3002,0x3008,0x3009,0x300A,0x300B,0x300C,0x300D,
    0x300E,0x300F,0x3010,0x3011,0x3014,0x3015,0xFE4F,
    0xFF01,0xFF02,0xFF03,0xFF04,0xFF05,0xFF06,0xFF07,0xFF08,0xFF09,
    0xFF0A,0xFF0B,0xFF0C,0xFF0D,0xFF0E,0xFF0F,0xFF1A,0xFF1B,
    0xFF1C,0xFF1D,0xFF1E,0xFF1F,0xFF20,0xFF3B,0xFF3C,0xFF3D,
    0xFF3E,0xFF3F,0xFF40,0xFF5B,0xFF5C,0xFF5D,0xFF5E
)
$symbols = -join ($codepoints | ForEach-Object { [char]$_ })
& node $converter --no-compress --no-prefilter --size 14 --bpp 4 --format lvgl `
    --font $source -r 0x20-0x7e --symbols $symbols --lv-include lvgl.h `
    --lv-font-name lv_font_asr_symbols -o $output
if ($LASTEXITCODE -ne 0) { throw 'ASR symbol font generation failed.' }
