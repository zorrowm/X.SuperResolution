[CmdletBinding()]
param(
    [string]$Configuration = 'Release',
    [Alias('SkipFullPublish')][switch]$SkipPublish,
    [string]$PublishDir,
    [string]$MakeNsisPath
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $PublishDir) { $PublishDir = Join-Path $repoRoot 'artifacts\publish\win-x64-full' }
$PublishDir = [IO.Path]::GetFullPath($PublishDir)
if (-not $MakeNsisPath) {
    $command = Get-Command makensis -ErrorAction SilentlyContinue
    $MakeNsisPath = if ($command) { $command.Source } else { Join-Path ${env:ProgramFiles(x86)} 'NSIS\makensis.exe' }
}
if (-not (Test-Path -LiteralPath $MakeNsisPath)) { throw 'NSIS 3 Unicode is required. Install NSIS or pass -MakeNsisPath.' }
if (-not $SkipPublish) {
    if ($PublishDir -ne (Join-Path $repoRoot 'artifacts\publish\win-x64-full')) { throw 'Use -SkipPublish with a custom prebuilt PublishDir.' }
    & (Join-Path $PSScriptRoot 'publish-win-x64-full.ps1') -Configuration $Configuration -SkipArchive
}
& $MakeNsisPath /V3 "/DPUBLISH_DIR=$PublishDir" "/DCONFIGURATION=$Configuration" (Join-Path $repoRoot 'installer\X.SuperResolution.nsi')
if ($LASTEXITCODE -ne 0) { throw "NSIS compilation failed ($LASTEXITCODE)." }
$build = Get-Content -LiteralPath (Join-Path $repoRoot 'installer\generated\build-info.json') -Raw | ConvertFrom-Json
if (-not (Test-Path -LiteralPath $build.InstallerPath)) { throw 'The expected installer was not created.' }
$hash = Get-FileHash -LiteralPath $build.InstallerPath -Algorithm SHA256
[IO.File]::WriteAllText(($build.InstallerPath + '.sha256'), "$($hash.Hash.ToLowerInvariant())  $([IO.Path]::GetFileName($build.InstallerPath))`n", [Text.UTF8Encoding]::new($false))
Write-Host "Installer: $($build.InstallerPath)"
Write-Host "SHA256:    $($hash.Hash)"
