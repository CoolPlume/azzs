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

. (Join-Path $PSScriptRoot "portable-artifact-content.ps1")

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$productVersionPath = Join-Path $repositoryRoot "release/product-version.json"
if (-not (Test-Path -LiteralPath $productVersionPath -PathType Leaf)) {
    throw "The authoritative product version source was not found: $productVersionPath"
}
try {
    $productVersion = Get-Content -LiteralPath $productVersionPath -Raw | ConvertFrom-Json
} catch {
    throw "The authoritative product version source is not valid JSON: $productVersionPath"
}
if ($productVersion.schemaVersion -ne 1 -or
    $productVersion.wixVersion -notmatch "^[0-9]+\.[0-9]+\.[0-9]+$") {
    throw "The authoritative product version source has an unsupported WiX version mapping."
}
if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot "build.ps1") -Architecture $Architecture
}

$payloadDirectory = Join-Path $repositoryRoot "out/windows/$Architecture/Release"
$executablePath = Join-Path $payloadDirectory "Azzs.WinUI.exe"
if (-not (Test-Path -LiteralPath $executablePath -PathType Leaf)) {
    throw "Installer packaging requires a completed $Architecture Release build."
}

$stagingDirectory = Join-Path $repositoryRoot "out/staging/installer/$Architecture"
$packageDirectory = Join-Path $repositoryRoot "out/packages"
$intermediateDirectory = Join-Path $repositoryRoot "out/obj/installer/$Architecture"
$intermediateDirectoryWithSeparator = "$intermediateDirectory\"
$installerPlatform = $Architecture.ToLowerInvariant()
$packagePath = Join-Path $packageDirectory "Azzs-standard-$installerPlatform-machine.msi"
$manifestPath = Join-Path $repositoryRoot "out/manifests/package-standard-$($Architecture.ToLowerInvariant())-machine.json"

foreach ($candidatePath in @($stagingDirectory, $packagePath, $manifestPath, $intermediateDirectory)) {
    Assert-PathChainWithoutReparsePoint `
        -Path $candidatePath `
        -Context "Installer packaging candidate '$candidatePath'"
}
if (Test-Path -LiteralPath $stagingDirectory) {
    $stagingItem = Get-Item -LiteralPath $stagingDirectory -Force -ErrorAction Stop
    if (-not $stagingItem.PSIsContainer) {
        throw "Installer packaging staging candidate must be a directory: $stagingDirectory"
    }
}
foreach ($fileCandidatePath in @($packagePath, $manifestPath)) {
    if (Test-Path -LiteralPath $fileCandidatePath) {
        $fileCandidateItem = Get-Item -LiteralPath $fileCandidatePath -Force -ErrorAction Stop
        if ($fileCandidateItem.PSIsContainer) {
            throw "Installer packaging file candidate must be a file: $fileCandidatePath"
        }
    }
}
if (Test-Path -LiteralPath $intermediateDirectory) {
    $intermediateItem = Get-Item -LiteralPath $intermediateDirectory -Force -ErrorAction Stop
    if (-not $intermediateItem.PSIsContainer) {
        throw "Installer packaging intermediate candidate must be a directory: $intermediateDirectory"
    }
}
if (Test-Path -LiteralPath $stagingDirectory) {
    Assert-NoReparsePointsBelow `
        -Path $stagingDirectory `
        -Context "Installer packaging staging candidate"
    Remove-Item -LiteralPath $stagingDirectory -Recurse -Force
}
if (Test-Path -LiteralPath $intermediateDirectory) {
    Assert-NoReparsePointsBelow `
        -Path $intermediateDirectory `
        -Context "Installer packaging intermediate candidate"
}
New-Item -ItemType Directory -Path $stagingDirectory, $packageDirectory, $intermediateDirectory -Force | Out-Null
Assert-NoReparsePointsBelow `
    -Path $payloadDirectory `
    -Context "Installer build payload"
Copy-Item -Path (Join-Path $payloadDirectory "*") -Destination $stagingDirectory -Recurse -Force
Assert-NoReparsePointsBelow `
    -Path $stagingDirectory `
    -Context "Installer staging payload"
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
    /p:AzzsWixVersion=$($productVersion.wixVersion) `
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
