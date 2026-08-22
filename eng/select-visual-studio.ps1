[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$VswherePath,

    [Parameter(Mandatory = $true)]
    [ValidateSet("x64", "ARM64")]
    [string]$Architecture
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$minimumVisualStudioVersion = [version]"18.8.2"
$maximumVisualStudioVersion = [version]"19.0.0"
$visualStudioVersionRange = "[18.8.2,19.0)"

$requiredVisualStudioComponents = @(
    "Microsoft.Component.MSBuild",
    "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
    "Microsoft.VisualStudio.Component.VC.CMake.Project"
)
if ($Architecture -eq "ARM64") {
    $requiredVisualStudioComponents += "Microsoft.VisualStudio.Component.VC.Tools.ARM64"
}

$uwpVisualStudioComponents = @(
    "Microsoft.VisualStudio.ComponentGroup.UWP.VC",
    "Microsoft.VisualStudio.ComponentGroup.UWP.VC.v142"
)
$visualStudioInstances = @()
foreach ($uwpVisualStudioComponent in $uwpVisualStudioComponents) {
    $componentsForQuery = @($requiredVisualStudioComponents + $uwpVisualStudioComponent)
    $instanceJson = & $VswherePath `
        -latest `
        -products * `
        -version $visualStudioVersionRange `
        -requires $componentsForQuery `
        -format json `
        -utf8
    if ($LASTEXITCODE -ne 0) {
        throw "vswhere.exe failed while locating Visual Studio 2026."
    }

    $visualStudioInstances = @()
    if ($instanceJson) {
        try {
            $parsedVisualStudioInstances = ConvertFrom-Json ($instanceJson -join [Environment]::NewLine)
            $visualStudioInstances = @($parsedVisualStudioInstances)
        }
        catch {
            throw "vswhere.exe returned invalid JSON: $($_.Exception.Message)"
        }
    }
    if ($visualStudioInstances.Count -gt 0) {
        break
    }
}

$componentSummary = if ($Architecture -eq "ARM64") { "x64 and ARM64" } else { "x64" }
if ($visualStudioInstances.Count -eq 0) {
    throw "Visual Studio 2026 Stable 18.8.2 or newer with $componentSummary C++, CMake, and UWP C++ components was not found."
}

$visualStudioInstance = $visualStudioInstances[0]
$catalogProperty = $visualStudioInstance.PSObject.Properties["catalog"]
if ($null -eq $catalogProperty) {
    throw "The selected Visual Studio instance did not report catalog metadata."
}
$productDisplayVersionProperty = $visualStudioInstance.catalog.PSObject.Properties["productDisplayVersion"]
if ($null -eq $productDisplayVersionProperty -or
    [string]::IsNullOrWhiteSpace([string]$productDisplayVersionProperty.Value)) {
    throw "The selected Visual Studio instance did not report a product version."
}

$visualStudioVersion = [version]::new(0, 0)
if (-not [version]::TryParse([string]$productDisplayVersionProperty.Value, [ref]$visualStudioVersion) -or
    $visualStudioVersion -lt $minimumVisualStudioVersion -or
    $visualStudioVersion -ge $maximumVisualStudioVersion) {
    throw "The selected Visual Studio product version '$($productDisplayVersionProperty.Value)' is outside the supported Stable 18.8.2 range."
}

$visualStudioInstance
