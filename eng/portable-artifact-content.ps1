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
        return [pscustomobject]@{ Value = $Object[$Name] }
    }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property -or $null -eq $property.Value) {
        throw "$Context is missing '$Name'."
    }
    return [pscustomobject]@{ Value = $property.Value }
}

function Get-RequiredStringProperty {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Object,

        [Parameter(Mandatory = $true)]
        [string]$Name,

        [Parameter(Mandatory = $true)]
        [string]$Context
    )

    $value = (Get-RequiredProperty -Object $Object -Name $Name -Context $Context).Value
    if ($value -isnot [string]) {
        throw "$Context '$Name' must be a JSON string."
    }
    return $value
}

function Get-RequiredIntegerProperty {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Object,

        [Parameter(Mandatory = $true)]
        [string]$Name,

        [Parameter(Mandatory = $true)]
        [string]$Context
    )

    $value = (Get-RequiredProperty -Object $Object -Name $Name -Context $Context).Value
    if ($value.GetType().FullName -notin @(
            "System.Byte", "System.SByte", "System.Int16", "System.UInt16",
            "System.Int32", "System.UInt32", "System.Int64", "System.UInt64"
        )) {
        throw "$Context '$Name' must be a JSON integer."
    }
    return [Int64]$value
}

function Get-RequiredBooleanProperty {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Object,

        [Parameter(Mandatory = $true)]
        [string]$Name,

        [Parameter(Mandatory = $true)]
        [string]$Context
    )

    $value = (Get-RequiredProperty -Object $Object -Name $Name -Context $Context).Value
    if ($value -isnot [bool]) {
        throw "$Context '$Name' must be a JSON boolean."
    }
    return $value
}

function Get-RequiredArrayProperty {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Object,

        [Parameter(Mandatory = $true)]
        [string]$Name,

        [Parameter(Mandatory = $true)]
        [string]$Context
    )

    # Preserve an empty JSON array instead of letting the PowerShell pipeline erase it.
    if ($Object -is [System.Collections.IDictionary]) {
        if (-not $Object.Contains($Name) -or $null -eq $Object[$Name]) {
            throw "$Context is missing '$Name'."
        }
        $value = $Object[$Name]
    }
    else {
        $property = $Object.PSObject.Properties[$Name]
        if ($null -eq $property -or $null -eq $property.Value) {
            throw "$Context is missing '$Name'."
        }
        $value = $property.Value
    }
    if ($value -isnot [System.Array]) {
        throw "$Context '$Name' must be a JSON array."
    }
    return [pscustomobject]@{ Value = [object[]]$value }
}

function Get-RequiredObjectProperty {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Object,

        [Parameter(Mandatory = $true)]
        [string]$Name,

        [Parameter(Mandatory = $true)]
        [string]$Context
    )

    $value = (Get-RequiredProperty -Object $Object -Name $Name -Context $Context).Value
    if ($value -is [System.Array] -or
        ($value -isnot [System.Collections.IDictionary] -and $value -isnot [pscustomobject])) {
        throw "$Context '$Name' must be a JSON object."
    }
    return $value
}

function Get-Sha256Hex {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-ExistingNonReparsePath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$Context
    )

    $fullPath = [System.IO.Path]::GetFullPath($Path)
    if (-not (Test-Path -LiteralPath $fullPath)) {
        throw "$Context must exist."
    }
    $root = [System.IO.Path]::GetPathRoot($fullPath)
    if ([string]::IsNullOrWhiteSpace($root)) {
        throw "$Context must be an absolute path."
    }

    $current = $root
    foreach ($segment in $fullPath.Substring($root.Length).Split(
            [char[]]@('\', '/'),
            [System.StringSplitOptions]::RemoveEmptyEntries)) {
        $current = Join-Path $current $segment
        $item = Get-Item -LiteralPath $current -Force -ErrorAction Stop
        if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "$Context must not pass through a reparse point."
        }
    }
    return $fullPath
}

