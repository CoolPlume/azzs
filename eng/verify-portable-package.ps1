[CmdletBinding()]
param(
    [string]$ArtifactId,

    [ValidateSet("x64", "ARM64")]
    [string]$Architecture,

    [string]$RepositoryRoot = "",

    [Parameter(Mandatory = $true)]
    [string]$StagingDirectory,

    [Parameter(Mandatory = $true)]
    [string]$PackagePath,

    [Parameter(Mandatory = $true)]
    [string]$ManifestPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "portable-artifact-content.ps1")

if ([string]::IsNullOrWhiteSpace($RepositoryRoot)) {
    $RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
}
if ([string]::IsNullOrWhiteSpace($ArtifactId)) {
    $ArtifactId = if ($PSBoundParameters.ContainsKey("Architecture")) {
        "standard-$Architecture-portable"
    } else {
        "standard-x64-portable"
    }
}
elseif ($PSBoundParameters.ContainsKey("Architecture")) {
    throw "Portable package verification is ArtifactId-driven; do not combine -ArtifactId with -Architecture."
}

$definition = Get-PortableArtifactDefinition -RepositoryRoot $RepositoryRoot -ArtifactId $ArtifactId
$artifact = $definition.Artifact
$Architecture = [string]$artifact.architecture
$bundledCatalogResources = @(Test-BundledCatalogResources -Content $definition.Content -RepositoryRoot $RepositoryRoot -ArtifactId $ArtifactId -ValidateSource)
$contentInputs = Test-PortableArtifactInputs -Definition $definition -RepositoryRoot $RepositoryRoot
if ($artifact.edition -eq "large-offline") {
    $rescueDefinition = Get-PortableArtifactDefinition -RepositoryRoot $RepositoryRoot -ArtifactId "rescue-x64-portable"
    $rescueInputs = Test-PortableArtifactInputs -Definition $rescueDefinition -RepositoryRoot $RepositoryRoot
    Test-LargeOfflineArtifactContent -LargeDefinition $definition -LargeInputs $contentInputs -RescueInputs $rescueInputs -RepositoryRoot $RepositoryRoot
}
Test-PortableBuildManifest -RepositoryRoot $RepositoryRoot -Architecture $Architecture | Out-Null

