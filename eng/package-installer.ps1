[CmdletBinding()]
param(
    [ValidateSet("x64", "ARM64")]
    [string]$Architecture = "x64",

    [switch]$SkipBuild,

    [switch]$AcceptWixEula
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if (-not $AcceptWixEula) {
    throw "WiX Toolset 7 requires an explicit terms decision. Re-run with -AcceptWixEula only after confirming the WiX 7 terms for this use."
}

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot "build.ps1") -Architecture $Architecture
}

$payloadDirectory = Join-Path $repositoryRoot "out/windows/$Architecture/Release"
$executablePath = Join-Path $payloadDirectory "Azzs.WinUI.exe"
if (-not (Test-Path -LiteralPath $executablePath)) {
    throw "Installer packaging requires a completed $Architecture Release build."
}

$stagingDirectory = Join-Path $repositoryRoot "out/staging/installer/$Architecture"
$packageDirectory = Join-Path $repositoryRoot "out/packages"
$intermediateDirectory = Join-Path $repositoryRoot "out/obj/installer/$Architecture"
$intermediateDirectoryWithSeparator = "$intermediateDirectory\"
$installerPlatform = $Architecture.ToLowerInvariant()
$packagePath = Join-Path $packageDirectory "Azzs-standard-$installerPlatform-machine.msi"
$manifestPath = Join-Path $repositoryRoot "out/manifests/package-standard-$($Architecture.ToLowerInvariant())-machine.json"

if (Test-Path -LiteralPath $stagingDirectory) {
    Remove-Item -LiteralPath $stagingDirectory -Recurse -Force
}
New-Item -ItemType Directory -Path $stagingDirectory, $packageDirectory, $intermediateDirectory -Force | Out-Null
Copy-Item -Path (Join-Path $payloadDirectory "*") -Destination $stagingDirectory -Recurse -Force
Get-ChildItem -LiteralPath $stagingDirectory -File -Recurse -Include *.pdb, *.ilk, *.iobj, *.ipdb, *.exp, *.lib | Remove-Item -Force

$vswherePath = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio/Installer/vswhere.exe"
if (-not (Test-Path -LiteralPath $vswherePath)) {
    throw "vswhere.exe was not found. Install Visual Studio 2026 before packaging."
}
$instanceJson = & $vswherePath -latest -products * -version "[18.8,18.9)" -requires Microsoft.Component.MSBuild -format json -utf8
if ($LASTEXITCODE -ne 0 -or -not $instanceJson) {
    throw "Visual Studio 2026 Stable 18.8.2 was not found."
}
$visualStudioInstance = @(ConvertFrom-Json ($instanceJson -join [Environment]::NewLine))[0]
if ($visualStudioInstance.catalog.productDisplayVersion -ne "18.8.2") {
    throw "Visual Studio 2026 Stable 18.8.2 is required; found '$($visualStudioInstance.catalog.productDisplayVersion)'."
}
$msbuildPath = Join-Path $visualStudioInstance.installationPath "MSBuild/Current/Bin/MSBuild.exe"

& $msbuildPath (Join-Path $repositoryRoot "installer/Azzs.Installer.wixproj") `
    /restore /m /t:Rebuild `
    /p:Configuration=Release `
    /p:Platform=$installerPlatform `
    /p:InstallerPlatform=$installerPlatform `
    /p:PayloadDirectory=$stagingDirectory `
    /p:AzzsPackageOutputDirectory=$packageDirectory `
    /p:AzzsInstallerIntermediateDirectory=$intermediateDirectoryWithSeparator `
    /p:BaseIntermediateOutputPath=$intermediateDirectoryWithSeparator `
    /p:MSBuildProjectExtensionsPath=$intermediateDirectoryWithSeparator `
    /p:AcceptEula=wix7
if ($LASTEXITCODE -ne 0) {
    throw "WiX installer build failed with exit code $LASTEXITCODE."
}
if (-not (Test-Path -LiteralPath $packagePath)) {
    throw "WiX completed without the expected MSI: $packagePath"
}

& (Join-Path $PSScriptRoot "write-package-manifest.ps1") -Kind machine-installer -Architecture $Architecture -RepositoryRoot $repositoryRoot -PayloadDirectory $stagingDirectory -PackagePath $packagePath -OutputPath $manifestPath
Write-Host "Machine installer: $packagePath"
Write-Host "Package manifest: $manifestPath"
