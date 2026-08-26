[CmdletBinding()]
param(
    [string]$PayloadDirectory = "",

    [string]$FilePath = "",

    [Parameter(Mandatory = $true)]
    [string]$CertificateThumbprint,

    [string[]]$AllowedUnsignedFileName = @(),

    [string]$TimestampUrl = "https://timestamp.digicert.com"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "portable-artifact-content.ps1")

if ($TimestampUrl -notmatch "^https://") {
    throw "TimestampUrl must use HTTPS for the RFC 3161 timestamp authority."
}

if ([string]::IsNullOrWhiteSpace($PayloadDirectory) -eq [string]::IsNullOrWhiteSpace($FilePath)) {
    throw "Specify exactly one of PayloadDirectory or FilePath for Authenticode signing."
}
$verifyScript = Join-Path $PSScriptRoot "verify-authenticode.ps1"
if (-not [string]::IsNullOrWhiteSpace($PayloadDirectory) -and -not (Test-Path -LiteralPath $PayloadDirectory -PathType Container)) {
    throw "Signing payload directory was not found: $PayloadDirectory"
}
if (-not [string]::IsNullOrWhiteSpace($FilePath) -and -not (Test-Path -LiteralPath $FilePath -PathType Leaf)) {
    throw "Signing file was not found: $FilePath"
}

if (-not [string]::IsNullOrWhiteSpace($PayloadDirectory)) {
    $null = Assert-NoReparsePointsBelow -Path $PayloadDirectory -Context "Signing payload directory"
}
else {
    $null = Assert-PathChainWithoutReparsePoint -Path $FilePath -Context "Signing file"
}

foreach ($allowedName in $AllowedUnsignedFileName) {
    if ([string]::IsNullOrWhiteSpace($allowedName) -or
        [System.IO.Path]::IsPathRooted($allowedName) -or
        $allowedName.Contains("\") -or
        $allowedName.Contains("/")) {
        throw "AllowedUnsignedFileName must contain only a file name, not a rooted or nested path: '$allowedName'."
    }
}

$thumbprint = ($CertificateThumbprint -replace "\s", "").ToUpperInvariant()
if ($thumbprint -notmatch "^[0-9A-F]{40}$") {
    throw "CertificateThumbprint must be a 40-character SHA-1 certificate thumbprint."
}

$certificate = @(
    Get-ChildItem -Path "Cert:\CurrentUser\My\$thumbprint", "Cert:\LocalMachine\My\$thumbprint" -ErrorAction SilentlyContinue
) | Select-Object -First 1
if ($null -eq $certificate -or -not $certificate.HasPrivateKey) {
    throw "The requested code-signing certificate was not found with a private key in the current user or local machine certificate store."
}
$codeSigningEku = @($certificate.EnhancedKeyUsageList | Where-Object {
    $_.ObjectId -eq "1.3.6.1.5.5.7.3.3"
})
if ($codeSigningEku.Count -eq 0) {
    throw "The requested certificate does not contain the Code Signing enhanced key usage."
}
if ($certificate.NotBefore.ToUniversalTime() -gt [DateTime]::UtcNow -or
    $certificate.NotAfter.ToUniversalTime() -lt [DateTime]::UtcNow) {
    throw "The requested code-signing certificate is outside its validity period."
}
if ($certificate.Subject -eq $certificate.Issuer) {
    throw "Self-signed code-signing certificates are not accepted for release payloads."
}
$chain = [System.Security.Cryptography.X509Certificates.X509Chain]::new()
$chain.ChainPolicy.RevocationMode = [System.Security.Cryptography.X509Certificates.X509RevocationMode]::NoCheck
if (-not $chain.Build($certificate) -or @($chain.ChainStatus | Where-Object { $_.Status -ne [System.Security.Cryptography.X509Certificates.X509ChainStatusFlags]::NoError }).Count -gt 0) {
    $chainStatus = @($chain.ChainStatus | ForEach-Object { $_.Status }) -join ", "
    throw "The requested code-signing certificate does not build to a trusted root: $chainStatus"
}

$signTool = Get-Command signtool.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
if ($null -eq $signTool) {
    $sdkRoot = Join-Path ${env:ProgramFiles(x86)} "Windows Kits/10/bin"
    $signTool = Get-ChildItem -LiteralPath $sdkRoot -Filter signtool.exe -File -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match "\\x64\\signtool\.exe$" } |
        Sort-Object FullName -Descending | Select-Object -First 1
}
if ($null -eq $signTool) {
    throw "signtool.exe was not found. Install the Windows SDK signing tools."
}
$signToolPath = if ($signTool.PSObject.Properties.Name -contains "Source") {
    [string]$signTool.Source
}
else {
    [string]$signTool.FullName
}

$extensions = @(".exe", ".dll", ".sys", ".msi", ".msix", ".appx")
$files = @(
    if (-not [string]::IsNullOrWhiteSpace($PayloadDirectory)) {
    @(Get-ChildItem -LiteralPath $PayloadDirectory -File -Recurse | Where-Object {
        $extensions -contains $_.Extension.ToLowerInvariant()
    })
    } else {
    @((Get-Item -LiteralPath $FilePath))
    }
)
if ($files.Count -eq 0) {
    $signingTarget = if (-not [string]::IsNullOrWhiteSpace($PayloadDirectory)) {
        $PayloadDirectory
    } else {
        $FilePath
    }
    throw "Signing found no executable payload files under $signingTarget."
}

foreach ($file in $files) {
    $existingSignature = Get-AuthenticodeSignature -LiteralPath $file.FullName
    if ([string]$existingSignature.Status -eq "Valid") {
        continue
    }
    $isExplicitlyAllowed = $false
    if (-not [string]::IsNullOrWhiteSpace($PayloadDirectory)) {
        $expectedRootFile = [System.IO.Path]::GetFullPath((Join-Path $PayloadDirectory $file.Name))
        $isExplicitlyAllowed = [string]::Equals(
            $expectedRootFile,
            $file.FullName,
            [System.StringComparison]::OrdinalIgnoreCase) -and
            ($AllowedUnsignedFileName -contains $file.Name)
    }
    else {
        $isExplicitlyAllowed = $AllowedUnsignedFileName -contains $file.Name
    }
    if (-not $isExplicitlyAllowed) {
        throw "Unsigned payload is not in the explicit project-owned signing allowlist: $($file.FullName)"
    }
    & $signToolPath sign /sha1 $thumbprint /fd SHA256 /tr $TimestampUrl /td SHA256 /a $file.FullName
    if ($LASTEXITCODE -ne 0) {
        throw "signtool failed for $($file.FullName) with exit code $LASTEXITCODE."
    }
}

if (-not [string]::IsNullOrWhiteSpace($PayloadDirectory)) {
    & $verifyScript -PayloadDirectory $PayloadDirectory -RequireRfc3161Timestamp
} else {
    & $verifyScript -FilePath $FilePath -RequireRfc3161Timestamp
}
Write-Output "Authenticode signing and verification passed for $($files.Count) payload file(s)."
