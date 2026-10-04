param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [switch]$IncludeLongStream
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'version.ps1')

function Write-ValidationSummary {
    param(
        [Parameter(Mandatory)][System.Collections.IDictionary]$Summary,
        [Parameter(Mandatory)][string]$Path
    )

    $temporaryPath = Join-Path (Split-Path -Parent $Path) ('.summary-' + [IO.Path]::GetRandomFileName())
    try {
        $json = $Summary | ConvertTo-Json -Depth 10
        $utf8 = New-Object System.Text.UTF8Encoding($false)
        [IO.File]::WriteAllText($temporaryPath, $json + [Environment]::NewLine, $utf8)
        Move-Item -LiteralPath $temporaryPath -Destination $Path -Force
    }
    finally {
        if (Test-Path -LiteralPath $temporaryPath -PathType Leaf) {
            Remove-Item -LiteralPath $temporaryPath -Force
        }
    }
}

function Test-NativePassOutput {
    param([Parameter(Mandatory)][string]$Path)

    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    $content = [IO.File]::ReadAllText($Path)
    $lines = @($content -split "`r?`n" |
        ForEach-Object { $_.Trim() } |
        Where-Object { $_.Length -gt 0 })
    return $lines.Count -gt 0 -and $lines[$lines.Count - 1] -eq 'PASS'
}

function Invoke-ValidationProcess {
    param(
        [Parameter(Mandatory)][System.Collections.IDictionary]$Record,
        [Parameter(Mandatory)][System.Collections.IDictionary]$Summary,
        [Parameter(Mandatory)][string]$SummaryPath,
        [Parameter(Mandatory)][string]$ArtifactDirectory,
        [Parameter(Mandatory)][string]$FilePath,
        [Parameter(Mandatory)][string]$WorkingDirectory,
        [Parameter(Mandatory)][string[]]$Arguments,
        [Parameter(Mandatory)][int]$TimeoutSeconds,
        [switch]$NativeOutput,
        [switch]$AllowJobProbeSkip
    )

    $outputPath = Join-Path $ArtifactDirectory ([string]$Record['OutputFile'])
    $errorPath = Join-Path $ArtifactDirectory ([string]$Record['ErrorFile'])
    $process = $null
    $Record['StartedUTC'] = [DateTime]::UtcNow.ToString('o')
    $Record['Status'] = 'NotRun'
    $Record['TimeoutSeconds'] = $TimeoutSeconds
    $Summary['ActiveProcessId'] = $null
    Write-ValidationSummary -Summary $Summary -Path $SummaryPath

    try {
        if (!(Test-Path -LiteralPath $FilePath -PathType Leaf)) {
            throw "The validation executable was not found at '$FilePath'."
        }
        if (!(Test-Path -LiteralPath $WorkingDirectory -PathType Container)) {
            throw "The validation working directory was not found at '$WorkingDirectory'."
        }
        $process = Start-Process -FilePath $FilePath -ArgumentList $Arguments `
            -WorkingDirectory $WorkingDirectory -WindowStyle Hidden `
            -RedirectStandardOutput $outputPath -RedirectStandardError $errorPath -PassThru
        $Record['ProcessId'] = $process.Id
        $Record['Status'] = 'Running'
        $Summary['ActiveProcessId'] = $process.Id
        Write-ValidationSummary -Summary $Summary -Path $SummaryPath

        $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
        $completed = $false
        while (!$completed) {
            if ($process.HasExited) {
                $completed = $true
                break
            }
            $remainingMilliseconds = ($deadline - [DateTime]::UtcNow).TotalMilliseconds
            if ($remainingMilliseconds -le 0) { break }
            $waitMilliseconds = [int][Math]::Min(1000, [Math]::Ceiling($remainingMilliseconds))
            $completed = $process.WaitForExit($waitMilliseconds)
        }
        if (!$completed -and $process.HasExited) { $completed = $true }
        if (!$completed) {
            $Record['TimedOut'] = $true
            # The Process object came directly from Start-Process. Terminate
            # only that owned PID; never search by name or kill descendants.
            if (!$process.HasExited) {
                Stop-Process -Id $process.Id -Force
            }
            [void]$process.WaitForExit(5000)
            try { $Record['ExitCode'] = $process.ExitCode } catch { $Record['ExitCode'] = $null }
            $Record['Status'] = 'Failed'
            $Record['Failure'] = "Timed out after $TimeoutSeconds seconds."
        }
        else {
            $Record['ExitCode'] = $process.ExitCode
            if ($AllowJobProbeSkip -and $Record['ExitCode'] -eq 3) {
                $Record['Status'] = 'NotRun'
                $Record['RequiredSkip'] = $true
                $Record['SkipReason'] = 'The parent or nested Windows job capability was unavailable.'
            }
            elseif ($NativeOutput) {
                if ($Record['ExitCode'] -eq 0 -and (Test-NativePassOutput -Path $outputPath)) {
                    $Record['Status'] = 'Passed'
                }
                else {
                    $Record['Status'] = 'Failed'
                    $Record['Failure'] = 'Native process did not exit 0 with a final standalone PASS line on stdout.'
                }
            }
            elseif ($Record['ExitCode'] -eq 0) {
                $Record['Status'] = 'Passed'
            }
            else {
                $Record['Status'] = 'Failed'
                $Record['Failure'] = "Process exited with code $($Record['ExitCode'])."
            }
        }
    }
    catch {
        $Record['Status'] = 'Failed'
        $Record['Failure'] = $_.Exception.Message
        try {
            if ($process -and !$process.HasExited) {
                Stop-Process -Id $process.Id -Force
                [void]$process.WaitForExit(5000)
            }
        }
        catch {
            $Record['TerminationFailure'] = $_.Exception.Message
        }
    }
    finally {
        $Record['EndedUTC'] = [DateTime]::UtcNow.ToString('o')
        $started = [DateTime]::Parse([string]$Record['StartedUTC'], [Globalization.CultureInfo]::InvariantCulture)
        $ended = [DateTime]::Parse([string]$Record['EndedUTC'], [Globalization.CultureInfo]::InvariantCulture)
        $Record['DurationSeconds'] = [Math]::Round(($ended - $started).TotalSeconds, 3)
        $Summary['ActiveProcessId'] = $null
        Write-ValidationSummary -Summary $Summary -Path $SummaryPath
    }
}

