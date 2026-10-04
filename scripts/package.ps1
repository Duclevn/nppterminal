param([ValidateSet('Debug','Release')][string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'version.ps1')
$currentVersion = Get-NppTerminalVersion -RootPath $projectRoot
$buildPath = Join-Path $projectRoot "out/x64/$Configuration"
$packageRoot = Join-Path $projectRoot "out/package/$Configuration"
$stagingRoot = Join-Path $packageRoot ("staging-" + [guid]::NewGuid().ToString('N'))
$packagePath = Join-Path $stagingRoot 'NppTerminal'
$dllPath = Join-Path $buildPath 'NppTerminal.dll'
$dllVersion = Get-NppTerminalVersionFromDll -Path $dllPath
if ($dllVersion.String -ne $currentVersion.String) {
    throw "The native plugin version $($dllVersion.String) does not match VERSION $($currentVersion.String). Build with scripts/build.ps1 first."
}
$brokerPath = Join-Path $buildPath 'NppTerminalBroker.exe'
$brokerVersion = Get-NppTerminalVersionFromDll -Path $brokerPath
if ($brokerVersion.String -ne $currentVersion.String) {
    throw "The session broker version $($brokerVersion.String) does not match VERSION $($currentVersion.String). Build with scripts/build.ps1 first."
}
if (!(Test-Path -LiteralPath (Join-Path $projectRoot 'web/vendor/xterm.js'))) { throw 'Build the web assets first.' }
$stagingCreated = $false
try {
    # A unique stage makes each archive independent of any prior package or
    # test artifacts while preserving NppTerminal as the archive root.
    New-Item -ItemType Directory -Path $stagingRoot -ErrorAction Stop | Out-Null
    $stagingCreated = $true
    New-Item -ItemType Directory -Path $packagePath,(Join-Path $packagePath 'web/vendor'),(Join-Path $packagePath 'licenses'),(Join-Path $packagePath 'docs') -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $buildPath 'NppTerminal.dll') -Destination $packagePath
    Copy-Item -LiteralPath $brokerPath -Destination $packagePath
    foreach ($name in @('index.html','terminal.js','terminal.css')) {
        Copy-Item -LiteralPath (Join-Path $projectRoot "web/$name") -Destination (Join-Path $packagePath 'web')
    }
    foreach ($name in @('xterm.js','xterm.css','addon-fit.js','xterm.LICENSE','addon-fit.LICENSE')) {
        Copy-Item -LiteralPath (Join-Path $projectRoot "web/vendor/$name") -Destination (Join-Path $packagePath 'web/vendor')
    }
    foreach ($name in @('README.md','LICENSE','THIRD_PARTY_NOTICES.md','dependencies.lock.json')) {
        Copy-Item -LiteralPath (Join-Path $projectRoot $name) -Destination $packagePath
    }
    Copy-Item -LiteralPath (Join-Path $projectRoot 'VERSION') -Destination $packagePath
    foreach ($name in @('audit-remediation-2026-10-04.md','compatibility.md','implementation-status.md','manual-tests.md','validation-2026-10-03-continuation.md','validation-2026-10-04.md','shutdown-ownership.md')) {
        Copy-Item -LiteralPath (Join-Path $projectRoot "docs/$name") -Destination (Join-Path $packagePath 'docs')
    }
    Copy-Item -LiteralPath (Join-Path $projectRoot 'third_party/notepadpp/LICENSE.txt') -Destination (Join-Path $packagePath 'licenses/notepadpp-template-GPL-2.0.txt')
    Copy-Item -LiteralPath (Join-Path $projectRoot 'third_party/notepadpp/Scintilla.LICENSE.txt') -Destination (Join-Path $packagePath 'licenses/Scintilla-LICENSE.txt')
    Copy-Item -LiteralPath (Join-Path $projectRoot 'third_party/json/LICENSE.MIT') -Destination (Join-Path $packagePath 'licenses/json-MIT.txt')
    $sdkPath = Join-Path $projectRoot 'packages/Microsoft.Web.WebView2.1.0.4258.31'
    Copy-Item -LiteralPath (Join-Path $sdkPath 'LICENSE.txt') -Destination (Join-Path $packagePath 'licenses/WebView2-LICENSE.txt')
    Copy-Item -LiteralPath (Join-Path $sdkPath 'NOTICE.txt') -Destination (Join-Path $packagePath 'licenses/WebView2-NOTICE.txt')
    $archivePath = Join-Path $projectRoot "out/NppTerminal-$($dllVersion.String)-$Configuration-x64.zip"
    Compress-Archive -LiteralPath $packagePath -DestinationPath $archivePath -Force
    Get-FileHash -LiteralPath $archivePath -Algorithm SHA256
}
finally {
    if ($stagingCreated) {
        $resolvedPackageRoot = (Resolve-Path -LiteralPath $packageRoot -ErrorAction SilentlyContinue).Path
        $resolvedStagingRoot = (Resolve-Path -LiteralPath $stagingRoot -ErrorAction SilentlyContinue).Path
        $packageRootPrefix = if ($resolvedPackageRoot) {
            $resolvedPackageRoot.TrimEnd('\') + '\'
        } else {
            $null
        }
        if ($resolvedStagingRoot -and $packageRootPrefix -and
            $resolvedStagingRoot.StartsWith($packageRootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
            Remove-Item -LiteralPath $resolvedStagingRoot -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}
