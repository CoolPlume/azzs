[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("x64", "ARM64")]
    [string]$Architecture,

    [Parameter(Mandatory = $true)]
    [ValidateSet("started", "succeeded", "failed")]
    [string]$Result,

    [Parameter(Mandatory = $true)]
    [string]$RepositoryRoot,

    [Parameter(Mandatory = $true)]
    [string]$VisualStudioPath,

    [Parameter(Mandatory = $true)]
    [string]$VisualStudioVersion,

    [Parameter(Mandatory = $true)]
    [string]$MSBuildPath,

    [Parameter(Mandatory = $true)]
    [string]$CMakePath,

    [Parameter(Mandatory = $true)]
    [string]$WindowsSdkRelease,

    [Parameter(Mandatory = $true)]
    [string]$OutputPath,

    [string]$FailureMessage = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "portable-artifact-content.ps1")

$projectPath = Join-Path $RepositoryRoot "src/adapters/ui/winui/Azzs.WinUI.vcxproj"
$projectPath = Get-ExistingNonReparsePath `
    -Path $projectPath `
    -Context "Build manifest project file"
$projectXml = [xml](Get-Content -LiteralPath $projectPath -Raw)
$namespaces = New-Object System.Xml.XmlNamespaceManager($projectXml.NameTable)
$namespaces.AddNamespace("msb", $projectXml.DocumentElement.NamespaceURI)

$packages = @(
    $projectXml.SelectNodes("//msb:PackageReference", $namespaces) | ForEach-Object {
        [ordered]@{
            id = $_.Include
            version = $_.Version
        }
    }
)

if (-not (Test-Path -LiteralPath $CMakePath)) {
    throw "Visual Studio CMake was not found."
}
$cmakeVersion = (& $CMakePath --version | Select-Object -First 1).Trim()
$msbuildVersion = (& $MSBuildPath -version -nologo | Select-Object -Last 1).Trim()

$toolsetVersionPath = Join-Path $VisualStudioPath "VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt"
$toolsetVersion = if (Test-Path -LiteralPath $toolsetVersionPath) {
    (Get-Content -LiteralPath $toolsetVersionPath -Raw).Trim()
} else {
    "unavailable"
}

$clPath = Join-Path $VisualStudioPath "VC/Tools/MSVC/$toolsetVersion/bin/Hostx64/$Architecture/cl.exe"
$clVersion = if (Test-Path -LiteralPath $clPath) {
    (Get-Item -LiteralPath $clPath).VersionInfo.FileVersion
} else {
    "unavailable"
}

$payloadDirectory = Join-Path $RepositoryRoot "out/windows/$Architecture/Release"
$artifacts = @()
if (Test-Path -LiteralPath $payloadDirectory -PathType Container) {
    Assert-NoReparsePointsBelow `
        -Path $payloadDirectory `
        -Context "Build manifest payload"
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
}

$gitCommit = (& git -C $RepositoryRoot rev-parse HEAD).Trim()
$gitDirty = @(& git -C $RepositoryRoot status --porcelain).Count -ne 0
$manifest = [ordered]@{
    schemaVersion = 1
    result = $Result
    generatedAtUtc = [DateTime]::UtcNow.ToString("o")
    source = [ordered]@{
        commit = $gitCommit
        dirty = $gitDirty
    }
    host = [ordered]@{
        os = [System.Environment]::OSVersion.VersionString
        processArchitecture = $env:PROCESSOR_ARCHITECTURE
        githubActions = $env:GITHUB_ACTIONS -eq "true"
        runnerName = $env:RUNNER_NAME
        runnerOs = $env:RUNNER_OS
        runnerArchitecture = $env:RUNNER_ARCH
        imageOs = $env:ImageOS
        imageVersion = $env:ImageVersion
    }
    target = [ordered]@{
        architecture = $Architecture
        configuration = "Release"
        windowsMinimumLoadVersion = "10.0.17763.0"
        windowsDesignTarget = "10.0.19045"
    }
    toolchain = [ordered]@{
        visualStudio = $VisualStudioVersion
        visualStudioPath = $VisualStudioPath
        msvc = $toolsetVersion
        compiler = $clVersion
        msbuild = $msbuildVersion
        cmake = $cmakeVersion
        windowsSdkRelease = $WindowsSdkRelease
        windowsSdkTarget = "10.0.28000.0"
        cppMode = "/std:c++latest"
        runtimeLibrary = "/MT"
        packages = $packages
    }
    payloadDirectory = $payloadDirectory
    artifacts = $artifacts
    failure = $FailureMessage
}

$outputFullPath = [System.IO.Path]::GetFullPath($OutputPath)
$repositoryFullPath = (Get-ExistingNonReparsePath `
        -Path $RepositoryRoot `
        -Context "Build manifest repository root").TrimEnd([char[]]@('\', '/'))
$repositoryPrefix = "$repositoryFullPath$([System.IO.Path]::DirectorySeparatorChar)"
if (-not $outputFullPath.StartsWith($repositoryPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Build manifest output path must be strictly inside the repository."
}
$OutputPath = Assert-PathChainWithoutReparsePoint `
    -Path $outputFullPath `
    -Context "Build manifest output path"
$outputDirectory = Split-Path -Parent $OutputPath
New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
$outputDirectory = Get-ExistingNonReparsePath `
    -Path $outputDirectory `
    -Context "Build manifest output directory"
if (Test-Path -LiteralPath $OutputPath) {
    $outputItem = Get-Item -LiteralPath $OutputPath -Force -ErrorAction Stop
    if ($outputItem.PSIsContainer) {
        throw "Build manifest output path must name a file."
    }
    $OutputPath = Get-ExistingNonReparsePath `
        -Path $outputItem.FullName `
        -Context "Build manifest output path"
}
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $OutputPath -Encoding UTF8
