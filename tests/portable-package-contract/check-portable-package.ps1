[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$RepositoryRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$fixtureRoots = [System.Collections.Generic.List[string]]::new()

function Require {
    param(
        [Parameter(Mandatory = $true)]
        [bool]$Condition,

        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    if (-not $Condition) {
        throw "portable package contract: $Message"
    }
}

function Require-EmptyFixedRescueFolders {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root,

        [Parameter(Mandatory = $true)]
        [string]$Context
    )

    foreach ($relativeFolder in @(
            "rescue-tools/generic-network-driver",
            "rescue-tools/offline-network-diagnostics"
        )) {
        $folder = Join-Path $Root ($relativeFolder.Replace("/", "\"))
        Require (Test-Path -LiteralPath $folder -PathType Container) "$Context is missing $relativeFolder"
        Require (@(Get-ChildItem -LiteralPath $folder -Force).Count -eq 0) "$Context must keep $relativeFolder empty"
    }
}

function Require-ZipFixedRescueFolders {
    param(
        [Parameter(Mandatory = $true)]
        [string]$PackagePath
    )

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead($PackagePath)
    try {
        $entries = @($archive.Entries | ForEach-Object { $_.FullName.Replace("\", "/") })
        foreach ($relativeFolder in @(
                "rescue-tools/generic-network-driver/",
                "rescue-tools/offline-network-diagnostics/"
            )) {
            $matchingEntries = @($entries | Where-Object {
                    $_.StartsWith($relativeFolder, [StringComparison]::OrdinalIgnoreCase)
                })
            Require ($matchingEntries.Count -eq 1 -and $matchingEntries[0] -eq $relativeFolder) "portable ZIP must keep $relativeFolder empty"
        }
    }
    finally {
        $archive.Dispose()
    }
}

function Require-BundledCatalogResources {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root,

        [Parameter(Mandatory = $true)]
        [object[]]$Resources,

        [Parameter(Mandatory = $true)]
        [string]$Context
    )

    Require ($Resources.Count -eq 2) "$Context must declare exactly two bundled catalog resources"
    $seenIds = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
    foreach ($resource in $Resources) {
        $id = [string]$resource.id
        Require ($seenIds.Add($id)) "$Context contains a duplicate bundled catalog resource id $id"
        Require ($id -in @("software-catalog", "software-optimization-catalog")) "$Context contains an unexpected bundled catalog resource $id"
        $relativePath = [string]$resource.relativePath
        $packagePath = [string]$resource.packagePath
        Require (-not [System.IO.Path]::IsPathRooted($relativePath)) "$Context resource $id has a rooted repository path"
        Require (-not $relativePath.Contains("\")) "$Context resource $id repository path must use POSIX separators"
        Require (-not ($relativePath.Split("/") -contains "..")) "$Context resource $id repository path must not traverse"
        Require ($packagePath -eq $relativePath) "$Context resource $id must preserve its fixed package path"
        $path = Join-Path $Root ($packagePath.Replace("/", "\"))
        Require (Test-Path -LiteralPath $path -PathType Leaf) "$Context is missing bundled catalog resource $packagePath"
        $file = Get-Item -LiteralPath $path
        Require ($file.Length -eq [Int64]$resource.bytes) "$Context resource $id bytes do not match the locked manifest"
        Require ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -eq ([string]$resource.sha256).ToLowerInvariant()) "$Context resource $id SHA256 does not match the locked manifest"
    }
    Require ($seenIds.Contains("software-catalog")) "$Context is missing software-catalog"
    Require ($seenIds.Contains("software-optimization-catalog")) "$Context is missing software-optimization-catalog"
}

function Write-JsonFile {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Value,

        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $directory = Split-Path -Parent $Path
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
    $Value | ConvertTo-Json -Depth 32 | Set-Content -LiteralPath $Path -Encoding UTF8
}

function New-X64PeFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $bytes = New-Object byte[] 512
    $bytes[0] = 0x4d
    $bytes[1] = 0x5a
    [BitConverter]::GetBytes([Int32]0x80).CopyTo($bytes, 0x3c)
    $bytes[0x80] = 0x50
    $bytes[0x81] = 0x45
    $bytes[0x82] = 0
    $bytes[0x83] = 0
    [BitConverter]::GetBytes([UInt16]0x8664).CopyTo($bytes, 0x84)
    [System.IO.File]::WriteAllBytes($Path, $bytes)
}

function Get-ContentManifestPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FixtureRoot
    )

    return Join-Path $FixtureRoot "release/artifact-content-manifest.v1.json"
}

function Get-ContentManifest {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FixtureRoot
    )

    return Get-Content -LiteralPath (Get-ContentManifestPath -FixtureRoot $FixtureRoot) -Raw | ConvertFrom-Json
}

function Save-ContentManifest {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FixtureRoot,

        [Parameter(Mandatory = $true)]
        [object]$Manifest
    )

    Write-JsonFile -Value $Manifest -Path (Get-ContentManifestPath -FixtureRoot $FixtureRoot)
}

function Get-ArtifactContent {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Manifest,

        [Parameter(Mandatory = $true)]
        [string]$ArtifactId
    )

    $matches = @($Manifest.artifacts | Where-Object { $_.artifactId -eq $ArtifactId })
    Require ($matches.Count -eq 1) "fixture content manifest lacks a unique $ArtifactId entry"
    return $matches[0]
}

function Get-BuildManifestPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FixtureRoot
    )

    return Join-Path $FixtureRoot "out/manifests/windows-x64-release.json"
}

function Write-ValidBuildManifest {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FixtureRoot
    )

    $commit = (& git -C $FixtureRoot rev-parse HEAD).Trim()
    $payloadDirectory = Join-Path $FixtureRoot "out/windows/x64/Release"
    $artifacts = @(
        Get-ChildItem -LiteralPath $payloadDirectory -File -Recurse |
            Sort-Object FullName |
            ForEach-Object {
                [ordered]@{
                    path = $_.FullName.Substring($payloadDirectory.Length).TrimStart("\", "/").Replace("\", "/")
                    bytes = $_.Length
                }
            }
    )
    Write-JsonFile -Value ([ordered]@{
            schemaVersion = 1
            result = "succeeded"
            source = [ordered]@{
                commit = $commit
                dirty = $false
            }
            target = [ordered]@{
                architecture = "x64"
                configuration = "Release"
            }
            artifacts = $artifacts
        }) -Path (Get-BuildManifestPath -FixtureRoot $FixtureRoot)
}