$version = Get-NppTerminalVersion -RootPath $projectRoot
$timestamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ', [Globalization.CultureInfo]::InvariantCulture)
$artifactDirectory = Join-Path $projectRoot (Join-Path 'out/validation' "$($version.String)-$timestamp")
New-Item -ItemType Directory -Path $artifactDirectory -Force | Out-Null
$summaryPath = Join-Path $artifactDirectory 'summary.json'

$preflight = [ordered]@{
    Status = 'NotRun'
    Version = $version.String
    DllPath = $null
    DllVersion = $null
    NativeBuildDirectory = $null
    NativeTestExecutable = $null
    BrokerVersions = [ordered]@{
        'NppTerminalBroker.exe' = $null
        'NppTerminalBroker.Tests.exe' = $null
    }
    CopiedExecutables = @()
    WebFixtureDirectory = $null
    WebFixtureAssets = @()
    NpmExecutable = $null
    Failure = $null
}
$summary = [ordered]@{
    SchemaVersion = 1
    Project = 'NppTerminal'
    ValidationStartedUTC = [DateTime]::UtcNow.ToString('o')
    ValidationEndedUTC = $null
    Version = $version.String
    Configuration = $Configuration
    IncludeLongStream = [bool]$IncludeLongStream
    ArtifactDirectory = $artifactDirectory
    MetadataPolicy = 'Metadata only: no terminal bytes, commands, environment variables, or shell history are recorded.'
    Preflight = $preflight
    ActiveProcessId = $null
    OverallStatus = 'NotRun'
    OverallExitCode = $null
    Tests = [System.Collections.Generic.List[object]]::new()
}
Write-ValidationSummary -Summary $summary -Path $summaryPath

