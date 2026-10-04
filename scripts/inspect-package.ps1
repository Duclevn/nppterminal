param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$projectRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'version.ps1')
$currentVersion = Get-NppTerminalVersion -RootPath $projectRoot
$archivePath = Join-Path $projectRoot "out/NppTerminal-$($currentVersion.String)-$Configuration-x64.zip"
$buildPath = Join-Path $projectRoot "out/x64/$Configuration"

function ConvertTo-UpperHex {
    param([Parameter(Mandatory)][byte[]]$Bytes)

    (-join ($Bytes | ForEach-Object { $_.ToString('X2') }))
}

function Get-StreamSha256 {
    param([Parameter(Mandatory)][IO.Stream]$Stream)

    $sha256 = [Security.Cryptography.SHA256]::Create()
    try {
        ConvertTo-UpperHex -Bytes $sha256.ComputeHash($Stream)
    }
    finally {
        $sha256.Dispose()
    }
}

function Get-ZipEntryBytes {
    param([Parameter(Mandatory)][IO.Compression.ZipArchiveEntry]$Entry)

    $entryStream = $null
    $memoryStream = $null
    try {
        $entryStream = $Entry.Open()
        $memoryStream = New-Object IO.MemoryStream
        $entryStream.CopyTo($memoryStream)
        return ,$memoryStream.ToArray()
    }
    finally {
        if ($memoryStream) { $memoryStream.Dispose() }
        if ($entryStream) { $entryStream.Dispose() }
    }
}

function Get-ZipEntrySha256 {
    param([Parameter(Mandatory)][IO.Compression.ZipArchiveEntry]$Entry)

    $entryStream = $null
    try {
        $entryStream = $Entry.Open()
        Get-StreamSha256 -Stream $entryStream
    }
    finally {
        if ($entryStream) { $entryStream.Dispose() }
    }
}

