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
    [string]$OutputPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$payload = @(
    Get-ChildItem -LiteralPath $PayloadDirectory -File -Recurse |
        Sort-Object FullName |
        ForEach-Object {
            [ordered]@{
                path = $_.FullName.Substring($PayloadDirectory.Length).TrimStart("\", "/").Replace("\", "/")
                bytes = $_.Length
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

$outputDirectory = Split-Path -Parent $OutputPath
New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
$manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $OutputPath -Encoding UTF8
