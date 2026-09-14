[CmdletBinding()]
param(
    [string]$ProjectPath,
    [string]$PublishDir,
    [string]$Configuration = 'Release',
    [string]$OutputDir
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (-not $ProjectPath) { $ProjectPath = Join-Path $PSScriptRoot '..\X.SuperResolution\X.SuperResolution.csproj' }
if (-not $PublishDir) { $PublishDir = Join-Path $PSScriptRoot '..\artifacts\publish\win-x64-full' }
if (-not $OutputDir) { $OutputDir = Join-Path $PSScriptRoot 'generated' }

function ConvertTo-NsisString([string]$Value) {
    if ($Value -match '[\r\n]') { throw 'NSIS metadata cannot contain line breaks.' }
    return $Value.Replace('$', '$$').Replace('"', '$\"')
}

function Write-GeneratedFile([string]$Path, [string[]]$Lines) {
    $content = ($Lines -join "`r`n") + "`r`n"
    if ((Test-Path -LiteralPath $Path) -and [IO.File]::ReadAllText($Path) -ceq $content) { return }
    [IO.File]::WriteAllText($Path, $content, [Text.UTF8Encoding]::new($true))
}

$project = (Resolve-Path -LiteralPath $ProjectPath).Path
$publish = (Resolve-Path -LiteralPath $PublishDir).Path.TrimEnd('\')
$repoRoot = Split-Path -Parent $PSScriptRoot
$output = [IO.Path]::GetFullPath($OutputDir)
New-Item -ItemType Directory -Path $output -Force | Out-Null
$propertyNames = 'AssemblyName,Product,Company,Authors,Copyright,RepositoryUrl,ApplicationIcon,TargetFramework'
$evaluated = & dotnet msbuild $project -nologo "-getProperty:$propertyNames" "-property:Configuration=$Configuration"
if ($LASTEXITCODE -ne 0) { throw 'MSBuild property evaluation failed.' }
$properties = (($evaluated -join "`n") | ConvertFrom-Json).Properties
$exeName = "$($properties.AssemblyName).exe"
$exe = Join-Path $publish $exeName
if (-not (Test-Path -LiteralPath $exe)) { throw "Published executable not found: $exe. Run scripts\build-installer.ps1 first." }
. (Join-Path $PSScriptRoot 'Read-SingleFileBundle.ps1')
$bundle = Read-SingleFileBundle -Executable $exe
if (-not $bundle.SelfContained -or -not $bundle.Files.ContainsKey('System.Private.CoreLib.dll') -or -not $bundle.Files.ContainsKey('libSkiaSharp.dll') -or -not $bundle.Files.ContainsKey("$($properties.AssemblyName).dll")) {
    throw 'A self-contained single-file publish with IncludeNativeLibrariesForSelfExtract=true is required.'
}
if (Test-Path -LiteralPath (Join-Path $publish "$($properties.AssemblyName).dll")) { throw 'The publish directory contains stale loose assemblies. Rebuild into a clean directory.' }
$pe = [IO.File]::OpenRead($exe)
try {
    $reader = [IO.BinaryReader]::new($pe)
    $pe.Position = 0x3c
    $pe.Position = $reader.ReadInt32() + 4
    if ($reader.ReadUInt16() -ne 0x8664) { throw 'The published application must target Windows x64.' }
} finally { $pe.Dispose() }

foreach ($required in @('vcomp140.dll','models\realesrgan-x4plus.bin','models\realesrgan-x4plus.param')) {
    if (-not (Test-Path -LiteralPath (Join-Path $publish $required))) { throw "Full publish is missing $required." }
}
if (-not $bundle.Files.ContainsKey('lucifer_ncnn_vulkan.dll')) { throw 'The single-file application must include the native NCNN engine.' }
$versionInfo = [Diagnostics.FileVersionInfo]::GetVersionInfo($exe)
$version = '{0}.{1}.{2}.{3}' -f $versionInfo.FileMajorPart, $versionInfo.FileMinorPart, $versionInfo.FileBuildPart, $versionInfo.FilePrivatePart
if ($version -eq '0.0.0.0') { throw 'Published executable has no usable file version.' }
$installerDir = Join-Path $repoRoot 'artifacts\installer'
New-Item -ItemType Directory -Path $installerDir -Force | Out-Null
$installerPath = Join-Path $installerDir "X.SuperResolution-Setup-$version-win-x64.exe"
$files = @(Get-ChildItem -LiteralPath $publish -File -Recurse | Where-Object {
    $_.Extension -notin @('.pdb', '.xml') -and $_.FullName -notmatch '\\refs\\'
} | Sort-Object FullName)
if (Get-ChildItem -LiteralPath $publish -Recurse -Attributes ReparsePoint) { throw 'Publish output cannot contain symbolic links or junctions.' }
if ($files.Name -contains 'Uninstall.exe' -or $files.Name -contains 'install-manifest.txt') { throw 'Publish output contains an installer-reserved filename.' }
$totalBytes = [long](($files | Measure-Object Length -Sum).Sum)
$requiredMb = [int][Math]::Ceiling($totalBytes / 1MB) + 32
$metadata = [ordered]@{
    APP_NAME = $properties.AssemblyName
    APP_PRODUCT = $properties.Product
    APP_EXE = $exeName
    APP_VERSION = $version
    APP_VERSION4 = $version
    APP_COMPANY = $properties.Company
    APP_AUTHORS = $properties.Authors
    APP_COPYRIGHT = $properties.Copyright
    APP_REPOSITORY_URL = $properties.RepositoryUrl
    APP_ICON = if ([IO.Path]::IsPathRooted($properties.ApplicationIcon)) { $properties.ApplicationIcon } else { [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $project) $properties.ApplicationIcon)) }
    APP_PUBLISH_DIR = $publish
    APP_INSTALLER_PATH = $installerPath
    APP_REQUIRED_MB = [string]$requiredMb
    APP_SIZE_KB = [string][int][Math]::Ceiling($totalBytes / 1KB)
}
$lines = @('; Generated from evaluated MSBuild properties and the published executable. Do not edit.')
foreach ($entry in $metadata.GetEnumerator()) { $lines += '!define {0} "{1}"' -f $entry.Key, (ConvertTo-NsisString $entry.Value) }
Write-GeneratedFile (Join-Path $output 'Generated.AppInfo.nsh') $lines

