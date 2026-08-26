[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$RepositoryRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Require([bool]$Condition, [string]$Message) {
    if (-not $Condition) {
        throw $Message
    }
}

function Require-DefaultReleaseInvocationRejected([string]$ScriptPath, [string[]]$Arguments) {
    $rejected = $false
    try {
        & $ScriptPath @Arguments 2>&1 | Out-Null
    } catch {
        $rejected = $true
    }
    Require $rejected "A release-capable entry point accepted an invocation without an explicit signing or development-build intent: $ScriptPath"
}

$buildText = Get-Content -LiteralPath (Join-Path $RepositoryRoot "eng/build.ps1") -Raw
$portableText = Get-Content -LiteralPath (Join-Path $RepositoryRoot "eng/package-portable.ps1") -Raw
$installerText = Get-Content -LiteralPath (Join-Path $RepositoryRoot "eng/package-installer.ps1") -Raw
$verifyText = Get-Content -LiteralPath (Join-Path $RepositoryRoot "eng/verify-authenticode.ps1") -Raw
$signText = Get-Content -LiteralPath (Join-Path $RepositoryRoot "eng/sign-release.ps1") -Raw
$vcxprojText = Get-Content -LiteralPath (Join-Path $RepositoryRoot "src/adapters/ui/winui/Azzs.WinUI.vcxproj") -Raw
$cmakeText = Get-Content -LiteralPath (Join-Path $RepositoryRoot "CMakeLists.txt") -Raw
$workflowText = Get-Content -LiteralPath (Join-Path $RepositoryRoot ".github/workflows/windows-readonly.yml") -Raw

Require-DefaultReleaseInvocationRejected (Join-Path $RepositoryRoot "eng/build.ps1") @("-Architecture", "x64", "-SkipCoreSmoke")
Require-DefaultReleaseInvocationRejected (Join-Path $RepositoryRoot "eng/package-portable.ps1") @("-ArtifactId", "standard-x64-portable", "-SkipBuild")
Require-DefaultReleaseInvocationRejected (Join-Path $RepositoryRoot "eng/package-installer.ps1") @("-Architecture", "x64", "-SkipBuild")

Require ($buildText -match "RequireAuthenticodeSignature") "build.ps1 lacks the release Authenticode gate."
Require ($buildText -match "DevelopmentBuild") "build.ps1 does not require an explicit unsigned development-build intent."
Require ($buildText -match "SigningCertificateThumbprint") "build.ps1 lacks certificate injection."
Require (([regex]::Matches($buildText, "ExpectedSignerThumbprint")).Count -ge 1) "build.ps1 does not pin the project signer thumbprint during release verification."
Require (([regex]::Matches($buildText, "ExpectedSignerFileName")).Count -ge 1) "build.ps1 does not identify the project executable during release verification."
Require ($portableText -match "verify-authenticode\.ps1") "portable packaging does not verify signed payloads."
Require (([regex]::Matches($portableText, "verify-authenticode\.ps1")).Count -ge 2) "portable packaging does not re-verify locked resources after staging."
Require (([regex]::Matches($portableText, "ExpectedSignerThumbprint")).Count -ge 2) "portable packaging does not pin the project signer for both payload checks."
Require (([regex]::Matches($portableText, "ExpectedSignerFileName")).Count -ge 2) "portable packaging does not identify the project executable for both payload checks."
Require ($installerText -match "verify-authenticode\.ps1") "installer packaging does not verify signed payloads."
Require (([regex]::Matches($installerText, "ExpectedSignerThumbprint")).Count -ge 3) "installer packaging does not pin the project signer for payload, staging, and MSI checks."
Require (([regex]::Matches($installerText, "ExpectedSignerFileName")).Count -ge 3) "installer packaging does not identify the expected signer file for all release checks."
Require ($portableText -match "DevelopmentBuild") "portable packaging does not require an explicit unsigned development-build intent."
Require ($installerText -match "DevelopmentBuild") "installer packaging does not require an explicit unsigned development-build intent."
$workflowBuildLines = @($workflowText -split "`r?`n" | Where-Object { $_ -match "eng/build\.ps1" })
Require ($workflowBuildLines.Count -eq 1) "Windows read-only validation must have one explicit build invocation."
Require ($workflowBuildLines[0] -match "-DevelopmentBuild") "Windows read-only validation must explicitly request an unsigned development build."
Require ($verifyText -match 'Status.*Valid') "verification must require the strict Valid Authenticode status."
Require ($verifyText -match 'RequireRfc3161Timestamp') "verification must expose an explicit RFC 3161 timestamp gate."
Require ($verifyText -match '1\.3\.6\.1\.4\.1\.311\.3\.3\.[12]') "verification must inspect the RFC 3161 timestamp attribute OID."
Require ($verifyText -match 'Add-Type -AssemblyName System\.Security') "PE CMS parsing must load on Windows PowerShell and PowerShell 7."
Require ($verifyText -match 'signtoolOutput') "non-PE and catalog timestamp verification must use signtool output."
Require ($verifyText -match '/pa /all /tw') "non-PE and catalog timestamp verification must require signtool timestamp evidence."
Require ($verifyText -match 'RFC3161') "non-PE and catalog timestamp verification must require an explicit RFC3161 result."
Require ($verifyText -notmatch 'SignatureType.*Catalog[\s\S]{0,240}return \$true') "catalog signatures must not pass solely because TimeStamperCertificate is present."
Require ($signText -match '/fd SHA256') "signing must use SHA-256 file digests."
Require ($signText -match '/tr') "signing must use an RFC 3161 timestamp."
Require ($signText -match 'RequireRfc3161Timestamp') "signing must re-verify the RFC 3161 timestamp type."
Require ($signText -match 'AllowedUnsignedFileName') "signing must require an explicit unsigned-file allowlist."
Require ($signText -notmatch 'file\.Name -notmatch "\^Azzs') "signing must not authorize unsigned payloads by filename prefix."
Require ($signText -match 'Assert-NoReparsePointsBelow') "signing must reject reparse points before enumerating a payload directory."
Require ($verifyText -match 'Assert-NoReparsePointsBelow') "verification must reject reparse points before enumerating a payload directory."
Require ($installerText -match '(?s)stagingDirectory.*?RequireRfc3161Timestamp') "installer packaging must verify the copied staging payload before WiX consumes it."
Require ($signText -match 'Self-signed') "release signing must reject self-signed certificates."
Require ($signText -match 'TimestampUrl -notmatch "\^https://') "signing must require an HTTPS timestamp authority."
Require ($signText -match 'https://timestamp\.digicert\.com') "signing must default to an HTTPS timestamp authority."
foreach ($property in @(
    '<ControlFlowGuard>Guard</ControlFlowGuard>',
    '<LinkControlFlowGuard>true</LinkControlFlowGuard>',
    '<RandomizedBaseAddress>true</RandomizedBaseAddress>',
    '<DataExecutionPrevention>true</DataExecutionPrevention>',
    '<HighEntropyVA>true</HighEntropyVA>'
)) {
    Require ($vcxprojText.Contains($property)) "WinUI PE mitigation property is missing: $property"
}
Require (-not ($vcxprojText -match '<Link>[\s\S]*?<AdditionalOptions>')) "Linker options must use checked MSBuild properties, not Link AdditionalOptions."

$tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("azzs-signing-contract-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempRoot -Force | Out-Null
try {
    Set-Content -LiteralPath (Join-Path $tempRoot "unsigned.exe") -Value "not a PE" -Encoding ASCII
    $verifyScript = Join-Path $RepositoryRoot "eng/verify-authenticode.ps1"
    $rejected = $false
    try {
        & $verifyScript -PayloadDirectory $tempRoot 2>&1 | Out-Null
    } catch {
        $rejected = $true
    }
    Require $rejected "unsigned payload was not rejected."

    $existingSignedFile = Join-Path $env:SystemRoot "System32\notepad.exe"
    if (Test-Path -LiteralPath $existingSignedFile) {
        & $verifyScript -FilePath $existingSignedFile -RequireTimestamp | Out-Null
        $existingSignature = Get-AuthenticodeSignature -LiteralPath $existingSignedFile
        if ($null -ne $existingSignature.SignerCertificate -and
            [string]$existingSignature.Status -eq "Valid") {
            $actualThumbprint = ($existingSignature.SignerCertificate.Thumbprint -replace "\s", "").ToUpperInvariant()
            & $verifyScript -FilePath $existingSignedFile `
                -ExpectedSignerThumbprint $actualThumbprint `
                -ExpectedSignerFileName (Split-Path -Leaf $existingSignedFile) | Out-Null
            $wrongSignerRejected = $false
            try {
                & $verifyScript -FilePath $existingSignedFile `
                    -ExpectedSignerThumbprint ("0" * 40) `
                    -ExpectedSignerFileName (Split-Path -Leaf $existingSignedFile) 2>&1 | Out-Null
            }
            catch {
                $wrongSignerRejected = $true
            }
            Require $wrongSignerRejected "a valid system signature passed with an unrelated expected signer thumbprint."
        }
        $catalogRejected = $false
        try {
            & $verifyScript -FilePath $existingSignedFile -RequireRfc3161Timestamp 2>&1 | Out-Null
        }
        catch {
            $catalogRejected = $true
        }
        Require $catalogRejected "catalog-backed signatures must fail closed when signtool cannot prove RFC3161."
    }
    $embeddedSignedFile = Join-Path $env:SystemRoot "py.exe"
    if (Test-Path -LiteralPath $embeddedSignedFile) {
        $embeddedSignature = Get-AuthenticodeSignature -LiteralPath $embeddedSignedFile
        if ([string]$embeddedSignature.SignatureType -eq "Authenticode" -and
            [string]$embeddedSignature.Status -eq "Valid") {
            & $verifyScript -FilePath $embeddedSignedFile -RequireRfc3161Timestamp | Out-Null
        }
    }
} finally {
    Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Output "release signing contract: PASS"
