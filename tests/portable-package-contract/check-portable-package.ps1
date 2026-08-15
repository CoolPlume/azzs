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
            "Microsoft.WindowsAppRuntime.dll"
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
}

try {
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
    Require (@($standardManifest.inputs).Count -eq 0) "standard package must not claim external content inputs"
    Require (-not ((Get-Content -LiteralPath $standardCandidates[2] -Raw).Contains($standardFixture))) "package manifest leaked an absolute fixture path"

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
