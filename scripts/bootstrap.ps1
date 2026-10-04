param()
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$dependencies = Get-Content -LiteralPath (Join-Path $projectRoot 'dependencies.lock.json') -Raw | ConvertFrom-Json
$cachePath = Join-Path $projectRoot '.cache'
$packageRoot = Join-Path $projectRoot 'packages'
New-Item -ItemType Directory -Path $cachePath,$packageRoot -Force | Out-Null
$archivePath = Join-Path $cachePath 'webview2.zip'
if (!(Test-Path -LiteralPath $archivePath)) {
    Invoke-WebRequest -Uri $dependencies.webview2.url -OutFile $archivePath
}
if ((Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash -ne $dependencies.webview2.sha256) {
    throw 'WebView2 package checksum does not match dependencies.lock.json.'
}
$sdkPath = Join-Path $packageRoot ('Microsoft.Web.WebView2.' + $dependencies.webview2.version)
Expand-Archive -LiteralPath $archivePath -DestinationPath $sdkPath -Force
& npm --prefix (Join-Path $projectRoot 'web') ci --ignore-scripts
if ($LASTEXITCODE -ne 0) { throw 'npm ci failed.' }
& npm --prefix (Join-Path $projectRoot 'web') run build
if ($LASTEXITCODE -ne 0) { throw 'Web asset build failed.' }

