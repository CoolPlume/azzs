Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Get-RequiredProperty {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Object,

        [Parameter(Mandatory = $true)]
        [string]$Name,

        [Parameter(Mandatory = $true)]
        [string]$Context
    )

    if ($Object -is [System.Collections.IDictionary]) {
        if (-not $Object.Contains($Name) -or $null -eq $Object[$Name]) {
            throw "$Context is missing '$Name'."
        }
        return $Object[$Name]
    }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property -or $null -eq $property.Value) {
        throw "$Context is missing '$Name'."
    }
    return $property.Value
}

function Get-Sha256Hex {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Resolve-RepositoryRelativePath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepositoryRoot,

        [Parameter(Mandatory = $true)]
        [string]$RelativePath,

        [Parameter(Mandatory = $true)]
        [string]$Context
    )

    if ([string]::IsNullOrWhiteSpace($RelativePath) -or
        $RelativePath.Contains("\") -or
        $RelativePath.StartsWith("/", [StringComparison]::Ordinal) -or
        $RelativePath -match "^[A-Za-z]:" -or
        ($RelativePath.Split("/") -contains "..")) {
        throw "$Context must use a non-empty repository-relative POSIX path."
    }

    $root = (Resolve-Path -LiteralPath $RepositoryRoot).Path.TrimEnd("\", "/")
    $candidate = $root
    foreach ($segment in $RelativePath.Split("/")) {
        if ([string]::IsNullOrWhiteSpace($segment) -or $segment -eq ".") {
            throw "$Context contains an empty or current-directory path segment."
        }
        $candidate = Join-Path $candidate $segment
    }
    $resolvedCandidate = [System.IO.Path]::GetFullPath($candidate)
    $rootWithSeparator = "$root$([System.IO.Path]::DirectorySeparatorChar)"
    if (-not $resolvedCandidate.StartsWith($rootWithSeparator, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "$Context resolves outside the repository."
    }
    return $resolvedCandidate
}

function Resolve-PortablePackagePath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$PackagePath,

        [Parameter(Mandatory = $true)]
        [string]$Context
    )

    if ([string]::IsNullOrWhiteSpace($PackagePath) -or
        $PackagePath.Contains("\") -or
        $PackagePath.StartsWith("/", [StringComparison]::Ordinal) -or
        $PackagePath -match "^[A-Za-z]:" -or
        -not $PackagePath.StartsWith("content/", [StringComparison]::Ordinal)) {
        throw "$Context must use a non-empty content/ relative POSIX package path."
    }
    foreach ($segment in $PackagePath.Split("/")) {
        if ([string]::IsNullOrWhiteSpace($segment) -or $segment -eq "." -or $segment -eq "..") {
            throw "$Context contains an empty, current-directory, or parent-directory package path segment."
        }
    }
    return $PackagePath
}

function Test-TrackedRepositoryFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepositoryRoot,

        [Parameter(Mandatory = $true)]
        [string]$RelativePath,

        [Parameter(Mandatory = $true)]
        [string]$Context
    )

    $null = & git -C $RepositoryRoot ls-files --error-unmatch -- $RelativePath 2>$null
    if ($LASTEXITCODE -ne 0) {
        throw "$Context must reference a tracked repository file."
    }
}