function Get-SourceSha256 {
    param([Parameter(Mandatory)][string]$Path)

    (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToUpperInvariant()
}

if (!(Test-Path -LiteralPath $archivePath -PathType Leaf)) {
    throw "The package archive was not found at '$archivePath'. Run scripts/package.ps1 after a matching native build."
}

$fileSpecs = @(
    [pscustomobject]@{ Zip = 'NppTerminal/NppTerminal.dll'; SourceRelative = "out/x64/$Configuration/NppTerminal.dll"; Kind = 'NativeBinary' },
    [pscustomobject]@{ Zip = 'NppTerminal/NppTerminalBroker.exe'; SourceRelative = "out/x64/$Configuration/NppTerminalBroker.exe"; Kind = 'NativeBinary' },
    [pscustomobject]@{ Zip = 'NppTerminal/web/index.html'; SourceRelative = 'web/index.html'; Kind = 'WebAsset' },
    [pscustomobject]@{ Zip = 'NppTerminal/web/terminal.js'; SourceRelative = 'web/terminal.js'; Kind = 'WebAsset' },
    [pscustomobject]@{ Zip = 'NppTerminal/web/terminal.css'; SourceRelative = 'web/terminal.css'; Kind = 'WebAsset' },
    [pscustomobject]@{ Zip = 'NppTerminal/web/vendor/xterm.js'; SourceRelative = 'web/vendor/xterm.js'; Kind = 'WebAsset' },
    [pscustomobject]@{ Zip = 'NppTerminal/web/vendor/xterm.css'; SourceRelative = 'web/vendor/xterm.css'; Kind = 'WebAsset' },
    [pscustomobject]@{ Zip = 'NppTerminal/web/vendor/addon-fit.js'; SourceRelative = 'web/vendor/addon-fit.js'; Kind = 'WebAsset' },
    [pscustomobject]@{ Zip = 'NppTerminal/web/vendor/xterm.LICENSE'; SourceRelative = 'web/vendor/xterm.LICENSE'; Kind = 'WebAssetLicense' },
    [pscustomobject]@{ Zip = 'NppTerminal/web/vendor/addon-fit.LICENSE'; SourceRelative = 'web/vendor/addon-fit.LICENSE'; Kind = 'WebAssetLicense' },
    [pscustomobject]@{ Zip = 'NppTerminal/README.md'; SourceRelative = 'README.md'; Kind = 'DistributedDocument' },
    [pscustomobject]@{ Zip = 'NppTerminal/LICENSE'; SourceRelative = 'LICENSE'; Kind = 'DistributedDocument' },
    [pscustomobject]@{ Zip = 'NppTerminal/THIRD_PARTY_NOTICES.md'; SourceRelative = 'THIRD_PARTY_NOTICES.md'; Kind = 'DistributedDocument' },
    [pscustomobject]@{ Zip = 'NppTerminal/dependencies.lock.json'; SourceRelative = 'dependencies.lock.json'; Kind = 'DistributedDocument' },
    [pscustomobject]@{ Zip = 'NppTerminal/VERSION'; SourceRelative = 'VERSION'; Kind = 'VersionText' },
    [pscustomobject]@{ Zip = 'NppTerminal/docs/compatibility.md'; SourceRelative = 'docs/compatibility.md'; Kind = 'DistributedDocument' },
    [pscustomobject]@{ Zip = 'NppTerminal/docs/implementation-status.md'; SourceRelative = 'docs/implementation-status.md'; Kind = 'DistributedDocument' },
    [pscustomobject]@{ Zip = 'NppTerminal/docs/manual-tests.md'; SourceRelative = 'docs/manual-tests.md'; Kind = 'DistributedDocument' },
    [pscustomobject]@{ Zip = 'NppTerminal/docs/validation-2026-10-03-continuation.md'; SourceRelative = 'docs/validation-2026-10-03-continuation.md'; Kind = 'DistributedDocument' },
    [pscustomobject]@{ Zip = 'NppTerminal/docs/validation-2026-10-04.md'; SourceRelative = 'docs/validation-2026-10-04.md'; Kind = 'DistributedDocument' },
    [pscustomobject]@{ Zip = 'NppTerminal/docs/shutdown-ownership.md'; SourceRelative = 'docs/shutdown-ownership.md'; Kind = 'DistributedDocument' },
    [pscustomobject]@{ Zip = 'NppTerminal/licenses/notepadpp-template-GPL-2.0.txt'; SourceRelative = 'third_party/notepadpp/LICENSE.txt'; Kind = 'License' },
    [pscustomobject]@{ Zip = 'NppTerminal/licenses/Scintilla-LICENSE.txt'; SourceRelative = 'third_party/notepadpp/Scintilla.LICENSE.txt'; Kind = 'License' },
    [pscustomobject]@{ Zip = 'NppTerminal/licenses/json-MIT.txt'; SourceRelative = 'third_party/json/LICENSE.MIT'; Kind = 'License' },
    [pscustomobject]@{ Zip = 'NppTerminal/licenses/WebView2-LICENSE.txt'; SourceRelative = 'packages/Microsoft.Web.WebView2.1.0.4258.31/LICENSE.txt'; Kind = 'License' },
    [pscustomobject]@{ Zip = 'NppTerminal/licenses/WebView2-NOTICE.txt'; SourceRelative = 'packages/Microsoft.Web.WebView2.1.0.4258.31/NOTICE.txt'; Kind = 'License' }
)

$expectedNames = @($fileSpecs | ForEach-Object { $_.Zip })
if ($expectedNames.Count -ne 26) {
    throw "The package inspector manifest must contain exactly 26 files; found $($expectedNames.Count)."
}

$archive = $null
try {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $resolvedArchivePath = (Resolve-Path -LiteralPath $archivePath).Path
    $archive = [IO.Compression.ZipFile]::OpenRead($resolvedArchivePath)
    $entries = @($archive.Entries)

    $duplicateGroups = @($entries | Group-Object -Property FullName | Where-Object Count -gt 1)
    if ($duplicateGroups.Count -gt 0) {
        $duplicateNames = $duplicateGroups | ForEach-Object { $_.Name }
        throw "The package contains duplicate ZIP entries: $($duplicateNames -join ', ')."
    }

    $directoryEntries = @($entries | Where-Object { $_.FullName.EndsWith('/') })
    if ($directoryEntries.Count -gt 0) {
        $directoryNames = $directoryEntries | ForEach-Object { $_.FullName }
        throw "The package contains unexpected directory entries: $($directoryNames -join ', ')."
    }

    $actualNames = @($entries | ForEach-Object { $_.FullName })
    $missingNames = @($expectedNames | Where-Object { $actualNames -notcontains $_ })
    $unexpectedNames = @($actualNames | Where-Object { $expectedNames -notcontains $_ })
    if ($missingNames.Count -gt 0 -or $unexpectedNames.Count -gt 0 -or $actualNames.Count -ne $expectedNames.Count) {
        $missingText = if ($missingNames.Count -gt 0) { $missingNames -join ', ' } else { '(none)' }
        $unexpectedText = if ($unexpectedNames.Count -gt 0) { $unexpectedNames -join ', ' } else { '(none)' }
        throw "The package must contain exactly the 26-file allowlist. Missing: $missingText. Unexpected (including test/runtime/node/shell artifacts): $unexpectedText. Entry count: $($actualNames.Count)."
    }

    $dllPath = Join-Path $buildPath 'NppTerminal.dll'
    $brokerPath = Join-Path $buildPath 'NppTerminalBroker.exe'
    $dllVersion = Get-NppTerminalVersionFromDll -Path $dllPath
    $brokerVersion = Get-NppTerminalVersionFromDll -Path $brokerPath
    if ($dllVersion.String -ne $currentVersion.String) {
        throw "The current native plugin build version $($dllVersion.String) does not match VERSION $($currentVersion.String)."
    }
    if ($brokerVersion.String -ne $currentVersion.String) {
        throw "The current session broker build version $($brokerVersion.String) does not match VERSION $($currentVersion.String)."
    }

    $contents = @()
    $versionTextMatch = $false
    foreach ($spec in $fileSpecs) {
        $entry = @($entries | Where-Object { $_.FullName -ceq $spec.Zip })[0]
        if (!$entry) {
            throw "The expected ZIP entry '$($spec.Zip)' was not found."
        }

        $sourcePath = Join-Path $projectRoot $spec.SourceRelative
        if (!(Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
            throw "The package source file was not found at '$sourcePath'."
        }

        $sourceFile = Get-Item -LiteralPath $sourcePath -ErrorAction Stop
        if ($entry.Length -ne $sourceFile.Length) {
            throw "The ZIP entry '$($spec.Zip)' length $($entry.Length) does not match source '$($spec.SourceRelative)' length $($sourceFile.Length)."
        }

        $archiveHash = Get-ZipEntrySha256 -Entry $entry
        $sourceHash = Get-SourceSha256 -Path $sourcePath
        if ($archiveHash -ne $sourceHash) {
            throw "The ZIP entry '$($spec.Zip)' does not match source '$($spec.SourceRelative)'. ZIP SHA256 $archiveHash; source SHA256 $sourceHash."
        }

        if ($spec.Kind -eq 'VersionText') {
            $archiveVersionBytes = Get-ZipEntryBytes -Entry $entry
            $versionTextMatch = $true
            $versionText = [Text.Encoding]::UTF8.GetString($archiveVersionBytes)
            if ($versionText -notmatch ('^' + [regex]::Escape($currentVersion.String) + "\r?\n$")) {
                throw "The ZIP VERSION text does not contain exactly VERSION $($currentVersion.String) followed by one newline."
            }
        }

        $contents += [pscustomobject]@{
            Path = $spec.Zip
            Kind = $spec.Kind
            Bytes = $entry.Length
            Sha256 = $archiveHash
            Source = $spec.SourceRelative
        }
    }

    $archiveSize = (Get-Item -LiteralPath $resolvedArchivePath -ErrorAction Stop).Length
    $archiveHash = Get-SourceSha256 -Path $resolvedArchivePath
    $report = [pscustomobject]@{
        InspectionStatus = 'Passed'
        CapturedUtc = [DateTime]::UtcNow.ToString('o')
        Version = $currentVersion.String
        Configuration = $Configuration
        ArchivePath = $resolvedArchivePath
        ArchiveBytes = $archiveSize
        ArchiveSha256 = $archiveHash
        FileCount = $contents.Count
        ExpectedFileCount = 26
        VersionTextMatch = $versionTextMatch
        NativeBinaries = @(
            [pscustomobject]@{ Path = 'NppTerminal/NppTerminal.dll'; EmbeddedVersion = $dllVersion.String; SourceSha256 = (Get-SourceSha256 -Path $dllPath); ZipSha256 = ($contents | Where-Object Path -ceq 'NppTerminal/NppTerminal.dll').Sha256 },
            [pscustomobject]@{ Path = 'NppTerminal/NppTerminalBroker.exe'; EmbeddedVersion = $brokerVersion.String; SourceSha256 = (Get-SourceSha256 -Path $brokerPath); ZipSha256 = ($contents | Where-Object Path -ceq 'NppTerminal/NppTerminalBroker.exe').Sha256 }
        )
        Contents = $contents
    }
    $json = $report | ConvertTo-Json -Depth 6
    if ($OutputPath) {
        $resolvedOutputPath = [IO.Path]::GetFullPath($OutputPath)
        $outputDirectory = Split-Path -Parent $resolvedOutputPath
        if (!(Test-Path -LiteralPath $outputDirectory -PathType Container)) {
            throw "The JSON report directory does not exist: '$outputDirectory'."
        }
        $utf8 = New-Object System.Text.UTF8Encoding($false)
        [IO.File]::WriteAllText($resolvedOutputPath, $json, $utf8)
    }
    Write-Output $json
}
finally {
    if ($archive) { $archive.Dispose() }
}