$install = [Collections.Generic.List[string]]::new()
$remove = [Collections.Generic.List[string]]::new()
$manifest = [Collections.Generic.List[string]]::new()
$directories = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$install.Add('!macro InstallPayload')
$remove.Add('!macro UninstallPayload')
$previousDir = $null
$processed = [long]0
foreach ($file in $files) {
    $relative = $file.FullName.Substring($publish.Length + 1)
    if ($relative.Length -gt 180) { throw "Payload path is too long for the installer: $relative" }
    $manifest.Add($relative)
    $directory = [IO.Path]::GetDirectoryName($relative)
    if ($directory -cne $previousDir) {
        $install.Add('    SetOutPath "$INSTDIR\{0}"' -f (ConvertTo-NsisString $directory))
        $previousDir = $directory
    }
    $install.Add('    File "{0}"' -f (ConvertTo-NsisString $file.FullName))
    $install.Add('    IfErrors install_failed')
    $processed += $file.Length
    $percent = 5 + [int][Math]::Floor(88 * $processed / [Math]::Max(1, $totalBytes))
    $install.Add(('    !insertmacro SkinProgress {0} "{1}"' -f $percent, (ConvertTo-NsisString $relative)))
    $remove.Add('    IfFileExists "$INSTDIR\{0}" 0 +2' -f (ConvertTo-NsisString $relative))
    $remove.Add('        Delete "$INSTDIR\{0}"' -f (ConvertTo-NsisString $relative))
    while ($directory) {
        [void]$directories.Add($directory)
        $directory = [IO.Path]::GetDirectoryName($directory)
    }
}
$install.Add('!macroend')
$remove.Add('!macroend')
$remove.Add('!macro RemoveEmptyPayloadDirectories')
foreach ($directory in ($directories | Sort-Object { $_.Length } -Descending)) {
    $remove.Add('    RMDir "$INSTDIR\{0}"' -f (ConvertTo-NsisString $directory))
}
$remove.Add('!macroend')
Write-GeneratedFile (Join-Path $output 'Generated.Payload.nsh') ($install.ToArray() + $remove.ToArray())
Write-GeneratedFile (Join-Path $output 'payload-manifest.txt') $manifest.ToArray()

# Use the application's semantic brushes with the same sidebar and accent-border tones as XXray.
[xml]$palette = Get-Content -LiteralPath (Join-Path $repoRoot 'X.SuperResolution\Assets\Themes\WorkbenchColors.axaml') -Raw
$themeLines = @('// Generated from WorkbenchColors.axaml. Do not edit.', '#pragma once')
$keys = @('AppBackgroundBrush','Sidebar','SurfaceBrush','ElevatedSurfaceBrush','HoverSurfaceBrush','BorderBrush','BorderStrongBrush','PrimaryBrush','PrimaryHoverBrush','SelectionSurfaceBrush','AccentBorder','AccentTextBrush','PrimaryContentBrush','WarningBrush','PrimaryBrush','PrimaryBrush','WarningBrush','WarningBrush','DangerBrush','DangerSurfaceBrush','DangerBorderBrush','TextPrimaryBrush','TextSecondaryBrush')
foreach ($themeName in @('Dark','Light')) {
    $dictionary = @($palette.SelectNodes('//*[local-name()="ResourceDictionary"]') | Where-Object { $_.GetAttribute('Key', 'http://schemas.microsoft.com/winfx/2006/xaml') -eq $themeName })
    if ($dictionary.Count -ne 1) { throw "Missing $themeName application palette." }
    $brushes = @{}
    foreach ($brush in $dictionary[0].SelectNodes('./*[local-name()="SolidColorBrush"]')) { $brushes[$brush.GetAttribute('Key', 'http://schemas.microsoft.com/winfx/2006/xaml')] = $brush.GetAttribute('Color') }
    $brushes['Sidebar'] = if ($themeName -eq 'Dark') { '#1B1B1B' } else { $brushes['SubtleSurfaceBrush'] }
    $brushes['AccentBorder'] = if ($themeName -eq 'Dark') { '#456851' } else { '#B5D6C2' }
    $colors = foreach ($key in $keys) {
        $color = $brushes[$key]
        if ($color -notmatch '^#[0-9A-Fa-f]{6}$') { throw "Invalid $themeName brush: $key" }
        '0xFF' + $color.Substring(1) + 'u'
    }
    $themeLines += 'inline constexpr unsigned int {0}Colors[] = {{ {1} }};' -f $themeName, ($colors -join ', ')
}
Write-GeneratedFile (Join-Path $output 'Generated.Theme.h') $themeLines
@{ InstallerPath = $installerPath; Version = $version; PublishDir = $publish; FileCount = $files.Count; PayloadBytes = $totalBytes; SingleFile = $true; SelfContained = $bundle.SelfContained } |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'build-info.json') -Encoding UTF8
Write-Host "Generated metadata: $version, $($files.Count) files, $requiredMb MB required."