function New-FixtureRoot {
    $fixtureRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("azzs-portable-package-contract-" + [Guid]::NewGuid().ToString("N"))
    $fixtureRoots.Add($fixtureRoot)
    New-Item -ItemType Directory -Path $fixtureRoot -Force | Out-Null

    $sourceRoot = (Resolve-Path -LiteralPath $RepositoryRoot).Path
    Copy-Item -LiteralPath (Join-Path $sourceRoot ".gitignore") -Destination (Join-Path $fixtureRoot ".gitignore")
    Copy-Item -LiteralPath (Join-Path $sourceRoot "eng") -Destination (Join-Path $fixtureRoot "eng") -Recurse
    Copy-Item -LiteralPath (Join-Path $sourceRoot "release") -Destination (Join-Path $fixtureRoot "release") -Recurse
    Copy-Item -LiteralPath (Join-Path $sourceRoot "catalog") -Destination (Join-Path $fixtureRoot "catalog") -Recurse
    New-Item -ItemType Directory -Path (Join-Path $fixtureRoot "docs/adr") -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $sourceRoot "docs/adr/0030-controlled-rescue-tool-release-boundary.md") -Destination (Join-Path $fixtureRoot "docs/adr/0030-controlled-rescue-tool-release-boundary.md")
    foreach ($evidencePath in @(
            "test-evidence/rescue/source.md",
            "test-evidence/rescue/reproducible-build.md",
            "test-evidence/rescue/minimal-smoke.md",
            "test-evidence/rescue/process-token-contract.md"
        )) {
        $fullEvidencePath = Join-Path $fixtureRoot $evidencePath
        New-Item -ItemType Directory -Path (Split-Path -Parent $fullEvidencePath) -Force | Out-Null
        Set-Content -LiteralPath $fullEvidencePath -Value "tracked fixture evidence: $evidencePath" -Encoding UTF8
    }

    $payloadDirectory = Join-Path $fixtureRoot "out/windows/x64/Release"
    New-Item -ItemType Directory -Path $payloadDirectory -Force | Out-Null
    New-X64PeFile -Path (Join-Path $payloadDirectory "Azzs.WinUI.exe")
    foreach ($runtimeFile in @(
            "Microsoft.ui.xaml.dll",
            "Microsoft.WindowsAppRuntime.Bootstrap.dll",
            "Microsoft.WindowsAppRuntime.dll",
            "Microsoft.UI.Xaml.Controls.dll"
        )) {
        Set-Content -LiteralPath (Join-Path $payloadDirectory $runtimeFile) -Value "fixture runtime $runtimeFile" -Encoding ASCII
    }
    Set-Content -LiteralPath (Join-Path $payloadDirectory "Azzs.WinUI.pdb") -Value "debug symbols must be excluded" -Encoding ASCII

    & git -C $fixtureRoot init --quiet
    & git -C $fixtureRoot config user.email "portable-contract@example.invalid"
    & git -C $fixtureRoot config user.name "portable package contract"
    & git -C $fixtureRoot add .
    & git -C $fixtureRoot commit --quiet -m "fixture"
    Write-ValidBuildManifest -FixtureRoot $fixtureRoot

    return $fixtureRoot
}

function New-PackageManifestFixture {
    $fixtureRoot = New-FixtureRoot
    $packagePath = Join-Path $fixtureRoot "out/packages/Azzs-standard-x64-portable.zip"
    New-Item -ItemType Directory -Path (Split-Path -Parent $packagePath) -Force | Out-Null
    [System.IO.File]::WriteAllBytes($packagePath, [byte[]](0, 1, 2, 3))
    return [pscustomobject]@{
        Root = $fixtureRoot
        PackagePath = $packagePath
        PayloadDirectory = Join-Path $fixtureRoot "out/windows/x64/Release"
    }
}

function New-DirectoryReparsePoint {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$Target,

        [Parameter(Mandatory = $true)]
        [ValidateSet("Junction", "SymbolicLink")]
        [string]$ItemType
    )

    New-Item -ItemType $ItemType -Path $Path -Target $Target | Out-Null
    $attributes = [System.IO.File]::GetAttributes($Path)
    Require (($attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) "reparse fixture $Path was not created as a reparse point"
}

function Remove-DirectoryReparsePoint {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if (Test-Path -LiteralPath $Path) {
        [System.IO.Directory]::Delete($Path)
    }
}

function Require-PackageManifestFailure {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FixtureRoot,

        [Parameter(Mandatory = $true)]
        [string]$PayloadDirectory,

        [Parameter(Mandatory = $true)]
        [string]$PackagePath,

        [Parameter(Mandatory = $true)]
        [string]$OutputPath,

        [Parameter(Mandatory = $true)]
        [string]$Scenario
    )

    Require (-not (Test-Path -LiteralPath $OutputPath)) "$Scenario fixture manifest output already exists"
    $failed = $false
    try {
        & (Join-Path $FixtureRoot "eng/write-package-manifest.ps1") `
            -Kind portable `
            -Architecture x64 `
            -RepositoryRoot $FixtureRoot `
            -PayloadDirectory $PayloadDirectory `
            -PackagePath $PackagePath `
            -OutputPath $OutputPath
    }
    catch {
        $failed = $true
    }
    Require $failed "$Scenario unexpectedly produced a package manifest"
    Require (-not (Test-Path -LiteralPath $OutputPath)) "$Scenario must not write a package manifest"
}

function Get-CandidatePaths {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FixtureRoot,

        [Parameter(Mandatory = $true)]
        [string]$ArtifactId
    )

    $parts = $ArtifactId.Split("-")
    $edition = if ($ArtifactId.StartsWith("large-offline-", [StringComparison]::Ordinal)) { "large-offline" } else { $parts[0] }
    $architecture = if ($ArtifactId -match "-x64-") { "x64" } else { "ARM64" }
    return @(
        (Join-Path $FixtureRoot "out/staging/portable/$ArtifactId"),
        (Join-Path $FixtureRoot "out/packages/Azzs-$edition-$architecture-portable.zip"),
        (Join-Path $FixtureRoot "out/manifests/package-$edition-$architecture-portable.json")
    )
}

function Require-NoCandidate {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FixtureRoot,

        [Parameter(Mandatory = $true)]
        [string]$ArtifactId
    )

    foreach ($candidatePath in Get-CandidatePaths -FixtureRoot $FixtureRoot -ArtifactId $ArtifactId) {
        Require (-not (Test-Path -LiteralPath $candidatePath)) "failed $ArtifactId packaging left candidate $candidatePath"
    }
}

function Invoke-Package {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FixtureRoot,

        [Parameter(Mandatory = $true)]
        [string]$ArtifactId,

        [switch]$RunBuild
    )

    Push-Location -Path $FixtureRoot
    try {
        if ($RunBuild) {
            & (Join-Path $FixtureRoot "eng/package-portable.ps1") -ArtifactId $ArtifactId
        }
        else {
            & (Join-Path $FixtureRoot "eng/package-portable.ps1") -ArtifactId $ArtifactId -SkipBuild
        }
    }
    finally {
        Pop-Location
    }
}

function Require-PackageFailure {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FixtureRoot,

        [Parameter(Mandatory = $true)]
        [string]$ArtifactId,

        [Parameter(Mandatory = $true)]
        [string]$Scenario,

        [switch]$RunBuild
    )

    $failed = $false
    try {
        Invoke-Package -FixtureRoot $FixtureRoot -ArtifactId $ArtifactId -RunBuild:$RunBuild
    }
    catch {
        $failed = $true
    }
    Require $failed "$Scenario unexpectedly produced a package"
    Require-NoCandidate -FixtureRoot $FixtureRoot -ArtifactId $ArtifactId
}