function ConvertTo-PayloadMap {
    param(
        [Parameter(Mandatory = $true)]
        [object[]]$Payload,

        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    $payloadMap = [System.Collections.Generic.Dictionary[string, Int64]]::new(
        [System.StringComparer]::OrdinalIgnoreCase
    )
    foreach ($file in $Payload) {
        $path = ([string]$file.path).Replace('\', '/')
        if ([string]::IsNullOrWhiteSpace($path)) {
            throw "$Name contains an empty path."
        }
        if ($payloadMap.ContainsKey($path)) {
            throw "$Name contains duplicate path '$path'."
        }
        $payloadMap.Add($path, [Int64]$file.bytes)
    }
    return $payloadMap
}

function Assert-PayloadMapMatches {
    param(
        [Parameter(Mandatory = $true)]
        [System.Collections.Generic.Dictionary[string, Int64]]$Expected,

        [Parameter(Mandatory = $true)]
        [System.Collections.Generic.Dictionary[string, Int64]]$Actual,

        [Parameter(Mandatory = $true)]
        [string]$ExpectedName,

        [Parameter(Mandatory = $true)]
        [string]$ActualName
    )

    if ($Actual.Count -ne $Expected.Count) {
        throw "$ActualName file count does not match $ExpectedName."
    }
    foreach ($entry in $Expected.GetEnumerator()) {
        if (-not $Actual.ContainsKey($entry.Key)) {
            throw "$ActualName is missing '$($entry.Key)' from $ExpectedName."
        }
        if ($Actual[$entry.Key] -ne $entry.Value) {
            throw "$ActualName byte count does not match $ExpectedName at '$($entry.Key)'."
        }
    }
}

function Get-ArchiveEntrySha256 {
    param(
        [Parameter(Mandatory = $true)]
        [System.IO.Compression.ZipArchiveEntry]$Entry
    )

    $stream = $Entry.Open()
    $algorithm = [System.Security.Cryptography.SHA256]::Create()
    try {
        return (($algorithm.ComputeHash($stream) | ForEach-Object { $_.ToString("x2") }) -join "")
    }
    finally {
        $algorithm.Dispose()
        $stream.Dispose()
    }
}

function ConvertTo-PayloadHashMap {
    param(
        [Parameter(Mandatory = $true)]
        [object[]]$Payload,

        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    $payloadMap = [System.Collections.Generic.Dictionary[string, string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase
    )
    foreach ($file in $Payload) {
        $path = ([string]$file.path).Replace('\', '/')
        $sha256 = [string](Get-RequiredProperty -Object $file -Name "sha256" -Context $Name)
        if ($sha256 -notmatch "^[a-fA-F0-9]{64}$") {
            throw "$Name has an invalid SHA256 for '$path'."
        }
        if ($payloadMap.ContainsKey($path)) {
            throw "$Name contains duplicate path '$path'."
        }
        $payloadMap.Add($path, $sha256.ToLowerInvariant())
    }
    return $payloadMap
}

function Assert-PayloadHashMapMatches {
    param(
        [Parameter(Mandatory = $true)]
        [System.Collections.Generic.Dictionary[string, string]]$Expected,

        [Parameter(Mandatory = $true)]
        [System.Collections.Generic.Dictionary[string, string]]$Actual,

        [Parameter(Mandatory = $true)]
        [string]$ExpectedName,

        [Parameter(Mandatory = $true)]
        [string]$ActualName
    )

    if ($Actual.Count -ne $Expected.Count) {
        throw "$ActualName SHA256 count does not match $ExpectedName."
    }
    foreach ($entry in $Expected.GetEnumerator()) {
        if (-not $Actual.ContainsKey($entry.Key) -or $Actual[$entry.Key] -ne $entry.Value) {
            throw "$ActualName SHA256 does not match $ExpectedName at '$($entry.Key)'."
        }
    }
}

if (-not (Test-Path -LiteralPath $StagingDirectory -PathType Container)) {
    throw "Portable package staging directory is missing: $StagingDirectory"
}
if (-not (Test-Path -LiteralPath $PackagePath -PathType Leaf)) {
    throw "Portable package ZIP is missing: $PackagePath"
}
if (-not (Test-Path -LiteralPath $ManifestPath -PathType Leaf)) {
    throw "Portable package manifest is missing: $ManifestPath"
}

$StagingDirectory = Get-ExistingNonReparsePath `
    -Path $StagingDirectory `
    -Context "Portable package staging directory"
$PackagePath = Get-ExistingNonReparsePath `
    -Path $PackagePath `
    -Context "Portable package ZIP"
$ManifestPath = Get-ExistingNonReparsePath `
    -Path $ManifestPath `
    -Context "Portable package manifest"
$stagingRoot = (Assert-NoReparsePointsBelow `
        -Path $StagingDirectory `
        -Context "Portable package staging payload").TrimEnd('\', '/')
$fixedRescueFolderPaths = @(
    "rescue-tools/generic-network-driver",
    "rescue-tools/offline-network-diagnostics"
)
foreach ($relativeFolder in $fixedRescueFolderPaths) {
    $folderPath = Join-Path $stagingRoot ($relativeFolder.Replace("/", "\"))
    if (-not (Test-Path -LiteralPath $folderPath -PathType Container)) {
        throw "Portable package staging directory is missing fixed rescue folder '$relativeFolder'."
    }
    if (@(Get-ChildItem -LiteralPath $folderPath -Force).Count -ne 0) {
        throw "Portable package fixed rescue folder '$relativeFolder' must remain empty."
    }
}
$stagedPayload = @(
    Get-ChildItem -LiteralPath $stagingRoot -File -Recurse |
        ForEach-Object {
            [ordered]@{
                path = $_.FullName.Substring($stagingRoot.Length).TrimStart('\', '/').Replace('\', '/')
                bytes = $_.Length
                sha256 = Get-Sha256Hex -Path $_.FullName
            }
        }
)
if ($stagedPayload.Count -eq 0) {
    throw "Portable package staging payload is empty."
}
$excludedExtensions = @(".pdb", ".ilk", ".iobj", ".ipdb", ".exp", ".lib")
if (@($stagedPayload | Where-Object {
            $excludedExtensions -contains [System.IO.Path]::GetExtension($_.path).ToLowerInvariant()
        }).Count -gt 0) {
    throw "Portable package staging payload contains excluded build artifacts."
}

$requiredRuntimeFiles = @(
    "Azzs.WinUI.exe",
    "Microsoft.ui.xaml.dll",
    "Microsoft.WindowsAppRuntime.Bootstrap.dll",
    "Microsoft.WindowsAppRuntime.dll"
)
foreach ($requiredFile in $requiredRuntimeFiles) {
    if (-not ($stagedPayload.path -contains $requiredFile)) {
        throw "Portable package staging payload is missing $requiredFile."
    }
}

$executablePath = Join-Path $stagingRoot "Azzs.WinUI.exe"
$executableBytes = [System.IO.File]::ReadAllBytes($executablePath)
if ($executableBytes.Length -lt 64 -or $executableBytes[0] -ne 0x4d -or $executableBytes[1] -ne 0x5a) {
    throw "Portable package entry executable is not a PE image."
}
$peOffset = [System.BitConverter]::ToInt32($executableBytes, 0x3c)
if ($peOffset -lt 0 -or $peOffset + 6 -gt $executableBytes.Length -or
    $executableBytes[$peOffset] -ne 0x50 -or $executableBytes[$peOffset + 1] -ne 0x45) {
    throw "Portable package entry executable has an invalid PE header."
}
$expectedMachine = if ($Architecture -eq "x64") { 0x8664 } else { 0xaa64 }
if ([System.BitConverter]::ToUInt16($executableBytes, $peOffset + 4) -ne $expectedMachine) {
    throw "Portable package entry executable architecture does not match $Architecture."
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [System.IO.Compression.ZipFile]::OpenRead($PackagePath)
try {
    $archiveEntries = @($archive.Entries | ForEach-Object { $_.FullName.Replace('\', '/') })
    $archivePayload = @(
        $archive.Entries |
            Where-Object {
                -not $_.FullName.Replace('\', '/').EndsWith("/", [StringComparison]::Ordinal)
            } |
            ForEach-Object {
                [ordered]@{
                    path = $_.FullName.Replace('\', '/')
                    bytes = $_.Length
                    sha256 = Get-ArchiveEntrySha256 -Entry $_
                }
            }
    )
}
finally {
    $archive.Dispose()
}
foreach ($relativeFolder in $fixedRescueFolderPaths) {
    $directoryEntry = "$relativeFolder/"
    if (@($archiveEntries | Where-Object { $_ -eq $directoryEntry }).Count -ne 1) {
        throw "Portable package ZIP must contain exactly one fixed empty rescue folder '$relativeFolder'."
    }
    if (@($archiveEntries | Where-Object {
                $_ -ne $directoryEntry -and $_.StartsWith($directoryEntry, [StringComparison]::OrdinalIgnoreCase)
            }).Count -ne 0) {
        throw "Portable package ZIP fixed rescue folder '$relativeFolder' must not contain additional entries."
    }
}

$stagedPayloadMap = ConvertTo-PayloadMap -Payload $stagedPayload -Name "Portable package staging payload"
$archivePayloadMap = ConvertTo-PayloadMap -Payload $archivePayload -Name "Portable package ZIP payload"
Assert-PayloadMapMatches -Expected $stagedPayloadMap -Actual $archivePayloadMap -ExpectedName "staging payload" -ActualName "Portable package ZIP"
$stagedPayloadHashMap = ConvertTo-PayloadHashMap -Payload $stagedPayload -Name "Portable package staging payload"
$archivePayloadHashMap = ConvertTo-PayloadHashMap -Payload $archivePayload -Name "Portable package ZIP payload"
Assert-PayloadHashMapMatches -Expected $stagedPayloadHashMap -Actual $archivePayloadHashMap -ExpectedName "staging payload" -ActualName "Portable package ZIP"
foreach ($resource in $bundledCatalogResources) {
    $packagePath = [string]$resource.packagePath
    if (-not $stagedPayloadMap.ContainsKey($packagePath) -or
        $stagedPayloadMap[$packagePath] -ne [Int64]$resource.bytes -or
        -not $stagedPayloadHashMap.ContainsKey($packagePath) -or
        $stagedPayloadHashMap[$packagePath] -ne $resource.sha256 -or
        -not $archivePayloadMap.ContainsKey($packagePath) -or
        $archivePayloadMap[$packagePath] -ne [Int64]$resource.bytes -or
        -not $archivePayloadHashMap.ContainsKey($packagePath) -or
        $archivePayloadHashMap[$packagePath] -ne $resource.sha256) {
        throw "Portable package bundled catalog resource '$($resource.id)' is missing or does not match its locked content."
    }
}

$manifest = Get-Content -LiteralPath $ManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
if ($manifest.kind -ne "portable" -or $manifest.architecture -ne $Architecture) {
    throw "Portable package manifest kind or architecture does not match the requested package."
}
$manifestPackagePath = [string](Get-RequiredProperty -Object $manifest.package -Name "path" -Context "Portable package manifest package")
$expectedPackageRelativePath = ConvertTo-RepositoryRelativePath -RepositoryRoot $RepositoryRoot -Path $PackagePath -Context "Portable package ZIP"
$packageSegments = $manifestPackagePath.Split("/")
if ([string]::IsNullOrWhiteSpace($manifestPackagePath) -or
    [System.IO.Path]::IsPathRooted($manifestPackagePath) -or
    $manifestPackagePath.Contains("\") -or
    $manifestPackagePath -match "^[A-Za-z]:" -or
    @($packageSegments | Where-Object {
        [string]::IsNullOrWhiteSpace($_) -or $_ -eq "." -or $_ -eq ".."
    }).Count -gt 0 -or
    $manifestPackagePath -ne $expectedPackageRelativePath) {
    throw "Portable package manifest package path is not the canonical repository-relative ZIP path."
}
$expectedContentManifestPath = ConvertTo-RepositoryRelativePath -RepositoryRoot $RepositoryRoot -Path $definition.ContentManifestPath -Context "Artifact content manifest"
if ($manifest.schemaVersion -ne 1 -or
    $manifest.artifactId -ne $ArtifactId -or
    $manifest.edition -ne $artifact.edition -or
    $manifest.sourceCommit -ne ((& git -C $RepositoryRoot rev-parse HEAD).Trim()) -or
    $null -eq $manifest.contentManifest -or
    $manifest.contentManifest.schemaVersion -ne 1 -or
    $manifest.contentManifest.path -ne $expectedContentManifestPath -or
    $manifest.contentManifest.sha256 -ne (Get-Sha256Hex -Path $definition.ContentManifestPath)) {
    throw "Portable package manifest artifact identity or content manifest binding does not match the verified inputs."
}

[object[]]$manifestBundledCatalogResources = @()
$manifestBundledCatalogResourcesProperty = $manifest.PSObject.Properties["bundledCatalogResources"]
if ($null -ne $manifestBundledCatalogResourcesProperty -and $null -ne $manifestBundledCatalogResourcesProperty.Value) {
    $manifestBundledCatalogResources = @($manifestBundledCatalogResourcesProperty.Value | ForEach-Object { $_ })
}
if ($manifestBundledCatalogResources.Count -ne $bundledCatalogResources.Count) {
    throw "Portable package manifest bundled catalog resource count does not match the verified content manifest."
}
$manifestResourcesById = @{}
foreach ($manifestResource in $manifestBundledCatalogResources) {
    $id = [string](Get-RequiredProperty -Object $manifestResource -Name "id" -Context "Portable package manifest bundled catalog resource")
    if ($manifestResourcesById.ContainsKey($id)) {
        throw "Portable package manifest contains duplicate bundled catalog resource '$id'."
    }
    $manifestResourcesById[$id] = $manifestResource
}
foreach ($expectedResource in $bundledCatalogResources) {
    if (-not $manifestResourcesById.ContainsKey($expectedResource.id)) {
        throw "Portable package manifest is missing bundled catalog resource '$($expectedResource.id)'."
    }
    $manifestResource = $manifestResourcesById[$expectedResource.id]
    foreach ($field in @("relativePath", "packagePath", "bytes", "sha256")) {
        if ((Get-RequiredProperty -Object $manifestResource -Name $field -Context "Portable package manifest bundled catalog resource '$($expectedResource.id)'") -ne $expectedResource.$field) {
            throw "Portable package manifest bundled catalog resource '$($expectedResource.id)' $field does not match the verified content manifest."
        }
    }
}

[object[]]$expectedInputs = @()
if ($null -ne $contentInputs) {
    $expectedInputs = @($contentInputs | ForEach-Object { $_ })
}
[object[]]$manifestInputs = @()
$manifestInputsProperty = $manifest.PSObject.Properties["inputs"]
if ($null -ne $manifestInputsProperty -and $null -ne $manifestInputsProperty.Value) {
    $manifestInputs = @($manifestInputsProperty.Value | ForEach-Object { $_ })
}
if ($manifestInputs.Count -ne $expectedInputs.Count) {
    throw "Portable package manifest input count does not match the verified content manifest."
}
$manifestInputsById = @{}
foreach ($manifestInput in $manifestInputs) {
    $id = [string](Get-RequiredProperty -Object $manifestInput -Name "id" -Context "Portable package manifest input")
    if ($manifestInputsById.ContainsKey($id)) {
        throw "Portable package manifest contains duplicate input '$id'."
    }
    $manifestInputsById[$id] = $manifestInput
}
foreach ($expectedInput in $expectedInputs) {
    if (-not $manifestInputsById.ContainsKey($expectedInput.id)) {
        throw "Portable package manifest is missing verified input '$($expectedInput.id)'."
    }
    $actualInput = $manifestInputsById[$expectedInput.id]
    foreach ($field in @("role", "version", "architecture", "relativePath", "packagePath", "bytes", "sha256", "license", "source", "securityClassification")) {
        if ($actualInput.$field -ne $expectedInput.$field) {
            throw "Portable package manifest input '$($expectedInput.id)' $field does not match the verified content manifest."
        }
    }
    $expectedRescueEvidence = $expectedInput.PSObject.Properties["rescueEvidence"]
    $actualRescueEvidence = $actualInput.PSObject.Properties["rescueEvidence"]
    if (($null -eq $expectedRescueEvidence) -ne ($null -eq $actualRescueEvidence)) {
        throw "Portable package manifest input '$($expectedInput.id)' rescueEvidence does not match the verified content manifest."
    }
    if ($null -ne $expectedRescueEvidence) {
        foreach ($field in @("sourcePath", "reproducibleBuildPath", "minimalSmokePath", "processTokenContractPath")) {
            if ((Get-RequiredProperty -Object $actualRescueEvidence.Value -Name $field -Context "Portable package manifest input '$($expectedInput.id)' rescueEvidence") -ne $expectedRescueEvidence.Value.$field) {
                throw "Portable package manifest input '$($expectedInput.id)' rescueEvidence $field does not match the verified content manifest."
            }
        }
    }
    if (-not $stagedPayloadMap.ContainsKey($expectedInput.packagePath) -or
        $stagedPayloadMap[$expectedInput.packagePath] -ne $expectedInput.bytes -or
        -not $stagedPayloadHashMap.ContainsKey($expectedInput.packagePath) -or
        $stagedPayloadHashMap[$expectedInput.packagePath] -ne $expectedInput.sha256) {
        throw "Portable package staging payload does not contain verified input '$($expectedInput.id)' at '$($expectedInput.packagePath)'."
    }
}

$manifestPayload = @($manifest.payload)
$manifestPayloadMap = ConvertTo-PayloadMap -Payload $manifestPayload -Name "Portable package manifest payload"
Assert-PayloadMapMatches -Expected $archivePayloadMap -Actual $manifestPayloadMap -ExpectedName "ZIP payload" -ActualName "Portable package manifest"
$manifestPayloadHashMap = ConvertTo-PayloadHashMap -Payload $manifestPayload -Name "Portable package manifest payload"
Assert-PayloadHashMapMatches -Expected $archivePayloadHashMap -Actual $manifestPayloadHashMap -ExpectedName "ZIP payload" -ActualName "Portable package manifest"

if ([Int64]$manifest.package.bytes -ne (Get-Item -LiteralPath $PackagePath).Length) {
    throw "Portable package manifest byte count does not match ZIP."
}

Write-Host "Portable package contract passed: $Architecture"
