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
        path = $PackagePath
        bytes = (Get-Item -LiteralPath $PackagePath).Length
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
