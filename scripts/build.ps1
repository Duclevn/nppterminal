param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [ValidateSet('Patch', 'Minor', 'Major')][string]$VersionBump = 'Patch',
    [switch]$SkipBootstrap
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'version.ps1')
if (!$SkipBootstrap) { & (Join-Path $PSScriptRoot 'bootstrap.ps1') }
$msbuildCommand = Get-Command msbuild -ErrorAction SilentlyContinue
if ($msbuildCommand) { $msbuildPath = $msbuildCommand.Source }
elseif (Test-Path -LiteralPath 'C:/BuildTools2022/MSBuild/Current/Bin/MSBuild.exe') {
    $msbuildPath = 'C:/BuildTools2022/MSBuild/Current/Bin/MSBuild.exe'
} else {
    $vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (!(Test-Path -LiteralPath $vswherePath)) { throw 'Install Visual Studio 2022 C++ build tools and a Windows SDK.' }
    $msbuildPath = & $vswherePath -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild/**/Bin/MSBuild.exe' | Select-Object -First 1
    if (!$msbuildPath) { throw 'MSBuild was not found.' }
}
$currentVersion = Get-NppTerminalVersion -RootPath $projectRoot
$nextVersion = Get-NppTerminalNextVersion -Current $currentVersion -VersionBump $VersionBump
Set-NppTerminalVersionFiles -RootPath $projectRoot -VersionString $nextVersion.String
Write-Host "Reserved NppTerminal version $($nextVersion.String) for this native build."
& $msbuildPath (Join-Path $projectRoot 'NppTerminal.sln') /m /restore "/p:Configuration=$Configuration" /p:Platform=x64 /verbosity:minimal /nologo
if ($LASTEXITCODE -ne 0) { throw 'Native build failed.' }
