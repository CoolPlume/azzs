[CmdletBinding()]
param(
    [string]$PayloadDirectory = "",

    [string]$FilePath = "",

    [switch]$RequireTimestamp,

    [switch]$RequireRfc3161Timestamp,

    [string]$ExpectedSignerThumbprint = "",

    [string]$ExpectedSignerFileName = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "portable-artifact-content.ps1")

$expectedSignerThumbprintNormalized = ($ExpectedSignerThumbprint -replace "\s", "").ToUpperInvariant()
if (-not [string]::IsNullOrWhiteSpace($expectedSignerThumbprintNormalized) -and
    $expectedSignerThumbprintNormalized -notmatch "^[0-9A-F]{40}$") {
    throw "ExpectedSignerThumbprint must be a 40-character SHA-1 certificate thumbprint."
}
if (-not [string]::IsNullOrWhiteSpace($ExpectedSignerThumbprint) -and
    [string]::IsNullOrWhiteSpace($ExpectedSignerFileName)) {
    throw "ExpectedSignerFileName is required when ExpectedSignerThumbprint is supplied."
}
if (-not [string]::IsNullOrWhiteSpace($ExpectedSignerFileName) -and
    [System.IO.Path]::IsPathRooted($ExpectedSignerFileName)) {
    throw "ExpectedSignerFileName must be a file name or payload-relative path."
}

function Test-EmbeddedRfc3161Timestamp {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $bytes = [System.IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 0x40) {
        return $false
    }
    $peOffset = [BitConverter]::ToUInt32($bytes, 0x3c)
    if ($peOffset -gt ($bytes.Length - 24) -or
        $bytes[$peOffset] -ne 0x50 -or
        $bytes[$peOffset + 1] -ne 0x45 -or
        $bytes[$peOffset + 2] -ne 0 -or
        $bytes[$peOffset + 3] -ne 0) {
        return $false
    }
    $optionalHeaderSize = [BitConverter]::ToUInt16($bytes, $peOffset + 20)
    $optionalHeaderOffset = $peOffset + 24
    if ($optionalHeaderOffset -gt ($bytes.Length - 2) -or $optionalHeaderSize -lt 96) {
        return $false
    }
    $magic = [BitConverter]::ToUInt16($bytes, $optionalHeaderOffset)
    $dataDirectoryOffset = if ($magic -eq 0x20b) {
        $optionalHeaderOffset + 112
    }
    elseif ($magic -eq 0x10b) {
        $optionalHeaderOffset + 96
    }
    else {
        return $false
    }
    $securityDirectoryOffset = $dataDirectoryOffset + (8 * 4)
    if ($securityDirectoryOffset -gt ($bytes.Length - 8) -or
        $securityDirectoryOffset -ge ($optionalHeaderOffset + $optionalHeaderSize)) {
        return $false
    }
    $certificateOffset = [BitConverter]::ToUInt32($bytes, $securityDirectoryOffset)
    $certificateSize = [BitConverter]::ToUInt32($bytes, $securityDirectoryOffset + 4)
    if ($certificateOffset -eq 0 -or $certificateSize -lt 8 -or
        $certificateOffset -gt $bytes.Length -or
        $certificateSize -gt ($bytes.Length - $certificateOffset)) {
        return $false
    }

    # Windows PowerShell 5.1 exposes SignedCms through System.Security while
    # PowerShell 7 ships the forwarding Pkcs assembly separately.
    Add-Type -AssemblyName System.Security
    $certificateEnd = [UInt64]$certificateOffset + [UInt64]$certificateSize
    $cursor = [UInt64]$certificateOffset
    while ($cursor + 8 -le $certificateEnd) {
        $entryLength = [BitConverter]::ToUInt32($bytes, [Int64]$cursor)
        if ($entryLength -lt 8 -or $cursor + $entryLength -gt $certificateEnd) {
            return $false
        }
        $certificateType = [BitConverter]::ToUInt16($bytes, [Int64]$cursor + 6)
        if ($certificateType -eq 2) {
            $cmsBytes = [byte[]]$bytes[([Int64]$cursor + 8)..([Int64]$cursor + $entryLength - 1)]
            try {
                $cms = [System.Security.Cryptography.Pkcs.SignedCms]::new()
                $cms.Decode($cmsBytes)
                foreach ($signer in $cms.SignerInfos) {
                    foreach ($attribute in $signer.UnsignedAttributes) {
                        if ($attribute.Oid.Value -in @(
                                "1.3.6.1.4.1.311.3.3.1",
                                "1.3.6.1.4.1.311.3.3.2"
                            )) {
                            return $true
                        }
                    }
                }
            }
            catch {
                return $false
            }
        }
        $cursor += (([UInt64]$entryLength + 7) -band 0xfffffff8)
    }
    return $false
}

