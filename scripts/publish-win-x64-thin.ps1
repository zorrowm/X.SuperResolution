[CmdletBinding()]
param([string]$Configuration = 'Release', [switch]$SkipArchive)
& (Join-Path $PSScriptRoot 'Publish-Windows.ps1') -Flavor Thin -Configuration $Configuration -SkipArchive:$SkipArchive
