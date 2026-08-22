[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$sdkRoot = Join-Path ${env:ProgramFiles(x86)} "Windows Kits/10"
$sdkTargetVersion = "10.0.28000.0"
$sdkReleaseVersion = "10.0.28000.2526"
$sdkProductVersion = "10.1.28000.2526"
$installerUrl = "https://download.microsoft.com/download/06fc99ac-527e-451e-a536-8866695a2e7e/KIT_BUNDLE_WINDOWSSDK_MEDIACREATION/winsdksetup.exe"
$installerBytes = 1451664
$installerSha256 = "02988EA51EAB2A2DB53E19735E51C97A6D221ADA74B9174FA0868870B9403BA0"
$downloadDirectory = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { $env:TEMP }
$installerPath = Join-Path $downloadDirectory "winsdksetup-$sdkReleaseVersion.exe"
$logDirectory = Join-Path $repositoryRoot "out/logs"
$installerLogPath = Join-Path $logDirectory "windows-sdk-$sdkReleaseVersion-install.log"

$requiredPaths = @(
    (Join-Path $sdkRoot "Include/$sdkTargetVersion/um/Windows.h"),
    (Join-Path $sdkRoot "Include/$sdkTargetVersion/ucrt/stdlib.h"),
    (Join-Path $sdkRoot "Include/$sdkTargetVersion/winrt/Windows.Foundation.idl"),
    (Join-Path $sdkRoot "Lib/$sdkTargetVersion/ucrt/x64/ucrt.lib"),
    (Join-Path $sdkRoot "Lib/$sdkTargetVersion/ucrt/arm64/ucrt.lib"),
    (Join-Path $sdkRoot "Lib/$sdkTargetVersion/um/x64/kernel32.lib"),
    (Join-Path $sdkRoot "Lib/$sdkTargetVersion/um/arm64/kernel32.lib"),
    (Join-Path $sdkRoot "bin/$sdkTargetVersion/x64/makepri.exe"),
    (Join-Path $sdkRoot "bin/$sdkTargetVersion/x64/rc.exe")
)

function Get-LockedSdkProduct {
    @(
        "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*",
        "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*"
    ) | ForEach-Object {
        Get-ItemProperty -Path $_ -ErrorAction SilentlyContinue
    } | Where-Object {
        $displayVersion = $_.PSObject.Properties["DisplayVersion"]
        $displayName = $_.PSObject.Properties["DisplayName"]
        $null -ne $displayVersion -and $null -ne $displayName -and
            $displayVersion.Value -eq $sdkProductVersion -and
            $displayName.Value -match "Windows.*(Software Development Kit|SDK)"
    }
}

function Test-LockedSdkFiles {
    @($requiredPaths | Where-Object { -not (Test-Path -LiteralPath $_) }).Count -eq 0
}

if (@(Get-LockedSdkProduct).Count -gt 0 -and (Test-LockedSdkFiles)) {
    Write-Host "Windows SDK $sdkReleaseVersion is already installed."
    exit 0
}

New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null
Write-Host "Downloading Windows SDK $sdkReleaseVersion bootstrapper."
Invoke-WebRequest -Uri $installerUrl -OutFile $installerPath

$download = Get-Item -LiteralPath $installerPath
if ($download.Length -ne $installerBytes) {
    throw "Windows SDK bootstrapper length mismatch: expected $installerBytes, found $($download.Length)."
}
$actualSha256 = (Get-FileHash -LiteralPath $installerPath -Algorithm SHA256).Hash
if ($actualSha256 -ne $installerSha256) {
    throw "Windows SDK bootstrapper SHA-256 mismatch: expected $installerSha256, found $actualSha256."
}
$signature = Get-AuthenticodeSignature -LiteralPath $installerPath
if ($signature.Status -ne "Valid" -or
    $signature.SignerCertificate.Subject -notmatch "O=Microsoft Corporation") {
    throw "Windows SDK bootstrapper does not have a valid Microsoft Authenticode signature."
}

$arguments = @(
    "/features",
    "OptionId.UWPCpp",
    "OptionId.DesktopCPPx86",
    "OptionId.DesktopCPPx64",
    "OptionId.DesktopCPParm64",
    "/quiet",
    "/norestart",
    "/ceip",
    "off",
    "/log",
    "`"$installerLogPath`""
)
$process = Start-Process -FilePath $installerPath -ArgumentList $arguments -Wait -PassThru
if ($process.ExitCode -ne 0) {
    if (Test-Path -LiteralPath $installerLogPath) {
        Get-Content -LiteralPath $installerLogPath -Tail 100
    }
    throw "Windows SDK installer failed with exit code $($process.ExitCode)."
}

$missingPaths = @($requiredPaths | Where-Object { -not (Test-Path -LiteralPath $_) })
if ($missingPaths.Count -gt 0) {
    throw "Windows SDK installation is missing required paths: $($missingPaths -join ', ')"
}
if (@(Get-LockedSdkProduct).Count -eq 0) {
    throw "Windows SDK files were installed, but release $sdkReleaseVersion was not registered."
}

Write-Host "Windows SDK $sdkReleaseVersion is installed with x64 and ARM64 C++ inputs."