$records = $summary['Tests']
try {
    $buildDirectory = Join-Path $projectRoot "out/x64/$Configuration"
    $dllPath = Join-Path $buildDirectory 'NppTerminal.dll'
    $preflight['DllPath'] = $dllPath
    $preflight['NativeBuildDirectory'] = $buildDirectory
    $dllVersion = Get-NppTerminalVersionFromDll -Path $dllPath
    $preflight['DllVersion'] = $dllVersion.String
    if ($dllVersion.String -ne $version.String) {
        throw "The native plugin version $($dllVersion.String) does not match VERSION $($version.String)."
    }

    $nativeExecutables = @(Get-ChildItem -LiteralPath $buildDirectory -Filter '*.exe' -File)
    $nativeTestSource = $nativeExecutables | Where-Object Name -eq 'NppTerminal.Tests.exe' | Select-Object -First 1
    if (!$nativeTestSource) {
        throw "The native test executable was not found in '$buildDirectory'."
    }
    foreach ($brokerName in @('NppTerminalBroker.exe', 'NppTerminalBroker.Tests.exe')) {
        $brokerSource = $nativeExecutables |
            Where-Object Name -eq $brokerName |
            Select-Object -First 1
        if (!$brokerSource) {
            throw "The required broker executable '$brokerName' was not found in '$buildDirectory'."
        }
        $brokerVersion = Get-NppTerminalVersionFromDll -Path $brokerSource.FullName
        $preflight['BrokerVersions'][$brokerName] = $brokerVersion.String
        if ($brokerVersion.String -ne $version.String) {
            throw "The broker executable '$brokerName' version $($brokerVersion.String) does not match VERSION $($version.String)."
        }
    }
    foreach ($nativeExecutable in $nativeExecutables) {
        Copy-Item -LiteralPath $nativeExecutable.FullName `
            -Destination (Join-Path $artifactDirectory $nativeExecutable.Name) -Force
    }
    $nativeTestExecutable = Join-Path $artifactDirectory 'NppTerminal.Tests.exe'
    $preflight['NativeTestExecutable'] = $nativeTestExecutable
    $preflight['CopiedExecutables'] = @($nativeExecutables | ForEach-Object Name)

    # The real WebView fixture loads the same packaged page beside its copied
    # executable. Copy only shipping assets, never node_modules or source tools.
    $webFixtureDirectory = Join-Path $artifactDirectory 'web'
    New-Item -ItemType Directory -Path (Join-Path $webFixtureDirectory 'vendor') -Force | Out-Null
    $webFixtureAssets = @('index.html', 'terminal.js', 'terminal.css',
        'vendor/xterm.js', 'vendor/xterm.css', 'vendor/addon-fit.js',
        'vendor/xterm.LICENSE', 'vendor/addon-fit.LICENSE')
    foreach ($assetName in $webFixtureAssets) {
        Copy-Item -LiteralPath (Join-Path $projectRoot (Join-Path 'web' $assetName)) `
            -Destination (Join-Path $webFixtureDirectory $assetName)
    }
    $preflight['WebFixtureDirectory'] = $webFixtureDirectory
    $preflight['WebFixtureAssets'] = $webFixtureAssets

    $npmCommand = Get-Command npm.cmd -CommandType Application -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if (!$npmCommand) {
        $npmCommand = Get-Command npm -CommandType Application -ErrorAction SilentlyContinue |
            Select-Object -First 1
    }
    if ($npmCommand) { $preflight['NpmExecutable'] = [string]$npmCommand.Source }
    $preflight['Status'] = 'Passed'
    Write-ValidationSummary -Summary $summary -Path $summaryPath
}
catch {
    $preflight['Status'] = 'Failed'
    $preflight['Failure'] = $_.Exception.Message
    $summary['OverallStatus'] = 'Failed'
    $summary['OverallExitCode'] = 1
    $summary['ValidationEndedUTC'] = [DateTime]::UtcNow.ToString('o')
    Write-ValidationSummary -Summary $summary -Path $summaryPath
    Write-Error $_.Exception.Message
    exit 1
}