function Assert-PathChainWithoutReparsePoint {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$Context
    )

    $fullPath = [System.IO.Path]::GetFullPath($Path)
    $root = [System.IO.Path]::GetPathRoot($fullPath)
    if ([string]::IsNullOrWhiteSpace($root)) {
        throw "$Context must be an absolute path."
    }

    $current = $root
    foreach ($segment in $fullPath.Substring($root.Length).Split(
            [char[]]@('\', '/'),
            [System.StringSplitOptions]::RemoveEmptyEntries)) {
        $current = Join-Path $current $segment
        try {
            $attributes = [System.IO.File]::GetAttributes($current)
        }
        catch [System.IO.FileNotFoundException] {
            break
        }
        catch [System.IO.DirectoryNotFoundException] {
            break
        }
        if (($attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "$Context must not pass through a reparse point."
        }
    }
    return $fullPath
}

function Assert-NoReparsePointsBelow {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$Context
    )

    $root = Assert-PathChainWithoutReparsePoint -Path $Path -Context $Context
    $pending = [System.Collections.Generic.Stack[string]]::new()
    $pending.Push($root)
    while ($pending.Count -gt 0) {
        $current = $pending.Pop()
        foreach ($entry in [System.IO.Directory]::EnumerateFileSystemEntries($current)) {
            $attributes = [System.IO.File]::GetAttributes($entry)
            if (($attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "$Context must not contain a reparse point."
            }
            if (($attributes -band [System.IO.FileAttributes]::Directory) -ne 0) {
                $pending.Push($entry)
            }
        }
    }
    return $root
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
    return Get-ExistingNonReparsePath -Path $resolvedCandidate -Context $Context
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

function Resolve-BundledCatalogPackagePath {
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
        -not $PackagePath.StartsWith("catalog/", [StringComparison]::Ordinal)) {
        throw "$Context must use a non-empty catalog/ relative POSIX package path."
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

    $root = (Get-ExistingNonReparsePath -Path $RepositoryRoot -Context "Repository root").TrimEnd("\", "/")
    $fullPath = Get-ExistingNonReparsePath -Path $Path -Context $Context
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
    $identityPath = Get-ExistingNonReparsePath `
        -Path $identityPath `
        -Context "Portable packaging product identity"
    $contentManifestPath = Get-ExistingNonReparsePath `
        -Path $contentManifestPath `
        -Context "Portable packaging artifact content manifest"

    $identity = Get-Content -LiteralPath $identityPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $identityMatrix = (Get-RequiredArrayProperty `
        -Object $identity `
        -Name "artifactMatrix" `
        -Context "Product identity").Value
    $identityMatches = @(
        foreach ($candidate in $identityMatrix) {
            if ((Get-RequiredStringProperty -Object $candidate -Name "id" -Context "Product identity artifact") -eq $ArtifactId) {
                $candidate
            }
        }
    )
    if ($identityMatches.Count -ne 1) {
        throw "ArtifactId '$ArtifactId' is not present exactly once in release/product-identity.json."
    }
    $artifact = $identityMatches[0]
    $artifactEdition = Get-RequiredStringProperty -Object $artifact -Name "edition" -Context "Product identity artifact '$ArtifactId'"
    $artifactPackageKind = Get-RequiredStringProperty -Object $artifact -Name "packageKind" -Context "Product identity artifact '$ArtifactId'"
    $artifactArchitecture = Get-RequiredStringProperty -Object $artifact -Name "architecture" -Context "Product identity artifact '$ArtifactId'"
    if ($artifactPackageKind -ne "portable") {
        throw "ArtifactId '$ArtifactId' is not a portable artifact."
    }
    if ($artifactArchitecture -ne "x64") {
        throw "ArtifactId '$ArtifactId' targets $artifactArchitecture; ARM64 portable packaging is deferred."
    }

    $contentManifest = Get-Content -LiteralPath $contentManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if ((Get-RequiredIntegerProperty -Object $contentManifest -Name "schemaVersion" -Context "Artifact content manifest") -ne 1) {
        throw "Artifact content manifest schemaVersion must be 1."
    }
    $contentArtifacts = (Get-RequiredArrayProperty `
        -Object $contentManifest `
        -Name "artifacts" `
        -Context "Artifact content manifest").Value
    $contentMatches = @(
        foreach ($candidate in $contentArtifacts) {
            if ((Get-RequiredStringProperty -Object $candidate -Name "artifactId" -Context "Artifact content manifest artifact") -eq $ArtifactId) {
                $candidate
            }
        }
    )
    if ($contentMatches.Count -ne 1) {
        throw "Artifact content manifest lacks a unique '$ArtifactId' entry."
    }
    $content = $contentMatches[0]
    foreach ($field in @("edition", "packageKind", "architecture")) {
        $contentValue = Get-RequiredStringProperty -Object $content -Name $field -Context "Artifact content '$ArtifactId'"
        $identityValue = switch ($field) {
            "edition" { $artifactEdition }
            "packageKind" { $artifactPackageKind }
            "architecture" { $artifactArchitecture }
        }
        if ($contentValue -ne $identityValue) {
            throw "Artifact content '$ArtifactId' $field does not match product identity."
        }
    }

    return [pscustomobject]@{
        Artifact = $artifact
        Content = $content
        BundledCatalogResources = @(Test-BundledCatalogResources -Content $content -RepositoryRoot $RepositoryRoot -ArtifactId $ArtifactId -ValidateSource:$false)
        ContentManifestPath = $contentManifestPath
        ContentManifest = $contentManifest
    }
}

function Test-BundledCatalogResources {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Content,

        [Parameter(Mandatory = $true)]
        [string]$RepositoryRoot,

        [Parameter(Mandatory = $true)]
        [string]$ArtifactId,

        [switch]$ValidateSource
    )

    $expectedResources = @(
        [pscustomobject]@{
            id = "software-catalog"
            relativePath = "catalog/software-catalog.toml"
            packagePath = "catalog/software-catalog.toml"
        },
        [pscustomobject]@{
            id = "software-optimization-catalog"
            relativePath = "catalog/software-optimization-catalog.toml"
            packagePath = "catalog/software-optimization-catalog.toml"
        }
    )
    [object[]]$rawResources = (Get-RequiredArrayProperty `
        -Object $Content `
        -Name "bundledCatalogResources" `
        -Context "Artifact content '$ArtifactId'").Value
    if ($rawResources.Count -ne $expectedResources.Count) {
        throw "Artifact content '$ArtifactId' must declare exactly two bundled catalog resources."
    }

    $resourcesById = @{}
    foreach ($rawResource in $rawResources) {
        $context = "Artifact content '$ArtifactId' bundled catalog resource"
        $id = Get-RequiredStringProperty -Object $rawResource -Name "id" -Context $context
        if ([string]::IsNullOrWhiteSpace($id) -or $resourcesById.ContainsKey($id)) {
            throw "$context contains an empty or duplicate id."
        }
        $resourcesById[$id] = $rawResource
    }

    $validated = [System.Collections.Generic.List[object]]::new()
    foreach ($expectedResource in $expectedResources) {
        if (-not $resourcesById.ContainsKey($expectedResource.id)) {
            throw "Artifact content '$ArtifactId' is missing bundled catalog resource '$($expectedResource.id)'."
        }
        $resource = $resourcesById[$expectedResource.id]
        $context = "Artifact content '$ArtifactId' bundled catalog resource '$($expectedResource.id)'"
        $relativePath = Get-RequiredStringProperty -Object $resource -Name "relativePath" -Context $context
        $packagePath = Resolve-BundledCatalogPackagePath `
            -PackagePath (Get-RequiredStringProperty -Object $resource -Name "packagePath" -Context $context) `
            -Context "$context packagePath"
        $bytes = Get-RequiredIntegerProperty -Object $resource -Name "bytes" -Context $context
        $sha256 = Get-RequiredStringProperty -Object $resource -Name "sha256" -Context $context
        if ($relativePath -ne $expectedResource.relativePath -or $packagePath -ne $expectedResource.packagePath) {
            throw "$context must use the fixed catalog resource layout."
        }
        if ($bytes -le 0 -or $sha256 -notmatch "^[a-fA-F0-9]{64}$") {
            throw "$context has invalid bytes or SHA256."
        }
        if ($ValidateSource) {
            $sourcePath = Resolve-RepositoryRelativePath -RepositoryRoot $RepositoryRoot -RelativePath $relativePath -Context "$context relativePath"
            if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
                throw "$context source file is missing."
            }
            Test-TrackedRepositoryFile -RepositoryRoot $RepositoryRoot -RelativePath $relativePath -Context "$context source file"
            $sourceFile = Get-Item -LiteralPath $sourcePath
            if ($sourceFile.Length -ne $bytes -or (Get-Sha256Hex -Path $sourcePath) -ne $sha256.ToLowerInvariant()) {
                throw "$context bytes or SHA256 do not match the locked resource."
            }
        }
        $validated.Add([pscustomobject][ordered]@{
                id = $expectedResource.id
                relativePath = $relativePath
                packagePath = $packagePath
                bytes = $bytes
                sha256 = $sha256.ToLowerInvariant()
            })
    }
    return $validated.ToArray()
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

    $gate = Get-RequiredObjectProperty -Object $Content -Name "rescueGate" -Context "Artifact content '$ArtifactId'"
    if ((Get-RequiredStringProperty -Object $gate -Name "adrId" -Context "Artifact content '$ArtifactId' rescueGate") -ne "ADR-0030") {
        throw "Artifact content '$ArtifactId' must be gated by ADR-0030."
    }
    if ((Get-RequiredStringProperty -Object $gate -Name "requiredStatus" -Context "Artifact content '$ArtifactId' rescueGate") -ne "accepted") {
        throw "Artifact content '$ArtifactId' rescueGate must require the accepted ADR-0030 status."
    }
    $expectedAdrRelativePath = "docs/adr/0030-controlled-rescue-tool-release-boundary.md"
    $adrRelativePath = Get-RequiredStringProperty `
        -Object $gate `
        -Name "relativePath" `
        -Context "Artifact content '$ArtifactId' rescueGate"
    if ($adrRelativePath -ne $expectedAdrRelativePath) {
        throw "Artifact content '$ArtifactId' rescueGate must reference $expectedAdrRelativePath."
    }
    $adrPath = Resolve-RepositoryRelativePath `
        -RepositoryRoot $RepositoryRoot `
        -RelativePath $adrRelativePath `
        -Context "Artifact content '$ArtifactId' rescueGate relativePath"
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

    $evidence = Get-RequiredObjectProperty -Object $LockedInput -Name "rescueEvidence" -Context $Context
    $validated = [ordered]@{}
    foreach ($field in @("sourcePath", "reproducibleBuildPath", "minimalSmokePath", "processTokenContractPath")) {
        $relativePath = Get-RequiredStringProperty -Object $evidence -Name $field -Context "$Context rescueEvidence"
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
    $artifactId = Get-RequiredStringProperty -Object $artifact -Name "id" -Context "Product identity artifact"
    $inputs = (Get-RequiredArrayProperty `
        -Object $content `
        -Name "inputs" `
        -Context "Artifact content '$artifactId'").Value
    [object[]]$rawInputs = $inputs
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
        $id = Get-RequiredStringProperty -Object $contentInput -Name "id" -Context $context
        $role = Get-RequiredStringProperty -Object $contentInput -Name "role" -Context $context
        $version = Get-RequiredStringProperty -Object $contentInput -Name "version" -Context $context
        $architecture = Get-RequiredStringProperty -Object $contentInput -Name "architecture" -Context $context
        $relativePath = Get-RequiredStringProperty -Object $contentInput -Name "relativePath" -Context $context
        $packagePath = Get-RequiredStringProperty -Object $contentInput -Name "packagePath" -Context $context
        $bytes = Get-RequiredIntegerProperty -Object $contentInput -Name "bytes" -Context $context
        $sha256 = Get-RequiredStringProperty -Object $contentInput -Name "sha256" -Context $context
        $license = Get-RequiredStringProperty -Object $contentInput -Name "license" -Context $context
        $source = Get-RequiredStringProperty -Object $contentInput -Name "source" -Context $context
        $securityClassification = Get-RequiredStringProperty -Object $contentInput -Name "securityClassification" -Context $context
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

    $bundledCatalogResources = @(
        Test-BundledCatalogResources `
            -Content $LargeDefinition.Content `
            -RepositoryRoot $RepositoryRoot `
            -ArtifactId $(Get-RequiredStringProperty `
                    -Object $LargeDefinition.Content `
                    -Name "artifactId" `
                    -Context "Large offline artifact content") `
            -ValidateSource
    )
    $gate = Get-RequiredObjectProperty -Object $LargeDefinition.Content -Name "releaseDirectoryGate" -Context "Large offline artifact content"
    if ((Get-RequiredStringProperty -Object $gate -Name "requiredReleaseState" -Context "Large offline releaseDirectoryGate") -ne "release") {
        throw "Large offline releaseDirectoryGate must require release state."
    }
    $gateRelativePath = Get-RequiredStringProperty -Object $gate -Name "relativePath" -Context "Large offline releaseDirectoryGate"
    $matchingResources = @($bundledCatalogResources | Where-Object {
            $_.relativePath -eq $gateRelativePath
        })
    if ($matchingResources.Count -ne 1) {
        throw "Large offline releaseDirectoryGate must match exactly one bundled catalog resource."
    }
    $directoryPath = Resolve-RepositoryRelativePath -RepositoryRoot $RepositoryRoot -RelativePath $gateRelativePath -Context "Large offline releaseDirectoryGate relativePath"
    if (-not (Test-Path -LiteralPath $directoryPath -PathType Leaf)) {
        throw "Large offline release directory is missing."
    }
    $directory = Get-Content -LiteralPath $directoryPath -Raw -Encoding UTF8
    $state = [regex]::Match($directory, "(?m)^\s*release_state\s*=\s*`"([^`"]+)`"\s*$")
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
    $manifestPath = Get-ExistingNonReparsePath `
        -Path $manifestPath `
        -Context "Portable build manifest"
    $manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $commit = (& git -C $RepositoryRoot rev-parse HEAD).Trim()
    $source = Get-RequiredObjectProperty -Object $manifest -Name "source" -Context "Portable build manifest"
    $target = Get-RequiredObjectProperty -Object $manifest -Name "target" -Context "Portable build manifest"
    if ((Get-RequiredIntegerProperty -Object $manifest -Name "schemaVersion" -Context "Portable build manifest") -ne 1 -or
        (Get-RequiredStringProperty -Object $manifest -Name "result" -Context "Portable build manifest") -ne "succeeded" -or
        (Get-RequiredBooleanProperty -Object $source -Name "dirty" -Context "Portable build manifest source") -ne $false -or
        (Get-RequiredStringProperty -Object $source -Name "commit" -Context "Portable build manifest source") -ne $commit -or
        (Get-RequiredStringProperty -Object $target -Name "architecture" -Context "Portable build manifest target") -ne $Architecture -or
        (Get-RequiredStringProperty -Object $target -Name "configuration" -Context "Portable build manifest target") -ne "Release") {
        throw "Portable packaging requires a clean, succeeded $Architecture Release build manifest for the current commit."
    }
    [object[]]$manifestArtifacts = (Get-RequiredArrayProperty `
            -Object $manifest `
            -Name "artifacts" `
            -Context "Portable build manifest").Value
    $payloadDirectory = Get-ExistingNonReparsePath `
        -Path (Join-Path $RepositoryRoot "out/windows/$Architecture/Release") `
        -Context "Portable build payload"
    Assert-NoReparsePointsBelow `
        -Path $payloadDirectory `
        -Context "Portable build payload" | Out-Null
    $payloadRoot = $payloadDirectory.TrimEnd("\", "/")
    $manifestArtifactPaths = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase
    )
    foreach ($artifact in $manifestArtifacts) {
        $artifactPath = Get-RequiredStringProperty `
            -Object $artifact `
            -Name "path" `
            -Context "Portable build manifest artifact"
        if ([string]::IsNullOrWhiteSpace($artifactPath) -or
            $artifactPath.Contains("\") -or
            $artifactPath.StartsWith("/", [StringComparison]::Ordinal) -or
            $artifactPath -match "^[A-Za-z]:" -or
            @($artifactPath.Split("/") | Where-Object {
                    [string]::IsNullOrWhiteSpace($_) -or $_ -eq "." -or $_ -eq ".."
                }).Count -gt 0) {
            throw "Portable build manifest artifact has an unsafe output path."
        }
        $artifactBytes = Get-RequiredIntegerProperty `
            -Object $artifact `
            -Name "bytes" `
            -Context "Portable build manifest artifact '$artifactPath'"
        $artifactSha256 = Get-RequiredStringProperty `
            -Object $artifact `
            -Name "sha256" `
            -Context "Portable build manifest artifact '$artifactPath'"
        if ($artifactBytes -lt 0 -or $artifactSha256 -notmatch "^[a-fA-F0-9]{64}$") {
            throw "Portable build manifest artifact '$artifactPath' has invalid bytes or SHA256."
        }
        if (-not $manifestArtifactPaths.Add($artifactPath)) {
            throw "Portable build manifest contains duplicate output artifact '$artifactPath'."
        }
        $artifactFullPath = Get-ExistingNonReparsePath `
            -Path (Join-Path $payloadRoot ($artifactPath.Replace("/", "\"))) `
            -Context "Portable build manifest artifact '$artifactPath'"
        if ((Get-Item -LiteralPath $artifactFullPath).PSIsContainer -or
            (Get-Item -LiteralPath $artifactFullPath).Length -ne $artifactBytes -or
            (Get-Sha256Hex -Path $artifactFullPath) -ne $artifactSha256.ToLowerInvariant()) {
            throw "Portable build manifest artifact '$artifactPath' does not match the current Release output."
        }
    }
    $payloadArtifactPaths = @(
        Get-ChildItem -LiteralPath $payloadRoot -File -Recurse |
            ForEach-Object {
                $_.FullName.Substring($payloadRoot.Length).TrimStart("\", "/").Replace("\", "/")
            }
    )
    if ($payloadArtifactPaths.Count -ne $manifestArtifactPaths.Count -or
        @($payloadArtifactPaths | Where-Object { -not $manifestArtifactPaths.Contains($_) }).Count -ne 0) {
        throw "Portable build manifest does not completely describe the current Release output."
    }
    Test-PortableRepositoryClean -RepositoryRoot $RepositoryRoot
    return $manifest
}
