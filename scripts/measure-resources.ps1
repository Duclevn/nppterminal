param(
    [Parameter(Mandatory = $true)][int]$HostProcessId,
    [long]$BaselinePrivateBytes = 0
)
$ErrorActionPreference = 'Stop'
# Metadata only: never read terminal bytes, command lines, or environment.
$hostProcess = Get-Process -Id $HostProcessId
$processes = Get-CimInstance Win32_Process
$familyIds = [System.Collections.Generic.HashSet[int]]::new()
[void]$familyIds.Add($HostProcessId)
do {
    $previousCount = $familyIds.Count
    foreach ($candidate in $processes) {
        if ($familyIds.Contains([int]$candidate.ParentProcessId)) {
            [void]$familyIds.Add([int]$candidate.ProcessId)
        }
    }
} while ($familyIds.Count -gt $previousCount)
$samples = foreach ($familyId in $familyIds) {
    $sample = Get-Process -Id $familyId -ErrorAction SilentlyContinue
    if ($sample) {
        [pscustomobject]@{
            ProcessId = $sample.Id
            Name = $sample.ProcessName
            PrivateBytes = $sample.PrivateMemorySize64
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
[pscustomobject]@{
    CapturedUtc = [DateTime]::UtcNow.ToString('o')
    HostProcessId = $HostProcessId
    BaselinePrivateBytes = $BaselinePrivateBytes
    HostPrivateBytes = $hostProcess.PrivateMemorySize64
    BrowserFamilyPrivateBytes = $browserBytes
    BrokerFamilyPrivateBytes = $brokerBytes
    # Legacy fields: host plus WebView browser processes, excluding the broker.
    IncrementalHostAndBrowserBytes = $incrementalHostAndBrowserBytes
    IncrementalHostAndBrowserMiB = [Math]::Round($incrementalHostAndBrowserBytes / 1MB, 2)
    IncrementalPluginFamilyBytes = $incrementalPluginFamilyBytes
    IncrementalPluginFamilyMiB = [Math]::Round($incrementalPluginFamilyBytes / 1MB, 2)
    Processes = @($samples)
} | ConvertTo-Json -Depth 4