function ConvertTo-RepositoryRelativePath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepositoryRoot,

        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$Context
    )

    $root = (Resolve-Path -LiteralPath $RepositoryRoot).Path.TrimEnd("\", "/")
    $fullPath = [System.IO.Path]::GetFullPath($Path)
    $rootWithSeparator = "$root$([System.IO.Path]::DirectorySeparatorChar)"
    if (-not $fullPath.StartsWith($rootWithSeparator, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "$Context is outside the repository."
    }
    return $fullPath.Substring($rootWithSeparator.Length).Replace("\", "/")
}

function Get-PortableArtifactDefinition {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepositoryRoot,

        [Parameter(Mandatory = $true)]
        [string]$ArtifactId
    )

    $identityPath = Join-Path $RepositoryRoot "release/product-identity.json"
    $contentManifestPath = Join-Path $RepositoryRoot "release/artifact-content-manifest.v1.json"
    if (-not (Test-Path -LiteralPath $identityPath -PathType Leaf)) {
        throw "Portable packaging requires release/product-identity.json."
    }
    if (-not (Test-Path -LiteralPath $contentManifestPath -PathType Leaf)) {
        throw "Portable packaging requires release/artifact-content-manifest.v1.json."
    }

    $identity = Get-Content -LiteralPath $identityPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $identityMatches = @($identity.artifactMatrix | Where-Object { $_.id -eq $ArtifactId })
    if ($identityMatches.Count -ne 1) {
        throw "ArtifactId '$ArtifactId' is not present exactly once in release/product-identity.json."
    }
    $artifact = $identityMatches[0]
    if ($artifact.packageKind -ne "portable") {
        throw "ArtifactId '$ArtifactId' is not a portable artifact."
    }
    if ($artifact.architecture -ne "x64") {
        throw "ArtifactId '$ArtifactId' targets $($artifact.architecture); ARM64 portable packaging is deferred."
    }

    $contentManifest = Get-Content -LiteralPath $contentManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($contentManifest.schemaVersion -ne 1) {
        throw "Artifact content manifest schemaVersion must be 1."
    }
    $contentMatches = @($contentManifest.artifacts | Where-Object { $_.artifactId -eq $ArtifactId })
    if ($contentMatches.Count -ne 1) {
        throw "Artifact content manifest lacks a unique '$ArtifactId' entry."
    }
    $content = $contentMatches[0]
    foreach ($field in @("edition", "packageKind", "architecture")) {
        if ((Get-RequiredProperty -Object $content -Name $field -Context "Artifact content '$ArtifactId'") -ne $artifact.$field) {
            throw "Artifact content '$ArtifactId' $field does not match product identity."
        }
    }

    return [pscustomobject]@{
        Artifact = $artifact
        Content = $content
        ContentManifestPath = $contentManifestPath
        ContentManifest = $contentManifest
    }
}

function Test-RescueGate {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Content,

        [Parameter(Mandatory = $true)]
        [string]$RepositoryRoot,

        [Parameter(Mandatory = $true)]
        [string]$ArtifactId
    )

    $gate = Get-RequiredProperty -Object $Content -Name "rescueGate" -Context "Artifact content '$ArtifactId'"
    if ((Get-RequiredProperty -Object $gate -Name "adrId" -Context "Artifact content '$ArtifactId' rescueGate") -ne "ADR-0030") {
        throw "Artifact content '$ArtifactId' must be gated by ADR-0030."
    }
    if ((Get-RequiredProperty -Object $gate -Name "requiredStatus" -Context "Artifact content '$ArtifactId' rescueGate") -ne "accepted") {
        throw "Artifact content '$ArtifactId' rescueGate must require the accepted ADR-0030 status."
    }
    $adrPath = Resolve-RepositoryRelativePath -RepositoryRoot $RepositoryRoot -RelativePath ([string](Get-RequiredProperty -Object $gate -Name "relativePath" -Context "Artifact content '$ArtifactId' rescueGate")) -Context "Artifact content '$ArtifactId' rescueGate relativePath"
    if (-not (Test-Path -LiteralPath $adrPath -PathType Leaf)) {
        throw "Artifact content '$ArtifactId' ADR-0030 gate file is missing."
    }
    $adr = Get-Content -LiteralPath $adrPath -Raw -Encoding UTF8
    if ($adr -notmatch "(?m)^Status:\s+accepted\s*$" -or
        $adr -notmatch "ADR-0009") {
        throw "Artifact content '$ArtifactId' ADR-0030 gate is not satisfied."
    }
}

function Test-RescueInputEvidence {
    param(
        [Parameter(Mandatory = $true)]
        [object]$LockedInput,

        [Parameter(Mandatory = $true)]
        [string]$RepositoryRoot,

        [Parameter(Mandatory = $true)]
        [string]$Context
    )

    $evidence = Get-RequiredProperty -Object $LockedInput -Name "rescueEvidence" -Context $Context
    $validated = [ordered]@{}
    foreach ($field in @("sourcePath", "reproducibleBuildPath", "minimalSmokePath", "processTokenContractPath")) {
        $relativePath = [string](Get-RequiredProperty -Object $evidence -Name $field -Context "$Context rescueEvidence")
        $evidencePath = Resolve-RepositoryRelativePath -RepositoryRoot $RepositoryRoot -RelativePath $relativePath -Context "$Context rescueEvidence $field"
        if (-not (Test-Path -LiteralPath $evidencePath -PathType Leaf)) {
            throw "$Context rescueEvidence $field file is missing."
        }
        Test-TrackedRepositoryFile -RepositoryRoot $RepositoryRoot -RelativePath $relativePath -Context "$Context rescueEvidence $field"
        $validated[$field] = $relativePath
    }
    return [pscustomobject]$validated
}

