param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$projectRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
. (Join-Path $PSScriptRoot 'version.ps1')
$currentVersion = Get-NppTerminalVersion -RootPath $projectRoot
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

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

function Get-FileSha256 {
    param([Parameter(Mandatory)][string]$Path)

    $stream = $null
    try {
        $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
        Get-StreamSha256 -Stream $stream
    }
    finally {
        if ($stream) { $stream.Dispose() }
    }
}

function Get-ReparseTag {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][bool]$Directory
    )

    if (-not ([System.Management.Automation.PSTypeName]'NppTerminal.SourcePackageNative').Type) {
        Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace NppTerminal {
    public static class SourcePackageNative {
        private const uint FileReadAttributes = 0x00000080;
        private const uint FileShareRead = 0x00000001;
        private const uint FileShareWrite = 0x00000002;
        private const uint FileShareDelete = 0x00000004;
        private const uint OpenExisting = 3;
        private const uint FileFlagOpenReparsePoint = 0x00200000;
        private const uint FileFlagBackupSemantics = 0x02000000;
        private const uint FsctlGetReparsePoint = 0x000900A8;

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern SafeFileHandle CreateFile(
            string fileName,
            uint desiredAccess,
            uint shareMode,
            IntPtr securityAttributes,
            uint creationDisposition,
            uint flagsAndAttributes,
            IntPtr templateFile);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool DeviceIoControl(
            SafeFileHandle device,
            uint controlCode,
            IntPtr inBuffer,
            uint inBufferSize,
            [Out] byte[] outBuffer,
            uint outBufferSize,
            out uint bytesReturned,
            IntPtr overlapped);

        public static uint GetReparseTag(string path, bool directory) {
            uint flags = FileFlagOpenReparsePoint;
            if (directory) flags |= FileFlagBackupSemantics;
            using (SafeFileHandle handle = CreateFile(
                path,
                FileReadAttributes,
                FileShareRead | FileShareWrite | FileShareDelete,
                IntPtr.Zero,
                OpenExisting,
                flags,
                IntPtr.Zero)) {
                if (handle == null || handle.IsInvalid) {
                    throw new Win32Exception(Marshal.GetLastWin32Error(), "Cannot open reparse point.");
                }

                byte[] buffer = new byte[16 * 1024];
                uint bytesReturned;
                if (!DeviceIoControl(handle, FsctlGetReparsePoint, IntPtr.Zero, 0, buffer,
                    (uint)buffer.Length, out bytesReturned, IntPtr.Zero) || bytesReturned < 4) {
                    throw new Win32Exception(Marshal.GetLastWin32Error(), "Cannot read reparse tag.");
                }
                return BitConverter.ToUInt32(buffer, 0);
            }
        }
    }
}
'@
    }

    [uint32][NppTerminal.SourcePackageNative]::GetReparseTag($Path, $Directory)
}

function Test-CloudReparseTag {
    param([Parameter(Mandatory)][uint32]$Tag)

    # Microsoft Cloud through Cloud_F tags are the OneDrive placeholder family
    # used by this workspace. Other reparse tags, including name-surrogate
    # junctions and symbolic links, are deliberately rejected.
    $tagValue = [uint64]$Tag
    # Decimal constants avoid Windows PowerShell treating the high-bit hex
    # literals as signed Int32 values before the cast to UInt64.
    $firstCloudTag = [uint64]2415919130
    $lastCloudTag = [uint64]2415980570
    if ($tagValue -lt $firstCloudTag -or $tagValue -gt $lastCloudTag) {
        return $false
    }
    (($tagValue - $firstCloudTag) % [uint64]0x1000) -eq 0
}

