[CmdletBinding()]
param(
    [ValidateSet("x64", "ARM64")]
    [string]$Architecture = "x64",

    [switch]$SkipBuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot "build.ps1") -Architecture $Architecture
}

$payloadDirectory = Join-Path $repositoryRoot "out/windows/$Architecture/Release"
$executablePath = Join-Path $payloadDirectory "Azzs.WinUI.exe"
if (-not (Test-Path -LiteralPath $executablePath)) {
    throw "Portable packaging requires a completed $Architecture Release build."
}

$stagingDirectory = Join-Path $repositoryRoot "out/staging/portable/$Architecture"
$packageDirectory = Join-Path $repositoryRoot "out/packages"
$packagePath = Join-Path $packageDirectory "Azzs-standard-$Architecture-portable.zip"
$manifestPath = Join-Path $repositoryRoot "out/manifests/package-standard-$($Architecture.ToLowerInvariant())-portable.json"

if (Test-Path -LiteralPath $stagingDirectory) {
    Remove-Item -LiteralPath $stagingDirectory -Recurse -Force
}
New-Item -ItemType Directory -Path $stagingDirectory, $packageDirectory -Force | Out-Null
Copy-Item -Path (Join-Path $payloadDirectory "*") -Destination $stagingDirectory -Recurse -Force
Get-ChildItem -LiteralPath $stagingDirectory -File -Recurse -Include *.pdb, *.ilk, *.iobj, *.ipdb, *.exp, *.lib | Remove-Item -Force

if (Test-Path -LiteralPath $packagePath) {
    Remove-Item -LiteralPath $packagePath -Force
}
Compress-Archive -Path (Join-Path $stagingDirectory "*") -DestinationPath $packagePath -CompressionLevel Optimal

& (Join-Path $PSScriptRoot "write-package-manifest.ps1") -Kind portable -Architecture $Architecture -RepositoryRoot $repositoryRoot -PayloadDirectory $stagingDirectory -PackagePath $packagePath -OutputPath $manifestPath
Write-Host "Portable package: $packagePath"
Write-Host "Package manifest: $manifestPath"