function Require-PortableVerificationFailure {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FixtureRoot,

        [Parameter(Mandatory = $true)]
        [string]$ArtifactId,

        [Parameter(Mandatory = $true)]
        [string]$Scenario
    )

    $candidates = Get-CandidatePaths -FixtureRoot $FixtureRoot -ArtifactId $ArtifactId
    $failed = $false
    try {
        & (Join-Path $FixtureRoot "eng/verify-portable-package.ps1") `
            -ArtifactId $ArtifactId `
            -RepositoryRoot $FixtureRoot `
            -StagingDirectory $candidates[0] `
            -PackagePath $candidates[1] `
            -ManifestPath $candidates[2]
    }
    catch {
        $failed = $true
    }
    Require $failed "$Scenario unexpectedly passed portable verification"
}

function New-LockedInput {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FixtureRoot,

        [Parameter(Mandatory = $true)]
        [string]$Id,

        [Parameter(Mandatory = $true)]
        [string]$Role,

        [Parameter(Mandatory = $true)]
        [string]$RelativePath
    )

    $fullPath = Join-Path $FixtureRoot $RelativePath
    New-Item -ItemType Directory -Path (Split-Path -Parent $fullPath) -Force | Out-Null
    $content = [Text.Encoding]::ASCII.GetBytes("contract input: $Id")
    [System.IO.File]::WriteAllBytes($fullPath, $content)
    $inputProperties = [ordered]@{
        id = $Id
        role = $Role
        version = "1.0.0-contract"
        architecture = "x64"
        relativePath = $RelativePath.Replace("\\", "/")
        packagePath = "content/$Id.bin"
        bytes = $content.Length
        sha256 = ([System.Security.Cryptography.SHA256]::Create().ComputeHash($content) | ForEach-Object { $_.ToString("x2") }) -join ""
        license = "MIT"
        source = "project-controlled contract fixture"
        securityClassification = "allowed"
    }
    $lockedInput = [pscustomobject]$inputProperties
    if ($Role -eq "rescue-companion-tool") {
        $lockedInput | Add-Member -NotePropertyName "rescueEvidence" -NotePropertyValue ([pscustomobject][ordered]@{
            sourcePath = "test-evidence/rescue/source.md"
            reproducibleBuildPath = "test-evidence/rescue/reproducible-build.md"
            minimalSmokePath = "test-evidence/rescue/minimal-smoke.md"
            processTokenContractPath = "test-evidence/rescue/process-token-contract.md"
        }) -Force
    }
    return $lockedInput
}

function Set-LockedInputs {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FixtureRoot,

        [Parameter(Mandatory = $true)]
        [string]$ArtifactId,

        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [object[]]$Inputs
    )

    $manifest = Get-ContentManifest -FixtureRoot $FixtureRoot
    $artifact = Get-ArtifactContent -Manifest $manifest -ArtifactId $ArtifactId
    $artifact.inputs = @($Inputs)
    Save-ContentManifest -FixtureRoot $FixtureRoot -Manifest $manifest
}

function Set-BundledCatalogResourceField {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FixtureRoot,

        [Parameter(Mandatory = $true)]
        [string]$ArtifactId,

        [Parameter(Mandatory = $true)]
        [string]$ResourceId,

        [Parameter(Mandatory = $true)]
        [string]$Field,

        [Parameter(Mandatory = $true)]
        [object]$Value
    )

    $manifest = Get-ContentManifest -FixtureRoot $FixtureRoot
    $artifact = Get-ArtifactContent -Manifest $manifest -ArtifactId $ArtifactId
    $matches = @($artifact.bundledCatalogResources | Where-Object { $_.id -eq $ResourceId })
    Require ($matches.Count -eq 1) "fixture content manifest lacks a unique bundled catalog resource $ResourceId"
    $matches[0].$Field = $Value
    Save-ContentManifest -FixtureRoot $FixtureRoot -Manifest $manifest
}

function Set-ReleaseDirectoryState {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FixtureRoot,

        [Parameter(Mandatory = $true)]
        [string]$State
    )

    $manifest = Get-ContentManifest -FixtureRoot $FixtureRoot
    $artifact = Get-ArtifactContent -Manifest $manifest -ArtifactId "large-offline-x64-portable"
    $path = Join-Path $FixtureRoot ([string]$artifact.releaseDirectoryGate.relativePath)
    New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
    Set-Content -LiteralPath $path -Value "release_state = `"$State`"" -Encoding UTF8
    $resources = @($artifact.bundledCatalogResources | Where-Object {
            $_.relativePath -eq $artifact.releaseDirectoryGate.relativePath
        })
    Require ($resources.Count -eq 1) "fixture large offline release directory must be a bundled catalog resource"
    $resource = $resources[0]
    $resource.bytes = [Int64](Get-Item -LiteralPath $path).Length
    $resource.sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    Save-ContentManifest -FixtureRoot $FixtureRoot -Manifest $manifest
}