function Get-CheckedAttributes {
    param([Parameter(Mandatory)][string]$Path)

    $attributes = [IO.File]::GetAttributes($Path)
    if (($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        $isDirectory = ($attributes -band [IO.FileAttributes]::Directory) -ne 0
        $tag = Get-ReparseTag -Path $Path -Directory $isDirectory
        if (-not (Test-CloudReparseTag -Tag $tag)) {
            throw "Unsupported reparse tag 0x$($tag.ToString('X8')) at '$Path'. Only Microsoft Cloud through Cloud_F placeholders are allowed."
        }
    }
    $attributes
}

function Get-PathPrefix {
    param([Parameter(Mandatory)][string]$Path)

    $fullPath = [IO.Path]::GetFullPath($Path)
    if ($fullPath.EndsWith([IO.Path]::DirectorySeparatorChar)) {
        $fullPath
    }
    else {
        $fullPath + [IO.Path]::DirectorySeparatorChar
    }
}

function Assert-PathWithin {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$Parent,
        [switch]$AllowEqual
    )

    $fullPath = [IO.Path]::GetFullPath($Path)
    $fullParent = [IO.Path]::GetFullPath($Parent)
    $isEqual = $fullPath.Equals($fullParent, [StringComparison]::OrdinalIgnoreCase)
    $isChild = $fullPath.StartsWith((Get-PathPrefix -Path $fullParent), [StringComparison]::OrdinalIgnoreCase)
    if ((!$AllowEqual -and $isEqual) -or (!$isEqual -and !$isChild)) {
        throw "Path '$fullPath' is outside the permitted directory '$fullParent'."
    }
    $fullPath
}

function ConvertTo-RelativePath {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$Parent
    )

    $fullPath = Assert-PathWithin -Path $Path -Parent $Parent
    $fullParent = [IO.Path]::GetFullPath($Parent).TrimEnd([IO.Path]::DirectorySeparatorChar)
    $relativePath = $fullPath.Substring($fullParent.Length).TrimStart([IO.Path]::DirectorySeparatorChar)
    $relativePath -replace [regex]::Escape([IO.Path]::DirectorySeparatorChar), '/'
}