$webDirectory = Join-Path $projectRoot 'web'
$npmExecutable = [string]$preflight['NpmExecutable']
if ([string]::IsNullOrWhiteSpace($npmExecutable)) {
    # Keep a missing npm installation as a recorded frontend failure instead
    # of letting parameter binding abort before the test record is finalized.
    $npmExecutable = Join-Path $artifactDirectory '.missing-npm.cmd'
}
$testDefinitions = @(
    [pscustomobject]@{ Name = 'frontend-npm-test'; Arguments = @('test'); FilePath = $npmExecutable; WorkingDirectory = $webDirectory; TimeoutSeconds = 120; Native = $false; JobProbe = $false },
    [pscustomobject]@{ Name = 'native-unit-conpty'; Arguments = @('--unit', '--conpty'); FilePath = $nativeTestExecutable; WorkingDirectory = $artifactDirectory; TimeoutSeconds = 120; Native = $true; JobProbe = $false },
    [pscustomobject]@{ Name = 'native-stress'; Arguments = @('--stress'); FilePath = $nativeTestExecutable; WorkingDirectory = $artifactDirectory; TimeoutSeconds = 300; Native = $true; JobProbe = $false },
    [pscustomobject]@{ Name = 'native-blocked-stop'; Arguments = @('--blocked-stop'); FilePath = $nativeTestExecutable; WorkingDirectory = $artifactDirectory; TimeoutSeconds = 60; Native = $true; JobProbe = $false },
    [pscustomobject]@{ Name = 'native-job-probe'; Arguments = @('--job-probe'); FilePath = $nativeTestExecutable; WorkingDirectory = $artifactDirectory; TimeoutSeconds = 60; Native = $true; JobProbe = $true },
    [pscustomobject]@{ Name = 'native-broker-stall'; Arguments = @('--broker-stall'); FilePath = $nativeTestExecutable; WorkingDirectory = $artifactDirectory; TimeoutSeconds = 60; Native = $true; JobProbe = $false },
    [pscustomobject]@{ Name = 'native-blocked-command'; Arguments = @('--blocked-command-stop'); FilePath = $nativeTestExecutable; WorkingDirectory = $artifactDirectory; TimeoutSeconds = 60; Native = $true; JobProbe = $false },
    [pscustomobject]@{ Name = 'native-orphan-command'; Arguments = @('--orphan-command'); FilePath = $nativeTestExecutable; WorkingDirectory = $artifactDirectory; TimeoutSeconds = 60; Native = $true; JobProbe = $false },
    [pscustomobject]@{ Name = 'native-profile-cleanup'; Arguments = @('--profile-cleanup'); FilePath = $nativeTestExecutable; WorkingDirectory = $artifactDirectory; TimeoutSeconds = 180; Native = $true; JobProbe = $false },
    [pscustomobject]@{ Name = 'native-webview-cleanup'; Arguments = @('--webview-cleanup'); FilePath = $nativeTestExecutable; WorkingDirectory = $artifactDirectory; TimeoutSeconds = 60; Native = $true; JobProbe = $false },
    [pscustomobject]@{ Name = 'native-webview-integration'; Arguments = @('--webview-integration'); FilePath = $nativeTestExecutable; WorkingDirectory = $artifactDirectory; TimeoutSeconds = 180; Native = $true; JobProbe = $false },
    [pscustomobject]@{ Name = 'native-webview-security'; Arguments = @('--webview-security'); FilePath = $nativeTestExecutable; WorkingDirectory = $artifactDirectory; TimeoutSeconds = 180; Native = $true; JobProbe = $false },
    [pscustomobject]@{ Name = 'native-webview-render'; Arguments = @('--webview-render'); FilePath = $nativeTestExecutable; WorkingDirectory = $artifactDirectory; TimeoutSeconds = 180; Native = $true; JobProbe = $false },
    [pscustomobject]@{ Name = 'native-shell-catalog'; Arguments = @('--shell-catalog'); FilePath = $nativeTestExecutable; WorkingDirectory = $artifactDirectory; TimeoutSeconds = 120; Native = $true; JobProbe = $false },
    [pscustomobject]@{ Name = 'native-settings'; Arguments = @('--settings'); FilePath = $nativeTestExecutable; WorkingDirectory = $artifactDirectory; TimeoutSeconds = 120; Native = $true; JobProbe = $false },
    [pscustomobject]@{ Name = 'native-shell-smoke'; Arguments = @('--shell-smoke'); FilePath = $nativeTestExecutable; WorkingDirectory = $artifactDirectory; TimeoutSeconds = 180; Native = $true; JobProbe = $false },
    [pscustomobject]@{ Name = 'native-terminal-panel'; Arguments = @('--terminal-panel'); FilePath = $nativeTestExecutable; WorkingDirectory = $artifactDirectory; TimeoutSeconds = 180; Native = $true; JobProbe = $false },
    [pscustomobject]@{ Name = 'native-thread-start-failures'; Arguments = @('--thread-start-failures'); FilePath = $nativeTestExecutable; WorkingDirectory = $artifactDirectory; TimeoutSeconds = 60; Native = $true; JobProbe = $false },
    [pscustomobject]@{ Name = 'native-stream-count'; Arguments = @('--stream-count=10000'); FilePath = $nativeTestExecutable; WorkingDirectory = $artifactDirectory; TimeoutSeconds = 180; Native = $true; JobProbe = $false }
)
if ($IncludeLongStream) {
    $testDefinitions += [pscustomobject]@{ Name = 'native-stream-30m'; Arguments = @('--stream-seconds=1800'); FilePath = $nativeTestExecutable; WorkingDirectory = $artifactDirectory; TimeoutSeconds = 1845; Native = $true; JobProbe = $false }
}

