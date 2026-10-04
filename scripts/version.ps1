Set-StrictMode -Version Latest

function ConvertTo-NppTerminalVersion {
    param([Parameter(Mandatory)][string]$Version)

    $normalized = $Version.Trim()
    if ($normalized -notmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$') {
        throw "VERSION must contain a three-part numeric version, for example 0.1.0. Found '$normalized'."
    }

    $parts = $normalized.Split('.') | ForEach-Object { [int]$_ }
    foreach ($part in $parts) {
        if ($part -gt 65535) {
            throw "Version components must be no greater than 65535 for Windows VERSIONINFO. Found '$normalized'."
        }
    }

    [pscustomobject]@{
        Major = $parts[0]
        Minor = $parts[1]
        Patch = $parts[2]
        String = $normalized
        FileComma = "$($parts[0]),$($parts[1]),$($parts[2]),0"
    }
}

function Get-NppTerminalVersion {
    param([Parameter(Mandatory)][string]$RootPath)

    $versionPath = Join-Path $RootPath 'VERSION'
    if (!(Test-Path -LiteralPath $versionPath -PathType Leaf)) {
        throw "The authoritative VERSION file was not found at '$versionPath'."
    }
    ConvertTo-NppTerminalVersion (Get-Content -LiteralPath $versionPath -Raw)
}

function Get-NppTerminalNextVersion {
    param(
        [Parameter(Mandatory)]$Current,
        [ValidateSet('Patch', 'Minor', 'Major')][string]$VersionBump = 'Patch'
    )

    $major = [int]$Current.Major
    $minor = [int]$Current.Minor
    $patch = [int]$Current.Patch
    switch ($VersionBump) {
        'Patch' { $patch++ }
        'Minor' { $minor++; $patch = 0 }
        'Major' { $major++; $minor = 0; $patch = 0 }
    }
    ConvertTo-NppTerminalVersion "$major.$minor.$patch"
}

function Get-NppTerminalVersionFromDll {
    param([Parameter(Mandatory)][string]$Path)

    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "The native plugin was not found at '$Path'."
    }
    $fileVersion = [Diagnostics.FileVersionInfo]::GetVersionInfo($Path).FileVersion
    if ([string]::IsNullOrWhiteSpace($fileVersion) -or
        $fileVersion.Trim() -notmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:\.(0|[1-9][0-9]*))?$') {
        throw "The native plugin at '$Path' has an invalid embedded FileVersion '$fileVersion'."
    }
    ConvertTo-NppTerminalVersion "$($Matches[1]).$($Matches[2]).$($Matches[3])"
}

function Write-NppTerminalTextAtomic {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$Content
    )

    $directory = Split-Path -Parent $Path
    if (!(Test-Path -LiteralPath $directory -PathType Container)) {
        throw "Cannot write '$Path' because '$directory' does not exist."
    }
    $temporaryPath = Join-Path $directory ('.nppterminal-version-' + [IO.Path]::GetRandomFileName())
    try {
        $utf8 = New-Object System.Text.UTF8Encoding($false)
        [IO.File]::WriteAllText($temporaryPath, $Content, $utf8)
        Move-Item -LiteralPath $temporaryPath -Destination $Path -Force
    }
    finally {
        if (Test-Path -LiteralPath $temporaryPath) {
            Remove-Item -LiteralPath $temporaryPath -Force
        }
    }
}

function Update-NppTerminalJsonVersion {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$Version,
        [Parameter(Mandatory)][string]$Pattern,
        [Parameter(Mandatory)][scriptblock]$Validate
    )

    $content = Get-Content -LiteralPath $Path -Raw
    $matches = [regex]::Matches($content, $Pattern)
    if ($matches.Count -ne 1) {
        throw "Expected one version entry in '$Path', found $($matches.Count)."
    }
    $updated = [regex]::Replace($content, $Pattern, {
        param($match)
        $match.Groups[1].Value + $Version + $match.Groups[2].Value
    }, 1)
    & $Validate $updated
    Write-NppTerminalTextAtomic -Path $Path -Content $updated
}

function Set-NppTerminalVersionFiles {
    param(
        [Parameter(Mandatory)][string]$RootPath,
        [Parameter(Mandatory)][string]$VersionString
    )

    $parsedVersion = ConvertTo-NppTerminalVersion $VersionString
    $versionText = "$($parsedVersion.String)`n"
    $headerText = @"
#pragma once

#define NPPTERMINAL_VERSION_MAJOR $($parsedVersion.Major)
#define NPPTERMINAL_VERSION_MINOR $($parsedVersion.Minor)
#define NPPTERMINAL_VERSION_PATCH $($parsedVersion.Patch)
#define NPPTERMINAL_VERSION_STRING "$($parsedVersion.String)"
#define NPPTERMINAL_VERSION_FILE_COMMA $($parsedVersion.FileComma)
"@

    Write-NppTerminalTextAtomic -Path (Join-Path $RootPath 'VERSION') -Content $versionText
    $headerText = $headerText.TrimStart("`r", "`n").TrimEnd("`r", "`n") + "`n"
    Write-NppTerminalTextAtomic -Path (Join-Path $RootPath 'src/Version.h') -Content $headerText

    $packageJsonPath = Join-Path $RootPath 'web/package.json'
    Update-NppTerminalJsonVersion -Path $packageJsonPath -Version $parsedVersion.String `
        -Pattern '(?m)^(  "version"\s*:\s*")[^"]+(".*)$' `
        -Validate {
            param($updated)
            $json = $updated | ConvertFrom-Json
            if ($json.version -ne $parsedVersion.String) {
                throw "web/package.json version did not update to $($parsedVersion.String)."
            }
        }

    $packageLockPath = Join-Path $RootPath 'web/package-lock.json'
    Update-NppTerminalJsonVersion -Path $packageLockPath -Version $parsedVersion.String `
        -Pattern '(?m)^(  "version"\s*:\s*")[^"]+(".*)$' `
        -Validate {
            param($updated)
            $topLevel = [regex]::Match($updated, '(?m)^  "version"\s*:\s*"([^"]+)"')
            if (!$topLevel.Success -or $topLevel.Groups[1].Value -ne $parsedVersion.String) {
                throw "web/package-lock.json root version did not update to $($parsedVersion.String)."
            }
        }

    $lockContent = Get-Content -LiteralPath $packageLockPath -Raw
    $lockRootPattern = '(?s)("packages"\s*:\s*\{\s*""\s*:\s*\{\s*"name"\s*:\s*"nppterminal-web"\s*,\s*"version"\s*:\s*")[^"]+(")'
    $lockRootMatches = [regex]::Matches($lockContent, $lockRootPattern)
    if ($lockRootMatches.Count -ne 1) {
        throw "Expected one root package version entry in '$packageLockPath', found $($lockRootMatches.Count)."
    }
    $updatedLock = [regex]::Replace($lockContent, $lockRootPattern, {
        param($match)
        $match.Groups[1].Value + $parsedVersion.String + $match.Groups[2].Value
    }, 1)
    $updatedRootMatch = [regex]::Match($updatedLock, $lockRootPattern)
    if (!$updatedRootMatch.Success -or $updatedRootMatch.Groups[0].Value -notmatch [regex]::Escape($parsedVersion.String)) {
        throw "web/package-lock.json root package version did not update to $($parsedVersion.String)."
    }
    Write-NppTerminalTextAtomic -Path $packageLockPath -Content $updatedLock
}