function Test-PortableArtifactInputs {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Definition,

        [Parameter(Mandatory = $true)]
        [string]$RepositoryRoot
    )

    $artifact = $Definition.Artifact
    $content = $Definition.Content
    $artifactId = [string]$artifact.id
    [object[]]$rawInputs = @()
    $inputsProperty = $content.PSObject.Properties["inputs"]
    if ($null -ne $inputsProperty -and $null -ne $inputsProperty.Value) {
        $rawInputs = @($inputsProperty.Value | ForEach-Object { $_ })
    }
    if ($artifact.edition -eq "standard") {
        if ($rawInputs.Count -ne 0) {
            throw "Standard portable content must not declare external inputs."
        }
        return @()
    }
    if ($rawInputs.Count -eq 0) {
        throw "Artifact content '$artifactId' has no locked inputs."
    }

    Test-RescueGate -Content $content -RepositoryRoot $RepositoryRoot -ArtifactId $artifactId

    $ids = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    $sourcePaths = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    $packagePaths = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    $validated = [System.Collections.Generic.List[object]]::new()
    $debugExtensions = @(".pdb", ".ilk", ".iobj", ".ipdb", ".exp", ".lib")
    $allowedSecurityClassifications = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
    $null = $allowedSecurityClassifications.Add("allowed")
    foreach ($contentInput in $rawInputs) {
        $context = "Artifact content '$artifactId' input"
        $id = [string](Get-RequiredProperty -Object $contentInput -Name "id" -Context $context)
        $role = [string](Get-RequiredProperty -Object $contentInput -Name "role" -Context $context)
        $version = [string](Get-RequiredProperty -Object $contentInput -Name "version" -Context $context)
        $architecture = [string](Get-RequiredProperty -Object $contentInput -Name "architecture" -Context $context)
        $relativePath = [string](Get-RequiredProperty -Object $contentInput -Name "relativePath" -Context $context)
        $packagePath = [string](Get-RequiredProperty -Object $contentInput -Name "packagePath" -Context $context)
        $bytes = [Int64](Get-RequiredProperty -Object $contentInput -Name "bytes" -Context $context)
        $sha256 = [string](Get-RequiredProperty -Object $contentInput -Name "sha256" -Context $context)
        $license = [string](Get-RequiredProperty -Object $contentInput -Name "license" -Context $context)
        $source = [string](Get-RequiredProperty -Object $contentInput -Name "source" -Context $context)
        $securityClassification = [string](Get-RequiredProperty -Object $contentInput -Name "securityClassification" -Context $context)
        if ([string]::IsNullOrWhiteSpace($id) -or [string]::IsNullOrWhiteSpace($role) -or
            [string]::IsNullOrWhiteSpace($version) -or [string]::IsNullOrWhiteSpace($license) -or
            [string]::IsNullOrWhiteSpace($source)) {
            throw "$context has empty identity, role, version, license, or source metadata."
        }
        $packagePath = Resolve-PortablePackagePath -PackagePath $packagePath -Context "$context '$id' packagePath"
        if (-not $ids.Add($id) -or -not $sourcePaths.Add($relativePath) -or -not $packagePaths.Add($packagePath)) {
            throw "$context contains a duplicate id, relativePath, or packagePath."
        }
        if ($architecture -ne "x64") {
            throw "$context '$id' architecture must be x64."
        }
        if ($bytes -lt 0 -or $sha256 -notmatch "^[a-fA-F0-9]{64}$") {
            throw "$context '$id' has invalid bytes or SHA256."
        }
        if (-not $allowedSecurityClassifications.Contains($securityClassification)) {
            throw "$context '$id' securityClassification must exactly match an allowed classification."
        }
        if ($debugExtensions -contains [System.IO.Path]::GetExtension($relativePath).ToLowerInvariant()) {
            throw "$context '$id' is a forbidden debug or build artifact."
        }
        $inputPath = Resolve-RepositoryRelativePath -RepositoryRoot $RepositoryRoot -RelativePath $relativePath -Context "$context '$id' relativePath"
        if (-not (Test-Path -LiteralPath $inputPath -PathType Leaf)) {
            throw "$context '$id' file is missing."
        }
        $file = Get-Item -LiteralPath $inputPath
        if ($file.Length -ne $bytes -or (Get-Sha256Hex -Path $inputPath) -ne $sha256.ToLowerInvariant()) {
            throw "$context '$id' bytes or SHA256 do not match the locked input."
        }
        $rescueEvidence = $null
        if ($role -eq "rescue-companion-tool") {
            $rescueEvidence = Test-RescueInputEvidence -LockedInput $contentInput -RepositoryRoot $RepositoryRoot -Context "$context '$id'"
        }
        elseif ($artifact.edition -eq "rescue") {
            throw "Artifact content '$artifactId' rescue input '$id' must use role rescue-companion-tool."
        }
        $validatedInput = [ordered]@{
                id = $id
                role = $role
                version = $version
                architecture = $architecture
                relativePath = $relativePath
                packagePath = $packagePath
                bytes = $bytes
                sha256 = $sha256.ToLowerInvariant()
                license = $license
                source = $source
                securityClassification = $securityClassification
        }
        if ($null -ne $rescueEvidence) {
            $validatedInput["rescueEvidence"] = $rescueEvidence
        }
        $validated.Add([pscustomobject]$validatedInput)
    }
    return $validated.ToArray()
}

