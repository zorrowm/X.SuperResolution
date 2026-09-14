[CmdletBinding()]
param(
    [ValidateSet('Full','Thin')][string]$Flavor = 'Full',
    [string]$Configuration = 'Release',
    [switch]$SkipArchive
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$publishParent = Join-Path $repoRoot 'artifacts\publish'
$publishDir = Join-Path $publishParent ('win-x64-' + $Flavor.ToLowerInvariant())
$stagingDir = $publishDir + '.staging-' + [Guid]::NewGuid().ToString('N')
$selfContained = $Flavor -eq 'Full'
$vcompPath = & (Join-Path $PSScriptRoot 'Resolve-Vcomp140.ps1')
$sevenZip = $null
if (-not $SkipArchive) {
    $command = Get-Command 7z -ErrorAction SilentlyContinue
    $sevenZip = if ($command) { $command.Source } else { Join-Path $env:ProgramFiles '7-Zip\7z.exe' }
    if (-not (Test-Path -LiteralPath $sevenZip)) { throw '7-Zip is required to create release archives.' }
}

Write-Host "Publishing $Flavor Windows x64 single-file application..."
& dotnet publish (Join-Path $repoRoot 'X.SuperResolution\X.SuperResolution.csproj') -c $Configuration -r win-x64 --self-contained $selfContained.ToString().ToLowerInvariant() -o $stagingDir '-p:PublishSingleFile=true' '-p:IncludeNativeLibrariesForSelfExtract=true' '-p:EnableCompressionInSingleFile=false' '-p:PublishTrimmed=false' "-p:IncludeModels=$($selfContained.ToString().ToLowerInvariant())" '-p:CopyOutputSymbolsToPublishDirectory=false' '-p:DebugType=None' '-p:DebugSymbols=false' '-p:AvaloniaBuildServicesEnabled=false'
if ($LASTEXITCODE -ne 0) { throw "dotnet publish failed ($LASTEXITCODE)." }
Copy-Item -LiteralPath $vcompPath -Destination (Join-Path $stagingDir 'vcomp140.dll')
Get-ChildItem -LiteralPath $stagingDir -File -Recurse | Where-Object { $_.Extension -in @('.pdb','.xml') } | ForEach-Object { Remove-Item -LiteralPath $_.FullName }
. (Join-Path $repoRoot 'installer\Read-SingleFileBundle.ps1')
$bundle = Read-SingleFileBundle -Executable (Join-Path $stagingDir 'X.SuperResolution.exe')
if ($bundle.SelfContained -ne $selfContained -or -not $bundle.Files.ContainsKey('libSkiaSharp.dll') -or -not $bundle.Files.ContainsKey('lucifer_ncnn_vulkan.dll')) { throw 'Published bundle does not contain the expected runtime and native dependencies.' }
if ($selfContained -and -not (Test-Path -LiteralPath (Join-Path $stagingDir 'models\realesrgan-x4plus.bin'))) { throw 'Full publish is missing model files.' }

# Both outputs are fixed children of this repository; keep previous files recoverable.
$parentFull = [IO.Path]::GetFullPath($publishParent).TrimEnd('\') + '\'
foreach ($candidate in @($publishDir, $stagingDir)) {
    if (-not [IO.Path]::GetFullPath($candidate).StartsWith($parentFull, [StringComparison]::OrdinalIgnoreCase)) { throw 'Publish path escaped artifacts/publish.' }
    if ((Test-Path -LiteralPath $candidate) -and ((Get-Item -LiteralPath $candidate).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Publish directories cannot be links.' }
}
if (Test-Path -LiteralPath $publishDir) {
    $backup = $publishDir + '.previous-' + [Guid]::NewGuid().ToString('N')
    if (-not [IO.Path]::GetFullPath($backup).StartsWith($parentFull, [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid publish backup path.' }
    Move-Item -LiteralPath $publishDir -Destination $backup
}
Move-Item -LiteralPath $stagingDir -Destination $publishDir

if (-not $SkipArchive) {
    $packageDir = Join-Path $repoRoot 'artifacts\packages'
    New-Item -ItemType Directory -Path $packageDir -Force | Out-Null
    $archive = Join-Path $packageDir ("X.SuperResolution-win-x64-$($Flavor.ToLowerInvariant()).7z")
    $temporaryArchive = $archive + '.staging-' + [Guid]::NewGuid().ToString('N')
    Push-Location $publishDir
    try {
        & $sevenZip a -t7z -mx=9 $temporaryArchive '.\*'
        if ($LASTEXITCODE -ne 0) { throw "7-Zip failed ($LASTEXITCODE)." }
    } finally { Pop-Location }
    Move-Item -LiteralPath $temporaryArchive -Destination $archive -Force
    Write-Host "Archive: $archive"
}
Write-Host "Publish: $publishDir"
