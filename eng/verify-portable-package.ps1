[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("x64", "ARM64")]
    [string]$Architecture,

    [Parameter(Mandatory = $true)]
    [string]$StagingDirectory,

    [Parameter(Mandatory = $true)]
    [string]$PackagePath,

    [Parameter(Mandatory = $true)]
    [string]$ManifestPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

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

if (-not (Test-Path -LiteralPath $StagingDirectory -PathType Container)) {
    throw "Portable package staging directory is missing: $StagingDirectory"
}
if (-not (Test-Path -LiteralPath $PackagePath -PathType Leaf)) {
    throw "Portable package ZIP is missing: $PackagePath"
}
if (-not (Test-Path -LiteralPath $ManifestPath -PathType Leaf)) {
    throw "Portable package manifest is missing: $ManifestPath"
}

$stagingRoot = (Resolve-Path -LiteralPath $StagingDirectory).Path.TrimEnd('\', '/')
$stagedPayload = @(
    Get-ChildItem -LiteralPath $stagingRoot -File -Recurse |
        ForEach-Object {
            [ordered]@{
                path = $_.FullName.Substring($stagingRoot.Length).TrimStart('\', '/').Replace('\', '/')
                bytes = $_.Length
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
    $archivePayload = @(
        $archive.Entries |
            Where-Object { -not $_.FullName.EndsWith("/", [StringComparison]::Ordinal) } |
            ForEach-Object {
                [ordered]@{
                    path = $_.FullName.Replace('\', '/')
                    bytes = $_.Length
                }
            }
    )
}
finally {
    $archive.Dispose()
}

$stagedPayloadMap = ConvertTo-PayloadMap -Payload $stagedPayload -Name "Portable package staging payload"
$archivePayloadMap = ConvertTo-PayloadMap -Payload $archivePayload -Name "Portable package ZIP payload"
Assert-PayloadMapMatches -Expected $stagedPayloadMap -Actual $archivePayloadMap -ExpectedName "staging payload" -ActualName "Portable package ZIP"

$manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
if ($manifest.kind -ne "portable" -or $manifest.architecture -ne $Architecture) {
    throw "Portable package manifest kind or architecture does not match the requested package."
}
$manifestPayload = @($manifest.payload)
$manifestPayloadMap = ConvertTo-PayloadMap -Payload $manifestPayload -Name "Portable package manifest payload"
Assert-PayloadMapMatches -Expected $archivePayloadMap -Actual $manifestPayloadMap -ExpectedName "ZIP payload" -ActualName "Portable package manifest"

if ([Int64]$manifest.package.bytes -ne (Get-Item -LiteralPath $PackagePath).Length) {
    throw "Portable package manifest byte count does not match ZIP."
}

Write-Host "Portable package contract passed: $Architecture"
