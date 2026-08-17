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

$supportedArtifactEditions = @{
    "standard-x64-portable" = "standard"
    "rescue-x64-portable" = "rescue"
    "large-offline-x64-portable" = "large-offline"
}
if (-not $supportedArtifactEditions.ContainsKey($ArtifactId)) {
    throw "Portable packaging supports only the three x64 portable artifact ids."
}

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$expectedEdition = [string]$supportedArtifactEditions[$ArtifactId]
$architecture = "x64"
$stagingDirectory = Join-Path $repositoryRoot "out/staging/portable/$ArtifactId"
$packageDirectory = Join-Path $repositoryRoot "out/packages"
$architectureToken = $architecture.ToLowerInvariant()
$packagePath = Join-Path $packageDirectory "Azzs-$expectedEdition-$architectureToken-portable.zip"
$manifestPath = Join-Path $repositoryRoot "out/manifests/package-$expectedEdition-$architectureToken-portable.json"
$fixedRescueFolderNames = @(
    "generic-network-driver",
    "offline-network-diagnostics"
)

function Remove-PortableCandidate {
    Assert-PortableCandidatePaths
    if (Test-Path -LiteralPath $stagingDirectory) {
        $stagingItem = Get-Item -LiteralPath $stagingDirectory -Force -ErrorAction Stop
        if (-not $stagingItem.PSIsContainer) {
            throw "Portable packaging staging candidate must be a directory: $stagingDirectory"
        }
    }
    foreach ($fileCandidatePath in @($packagePath, $manifestPath)) {
        if (Test-Path -LiteralPath $fileCandidatePath) {
            $fileCandidateItem = Get-Item -LiteralPath $fileCandidatePath -Force -ErrorAction Stop
            if ($fileCandidateItem.PSIsContainer) {
                throw "Portable packaging file candidate must be a file: $fileCandidatePath"
            }
        }
    }
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
    $definition = Get-PortableArtifactDefinition -RepositoryRoot $repositoryRoot -ArtifactId $ArtifactId
    $artifact = $definition.Artifact
    if ($artifact.edition -ne $expectedEdition -or $artifact.architecture -ne $architecture) {
        throw "Portable artifact identity does not match its fixed x64 candidate paths."
    }
    $bundledCatalogResources = @(Test-BundledCatalogResources -Content $definition.Content -RepositoryRoot $repositoryRoot -ArtifactId $ArtifactId -ValidateSource)
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
    Assert-NoReparsePointsBelow `
        -Path $payloadDirectory `
        -Context "Portable build payload"

    Assert-PortableCandidatePaths
    New-Item -ItemType Directory -Path $stagingDirectory, $packageDirectory -Force | Out-Null
    Assert-PortableCandidatePaths
    Copy-Item -Path (Join-Path $payloadDirectory "*") -Destination $stagingDirectory -Recurse -Force
    Assert-NoReparsePointsBelow `
        -Path $stagingDirectory `
        -Context "Portable staging payload"
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

    foreach ($resource in $bundledCatalogResources) {
        $resourcePackagePath = [string]$resource.packagePath
        $destinationPath = Join-Path $stagingDirectory ($resourcePackagePath.Replace("/", "\"))
        if ($stagedPaths.Contains($resourcePackagePath)) {
            if (-not (Test-Path -LiteralPath $destinationPath -PathType Leaf)) {
                throw "Bundled catalog resource '$($resource.id)' package path '$resourcePackagePath' is not a regular staged file."
            }
            $stagedResource = Get-Item -LiteralPath $destinationPath
            if ($stagedResource.Length -ne [Int64]$resource.bytes -or
                (Get-Sha256Hex -Path $destinationPath) -ne $resource.sha256) {
                throw "Bundled catalog resource '$($resource.id)' in the workbench payload does not match its locked content."
            }
            continue
        }
        $sourcePath = Resolve-RepositoryRelativePath -RepositoryRoot $repositoryRoot -RelativePath $resource.relativePath -Context "Bundled catalog resource '$($resource.id)'"
        New-Item -ItemType Directory -Path (Split-Path -Parent $destinationPath) -Force | Out-Null
        Copy-Item -LiteralPath $sourcePath -Destination $destinationPath -Force
        $stagedResource = Get-Item -LiteralPath $destinationPath
        if ($stagedResource.Length -ne [Int64]$resource.bytes -or
            (Get-Sha256Hex -Path $destinationPath) -ne $resource.sha256) {
            throw "Bundled catalog resource '$($resource.id)' was not copied with its locked content."
        }
        if (-not $stagedPaths.Add($resourcePackagePath)) {
            throw "Bundled catalog resource '$($resource.id)' packagePath '$resourcePackagePath' collides with the workbench payload."
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
        -BundledCatalogResources $bundledCatalogResources `
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
