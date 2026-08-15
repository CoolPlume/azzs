[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("portable", "machine-installer")]
    [string]$Kind,

    [Parameter(Mandatory = $true)]
    [ValidateSet("x64", "ARM64")]
    [string]$Architecture,

    [Parameter(Mandatory = $true)]
    [string]$RepositoryRoot,

    [Parameter(Mandatory = $true)]
    [string]$PayloadDirectory,

    [Parameter(Mandatory = $true)]
    [string]$PackagePath,

    [Parameter(Mandatory = $true)]
    [string]$OutputPath,

    [string]$ArtifactId = "",

    [string]$Edition = "",

    [string]$ContentManifestPath = "",

    [object[]]$Inputs = @()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "portable-artifact-content.ps1")

function ConvertTo-CanonicalRepositoryPackagePath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepositoryRoot,

        [Parameter(Mandatory = $true)]
        [string]$PackagePath
    )

    $repositoryItem = Get-Item -LiteralPath $RepositoryRoot -ErrorAction Stop
    if (-not $repositoryItem.PSIsContainer) {
        throw "Package manifest repository root must be a directory."
    }
    $packageItem = Get-Item -LiteralPath $PackagePath -ErrorAction Stop
    if ($packageItem.PSIsContainer) {
        throw "Package manifest package path must name a file."
    }

    $repository = (Get-ExistingNonReparsePath -Path $repositoryItem.FullName -Context "Package manifest repository root").TrimEnd([char[]]@('\', '/'))
    $package = Get-ExistingNonReparsePath -Path $packageItem.FullName -Context "Package manifest package path"
    $repositoryPrefix = "$repository$([System.IO.Path]::DirectorySeparatorChar)"
    if (-not $package.StartsWith($repositoryPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Package manifest package path must be strictly inside the repository."
    }

    $relative = $package.Substring($repositoryPrefix.Length)
    if ([string]::IsNullOrWhiteSpace($relative) -or
        [System.IO.Path]::IsPathRooted($relative) -or
        $relative -match "^[A-Za-z]:") {
        throw "Package manifest package path must be a non-rooted repository-relative path."
    }
    $segments = $relative.Split([char[]]@('\', '/'))
    if ($segments.Count -eq 0 -or @($segments | Where-Object {
            [string]::IsNullOrWhiteSpace($_) -or $_ -eq "." -or $_ -eq ".."
        }).Count -gt 0) {
        throw "Package manifest package path contains an unsafe relative segment."
    }

    return [pscustomobject]@{
        repository = $repository
        package = $package
        relativePackage = ($segments -join "/")
        packageItem = $packageItem
    }
}

$canonicalPaths = ConvertTo-CanonicalRepositoryPackagePath -RepositoryRoot $RepositoryRoot -PackagePath $PackagePath
$RepositoryRoot = $canonicalPaths.repository
$PackagePath = $canonicalPaths.package

$payload = @(
    Get-ChildItem -LiteralPath $PayloadDirectory -File -Recurse |
        Sort-Object FullName |
        ForEach-Object {
            [ordered]@{
                path = $_.FullName.Substring($PayloadDirectory.Length).TrimStart("\", "/").Replace("\", "/")
                bytes = $_.Length
                sha256 = Get-Sha256Hex -Path $_.FullName
            }
        }
)

$manifest = [ordered]@{
    schemaVersion = 1
    generatedAtUtc = [DateTime]::UtcNow.ToString("o")
    sourceCommit = (& git -C $RepositoryRoot rev-parse HEAD).Trim()
    kind = $Kind
    architecture = $Architecture
    package = [ordered]@{
        path = $canonicalPaths.relativePackage
        bytes = $canonicalPaths.packageItem.Length
    }
    payload = $payload
}

if (-not [string]::IsNullOrWhiteSpace($ArtifactId)) {
    if ([string]::IsNullOrWhiteSpace($Edition) -or [string]::IsNullOrWhiteSpace($ContentManifestPath)) {
        throw "Artifact package manifests require edition and content manifest metadata."
    }
    if (-not (Test-Path -LiteralPath $ContentManifestPath -PathType Leaf)) {
        throw "Artifact content manifest is missing: $ContentManifestPath"
    }
    $contentManifest = Get-Content -LiteralPath $ContentManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($contentManifest.schemaVersion -ne 1) {
        throw "Artifact content manifest schemaVersion must be 1."
    }
    $manifest.artifactId = $ArtifactId
    $manifest.edition = $Edition
    $manifest.contentManifest = [ordered]@{
        schemaVersion = $contentManifest.schemaVersion
        path = ConvertTo-RepositoryRelativePath -RepositoryRoot $RepositoryRoot -Path $ContentManifestPath -Context "Artifact content manifest"
        sha256 = Get-Sha256Hex -Path $ContentManifestPath
    }
    $manifest.inputs = @(
        foreach ($contentInput in $Inputs) {
            $manifestInput = [ordered]@{
                id = $contentInput.id
                role = $contentInput.role
                version = $contentInput.version
                architecture = $contentInput.architecture
                relativePath = $contentInput.relativePath
                packagePath = $contentInput.packagePath
                bytes = $contentInput.bytes
                sha256 = $contentInput.sha256
                license = $contentInput.license
                source = $contentInput.source
                securityClassification = $contentInput.securityClassification
            }
            $rescueEvidence = $contentInput.PSObject.Properties["rescueEvidence"]
            if ($null -ne $rescueEvidence) {
                $manifestInput["rescueEvidence"] = [ordered]@{
                    sourcePath = $rescueEvidence.Value.sourcePath
                    reproducibleBuildPath = $rescueEvidence.Value.reproducibleBuildPath
                    minimalSmokePath = $rescueEvidence.Value.minimalSmokePath
                    processTokenContractPath = $rescueEvidence.Value.processTokenContractPath
                }
            }
            $manifestInput
        }
    )
}

$outputDirectory = Split-Path -Parent $OutputPath
New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
$manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $OutputPath -Encoding UTF8
