param(
    [Parameter(Mandatory = $true)][int]$HostProcessId,
    [long]$BaselinePrivateBytes = 0,
    [string]$State = 'Unspecified',
    [string]$OutputPath
)
$ErrorActionPreference = 'Stop'
# Metadata only: never read terminal bytes, command lines, or environment.
$hostProcess = Get-Process -Id $HostProcessId
$processes = Get-CimInstance Win32_Process
$processById = @{}
foreach ($candidate in $processes) {
    $processById[[int]$candidate.ProcessId] = $candidate
}
if (!$processById.ContainsKey($HostProcessId) -or
    [Math]::Abs(($hostProcess.StartTime.ToUniversalTime() -
        $processById[$HostProcessId].CreationDate.ToUniversalTime()).TotalMilliseconds) -gt 2) {
    throw 'The host exited or changed identity during resource capture.'
}
$familyIds = [System.Collections.Generic.HashSet[int]]::new()
[void]$familyIds.Add($HostProcessId)
do {
    $previousCount = $familyIds.Count
    foreach ($candidate in $processes) {
        if ($familyIds.Contains([int]$candidate.ParentProcessId) -and
            $candidate.CreationDate -ge $processById[[int]$candidate.ParentProcessId].CreationDate) {
            [void]$familyIds.Add([int]$candidate.ProcessId)
        }
    }
} while ($familyIds.Count -gt $previousCount)
$samples = foreach ($familyId in $familyIds) {
    $sample = Get-Process -Id $familyId -ErrorAction SilentlyContinue
    $identity = $processById[$familyId]
    if ($sample -and $identity -and
        [Math]::Abs(($sample.StartTime.ToUniversalTime() -
            $identity.CreationDate.ToUniversalTime()).TotalMilliseconds) -le 2) {
        [pscustomobject]@{
            ProcessId = $sample.Id
            ParentProcessId = [int]$identity.ParentProcessId
            StartedUtc = $sample.StartTime.ToUniversalTime().ToString('o')
            Name = $sample.ProcessName
            PrivateBytes = $sample.PrivateMemorySize64
            CpuSeconds = $sample.TotalProcessorTime.TotalSeconds
            Handles = $sample.HandleCount
            Threads = $sample.Threads.Count
        }
    }
}
# Keep the legacy host/browser-only measurement stable for comparisons with the
# earlier resource captures.  The broker is reported separately and included
# only in the new plugin-family total below.
$browserSamples = @($samples | Where-Object Name -eq 'msedgewebview2')
$brokerSamples = @($samples | Where-Object Name -eq 'NppTerminalBroker')
$conPtySamples = @($samples | Where-Object Name -eq 'conhost')
$shellSamples = @($samples | Where-Object {
    $_.ProcessId -ne $HostProcessId -and
    $_.Name -notin @('msedgewebview2', 'NppTerminalBroker', 'conhost')
})
$browserBytes = if ($browserSamples.Count -gt 0) {
    [long](($browserSamples | Measure-Object PrivateBytes -Sum).Sum)
} else {
    0L
}
$brokerBytes = if ($brokerSamples.Count -gt 0) {
    [long](($brokerSamples | Measure-Object PrivateBytes -Sum).Sum)
} else {
    0L
}
$incrementalHostAndBrowserBytes = [long]($hostProcess.PrivateMemorySize64 +
    $browserBytes - $BaselinePrivateBytes)
$incrementalPluginFamilyBytes = [long]($hostProcess.PrivateMemorySize64 +
    $browserBytes + $brokerBytes - $BaselinePrivateBytes)
$result = [pscustomobject]@{
    CapturedUtc = [DateTime]::UtcNow.ToString('o')
    State = $State
    HostProcessId = $HostProcessId
    HostStartedUtc = $hostProcess.StartTime.ToUniversalTime().ToString('o')
    BaselinePrivateBytes = $BaselinePrivateBytes
    HostPrivateBytes = $hostProcess.PrivateMemorySize64
    BrowserFamilyPrivateBytes = $browserBytes
    BrokerFamilyPrivateBytes = $brokerBytes
    ConPtyPrivateBytes = [long](($conPtySamples | Measure-Object PrivateBytes -Sum).Sum)
    ShellAndDescendantPrivateBytes = [long](($shellSamples | Measure-Object PrivateBytes -Sum).Sum)
    # Legacy fields: host plus WebView browser processes, excluding the broker.
    IncrementalHostAndBrowserBytes = $incrementalHostAndBrowserBytes
    IncrementalHostAndBrowserMiB = [Math]::Round($incrementalHostAndBrowserBytes / 1MB, 2)
    IncrementalPluginFamilyBytes = $incrementalPluginFamilyBytes
    IncrementalPluginFamilyMiB = [Math]::Round($incrementalPluginFamilyBytes / 1MB, 2)
    Processes = @($samples)
} | ConvertTo-Json -Depth 4
if ($OutputPath) {
    [IO.File]::WriteAllText($OutputPath, $result + [Environment]::NewLine,
        (New-Object System.Text.UTF8Encoding($false)))
}
$result