try {
    $sourceContentManifest = Get-Content -LiteralPath (Join-Path $RepositoryRoot "release/artifact-content-manifest.v1.json") -Raw | ConvertFrom-Json
    Require (@($sourceContentManifest.artifacts).Count -eq 3) "artifact content manifest must cover the three x64 portable artifacts"
    $compositionRoot = Get-Content -LiteralPath (Join-Path $RepositoryRoot "src/composition/windows/composition_root.cpp") -Raw
    Require (-not $compositionRoot.Contains("__FILE__")) "startup composition must not derive bundled catalog paths from source-file locations"
    Require ($compositionRoot.Contains("bundled_catalog_paths")) "startup composition must resolve bundled catalog resources from the workbench module directory"
    Require ($compositionRoot.Contains("path_chain_has_no_reparse_points")) "startup composition must reject reparse points along bundled catalog resource paths"
    Require ($compositionRoot.Contains("bundled_catalog_resource_matches")) "startup composition must fail closed when bundled catalog resource bytes or SHA256 do not match"
    $winuiProject = Get-Content -LiteralPath (Join-Path $RepositoryRoot "src/adapters/ui/winui/Azzs.WinUI.vcxproj") -Raw
    Require ($winuiProject.Contains("CopyAzzsBundledCatalogResources")) "WinUI project must copy bundled catalog resources to the output directory"
    foreach ($resourcePath in @("catalog\software-catalog.toml", "catalog\software-optimization-catalog.toml")) {
        Require ($winuiProject.Contains($resourcePath)) "WinUI project must include bundled catalog resource $resourcePath"
    }
    foreach ($artifactId in @("standard-x64-portable", "rescue-x64-portable", "large-offline-x64-portable")) {
        $sourceArtifact = Get-ArtifactContent -Manifest $sourceContentManifest -ArtifactId $artifactId
        Require-BundledCatalogResources -Root $RepositoryRoot -Resources @($sourceArtifact.bundledCatalogResources) -Context "source artifact $artifactId"
    }
    foreach ($artifactId in @("rescue-x64-portable", "large-offline-x64-portable")) {
        $sourceArtifact = Get-ArtifactContent -Manifest $sourceContentManifest -ArtifactId $artifactId
        Require (@($sourceArtifact.inputs).Count -eq 0) "$artifactId must remain fail-closed without locked rescue or large inputs"
    }

    $standardFixture = New-FixtureRoot
    Invoke-Package -FixtureRoot $standardFixture -ArtifactId "standard-x64-portable"
    $standardCandidates = Get-CandidatePaths -FixtureRoot $standardFixture -ArtifactId "standard-x64-portable"
    foreach ($candidatePath in $standardCandidates) {
        Require (Test-Path -LiteralPath $candidatePath) "standard x64 package did not create $candidatePath"
    }
    Require (-not (Test-Path -LiteralPath (Join-Path $standardCandidates[0] "Azzs.WinUI.pdb"))) "standard x64 staging kept debug symbols"
    $standardManifest = Get-Content -LiteralPath $standardCandidates[2] -Raw | ConvertFrom-Json
    Require ($standardManifest.artifactId -eq "standard-x64-portable") "package manifest lacks artifactId"
    Require ($standardManifest.edition -eq "standard") "package manifest lacks edition"
    Require ($null -ne $standardManifest.contentManifest) "package manifest lacks contentManifest"
    Require-BundledCatalogResources -Root $standardCandidates[0] -Resources @($standardManifest.bundledCatalogResources) -Context "standard package manifest and staging"
    Require (@($standardManifest.inputs).Count -eq 0) "standard package must not claim external content inputs"
    Require (-not ((Get-Content -LiteralPath $standardCandidates[2] -Raw).Contains($standardFixture))) "package manifest leaked an absolute fixture path"
    Require ($standardManifest.package.path -eq "out/packages/Azzs-standard-x64-portable.zip") "package manifest must use the canonical repository-relative ZIP path"
    Require (-not [System.IO.Path]::IsPathRooted([string]$standardManifest.package.path)) "package manifest package.path must not be rooted"
    Require (-not ([string]$standardManifest.package.path).Contains("\")) "package manifest package.path must use POSIX separators"
    Require (-not (([string]$standardManifest.package.path).Split("/") -contains "..")) "package manifest package.path must not traverse outside the repository"
    Require-EmptyFixedRescueFolders -Root $standardCandidates[0] -Context "standard staging"
    Require-ZipFixedRescueFolders -PackagePath $standardCandidates[1]
    $extractedStandardDirectory = Join-Path $standardFixture "out/extracted/standard-x64-portable"
    [System.IO.Compression.ZipFile]::ExtractToDirectory($standardCandidates[1], $extractedStandardDirectory)
    Require-EmptyFixedRescueFolders -Root $extractedStandardDirectory -Context "extracted standard ZIP"
    Require-BundledCatalogResources -Root $extractedStandardDirectory -Resources @($standardManifest.bundledCatalogResources) -Context "extracted standard ZIP"

    $missingRuntimeFixture = New-FixtureRoot
    Invoke-Package -FixtureRoot $missingRuntimeFixture -ArtifactId "standard-x64-portable"
    $missingRuntimeCandidates = Get-CandidatePaths -FixtureRoot $missingRuntimeFixture -ArtifactId "standard-x64-portable"
    Remove-Item -LiteralPath (Join-Path $missingRuntimeCandidates[0] "Microsoft.UI.Xaml.Controls.dll") -Force
    $missingRuntimeArchive = [System.IO.Compression.ZipFile]::Open(
        $missingRuntimeCandidates[1],
        [System.IO.Compression.ZipArchiveMode]::Update
    )
    try {
        $missingRuntimeEntry = $missingRuntimeArchive.GetEntry("Microsoft.UI.Xaml.Controls.dll")
        Require ($null -ne $missingRuntimeEntry) "fixture ZIP lacks the secondary runtime dependency"
        $missingRuntimeEntry.Delete()
    }
    finally {
        $missingRuntimeArchive.Dispose()
    }
    Require-PortableVerificationFailure `
        -FixtureRoot $missingRuntimeFixture `
        -ArtifactId "standard-x64-portable" `
        -Scenario "a missing non-minimal WinUI runtime dependency"

    $missingCatalogFixture = New-FixtureRoot
    Invoke-Package -FixtureRoot $missingCatalogFixture -ArtifactId "standard-x64-portable"
    Remove-Item -LiteralPath (Join-Path $missingCatalogFixture "catalog/software-catalog.toml") -Force
    Require-PackageFailure -FixtureRoot $missingCatalogFixture -ArtifactId "standard-x64-portable" -Scenario "missing bundled software catalog"

    $catalogPathDriftFixture = New-FixtureRoot
    Invoke-Package -FixtureRoot $catalogPathDriftFixture -ArtifactId "standard-x64-portable"
    Set-BundledCatalogResourceField -FixtureRoot $catalogPathDriftFixture -ArtifactId "standard-x64-portable" -ResourceId "software-catalog" -Field "packagePath" -Value "catalog/drifted-software-catalog.toml"
    Require-PackageFailure -FixtureRoot $catalogPathDriftFixture -ArtifactId "standard-x64-portable" -Scenario "drifted bundled catalog package path"

    $catalogDigestDriftFixture = New-FixtureRoot
    Invoke-Package -FixtureRoot $catalogDigestDriftFixture -ArtifactId "standard-x64-portable"
    Set-BundledCatalogResourceField -FixtureRoot $catalogDigestDriftFixture -ArtifactId "standard-x64-portable" -ResourceId "software-catalog" -Field "sha256" -Value ("0" * 64)
    Require-PackageFailure -FixtureRoot $catalogDigestDriftFixture -ArtifactId "standard-x64-portable" -Scenario "tampered bundled catalog source digest"

    $stagingCatalogTamperFixture = New-FixtureRoot
    Invoke-Package -FixtureRoot $stagingCatalogTamperFixture -ArtifactId "standard-x64-portable"
    $stagingCatalogCandidates = Get-CandidatePaths -FixtureRoot $stagingCatalogTamperFixture -ArtifactId "standard-x64-portable"
    Set-Content -LiteralPath (Join-Path $stagingCatalogCandidates[0] "catalog/software-catalog.toml") -Value "tampered catalog resource" -Encoding UTF8
    Require-PortableVerificationFailure -FixtureRoot $stagingCatalogTamperFixture -ArtifactId "standard-x64-portable" -Scenario "a tampered staged bundled catalog resource"

    $manifestCatalogDigestTamperFixture = New-FixtureRoot
    Invoke-Package -FixtureRoot $manifestCatalogDigestTamperFixture -ArtifactId "standard-x64-portable"
    $manifestCatalogDigestCandidates = Get-CandidatePaths -FixtureRoot $manifestCatalogDigestTamperFixture -ArtifactId "standard-x64-portable"
    $manifestCatalogDigest = Get-Content -LiteralPath $manifestCatalogDigestCandidates[2] -Raw | ConvertFrom-Json
    $manifestCatalogDigest.bundledCatalogResources[0].sha256 = "0" * 64
    Write-JsonFile -Value $manifestCatalogDigest -Path $manifestCatalogDigestCandidates[2]
    Require-PortableVerificationFailure -FixtureRoot $manifestCatalogDigestTamperFixture -ArtifactId "standard-x64-portable" -Scenario "a tampered bundled catalog resource manifest digest"

    $tamperedArchive = [System.IO.Compression.ZipFile]::Open(
        $standardCandidates[1],
        [System.IO.Compression.ZipArchiveMode]::Update
    )
    try {
        $null = $tamperedArchive.CreateEntry("rescue-tools/generic-network-driver/unexpected/")
    }
    finally {
        $tamperedArchive.Dispose()
    }
    Require-PortableVerificationFailure -FixtureRoot $standardFixture -ArtifactId "standard-x64-portable" -Scenario "a ZIP rescue subdirectory"

    $caseVariantFixture = New-FixtureRoot
    Invoke-Package -FixtureRoot $caseVariantFixture -ArtifactId "standard-x64-portable"
    $caseVariantCandidates = Get-CandidatePaths -FixtureRoot $caseVariantFixture -ArtifactId "standard-x64-portable"
    $caseVariantArchive = [System.IO.Compression.ZipFile]::Open(
        $caseVariantCandidates[1],
        [System.IO.Compression.ZipArchiveMode]::Update
    )
    try {
        $null = $caseVariantArchive.CreateEntry("rescue-tools/GENERIC-network-driver/unexpected/")
    }
    finally {
        $caseVariantArchive.Dispose()
    }
    Require-PortableVerificationFailure -FixtureRoot $caseVariantFixture -ArtifactId "standard-x64-portable" -Scenario "a case-variant ZIP rescue subdirectory"

    $externalPackagePath = Join-Path ([System.IO.Path]::GetTempPath()) ("azzs-external-package-" + [Guid]::NewGuid().ToString("N") + ".zip")
    try {
        [System.IO.File]::WriteAllBytes($externalPackagePath, [byte[]](0, 1, 2, 3))
        $externalManifestPath = Join-Path $standardFixture "out/manifests/external-package.json"
        $externalPackageRejected = $false
        try {
            & (Join-Path $standardFixture "eng/write-package-manifest.ps1") `
                -Kind portable `
                -Architecture x64 `
                -RepositoryRoot $standardFixture `
                -PayloadDirectory $standardCandidates[0] `
                -PackagePath $externalPackagePath `
                -OutputPath $externalManifestPath
        }
        catch {
            $externalPackageRejected = $true
        }
        Require $externalPackageRejected "an external package path must fail closed"
        Require (-not (Test-Path -LiteralPath $externalManifestPath)) "an external package path must not write a manifest"
    }
    finally {
        if (Test-Path -LiteralPath $externalPackagePath) {
            Remove-Item -LiteralPath $externalPackagePath -Force
        }
    }

    $payloadFileFixture = New-PackageManifestFixture
    Require-PackageManifestFailure `
        -FixtureRoot $payloadFileFixture.Root `
        -PayloadDirectory (Join-Path $payloadFileFixture.PayloadDirectory "Azzs.WinUI.exe") `
        -PackagePath $payloadFileFixture.PackagePath `
        -OutputPath (Join-Path $payloadFileFixture.Root "out/manifests/payload-file.json") `
        -Scenario "a payload parameter naming a file"

    $payloadJunctionFixture = New-PackageManifestFixture
    $payloadJunctionTarget = Join-Path ([System.IO.Path]::GetTempPath()) ("azzs-payload-junction-" + [Guid]::NewGuid().ToString("N"))
    try {
        New-Item -ItemType Directory -Path $payloadJunctionTarget -Force | Out-Null
        Remove-Item -LiteralPath $payloadJunctionFixture.PayloadDirectory -Recurse -Force
        New-DirectoryReparsePoint `
            -Path $payloadJunctionFixture.PayloadDirectory `
            -Target $payloadJunctionTarget `
            -ItemType Junction
        Require-PackageManifestFailure `
            -FixtureRoot $payloadJunctionFixture.Root `
            -PayloadDirectory $payloadJunctionFixture.PayloadDirectory `
            -PackagePath $payloadJunctionFixture.PackagePath `
            -OutputPath (Join-Path $payloadJunctionFixture.Root "out/manifests/payload-junction.json") `
            -Scenario "a payload root junction"
    }
    finally {
        Remove-DirectoryReparsePoint -Path $payloadJunctionFixture.PayloadDirectory
        if (Test-Path -LiteralPath $payloadJunctionTarget) {
            Remove-Item -LiteralPath $payloadJunctionTarget -Recurse -Force
        }
    }

    $payloadSymlinkFixture = New-PackageManifestFixture
    $payloadSymlinkTarget = Join-Path ([System.IO.Path]::GetTempPath()) ("azzs-payload-symlink-" + [Guid]::NewGuid().ToString("N"))
    try {
        New-Item -ItemType Directory -Path $payloadSymlinkTarget -Force | Out-Null
        Remove-Item -LiteralPath $payloadSymlinkFixture.PayloadDirectory -Recurse -Force
        New-DirectoryReparsePoint `
            -Path $payloadSymlinkFixture.PayloadDirectory `
            -Target $payloadSymlinkTarget `
            -ItemType SymbolicLink
        Require-PackageManifestFailure `
            -FixtureRoot $payloadSymlinkFixture.Root `
            -PayloadDirectory $payloadSymlinkFixture.PayloadDirectory `
            -PackagePath $payloadSymlinkFixture.PackagePath `
            -OutputPath (Join-Path $payloadSymlinkFixture.Root "out/manifests/payload-symlink.json") `
            -Scenario "a payload root symbolic link"
    }
    finally {
        Remove-DirectoryReparsePoint -Path $payloadSymlinkFixture.PayloadDirectory
        if (Test-Path -LiteralPath $payloadSymlinkTarget) {
            Remove-Item -LiteralPath $payloadSymlinkTarget -Recurse -Force
        }
    }

    $nestedPayloadReparseFixture = New-PackageManifestFixture
    $nestedPayloadTarget = Join-Path ([System.IO.Path]::GetTempPath()) ("azzs-nested-payload-reparse-" + [Guid]::NewGuid().ToString("N"))
    $nestedPayloadLink = Join-Path $nestedPayloadReparseFixture.PayloadDirectory "nested-reparse"
    try {
        New-Item -ItemType Directory -Path $nestedPayloadTarget -Force | Out-Null
        Set-Content -LiteralPath (Join-Path $nestedPayloadTarget "unexpected.dll") -Value "external payload" -Encoding ASCII
        New-DirectoryReparsePoint `
            -Path $nestedPayloadLink `
            -Target $nestedPayloadTarget `
            -ItemType Junction
        Require-PackageManifestFailure `
            -FixtureRoot $nestedPayloadReparseFixture.Root `
            -PayloadDirectory $nestedPayloadReparseFixture.PayloadDirectory `
            -PackagePath $nestedPayloadReparseFixture.PackagePath `
            -OutputPath (Join-Path $nestedPayloadReparseFixture.Root "out/manifests/nested-payload-reparse.json") `
            -Scenario "a payload subtree containing a reparse point"
    }
    finally {
        Remove-DirectoryReparsePoint -Path $nestedPayloadLink
        if (Test-Path -LiteralPath $nestedPayloadTarget) {
            Remove-Item -LiteralPath $nestedPayloadTarget -Recurse -Force
        }
    }

    $reparsePackageFixture = New-FixtureRoot
    $reparsePackageLink = Join-Path $reparsePackageFixture "out/packages"
    $outsidePackageDirectory = Join-Path ([System.IO.Path]::GetTempPath()) ("azzs-external-package-directory-" + [Guid]::NewGuid().ToString("N"))
    try {
        New-Item -ItemType Directory -Path $outsidePackageDirectory -Force | Out-Null
        New-Item -ItemType Junction -Path $reparsePackageLink -Target $outsidePackageDirectory | Out-Null
        $reparsePackagePath = Join-Path $reparsePackageLink "Azzs-standard-x64-portable.zip"
        [System.IO.File]::WriteAllBytes($reparsePackagePath, [byte[]](0, 1, 2, 3))
        $reparseManifestPath = Join-Path $reparsePackageFixture "out/manifests/reparse-package.json"
        $reparsePackageRejected = $false
        try {
            & (Join-Path $reparsePackageFixture "eng/write-package-manifest.ps1") `
                -Kind portable `
                -Architecture x64 `
                -RepositoryRoot $reparsePackageFixture `
                -PayloadDirectory (Join-Path $reparsePackageFixture "out/windows/x64/Release") `
                -PackagePath $reparsePackagePath `
                -OutputPath $reparseManifestPath
        }
        catch {
            $reparsePackageRejected = $true
        }
        Require $reparsePackageRejected "a package path below a reparse parent must fail closed"
        Require (-not (Test-Path -LiteralPath $reparseManifestPath)) "a reparse package path must not write a manifest"
    }
    finally {
        if (Test-Path -LiteralPath $reparsePackageLink) {
            [System.IO.Directory]::Delete($reparsePackageLink)
        }
        if (Test-Path -LiteralPath $outsidePackageDirectory) {
            Remove-Item -LiteralPath $outsidePackageDirectory -Recurse -Force
        }
    }

    $reparseCleanupFixture = New-FixtureRoot
    $reparseCleanupLink = Join-Path $reparseCleanupFixture "out/packages"
    $outsideCleanupDirectory = Join-Path ([System.IO.Path]::GetTempPath()) ("azzs-external-package-cleanup-" + [Guid]::NewGuid().ToString("N"))
    try {
        New-Item -ItemType Directory -Path $outsideCleanupDirectory -Force | Out-Null
        New-Item -ItemType Junction -Path $reparseCleanupLink -Target $outsideCleanupDirectory | Out-Null
        $sentinelPackagePath = Join-Path $outsideCleanupDirectory "Azzs-standard-x64-portable.zip"
        [System.IO.File]::WriteAllBytes($sentinelPackagePath, [byte[]](0, 1, 2, 3))
        $sentinelHash = (Get-FileHash -LiteralPath $sentinelPackagePath -Algorithm SHA256).Hash
        $cleanupPackagingFailed = $false
        try {
            Invoke-Package -FixtureRoot $reparseCleanupFixture -ArtifactId "standard-x64-portable"
        }
        catch {
            $cleanupPackagingFailed = $true
        }
        Require $cleanupPackagingFailed "a package destination below a reparse parent must fail before cleanup"
        Require (Test-Path -LiteralPath $sentinelPackagePath -PathType Leaf) "a package cleanup must not delete an external sentinel"
        Require ((Get-FileHash -LiteralPath $sentinelPackagePath -Algorithm SHA256).Hash -eq $sentinelHash) "a package cleanup must not replace an external sentinel"
        $cleanupCandidates = Get-CandidatePaths -FixtureRoot $reparseCleanupFixture -ArtifactId "standard-x64-portable"
        Require (-not (Test-Path -LiteralPath $cleanupCandidates[0])) "a rejected reparse package destination must not create staging output"
        Require (-not (Test-Path -LiteralPath $cleanupCandidates[2])) "a rejected reparse package destination must not create a manifest"
    }
    finally {
        if (Test-Path -LiteralPath $reparseCleanupLink) {
            [System.IO.Directory]::Delete($reparseCleanupLink)
        }
        if (Test-Path -LiteralPath $outsideCleanupDirectory) {
            Remove-Item -LiteralPath $outsideCleanupDirectory -Recurse -Force
        }
    }

    $rescueSuccessFixture = New-FixtureRoot
    $rescueSuccessInput = New-LockedInput -FixtureRoot $rescueSuccessFixture -Id "rescue-tool" -Role "rescue-companion-tool" -RelativePath "release-inputs/rescue-tool.bin"
    Set-LockedInputs -FixtureRoot $rescueSuccessFixture -ArtifactId "rescue-x64-portable" -Inputs @($rescueSuccessInput)
    & git -C $rescueSuccessFixture add release/artifact-content-manifest.v1.json release-inputs/rescue-tool.bin
    & git -C $rescueSuccessFixture commit --quiet -m "rescue package fixture"
    Write-ValidBuildManifest -FixtureRoot $rescueSuccessFixture
    New-Item -ItemType Directory -Path (Join-Path $rescueSuccessFixture "content") -Force | Out-Null
    Invoke-Package -FixtureRoot $rescueSuccessFixture -ArtifactId "rescue-x64-portable"
    $rescueCandidates = Get-CandidatePaths -FixtureRoot $rescueSuccessFixture -ArtifactId "rescue-x64-portable"
    foreach ($candidatePath in $rescueCandidates) {
        Require (Test-Path -LiteralPath $candidatePath) "rescue x64 package did not create $candidatePath"
    }
    Require (Test-Path -LiteralPath (Join-Path $rescueCandidates[0] "content/rescue-tool.bin")) "rescue x64 staging lacks the locked content input"
    Require (-not (Test-Path -LiteralPath (Join-Path $rescueSuccessFixture "content/rescue-tool.bin"))) "rescue x64 package wrote a bypass ZIP outside out/packages"
    & (Join-Path $rescueSuccessFixture "eng/verify-portable-package.ps1") -ArtifactId "rescue-x64-portable" -RepositoryRoot $rescueSuccessFixture -StagingDirectory $rescueCandidates[0] -PackagePath $rescueCandidates[1] -ManifestPath $rescueCandidates[2]

    $largeOfflineSuccessFixture = New-FixtureRoot
    $largeOfflineRescueInput = New-LockedInput -FixtureRoot $largeOfflineSuccessFixture -Id "rescue-tool" -Role "rescue-companion-tool" -RelativePath "release-inputs/rescue-tool.bin"
    $largeOfflineInput = New-LockedInput -FixtureRoot $largeOfflineSuccessFixture -Id "offline-tool" -Role "offline-package" -RelativePath "release-inputs/offline-tool.bin"
    Set-LockedInputs -FixtureRoot $largeOfflineSuccessFixture -ArtifactId "rescue-x64-portable" -Inputs @($largeOfflineRescueInput)
    Set-LockedInputs -FixtureRoot $largeOfflineSuccessFixture -ArtifactId "large-offline-x64-portable" -Inputs @($largeOfflineRescueInput, $largeOfflineInput)
    Set-ReleaseDirectoryState -FixtureRoot $largeOfflineSuccessFixture -State "release"
    & git -C $largeOfflineSuccessFixture add release/artifact-content-manifest.v1.json catalog/software-catalog.toml release-inputs/rescue-tool.bin release-inputs/offline-tool.bin
    & git -C $largeOfflineSuccessFixture commit --quiet -m "large offline package fixture"
    Write-ValidBuildManifest -FixtureRoot $largeOfflineSuccessFixture
    Invoke-Package -FixtureRoot $largeOfflineSuccessFixture -ArtifactId "large-offline-x64-portable"
    $largeOfflineCandidates = Get-CandidatePaths -FixtureRoot $largeOfflineSuccessFixture -ArtifactId "large-offline-x64-portable"
    foreach ($candidatePath in $largeOfflineCandidates) {
        Require (Test-Path -LiteralPath $candidatePath) "large offline x64 package did not create $candidatePath"
    }
    Require (Test-Path -LiteralPath (Join-Path $largeOfflineCandidates[0] "content/rescue-tool.bin")) "large offline x64 staging lacks the rescue input"
    Require (Test-Path -LiteralPath (Join-Path $largeOfflineCandidates[0] "content/offline-tool.bin")) "large offline x64 staging lacks the offline input"
    & (Join-Path $largeOfflineSuccessFixture "eng/verify-portable-package.ps1") -ArtifactId "large-offline-x64-portable" -RepositoryRoot $largeOfflineSuccessFixture -StagingDirectory $largeOfflineCandidates[0] -PackagePath $largeOfflineCandidates[1] -ManifestPath $largeOfflineCandidates[2]

    $unbundledReleaseGateFixture = New-FixtureRoot
    $unbundledRescueInput = New-LockedInput -FixtureRoot $unbundledReleaseGateFixture -Id "rescue-tool" -Role "rescue-companion-tool" -RelativePath "release-inputs/rescue-tool.bin"
    $unbundledOfflineInput = New-LockedInput -FixtureRoot $unbundledReleaseGateFixture -Id "offline-tool" -Role "offline-package" -RelativePath "release-inputs/offline-tool.bin"
    Set-LockedInputs -FixtureRoot $unbundledReleaseGateFixture -ArtifactId "rescue-x64-portable" -Inputs @($unbundledRescueInput)
    Set-LockedInputs -FixtureRoot $unbundledReleaseGateFixture -ArtifactId "large-offline-x64-portable" -Inputs @($unbundledRescueInput, $unbundledOfflineInput)
    $unbundledManifest = Get-ContentManifest -FixtureRoot $unbundledReleaseGateFixture
    $unbundledArtifact = Get-ArtifactContent -Manifest $unbundledManifest -ArtifactId "large-offline-x64-portable"
    $unbundledArtifact.releaseDirectoryGate.relativePath = "release/product-identity.json"
    Save-ContentManifest -FixtureRoot $unbundledReleaseGateFixture -Manifest $unbundledManifest
    & git -C $unbundledReleaseGateFixture add release/artifact-content-manifest.v1.json release-inputs/rescue-tool.bin release-inputs/offline-tool.bin
    & git -C $unbundledReleaseGateFixture commit --quiet -m "reject unbundled release gate fixture"
    Write-ValidBuildManifest -FixtureRoot $unbundledReleaseGateFixture
    Require-PackageFailure -FixtureRoot $unbundledReleaseGateFixture -ArtifactId "large-offline-x64-portable" -Scenario "release directory gate outside bundled resources"

    $invalidArtifactFixture = New-FixtureRoot
    Require-PackageFailure -FixtureRoot $invalidArtifactFixture -ArtifactId "invalid-x64-portable" -Scenario "invalid artifact id"

    $arm64Fixture = New-FixtureRoot
    Require-PackageFailure -FixtureRoot $arm64Fixture -ArtifactId "standard-arm64-portable" -Scenario "deferred ARM64 portable artifact"

    $buildFixture = New-FixtureRoot
    $buildManifest = Get-Content -LiteralPath (Get-BuildManifestPath -FixtureRoot $buildFixture) -Raw | ConvertFrom-Json
    $buildManifest.source.dirty = $true
    Write-JsonFile -Value $buildManifest -Path (Get-BuildManifestPath -FixtureRoot $buildFixture)
    Require-PackageFailure -FixtureRoot $buildFixture -ArtifactId "standard-x64-portable" -Scenario "dirty build manifest"

    $dirtyAfterBuildFixture = New-FixtureRoot
    @'
param([string]$Architecture)
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Set-Content -LiteralPath (Join-Path $repositoryRoot "docs/adr/0030-controlled-rescue-tool-release-boundary.md") -Value "modified after a clean build manifest" -Encoding UTF8
'@ | Set-Content -LiteralPath (Join-Path $dirtyAfterBuildFixture "eng/build.ps1") -Encoding UTF8
    & git -C $dirtyAfterBuildFixture add eng/build.ps1
    & git -C $dirtyAfterBuildFixture commit --quiet -m "dirty build fixture"
    Write-ValidBuildManifest -FixtureRoot $dirtyAfterBuildFixture
    Require-PackageFailure -FixtureRoot $dirtyAfterBuildFixture -ArtifactId "standard-x64-portable" -Scenario "dirty repository after build" -RunBuild

    $rescueMissingFixture = New-FixtureRoot
    Require-PackageFailure -FixtureRoot $rescueMissingFixture -ArtifactId "rescue-x64-portable" -Scenario "rescue package without locked inputs"

    $checksumFixture = New-FixtureRoot
    $checksumInput = New-LockedInput -FixtureRoot $checksumFixture -Id "rescue-tool" -Role "rescue-companion-tool" -RelativePath "release-inputs/rescue-tool.bin"
    $checksumInput.sha256 = "0" * 64
    Set-LockedInputs -FixtureRoot $checksumFixture -ArtifactId "rescue-x64-portable" -Inputs @($checksumInput)
    Require-PackageFailure -FixtureRoot $checksumFixture -ArtifactId "rescue-x64-portable" -Scenario "content checksum mismatch"

    $duplicateFixture = New-FixtureRoot
    $firstDuplicate = New-LockedInput -FixtureRoot $duplicateFixture -Id "rescue-tool" -Role "rescue-companion-tool" -RelativePath "release-inputs/rescue-tool.bin"
    $secondDuplicate = New-LockedInput -FixtureRoot $duplicateFixture -Id "rescue-tool" -Role "rescue-companion-tool" -RelativePath "release-inputs/rescue-tool.bin"
    Set-LockedInputs -FixtureRoot $duplicateFixture -ArtifactId "rescue-x64-portable" -Inputs @($firstDuplicate, $secondDuplicate)
    Require-PackageFailure -FixtureRoot $duplicateFixture -ArtifactId "rescue-x64-portable" -Scenario "duplicate locked content"

    $traversalFixture = New-FixtureRoot
    $traversalInput = New-LockedInput -FixtureRoot $traversalFixture -Id "rescue-tool" -Role "rescue-companion-tool" -RelativePath "release-inputs/rescue-tool.bin"
    $traversalInput.relativePath = "../escape.bin"
    Set-LockedInputs -FixtureRoot $traversalFixture -ArtifactId "rescue-x64-portable" -Inputs @($traversalInput)
    Require-PackageFailure -FixtureRoot $traversalFixture -ArtifactId "rescue-x64-portable" -Scenario "path traversal"

    $absoluteFixture = New-FixtureRoot
    $absoluteInput = New-LockedInput -FixtureRoot $absoluteFixture -Id "rescue-tool" -Role "rescue-companion-tool" -RelativePath "release-inputs/rescue-tool.bin"
    $absoluteInput.relativePath = "C:/absolute-input.bin"
    Set-LockedInputs -FixtureRoot $absoluteFixture -ArtifactId "rescue-x64-portable" -Inputs @($absoluteInput)
    Require-PackageFailure -FixtureRoot $absoluteFixture -ArtifactId "rescue-x64-portable" -Scenario "absolute input path"

    $payloadCollisionFixture = New-FixtureRoot
    $payloadCollisionInput = New-LockedInput -FixtureRoot $payloadCollisionFixture -Id "rescue-tool" -Role "rescue-companion-tool" -RelativePath "release-inputs/rescue-tool.bin"
    $payloadCollisionInput.packagePath = "Azzs.WinUI.exe"
    Set-LockedInputs -FixtureRoot $payloadCollisionFixture -ArtifactId "rescue-x64-portable" -Inputs @($payloadCollisionInput)
    Require-PackageFailure -FixtureRoot $payloadCollisionFixture -ArtifactId "rescue-x64-portable" -Scenario "workbench executable overwrite"

    $architectureFixture = New-FixtureRoot
    $architectureInput = New-LockedInput -FixtureRoot $architectureFixture -Id "rescue-tool" -Role "rescue-companion-tool" -RelativePath "release-inputs/rescue-tool.bin"
    $architectureInput.architecture = "ARM64"
    Set-LockedInputs -FixtureRoot $architectureFixture -ArtifactId "rescue-x64-portable" -Inputs @($architectureInput)
    Require-PackageFailure -FixtureRoot $architectureFixture -ArtifactId "rescue-x64-portable" -Scenario "wrong content architecture"

    $metadataFixture = New-FixtureRoot
    $metadataInput = New-LockedInput -FixtureRoot $metadataFixture -Id "rescue-tool" -Role "rescue-companion-tool" -RelativePath "release-inputs/rescue-tool.bin"
    $metadataInput.PSObject.Properties.Remove("license")
    Set-LockedInputs -FixtureRoot $metadataFixture -ArtifactId "rescue-x64-portable" -Inputs @($metadataInput)
    Require-PackageFailure -FixtureRoot $metadataFixture -ArtifactId "rescue-x64-portable" -Scenario "missing license metadata"

    $sourceMetadataFixture = New-FixtureRoot
    $sourceMetadataInput = New-LockedInput -FixtureRoot $sourceMetadataFixture -Id "rescue-tool" -Role "rescue-companion-tool" -RelativePath "release-inputs/rescue-tool.bin"
    $sourceMetadataInput.PSObject.Properties.Remove("source")
    Set-LockedInputs -FixtureRoot $sourceMetadataFixture -ArtifactId "rescue-x64-portable" -Inputs @($sourceMetadataInput)
    Require-PackageFailure -FixtureRoot $sourceMetadataFixture -ArtifactId "rescue-x64-portable" -Scenario "missing source metadata"

    $adrFixture = New-FixtureRoot
    $adrInput = New-LockedInput -FixtureRoot $adrFixture -Id "rescue-tool" -Role "rescue-companion-tool" -RelativePath "release-inputs/rescue-tool.bin"
    Set-LockedInputs -FixtureRoot $adrFixture -ArtifactId "rescue-x64-portable" -Inputs @($adrInput)
    $adrManifest = Get-ContentManifest -FixtureRoot $adrFixture
    $adrArtifact = Get-ArtifactContent -Manifest $adrManifest -ArtifactId "rescue-x64-portable"
    $adrArtifact.PSObject.Properties.Remove("rescueGate")
    Save-ContentManifest -FixtureRoot $adrFixture -Manifest $adrManifest
    Require-PackageFailure -FixtureRoot $adrFixture -ArtifactId "rescue-x64-portable" -Scenario "missing ADR-0030 gate"

    $missingEvidenceFixture = New-FixtureRoot
    $missingEvidenceInput = New-LockedInput -FixtureRoot $missingEvidenceFixture -Id "rescue-tool" -Role "rescue-companion-tool" -RelativePath "release-inputs/rescue-tool.bin"
    $missingEvidenceInput.rescueEvidence.PSObject.Properties.Remove("sourcePath")
    Set-LockedInputs -FixtureRoot $missingEvidenceFixture -ArtifactId "rescue-x64-portable" -Inputs @($missingEvidenceInput)
    Require-PackageFailure -FixtureRoot $missingEvidenceFixture -ArtifactId "rescue-x64-portable" -Scenario "missing controlled source evidence"

    $missingEvidenceFileFixture = New-FixtureRoot
    $missingEvidenceFileInput = New-LockedInput -FixtureRoot $missingEvidenceFileFixture -Id "rescue-tool" -Role "rescue-companion-tool" -RelativePath "release-inputs/rescue-tool.bin"
    $missingEvidenceFileInput.rescueEvidence.reproducibleBuildPath = "test-evidence/rescue/missing-build.md"
    Set-LockedInputs -FixtureRoot $missingEvidenceFileFixture -ArtifactId "rescue-x64-portable" -Inputs @($missingEvidenceFileInput)
    Require-PackageFailure -FixtureRoot $missingEvidenceFileFixture -ArtifactId "rescue-x64-portable" -Scenario "missing reproducible build evidence file"

    $untrackedEvidenceFixture = New-FixtureRoot
    $untrackedEvidenceInput = New-LockedInput -FixtureRoot $untrackedEvidenceFixture -Id "rescue-tool" -Role "rescue-companion-tool" -RelativePath "release-inputs/rescue-tool.bin"
    $untrackedEvidencePath = Join-Path $untrackedEvidenceFixture "test-evidence/rescue/untracked-smoke.md"
    Set-Content -LiteralPath $untrackedEvidencePath -Value "untracked fixture evidence" -Encoding UTF8
    $untrackedEvidenceInput.rescueEvidence.minimalSmokePath = "test-evidence/rescue/untracked-smoke.md"
    Set-LockedInputs -FixtureRoot $untrackedEvidenceFixture -ArtifactId "rescue-x64-portable" -Inputs @($untrackedEvidenceInput)
    Require-PackageFailure -FixtureRoot $untrackedEvidenceFixture -ArtifactId "rescue-x64-portable" -Scenario "untracked minimal smoke evidence"

    $unsafeEvidenceFixture = New-FixtureRoot
    $unsafeEvidenceInput = New-LockedInput -FixtureRoot $unsafeEvidenceFixture -Id "rescue-tool" -Role "rescue-companion-tool" -RelativePath "release-inputs/rescue-tool.bin"
    $unsafeEvidenceInput.rescueEvidence.processTokenContractPath = "../process-token-contract.md"
    Set-LockedInputs -FixtureRoot $unsafeEvidenceFixture -ArtifactId "rescue-x64-portable" -Inputs @($unsafeEvidenceInput)
    Require-PackageFailure -FixtureRoot $unsafeEvidenceFixture -ArtifactId "rescue-x64-portable" -Scenario "unsafe process token contract evidence path"

    foreach ($securityScenario in @(
            [pscustomobject]@{ Value = "SEC-08"; Name = "SEC-08 content" },
            [pscustomobject]@{ Value = "sec-08"; Name = "lowercase SEC-08 content" },
            [pscustomobject]@{ Value = " allowed"; Name = "whitespace security classification" },
            [pscustomobject]@{ Value = "ALLOWED"; Name = "uppercase security classification" },
            [pscustomobject]@{ Value = "unknown"; Name = "unknown security classification" }
        )) {
        $securityFixture = New-FixtureRoot
        $securityInput = New-LockedInput -FixtureRoot $securityFixture -Id "rescue-tool" -Role "rescue-companion-tool" -RelativePath "release-inputs/rescue-tool.bin"
        $securityInput.securityClassification = $securityScenario.Value
        Set-LockedInputs -FixtureRoot $securityFixture -ArtifactId "rescue-x64-portable" -Inputs @($securityInput)
        Require-PackageFailure -FixtureRoot $securityFixture -ArtifactId "rescue-x64-portable" -Scenario $securityScenario.Name
    }

    $supersetFixture = New-FixtureRoot
    $rescueInput = New-LockedInput -FixtureRoot $supersetFixture -Id "rescue-tool" -Role "rescue-companion-tool" -RelativePath "release-inputs/rescue-tool.bin"
    $nonRescueInput = New-LockedInput -FixtureRoot $supersetFixture -Id "offline-tool" -Role "offline-package" -RelativePath "release-inputs/offline-tool.bin"
    Set-LockedInputs -FixtureRoot $supersetFixture -ArtifactId "rescue-x64-portable" -Inputs @($rescueInput)
    Set-LockedInputs -FixtureRoot $supersetFixture -ArtifactId "large-offline-x64-portable" -Inputs @($nonRescueInput)
    Set-ReleaseDirectoryState -FixtureRoot $supersetFixture -State "release"
    Require-PackageFailure -FixtureRoot $supersetFixture -ArtifactId "large-offline-x64-portable" -Scenario "large offline package without the rescue superset"

    $draftFixture = New-FixtureRoot
    $draftRescueInput = New-LockedInput -FixtureRoot $draftFixture -Id "rescue-tool" -Role "rescue-companion-tool" -RelativePath "release-inputs/rescue-tool.bin"
    $offlineInput = New-LockedInput -FixtureRoot $draftFixture -Id "offline-tool" -Role "offline-package" -RelativePath "release-inputs/offline-tool.bin"
    Set-LockedInputs -FixtureRoot $draftFixture -ArtifactId "rescue-x64-portable" -Inputs @($draftRescueInput)
    Set-LockedInputs -FixtureRoot $draftFixture -ArtifactId "large-offline-x64-portable" -Inputs @($draftRescueInput, $offlineInput)
    Set-ReleaseDirectoryState -FixtureRoot $draftFixture -State "draft"
    Require-PackageFailure -FixtureRoot $draftFixture -ArtifactId "large-offline-x64-portable" -Scenario "draft release directory"

    Write-Host "portable package contract: PASS"
}
finally {
    foreach ($fixtureRoot in $fixtureRoots) {
        if (Test-Path -LiteralPath $fixtureRoot) {
            Remove-Item -LiteralPath $fixtureRoot -Recurse -Force
        }
    }
}
