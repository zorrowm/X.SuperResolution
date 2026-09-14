[CmdletBinding()]
param(
    [string]$PublishDir = '..\artifacts\publish\win-x64-full',
    [string]$Configuration = 'Release',
    [Parameter(Mandatory = $true)][string]$NsisDir
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'Generate-NsisMetadata.ps1') -PublishDir $PublishDir -Configuration $Configuration
& (Join-Path $PSScriptRoot 'Build-NsisSkin.ps1') -NsisDir $NsisDir