function ConvertTo-ZipRelativePath {
    param([Parameter(Mandatory)][string]$RelativePath)

    $normalized = $RelativePath.Replace('\', '/')
    if ([string]::IsNullOrWhiteSpace($normalized) -or
        $normalized.StartsWith('/') -or
        $normalized.Contains(':') -or
        $normalized.IndexOf([char]0) -ge 0 -or
        $normalized -match '(^|/)\.\.?(/|$)' -or
        $normalized -match '//') {
        throw "Invalid source relative path '$RelativePath'."
    }
    foreach ($part in $normalized.Split('/')) {
        if ([string]::IsNullOrWhiteSpace($part)) {
            throw "Invalid source relative path '$RelativePath'."
        }
    }
    $normalized
}

function ConvertTo-ZipMemberPath {
    param([Parameter(Mandatory)][string]$RelativePath)

    $normalized = ConvertTo-ZipRelativePath -RelativePath $RelativePath
    ConvertTo-ZipRelativePath -RelativePath ("NppTerminal/$normalized")
}

function Get-ScopedRegularFiles {
    param([Parameter(Mandatory)][string]$RelativeDirectory)

    $directoryPath = Join-Path $projectRoot $RelativeDirectory
    $directoryItem = Get-Item -LiteralPath $directoryPath -Force -ErrorAction SilentlyContinue
    if (!$directoryItem) {
        throw "Required source directory '$RelativeDirectory' was not found."
    }
    $lexicalDirectoryAttributes = Get-CheckedAttributes -Path $directoryPath
    if (!$directoryItem.PSIsContainer -or
        ($lexicalDirectoryAttributes -band [IO.FileAttributes]::Directory) -eq 0) {
        throw "Source path '$RelativeDirectory' is not a directory."
    }
    $directoryPath = (Resolve-Path -LiteralPath $directoryPath -ErrorAction Stop).Path
    Assert-PathWithin -Path $directoryPath -Parent $projectRoot -AllowEqual | Out-Null
    $directoryAttributes = Get-CheckedAttributes -Path $directoryPath
    if (($directoryAttributes -band [IO.FileAttributes]::Directory) -eq 0) {
        throw "Source path '$RelativeDirectory' is not a directory."
    }

    $pending = New-Object 'System.Collections.Generic.Stack[string]'
    $pending.Push($directoryPath)
    while ($pending.Count -gt 0) {
        $current = $pending.Pop()
        $childPaths = [IO.Directory]::GetFileSystemEntries($current)
        [Array]::Sort($childPaths, [StringComparer]::OrdinalIgnoreCase)
        foreach ($childPath in $childPaths) {
            Assert-PathWithin -Path $childPath -Parent $directoryPath -AllowEqual | Out-Null
            $attributes = Get-CheckedAttributes -Path $childPath
            $item = Get-Item -LiteralPath $childPath -Force -ErrorAction Stop
            if ($item.PSIsContainer) {
                if (($attributes -band [IO.FileAttributes]::Directory) -eq 0) {
                    throw "Filesystem type changed while enumerating '$childPath'."
                }
                $pending.Push($childPath)
            }
            elseif ([IO.File]::Exists($childPath)) {
                $childPath
            }
            else {
                throw "Unsupported non-regular source entry '$childPath'."
            }
        }
    }
}

function Add-SourceSpec {
    param(
        [Parameter(Mandatory)][AllowEmptyCollection()][System.Collections.Generic.List[object]]$Specs,
        [Parameter(Mandatory)][hashtable]$Seen,
        [Parameter(Mandatory)][string]$RelativePath,
        [Parameter(Mandatory)][string]$Kind
    )

    $normalized = ConvertTo-ZipRelativePath -RelativePath $RelativePath
    $key = $normalized.ToLowerInvariant()
    if ($Seen.ContainsKey($key)) {
        throw "Duplicate source manifest path '$normalized'."
    }
    $Seen[$key] = $true
    $Specs.Add([pscustomobject]@{
        SourceRelative = $normalized
        Kind = $Kind
    })
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

function Remove-TemporaryPathSafely {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$OutRoot
    )

    if (!(Test-Path -LiteralPath $Path)) { return }
    # Inspect the lexical path before Resolve-Path so an unknown link cannot
    # resolve to an apparently safe target outside the intended output tree.
    Get-CheckedAttributes -Path $Path | Out-Null
    $resolvedPath = (Resolve-Path -LiteralPath $Path -ErrorAction Stop).Path
    Assert-PathWithin -Path $resolvedPath -Parent $OutRoot | Out-Null
    Remove-Item -LiteralPath $resolvedPath -Recurse -Force -ErrorAction Stop
}

$outRoot = Join-Path $projectRoot 'out'
if (Get-Item -LiteralPath $outRoot -Force -ErrorAction SilentlyContinue) {
    Get-CheckedAttributes -Path $outRoot | Out-Null
}
New-Item -ItemType Directory -Path $outRoot -Force | Out-Null
$outRoot = (Resolve-Path -LiteralPath $outRoot -ErrorAction Stop).Path
Get-CheckedAttributes -Path $projectRoot | Out-Null
Get-CheckedAttributes -Path $outRoot | Out-Null
Assert-PathWithin -Path $outRoot -Parent $projectRoot | Out-Null

$archivePath = Join-Path $outRoot "NppTerminal-$($currentVersion.String)-source.zip"
$temporaryArchivePath = Join-Path $outRoot ('.source-archive-' + [guid]::NewGuid().ToString('N') + '.zip')
$stagingRoot = Join-Path $outRoot ('.source-stage-' + [guid]::NewGuid().ToString('N'))
$stagingProjectRoot = Join-Path $stagingRoot 'NppTerminal'
Assert-PathWithin -Path $archivePath -Parent $outRoot | Out-Null
Assert-PathWithin -Path $temporaryArchivePath -Parent $outRoot | Out-Null
Assert-PathWithin -Path $stagingRoot -Parent $outRoot | Out-Null
if (Get-Item -LiteralPath $archivePath -Force -ErrorAction SilentlyContinue) {
    Get-CheckedAttributes -Path $archivePath | Out-Null
}
if (Get-Item -LiteralPath $temporaryArchivePath -Force -ErrorAction SilentlyContinue) {
    Get-CheckedAttributes -Path $temporaryArchivePath | Out-Null
}
if (Get-Item -LiteralPath $stagingRoot -Force -ErrorAction SilentlyContinue) {
    Get-CheckedAttributes -Path $stagingRoot | Out-Null
}

$sourceSpecs = New-Object 'System.Collections.Generic.List[object]'
$seenPaths = @{}

foreach ($rootSpec in @(
    [pscustomobject]@{ Path = 'VERSION'; Kind = 'VersionText' },
    [pscustomobject]@{ Path = 'LICENSE'; Kind = 'License' },
    [pscustomobject]@{ Path = 'THIRD_PARTY_NOTICES.md'; Kind = 'Notice' },
    [pscustomobject]@{ Path = 'README.md'; Kind = 'ProjectDocument' },
    [pscustomobject]@{ Path = 'dependencies.lock.json'; Kind = 'DependencyLock' },
    [pscustomobject]@{ Path = 'AGENTS.md'; Kind = 'ProjectInstructions' },
    [pscustomobject]@{ Path = '.gitignore'; Kind = 'ProjectInstructions' },
    [pscustomobject]@{ Path = 'NppTerminal_Implementation_Plan.md'; Kind = 'ProjectDocument' },
    [pscustomobject]@{ Path = 'NppTerminal_Requirements_Specification.md'; Kind = 'ProjectDocument' },
    [pscustomobject]@{ Path = 'NppTerminal.sln'; Kind = 'BuildProject' },
    [pscustomobject]@{ Path = 'NppTerminal.Tests.vcxproj'; Kind = 'BuildProject' },
    [pscustomobject]@{ Path = 'NppTerminal.vcxproj'; Kind = 'BuildProject' },
    [pscustomobject]@{ Path = 'NppTerminalBroker.Tests.vcxproj'; Kind = 'BuildProject' },
    [pscustomobject]@{ Path = 'NppTerminalBroker.vcxproj'; Kind = 'BuildProject' }
)) {
    Add-SourceSpec -Specs $sourceSpecs -Seen $seenPaths -RelativePath $rootSpec.Path -Kind $rootSpec.Kind
}

foreach ($directoryName in @('src', 'tests', 'scripts', 'docs', 'third_party')) {
    foreach ($filePath in @(Get-ScopedRegularFiles -RelativeDirectory $directoryName)) {
        $relativePath = ConvertTo-RelativePath -Path $filePath -Parent $projectRoot
        Add-SourceSpec -Specs $sourceSpecs -Seen $seenPaths -RelativePath $relativePath -Kind ($directoryName + 'Source')
    }
}

foreach ($webDirectory in @('web', 'web/vendor')) {
    $webDirectoryPath = Join-Path $projectRoot $webDirectory
    $webDirectoryItem = Get-Item -LiteralPath $webDirectoryPath -Force -ErrorAction Stop
    if (!$webDirectoryItem.PSIsContainer) {
        throw "Required web source directory '$webDirectory' is not a directory."
    }
    Get-CheckedAttributes -Path $webDirectoryPath | Out-Null
    $webDirectoryPath = (Resolve-Path -LiteralPath $webDirectoryPath -ErrorAction Stop).Path
    Get-CheckedAttributes -Path $webDirectoryPath | Out-Null
}

foreach ($webSpec in @(
    [pscustomobject]@{ Path = 'web/package.json'; Kind = 'WebBuildInput' },
    [pscustomobject]@{ Path = 'web/package-lock.json'; Kind = 'WebBuildInput' },
    [pscustomobject]@{ Path = 'web/build.mjs'; Kind = 'WebBuildInput' },
    [pscustomobject]@{ Path = 'web/index.html'; Kind = 'WebAsset' },
    [pscustomobject]@{ Path = 'web/terminal.js'; Kind = 'WebAsset' },
    [pscustomobject]@{ Path = 'web/terminal.css'; Kind = 'WebAsset' },
    [pscustomobject]@{ Path = 'web/terminal.test.mjs'; Kind = 'WebTest' },
    [pscustomobject]@{ Path = 'web/vendor/xterm.js'; Kind = 'WebVendor' },
    [pscustomobject]@{ Path = 'web/vendor/xterm.css'; Kind = 'WebVendor' },
    [pscustomobject]@{ Path = 'web/vendor/addon-fit.js'; Kind = 'WebVendor' },
    [pscustomobject]@{ Path = 'web/vendor/xterm.LICENSE'; Kind = 'WebVendorLicense' },
    [pscustomobject]@{ Path = 'web/vendor/addon-fit.LICENSE'; Kind = 'WebVendorLicense' }
)) {
    Add-SourceSpec -Specs $sourceSpecs -Seen $seenPaths -RelativePath $webSpec.Path -Kind $webSpec.Kind
}

$sourceSpecs = @($sourceSpecs | Sort-Object -Property SourceRelative)
$expectedZipMembers = @($sourceSpecs | ForEach-Object { ConvertTo-ZipMemberPath -RelativePath $_.SourceRelative })
if ($expectedZipMembers.Count -eq 0) {
    throw 'The source manifest is empty.'
}

$stagingCreated = $false
$temporaryArchiveCreated = $false
$snapshots = New-Object 'System.Collections.Generic.List[object]'
$archive = $null
$archiveStream = $null
try {
    $stagingCreated = $true
    New-Item -ItemType Directory -Path $stagingProjectRoot -Force -ErrorAction Stop | Out-Null

    foreach ($spec in $sourceSpecs) {
        $sourcePath = Join-Path $projectRoot $spec.SourceRelative
        $sourceItem = Get-Item -LiteralPath $sourcePath -Force -ErrorAction SilentlyContinue
        if (!$sourceItem) {
            throw "Required source file '$($spec.SourceRelative)' was not found."
        }
        $sourceAttributes = Get-CheckedAttributes -Path $sourcePath
        if ($sourceItem.PSIsContainer) {
            throw "Expected a regular source file at '$($spec.SourceRelative)'."
        }
        $sourcePath = (Resolve-Path -LiteralPath $sourcePath -ErrorAction Stop).Path
        Assert-PathWithin -Path $sourcePath -Parent $projectRoot | Out-Null
        Get-CheckedAttributes -Path $sourcePath | Out-Null

        $zipPath = ConvertTo-ZipMemberPath -RelativePath $spec.SourceRelative
        $stagePath = Join-Path $stagingRoot $zipPath
        Assert-PathWithin -Path $stagePath -Parent $stagingRoot | Out-Null
        $stageDirectory = Split-Path -Parent $stagePath
        New-Item -ItemType Directory -Path $stageDirectory -Force -ErrorAction Stop | Out-Null

        $sourceStream = $null
        $stageStream = $null
        try {
            # FileShare.Read prevents a newly opened writer from changing the
            # bytes while this frozen source snapshot is copied.
            $sourceStream = [IO.File]::Open($sourcePath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
            $stageStream = [IO.File]::Open($stagePath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
            $sourceStream.CopyTo($stageStream)
            $stageStream.Flush()
            $sourceLength = $sourceStream.Length
            $sourceStream.Position = 0
            $sourceSha256 = Get-StreamSha256 -Stream $sourceStream
        }
        finally {
            if ($stageStream) { $stageStream.Dispose() }
            if ($sourceStream) { $sourceStream.Dispose() }
        }

        $stageItem = Get-Item -LiteralPath $stagePath -Force -ErrorAction Stop
        if ($stageItem.Length -ne $sourceLength) {
            throw "Staged source '$($spec.SourceRelative)' changed length during snapshot."
        }
        $stageSha256 = Get-FileSha256 -Path $stagePath
        if ($stageSha256 -ne $sourceSha256) {
            throw "Staged source '$($spec.SourceRelative)' does not match its frozen source snapshot."
        }
        [IO.File]::SetLastWriteTimeUtc($stagePath, $sourceItem.LastWriteTimeUtc)

        $snapshots.Add([pscustomobject]@{
            SourceRelative = $spec.SourceRelative
            ZipPath = $zipPath
            StagePath = $stagePath
            Kind = $spec.Kind
            Bytes = [int64]$sourceLength
            SourceSha256 = $sourceSha256
            StageSha256 = $stageSha256
            SourceLastWrite = $sourceItem.LastWriteTimeUtc
            SourceLastWriteUtc = $sourceItem.LastWriteTimeUtc.ToString('o')
            SourceAttributesValue = [int]$sourceAttributes
            SourceAttributes = $sourceAttributes.ToString()
            SourceReparseTag = if (($sourceAttributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                ('0x{0:X8}' -f ([NppTerminal.SourcePackageNative]::GetReparseTag($sourcePath, $false)))
            } else { $null }
        })
    }

    $temporaryArchiveCreated = $true
    $archiveStream = [IO.File]::Open($temporaryArchivePath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    $archive = New-Object IO.Compression.ZipArchive($archiveStream, ([IO.Compression.ZipArchiveMode]::Create), $false)
    foreach ($snapshot in $snapshots) {
        $entry = $archive.CreateEntry($snapshot.ZipPath, [IO.Compression.CompressionLevel]::Optimal)
        $entry.LastWriteTime = [DateTimeOffset]$snapshot.SourceLastWrite
        $entry.ExternalAttributes = ($snapshot.SourceAttributesValue -band 0xFFFF)
        $entryStream = $null
        $stageReadStream = $null
        try {
            $entryStream = $entry.Open()
            $stageReadStream = [IO.File]::Open($snapshot.StagePath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
            $stageReadStream.CopyTo($entryStream)
        }
        finally {
            if ($entryStream) { $entryStream.Dispose() }
            if ($stageReadStream) { $stageReadStream.Dispose() }
        }
    }
    $archive.Dispose()
    $archive = $null
    $archiveStream.Dispose()
    $archiveStream = $null

    $verifyStream = $null
    $verifyArchive = $null
    try {
        $verifyStream = [IO.File]::Open($temporaryArchivePath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
        $verifyArchive = New-Object IO.Compression.ZipArchive($verifyStream, ([IO.Compression.ZipArchiveMode]::Read), $false)
        $entries = @($verifyArchive.Entries)
        if ($entries.Count -ne $expectedZipMembers.Count) {
            throw "Source archive entry count $($entries.Count) does not match exact manifest count $($expectedZipMembers.Count)."
        }

        $actualNames = @()
        $seenZipNames = @{}
        foreach ($entry in $entries) {
            if (!$entry.FullName.StartsWith('NppTerminal/', [StringComparison]::Ordinal)) {
                throw "Source archive entry '$($entry.FullName)' is not rooted at NppTerminal/."
            }
            $entryRelativePath = $entry.FullName.Substring('NppTerminal/'.Length)
            $validatedName = ConvertTo-ZipMemberPath -RelativePath $entryRelativePath
            if ($validatedName -cne $entry.FullName) {
                throw "Source archive entry '$($entry.FullName)' is not normalized."
            }
            $nameKey = $entry.FullName.ToLowerInvariant()
            if ($seenZipNames.ContainsKey($nameKey)) {
                throw "Source archive contains duplicate entry '$($entry.FullName)'."
            }
            $seenZipNames[$nameKey] = $true
            $actualNames += $entry.FullName
        }
        $missingNames = @($expectedZipMembers | Where-Object { $actualNames -cnotcontains $_ })
        $unexpectedNames = @($actualNames | Where-Object { $expectedZipMembers -cnotcontains $_ })
        if ($missingNames.Count -gt 0 -or $unexpectedNames.Count -gt 0) {
            throw "Source archive manifest mismatch. Missing: $($missingNames -join ', '); unexpected: $($unexpectedNames -join ', ')."
        }

        foreach ($snapshot in $snapshots) {
            $entry = @($entries | Where-Object { $_.FullName -ceq $snapshot.ZipPath })[0]
            if (!$entry) { throw "Source archive entry '$($snapshot.ZipPath)' was not found."
            }
            if ([int64]$entry.Length -ne $snapshot.Bytes) {
                throw "Source archive entry '$($snapshot.ZipPath)' length does not match its frozen source snapshot."
            }
            $zipSha256 = Get-ZipEntrySha256 -Entry $entry
            if ($zipSha256 -ne $snapshot.SourceSha256) {
                throw "Source archive entry '$($snapshot.ZipPath)' does not match its source SHA-256."
            }
            $snapshot | Add-Member -NotePropertyName ZipSha256 -NotePropertyValue $zipSha256
            $snapshot | Add-Member -NotePropertyName HashesMatch -NotePropertyValue ($zipSha256 -eq $snapshot.SourceSha256)
            $snapshot | Add-Member -NotePropertyName ZipBytes -NotePropertyValue ([int64]$entry.Length)
            $snapshot | Add-Member -NotePropertyName ZipLastWriteUtc -NotePropertyValue $entry.LastWriteTime.UtcDateTime.ToString('o')
        }
    }
    finally {
        if ($verifyArchive) { $verifyArchive.Dispose() }
        if ($verifyStream) { $verifyStream.Dispose() }
    }

    Move-Item -LiteralPath $temporaryArchivePath -Destination $archivePath -Force -ErrorAction Stop
    $temporaryArchiveCreated = $false

    $versionSnapshot = @($snapshots | Where-Object { $_.SourceRelative -ceq 'VERSION' })[0]
    if (!$versionSnapshot) { throw 'The source manifest does not contain VERSION.' }
    $versionContent = Get-Content -LiteralPath (Join-Path $projectRoot 'VERSION') -Raw
    $versionTextValid = $versionContent -ceq ($currentVersion.String + "`n") -or
        $versionContent -ceq ($currentVersion.String + "`r`n")
    if (!$versionTextValid) { throw "VERSION does not contain exactly the authoritative version $($currentVersion.String) followed by one newline." }

    $archiveBytes = (Get-Item -LiteralPath $archivePath -Force -ErrorAction Stop).Length
    $archiveSha256 = Get-FileSha256 -Path $archivePath
    $reportPath = Join-Path $outRoot ("source-inspection-$($currentVersion.String)-$([DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')).json")
    Assert-PathWithin -Path $reportPath -Parent $outRoot | Out-Null
    $report = [pscustomobject]@{
        InspectionStatus = 'Passed'
        CapturedUtc = [DateTime]::UtcNow.ToString('o')
        Version = $currentVersion.String
        VersionProof = [pscustomobject]@{
            SourceRelative = 'VERSION'
            ZipPath = $versionSnapshot.ZipPath
            TextValid = $versionTextValid
            SourceSha256 = $versionSnapshot.SourceSha256
            ZipSha256 = $versionSnapshot.ZipSha256
            HashesMatch = $versionSnapshot.HashesMatch
        }
        ArchivePath = (Resolve-Path -LiteralPath $archivePath -ErrorAction Stop).Path
        ArchiveBytes = [int64]$archiveBytes
        ArchiveSha256 = $archiveSha256
        FileCount = $snapshots.Count
        ExpectedFileCount = $expectedZipMembers.Count
        Excluded = [pscustomobject]@{
            RootDirectories = @('out', '.cache', 'packages')
            WebDirectories = @('web/node_modules')
            RootFiles = @('Settings.TestMain.obj')
        }
        Contents = @($snapshots | ForEach-Object {
            [pscustomobject]@{
                SourceRelative = $_.SourceRelative
                ZipPath = $_.ZipPath
                Kind = $_.Kind
                Bytes = $_.Bytes
                SourceSha256 = $_.SourceSha256
                StageSha256 = $_.StageSha256
                ZipSha256 = $_.ZipSha256
                ZipBytes = $_.ZipBytes
                HashesMatch = $_.HashesMatch
                SourceLastWriteUtc = $_.SourceLastWriteUtc
                ZipLastWriteUtc = $_.ZipLastWriteUtc
                SourceAttributes = $_.SourceAttributes
                SourceReparseTag = $_.SourceReparseTag
            }
        })
    }
    $reportJson = $report | ConvertTo-Json -Depth 8
    [IO.File]::WriteAllText($reportPath, $reportJson, (New-Object Text.UTF8Encoding($false)))
    Write-Output $reportJson
}
finally {
    if ($archive) { $archive.Dispose() }
    if ($archiveStream) { $archiveStream.Dispose() }
    if ($temporaryArchiveCreated) {
        Remove-TemporaryPathSafely -Path $temporaryArchivePath -OutRoot $outRoot
    }
    if ($stagingCreated) {
        Remove-TemporaryPathSafely -Path $stagingRoot -OutRoot $outRoot
    }
}
