<#
.SYNOPSIS
Assembles the release ZIP and the setup of the Portal Revolution VR mod.

.DESCRIPTION
The ZIP root is the game folder, so players extract it there:

  bin\win64\d3d11.dll                     build output
  VR\                                     package-files\ without the .cmd, thirdparty\openvr\bin\win64\openvr_api.dll
  VR\licenses\                            COPYING and the licenses of MinHook, OpenVR, Source SDK
  Uninstall_Portal_Revolution_VR_Mod.cmd  package-files\

The setup (tools\installer.iss) installs the same files, without the .cmd. It needs Inno Setup 6.

Build the configuration first. Players get Release: it writes the same log, but has no frame trace.

.EXAMPLE
tools\package-release.ps1 -Version 1.0
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'RelWithDebInfo', 'Debug')][string]$Configuration = 'Release',
    [string]$Version = ''
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

if (-not $Version) {
    $Version = git -C $root describe --tags --always --dirty 2>$null
    if (-not $Version) { $Version = 'dev' }
}

$dll = Join-Path $root "x64\$Configuration\d3d11.dll"
if (-not (Test-Path $dll)) { throw "$dll not found. Build the $Configuration configuration first (x64)." }

$name = "PortalRevolutionVR-$Version"
if ($Configuration -ne 'Release') { $name += "-$Configuration" }
$stage = Join-Path $root "dist\$name"
$zip = "$stage.zip"
$setup = "$stage-Setup.exe"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
if (Test-Path $zip) { Remove-Item -Force $zip }
if (Test-Path $setup) { Remove-Item -Force $setup }

$bin = Join-Path $stage 'bin\win64'
$vr = Join-Path $stage 'VR'
$licenses = Join-Path $vr 'licenses'
New-Item -ItemType Directory -Force $bin, $licenses | Out-Null

Copy-Item $dll $bin
$uninstall = 'Uninstall_Portal_Revolution_VR_Mod.cmd'
Copy-Item -Recurse (Join-Path $root 'package-files\*') $vr -Exclude $uninstall
Copy-Item (Join-Path $root 'thirdparty\openvr\bin\win64\openvr_api.dll') $vr
Copy-Item (Join-Path $root "package-files\$uninstall") $stage

# The GPL and the BSD licenses require these with a binary, the GPL also a way to the source.
Copy-Item (Join-Path $root 'COPYING') (Join-Path $licenses 'COPYING.txt')
Copy-Item (Join-Path $root 'thirdparty\minhook\LICENSE.txt') (Join-Path $licenses 'MinHook-LICENSE.txt')
Copy-Item (Join-Path $root 'thirdparty\openvr\LICENSE') (Join-Path $licenses 'OpenVR-LICENSE.txt')
Copy-Item (Join-Path $root 'src\sdk\LICENSE.txt') (Join-Path $licenses 'Source-SDK-LICENSE.txt')
Set-Content -Encoding ascii (Join-Path $licenses 'README.txt') @"
Portal Revolution VR $Version. Free of charge, licensed under the GNU GPL v3 (COPYING.txt).
Source code and exceptions to the license: https://github.com/stsichler/portal_revolution_vr
"@

Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip, [IO.Compression.CompressionLevel]::Optimal, $false)

$archive = [IO.Compression.ZipFile]::OpenRead($zip)
try { $archive.Entries | Where-Object { $_.Length -gt 0 } | ForEach-Object { '{0,10}  {1}' -f $_.Length, $_.FullName } }
finally { $archive.Dispose() }
"-> $zip"

$iscc = (Get-Command ISCC.exe -ErrorAction SilentlyContinue).Source
if (-not $iscc) {
    $iscc = "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe" |
        Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $iscc) { Write-Warning 'Inno Setup 6 not found: no setup.'; return }
& $iscc /Q "/DStage=$stage" "/DVersion=$Version" "/O$(Split-Path -Parent $stage)" "/F$name-Setup" (Join-Path $PSScriptRoot 'installer.iss')
if ($LASTEXITCODE -ne 0) { throw "ISCC failed ($LASTEXITCODE)" }
"-> $setup"