function Test-Rfc3161Timestamp {
    param(
        [Parameter(Mandatory = $true)]
        [System.IO.FileInfo]$File,

        [Parameter(Mandatory = $true)]
        [System.Management.Automation.Signature]$Signature
    )

    if ($null -eq $Signature.TimeStamperCertificate) {
        return $false
    }
    if ([string]$Signature.SignatureType -ne "Catalog" -and
        $File.Extension.ToLowerInvariant() -in @(".exe", ".dll", ".sys")) {
        return Test-EmbeddedRfc3161Timestamp -Path $File.FullName
    }

    # MSI/MSIX/APPX and catalog-backed files do not expose their CMS unsigned
    # attributes through Get-AuthenticodeSignature.  Do not infer RFC 3161
    # from a timestamp certificate alone; signtool must explicitly report the
    # RFC3161 timestamp token, otherwise verification fails closed.
    $signTool = Get-Command signtool.exe -CommandType Application -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($null -eq $signTool) {
        $sdkRoot = Join-Path ${env:ProgramFiles(x86)} "Windows Kits/10/bin"
        $signTool = Get-ChildItem -LiteralPath $sdkRoot -Filter signtool.exe -File -Recurse -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match "\\x64\\signtool\.exe$" } |
            Sort-Object FullName -Descending |
            Select-Object -First 1
    }
    if ($null -eq $signTool) {
        return $false
    }
    $signToolPath = if ($null -ne $signTool.PSObject.Properties["Source"]) {
        [string]$signTool.Source
    }
    else {
        [string]$signTool.FullName
    }
    if ([string]::IsNullOrWhiteSpace($signToolPath)) {
        return $false
    }
    $signtoolOutput = @(& $signToolPath verify /pa /all /tw $File.FullName 2>&1 | ForEach-Object {
        [string]$_
    })
    if ($LASTEXITCODE -ne 0) {
        return $false
    }
    return (@($signtoolOutput | Where-Object {
        $_ -match "(?i)^\s*\d+\s+\S+\s+RFC3161\s*$"
    }).Count -gt 0)
}

if ([string]::IsNullOrWhiteSpace($PayloadDirectory) -eq [string]::IsNullOrWhiteSpace($FilePath)) {
    throw "Specify exactly one of PayloadDirectory or FilePath for Authenticode verification."
}

$extensions = @(".exe", ".dll", ".sys", ".msi", ".msix", ".appx")
$files = @(
    if (-not [string]::IsNullOrWhiteSpace($PayloadDirectory)) {
    if (-not (Test-Path -LiteralPath $PayloadDirectory -PathType Container)) {
        throw "Authenticode verification payload directory was not found: $PayloadDirectory"
    }
    $null = Assert-NoReparsePointsBelow -Path $PayloadDirectory -Context "Authenticode verification payload directory"
    @(Get-ChildItem -LiteralPath $PayloadDirectory -File -Recurse | Where-Object {
        $extensions -contains $_.Extension.ToLowerInvariant()
    })
    } else {
    if (-not (Test-Path -LiteralPath $FilePath -PathType Leaf)) {
        throw "Authenticode verification file was not found: $FilePath"
    }
    $null = Assert-PathChainWithoutReparsePoint -Path $FilePath -Context "Authenticode verification file"
    @((Get-Item -LiteralPath $FilePath))
    }
)
if ($files.Count -eq 0) {
    $verificationTarget = if (-not [string]::IsNullOrWhiteSpace($PayloadDirectory)) {
        $PayloadDirectory
    } else {
        $FilePath
    }
    throw "Authenticode verification found no executable payload files under $verificationTarget."
}

$failures = @()
$expectedSignerMatchCount = 0
foreach ($file in $files) {
    $signature = Get-AuthenticodeSignature -LiteralPath $file.FullName
    $status = [string]$signature.Status
    if ($status -ne "Valid") {
        $failures += "$( $file.FullName ): status=$status"
        continue
    }
    if (-not [string]::IsNullOrWhiteSpace($ExpectedSignerFileName)) {
        $expectedFileMatches = if (-not [string]::IsNullOrWhiteSpace($PayloadDirectory)) {
            $payloadRoot = [System.IO.Path]::GetFullPath($PayloadDirectory).TrimEnd('\', '/')
            $relativePath = $file.FullName.Substring($payloadRoot.Length).TrimStart('\', '/').Replace('\', '/')
            [string]::Equals($relativePath, $ExpectedSignerFileName.Replace('\', '/'), [System.StringComparison]::OrdinalIgnoreCase)
        }
        else {
            [string]::Equals($file.Name, (Split-Path -Leaf $ExpectedSignerFileName), [System.StringComparison]::OrdinalIgnoreCase)
        }
        if ($expectedFileMatches) {
            $expectedSignerMatchCount++
            $actualSignerThumbprint = if ($null -ne $signature.SignerCertificate) {
                ($signature.SignerCertificate.Thumbprint -replace "\s", "").ToUpperInvariant()
            }
            else {
                ""
            }
            if (-not [string]::Equals($actualSignerThumbprint, $expectedSignerThumbprintNormalized, [System.StringComparison]::OrdinalIgnoreCase)) {
                $failures += "$( $file.FullName ): signer thumbprint '$actualSignerThumbprint' does not match the expected project certificate."
            }
        }
    }
    if ($RequireRfc3161Timestamp -and -not (Test-Rfc3161Timestamp -File $file -Signature $signature)) {
        $failures += "$( $file.FullName ): RFC 3161 timestamp is missing or could not be proven by format-aware verification"
    }
    elseif ($RequireTimestamp -and $null -eq $signature.TimeStamperCertificate) {
        $failures += "$( $file.FullName ): RFC 3161 timestamp is missing"
    }
}

if (-not [string]::IsNullOrWhiteSpace($ExpectedSignerFileName) -and $expectedSignerMatchCount -ne 1) {
    $failures += "Expected signer file '$ExpectedSignerFileName' matched $expectedSignerMatchCount payload file(s); exactly one is required."
}

if ($failures.Count -gt 0) {
    throw "Authenticode verification failed:`n$($failures -join "`n")"
}

Write-Output "Authenticode verification passed for $($files.Count) payload file(s)."
