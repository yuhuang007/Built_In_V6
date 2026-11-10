param([string]$EdgePath = 'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe')
$ErrorActionPreference = 'Stop'
$previewRoot = Join-Path $PSScriptRoot 'previews'
New-Item -ItemType Directory -Path $previewRoot -Force | Out-Null
$schemes = @('01-chengye','04-qinghe','05-lanqiao','06-moyu')
$states = @(
    @{ Name='menu'; Query='page=menu' },
    @{ Name='home'; Query='page=home' },
    @{ Name='transcript'; Query='page=transcript&scenario=reading' },
    @{ Name='keyboard'; Query='page=transcript&scenario=keyboard' },
    @{ Name='confirm'; Query='page=transcript&scenario=confirm' }
)
foreach ($state in $states) {
    $renderJobs = @()
    foreach ($scheme in $schemes) {
        $profilePath = Join-Path $env:TEMP ('p4-fusion-render-' + [guid]::NewGuid().ToString('N'))
        $outputPath = Join-Path $previewRoot ($scheme + '-' + $state.Name + '.png')
        $pagePath = Join-Path $PSScriptRoot ($scheme + '\index.html')
        $pageUrl = ([uri]$pagePath).AbsoluteUri + '?embed=1&' + $state.Query
        $arguments = @('--headless=new','--disable-gpu','--no-first-run',
            ('--user-data-dir="' + $profilePath + '"'),'--hide-scrollbars',
            '--window-size=600,1024','--timeout=10000',
            ('--screenshot="' + $outputPath + '"'),$pageUrl)
        $process = Start-Process -FilePath $EdgePath -ArgumentList $arguments -PassThru -WindowStyle Hidden
        $renderJobs += @{ Process=$process; Path=$outputPath; Name=($scheme + '-' + $state.Name) }
    }
    foreach ($job in $renderJobs) {
        if (-not $job.Process.WaitForExit(20000)) { throw ('Render did not exit: ' + $job.Name) }
        if (-not (Test-Path -LiteralPath $job.Path)) { throw ('Missing rendered image: ' + $job.Name) }
    }
    Write-Output ('Rendered four ' + $state.Name + ' pages')
}
Add-Type -AssemblyName System.Drawing
$titles = @('01 澄页 · 纸白钴蓝','04 青禾 · 鼠尾草绿','05 蓝桥 · 冰蓝靛蓝','06 墨语 · 极简黑白')
$boards = @(@{State='menu';Name='home-overview'},@{State='home';Name='chat-overview'},@{State='transcript';Name='transcript-overview'},@{State='keyboard';Name='keyboard-overview'})
foreach ($board in $boards) {
    $canvas = [Drawing.Bitmap]::new(2490,1120)
    $graphics = [Drawing.Graphics]::FromImage($canvas)
    $graphics.Clear([Drawing.Color]::FromArgb(238,239,235))
    $font = [Drawing.Font]::new('Microsoft YaHei UI',23,[Drawing.FontStyle]::Bold)
    for ($i=0; $i -lt 4; $i++) {
        $imagePath = Join-Path $previewRoot ($schemes[$i] + '-' + $board.State + '.png')
        $pageImage = [Drawing.Image]::FromFile($imagePath)
        $imageX = 15 + $i*620
        $graphics.DrawString($titles[$i],$font,[Drawing.Brushes]::Black,$imageX+8,16)
        $graphics.DrawImage($pageImage,[Drawing.Rectangle]::new($imageX,75,600,1024))
        $pageImage.Dispose()
    }
    $canvas.Save((Join-Path $previewRoot ($board.Name + '.png')),[Drawing.Imaging.ImageFormat]::Png)
    $graphics.Dispose(); $font.Dispose(); $canvas.Dispose()
}
