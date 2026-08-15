[CmdletBinding()]
param(
    [string]$ArtifactId,

    [ValidateSet("x64", "ARM64")]
    [string]$Architecture,

    [switch]$SkipBuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "portable-artifact-content.ps1")

if ([string]::IsNullOrWhiteSpace($ArtifactId)) {
    $ArtifactId = if ($PSBoundParameters.ContainsKey("Architecture")) {
        "standard-$Architecture-portable"
    } else {
        "standard-x64-portable"
    }
}
elseif ($PSBoundParameters.ContainsKey("Architecture")) {
    throw "Portable packaging is ArtifactId-driven; do not combine -ArtifactId with -Architecture."
}

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$definition = Get-PortableArtifactDefinition -RepositoryRoot $repositoryRoot -ArtifactId $ArtifactId
$artifact = $definition.Artifact
$architecture = [string]$artifact.architecture
$stagingDirectory = Join-Path $repositoryRoot "out/staging/portable/$ArtifactId"
$packageDirectory = Join-Path $repositoryRoot "out/packages"
$architectureToken = $architecture.ToLowerInvariant()
$packagePath = Join-Path $packageDirectory "Azzs-$($artifact.edition)-$architectureToken-portable.zip"
$manifestPath = Join-Path $repositoryRoot "out/manifests/package-$($artifact.edition)-$architectureToken-portable.json"
$fixedRescueFolderNames = @(
    "generic-network-driver",
    "offline-network-diagnostics"
)

function Remove-PortableCandidate {
    Assert-PortableCandidatePaths
    foreach ($candidatePath in @($stagingDirectory, $packagePath, $manifestPath)) {
        if (Test-Path -LiteralPath $candidatePath) {
            Remove-Item -LiteralPath $candidatePath -Recurse -Force
        }
    }
}

function Assert-PortableCandidatePaths {
    foreach ($candidatePath in @($stagingDirectory, $packagePath, $manifestPath)) {
        Assert-PathChainWithoutReparsePoint `
            -Path $candidatePath `
            -Context "Portable packaging candidate '$candidatePath'"
    }
}

Remove-PortableCandidate
try {
    $contentInputs = Test-PortableArtifactInputs -Definition $definition -RepositoryRoot $repositoryRoot
    if ($artifact.edition -eq "large-offline") {
        $rescueDefinition = Get-PortableArtifactDefinition -RepositoryRoot $repositoryRoot -ArtifactId "rescue-x64-portable"
        $rescueInputs = Test-PortableArtifactInputs -Definition $rescueDefinition -RepositoryRoot $repositoryRoot
        Test-LargeOfflineArtifactContent -LargeDefinition $definition -LargeInputs $contentInputs -RescueInputs $rescueInputs -RepositoryRoot $repositoryRoot
    }
    if (-not $SkipBuild) {
        & (Join-Path $PSScriptRoot "build.ps1") -Architecture $architecture
    }
    Test-PortableBuildManifest -RepositoryRoot $repositoryRoot -Architecture $architecture | Out-Null

    $payloadDirectory = Join-Path $repositoryRoot "out/windows/$architecture/Release"
    $executablePath = Join-Path $payloadDirectory "Azzs.WinUI.exe"
    if (-not (Test-Path -LiteralPath $executablePath -PathType Leaf)) {
        throw "Portable packaging requires a completed $architecture Release build."
    }

    Assert-PortableCandidatePaths
    New-Item -ItemType Directory -Path $stagingDirectory, $packageDirectory -Force | Out-Null
    Assert-PortableCandidatePaths
    Copy-Item -Path (Join-Path $payloadDirectory "*") -Destination $stagingDirectory -Recurse -Force
    $excludedExtensions = @(".pdb", ".ilk", ".iobj", ".ipdb", ".exp", ".lib")
    $excludedFiles = @(
        Get-ChildItem -LiteralPath $stagingDirectory -File -Recurse |
            Where-Object { $excludedExtensions -contains $_.Extension.ToLowerInvariant() }
    )
    if ($excludedFiles.Count -gt 0) {
        $excludedFiles | Remove-Item -Force
    }

    $stagingRoot = (Resolve-Path -LiteralPath $stagingDirectory).Path.TrimEnd("\", "/")
    $stagedPaths = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($stagedFile in Get-ChildItem -LiteralPath $stagingRoot -File -Recurse) {
        $stagedPath = $stagedFile.FullName.Substring($stagingRoot.Length).TrimStart("\", "/").Replace("\", "/")
        if (-not $stagedPaths.Add($stagedPath)) {
            throw "Portable workbench payload contains a duplicate path '$stagedPath'."
        }
    }

    foreach ($contentInput in $contentInputs) {
        $sourcePath = Resolve-RepositoryRelativePath -RepositoryRoot $repositoryRoot -RelativePath $contentInput.relativePath -Context "Locked input '$($contentInput.id)'"
        $inputPackagePath = [string]$contentInput.packagePath
        if (-not $stagedPaths.Add($inputPackagePath)) {
            throw "Locked input '$($contentInput.id)' packagePath '$inputPackagePath' collides with the workbench payload."
        }
        $destinationPath = Join-Path $stagingDirectory ($inputPackagePath.Replace("/", "\"))
        New-Item -ItemType Directory -Path (Split-Path -Parent $destinationPath) -Force | Out-Null
        Copy-Item -LiteralPath $sourcePath -Destination $destinationPath -Force
    }

    # These two directories are fixed external handoff destinations. They are
    # deliberately empty and do not make any rescue artifact an input.
    foreach ($folderName in $fixedRescueFolderNames) {
        New-Item -ItemType Directory -Path (Join-Path $stagingDirectory "rescue-tools/$folderName") -Force | Out-Null
    }

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [System.IO.Compression.ZipFile]::CreateFromDirectory(
        $stagingRoot,
        $packagePath,
        [System.IO.Compression.CompressionLevel]::Optimal,
        $false)

    & (Join-Path $PSScriptRoot "write-package-manifest.ps1") `
        -Kind portable `
        -Architecture $architecture `
        -RepositoryRoot $repositoryRoot `
        -PayloadDirectory $stagingDirectory `
        -PackagePath $packagePath `
        -OutputPath $manifestPath `
        -ArtifactId $ArtifactId `
        -Edition $artifact.edition `
        -ContentManifestPath $definition.ContentManifestPath `
        -Inputs $contentInputs
    & (Join-Path $PSScriptRoot "verify-portable-package.ps1") `
        -ArtifactId $ArtifactId `
        -RepositoryRoot $repositoryRoot `
        -StagingDirectory $stagingDirectory `
        -PackagePath $packagePath `
        -ManifestPath $manifestPath
}
catch {
    Remove-PortableCandidate
    throw
}

Write-Host "Portable package: $packagePath"
Write-Host "Package manifest: $manifestPath"