function Test-LargeOfflineArtifactContent {
    param(
        [Parameter(Mandatory = $true)]
        [object]$LargeDefinition,

        [Parameter(Mandatory = $true)]
        [object[]]$LargeInputs,

        [Parameter(Mandatory = $true)]
        [object[]]$RescueInputs,

        [Parameter(Mandatory = $true)]
        [string]$RepositoryRoot
    )

    if ($LargeInputs.Count -le $RescueInputs.Count) {
        throw "Large offline content must be a strict rescue input superset."
    }
    $largeById = @{}
    foreach ($largeContentInput in $LargeInputs) {
        $largeById[$largeContentInput.id] = $largeContentInput
    }
    foreach ($rescueContentInput in $RescueInputs) {
        if (-not $largeById.ContainsKey($rescueContentInput.id)) {
            throw "Large offline content is missing rescue input '$($rescueContentInput.id)'."
        }
        $largeInput = $largeById[$rescueContentInput.id]
        foreach ($field in @("role", "version", "architecture", "relativePath", "packagePath", "bytes", "sha256", "license", "source", "securityClassification")) {
            if ($largeInput.$field -ne $rescueContentInput.$field) {
                throw "Large offline content changes rescue input '$($rescueContentInput.id)' $field."
            }
        }
        $inputEvidence = $rescueContentInput.PSObject.Properties["rescueEvidence"]
        $largeEvidence = $largeInput.PSObject.Properties["rescueEvidence"]
        if (($null -eq $inputEvidence) -ne ($null -eq $largeEvidence)) {
            throw "Large offline content changes rescue input '$($rescueContentInput.id)' rescueEvidence."
        }
        if ($null -ne $inputEvidence) {
            foreach ($field in @("sourcePath", "reproducibleBuildPath", "minimalSmokePath", "processTokenContractPath")) {
                if ($largeEvidence.Value.$field -ne $inputEvidence.Value.$field) {
                    throw "Large offline content changes rescue input '$($rescueContentInput.id)' rescueEvidence $field."
                }
            }
        }
    }

    $gate = Get-RequiredProperty -Object $LargeDefinition.Content -Name "releaseDirectoryGate" -Context "Large offline artifact content"
    if ((Get-RequiredProperty -Object $gate -Name "requiredReleaseState" -Context "Large offline releaseDirectoryGate") -ne "release") {
        throw "Large offline releaseDirectoryGate must require release state."
    }
    $directoryPath = Resolve-RepositoryRelativePath -RepositoryRoot $RepositoryRoot -RelativePath ([string](Get-RequiredProperty -Object $gate -Name "relativePath" -Context "Large offline releaseDirectoryGate")) -Context "Large offline releaseDirectoryGate relativePath"
    if (-not (Test-Path -LiteralPath $directoryPath -PathType Leaf)) {
        throw "Large offline release directory is missing."
    }
    $directory = Get-Content -LiteralPath $directoryPath -Raw -Encoding UTF8
    $state = [regex]::Match($directory, "(?m)^\\s*release_state\\s*=\\s*`"([^`"]+)`"\\s*$")
    if (-not $state.Success -or $state.Groups[1].Value -ne "release") {
        throw "Large offline release directory is not release_state = `"release`"."
    }
}

function Test-PortableRepositoryClean {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepositoryRoot
    )

    $status = @(& git -C $RepositoryRoot status --porcelain=v1 --untracked-files=all 2>$null)
    if ($LASTEXITCODE -ne 0) {
        throw "Portable packaging could not inspect the current Git status."
    }
    if ($status.Count -ne 0) {
        throw "Portable packaging requires a clean current Git index, worktree, and untracked-file state."
    }
}

function Test-PortableBuildManifest {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepositoryRoot,

        [Parameter(Mandatory = $true)]
        [string]$Architecture
    )

    $manifestPath = Join-Path $RepositoryRoot "out/manifests/windows-$($Architecture.ToLowerInvariant())-release.json"
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        throw "Portable packaging requires a completed $Architecture build manifest."
    }
    $manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $commit = (& git -C $RepositoryRoot rev-parse HEAD).Trim()
    if ($manifest.schemaVersion -ne 1 -or $manifest.result -ne "succeeded" -or
        $manifest.source.dirty -ne $false -or $manifest.source.commit -ne $commit -or
        $manifest.target.architecture -ne $Architecture -or $manifest.target.configuration -ne "Release") {
        throw "Portable packaging requires a clean, succeeded $Architecture Release build manifest for the current commit."
    }
    Test-PortableRepositoryClean -RepositoryRoot $RepositoryRoot
    return $manifest
}