foreach ($definition in $testDefinitions) {
    $logStem = $definition.Name
    $record = [ordered]@{
        Name = $definition.Name
        Arguments = @($definition.Arguments)
        TimeoutSeconds = $definition.TimeoutSeconds
        OutputFile = "$logStem.stdout.log"
        ErrorFile = "$logStem.stderr.log"
        StartedUTC = $null
        EndedUTC = $null
        DurationSeconds = $null
        ProcessId = $null
        ExitCode = $null
        TimedOut = $false
        RequiredSkip = $false
        SkipReason = $null
        Failure = $null
        Status = 'NotRun'
    }
    [void]$records.Add($record)
    Write-ValidationSummary -Summary $summary -Path $summaryPath
    Invoke-ValidationProcess -Record $record -Summary $summary `
        -SummaryPath $summaryPath -ArtifactDirectory $artifactDirectory `
        -FilePath $definition.FilePath -WorkingDirectory $definition.WorkingDirectory `
        -Arguments $definition.Arguments -TimeoutSeconds $definition.TimeoutSeconds `
        -NativeOutput:$definition.Native -AllowJobProbeSkip:$definition.JobProbe
    Write-Host "$($definition.Name): $($record['Status']) (exit $($record['ExitCode']))"
}

$failedTests = @($records | Where-Object { $_['Status'] -eq 'Failed' })
$requiredSkips = @($records | Where-Object { $_['RequiredSkip'] -eq $true })
$incompleteTests = @($records | Where-Object { $_['Status'] -eq 'NotRun' })
$summary['OverallStatus'] = if ($failedTests.Count -eq 0 -and $requiredSkips.Count -eq 0 -and $incompleteTests.Count -eq 0) { 'Passed' } else { 'Failed' }
$summary['OverallExitCode'] = if ($summary['OverallStatus'] -eq 'Passed') { 0 } else { 1 }
$summary['ValidationEndedUTC'] = [DateTime]::UtcNow.ToString('o')
Write-ValidationSummary -Summary $summary -Path $summaryPath
Write-Host "Validation $($summary['OverallStatus']). Summary: $summaryPath"
exit ([int]$summary['OverallExitCode'])
