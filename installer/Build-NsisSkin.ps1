[CmdletBinding()]
param(
    [string]$NsisDir = (Join-Path ${env:ProgramFiles(x86)} 'NSIS'),
    [switch]$Preview
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio C++ Build Tools and the Windows SDK are required to build the native installer skin.' }
$vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw 'Install the Desktop development with C++ workload (MSVC x86/x64 and Windows SDK).' }
$toolset = Get-ChildItem -LiteralPath (Join-Path $vsRoot 'VC\Tools\MSVC') -Directory | Sort-Object Name -Descending | Select-Object -First 1
$sdkRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
$sdk = Get-ChildItem -LiteralPath (Join-Path $sdkRoot 'Lib') -Directory | Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'um\x86\gdiplus.lib') } | Sort-Object Name -Descending | Select-Object -First 1
if (-not $sdk) { throw 'Windows 10/11 SDK with x86 libraries is required.' }
$compiler = Join-Path $toolset.FullName 'bin\Hostx64\x86\cl.exe'
$output = Join-Path $PSScriptRoot 'generated'
New-Item -ItemType Directory -Path $output -Force | Out-Null
$source = Join-Path $PSScriptRoot 'native\InstallerSkin.cpp'
$destination = Join-Path $output $(if ($Preview) { 'InstallerSkinPreview.exe' } else { 'InstallerSkin.dll' })
$dependencies = @($source, (Join-Path $PSScriptRoot 'native\InstallerSkin.def'), (Join-Path $output 'Generated.Theme.h'), $PSCommandPath)
if ((Test-Path -LiteralPath $destination) -and -not ($dependencies | Where-Object { (Get-Item -LiteralPath $_).LastWriteTimeUtc -gt (Get-Item -LiteralPath $destination).LastWriteTimeUtc })) { return }
$sdkInclude = Join-Path $sdkRoot "Include\$($sdk.Name)"
$arguments = @('/nologo', '/std:c++17', '/utf-8', '/W4', '/WX', '/O2', '/MT', '/EHsc', '/DUNICODE', '/D_UNICODE', '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN', '/D_WIN32_WINNT=0x0A00')
foreach ($include in @((Join-Path $toolset.FullName 'include'), (Join-Path $sdkInclude 'ucrt'), (Join-Path $sdkInclude 'shared'), (Join-Path $sdkInclude 'um'), (Join-Path $NsisDir 'Examples\Plugin\nsis'), $output)) { $arguments += "/I$include" }
$arguments += @($source, "/Fo$(Join-Path $output $(if ($Preview) { 'preview.obj' } else { 'skin.obj' }))")
if ($Preview) { $arguments += '/DXRAY_SKIN_PREVIEW' } else { $arguments += '/LD' }
$arguments += @('/link', '/MACHINE:X86', '/SUBSYSTEM:WINDOWS', '/DYNAMICBASE', '/NXCOMPAT', "/OUT:$destination", "/LIBPATH:$(Join-Path $toolset.FullName 'lib\x86')", "/LIBPATH:$(Join-Path $sdk.FullName 'ucrt\x86')", "/LIBPATH:$(Join-Path $sdk.FullName 'um\x86')", 'user32.lib', 'gdi32.lib', 'gdiplus.lib', 'shell32.lib', 'ole32.lib', 'uuid.lib', 'advapi32.lib', 'comctl32.lib', 'dwmapi.lib', 'shlwapi.lib')
if (-not $Preview) {
    $arguments += "/DEF:$(Join-Path $PSScriptRoot 'native\InstallerSkin.def')"
    $arguments += "/IMPLIB:$(Join-Path $output 'InstallerSkin.lib')"
}
& $compiler @arguments
if ($LASTEXITCODE -ne 0) { throw "Native skin compilation failed ($LASTEXITCODE)." }
