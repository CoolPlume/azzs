[CmdletBinding()]
param(
    [ValidateSet("x64", "ARM64")]
    [string]$Architecture = "x64",

    [switch]$RunCoreSmoke,

    [switch]$SkipCoreSmoke,

    [switch]$DevelopmentBuild,

    [switch]$EnableStartupDiagnosticDeviceDataRoot,

    [string]$SigningCertificateThumbprint = "",

    [string]$TimestampUrl = "https://timestamp.digicert.com",

    [switch]$RequireAuthenticodeSignature
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if (([bool]$RequireAuthenticodeSignature) -eq ([bool]$DevelopmentBuild)) {
    throw "Specify exactly one of DevelopmentBuild or RequireAuthenticodeSignature."
}
if ($RequireAuthenticodeSignature -and [string]::IsNullOrWhiteSpace($SigningCertificateThumbprint)) {
    throw "Release builds that require Authenticode must provide SigningCertificateThumbprint."
}
if ($DevelopmentBuild -and -not [string]::IsNullOrWhiteSpace($SigningCertificateThumbprint)) {
    throw "Development builds cannot supply a signing certificate. Use RequireAuthenticodeSignature for a release build."
}
if ($RunCoreSmoke -and $SkipCoreSmoke) {
    throw "RunCoreSmoke and SkipCoreSmoke cannot be used together."
}
if ($RunCoreSmoke -and $Architecture -ne "x64") {
    throw "RunCoreSmoke is supported only for x64 builds on this hosted build machine."
}

. (Join-Path $PSScriptRoot "portable-artifact-content.ps1")
. (Join-Path $PSScriptRoot "msbuild-arguments.ps1")

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$productVersionPath = Join-Path $repositoryRoot "release/product-version.json"
if (-not (Test-Path -LiteralPath $productVersionPath -PathType Leaf)) {
    throw "The authoritative product version source was not found: $productVersionPath"
}
try {
    $productVersion = Get-Content -LiteralPath $productVersionPath -Raw | ConvertFrom-Json
} catch {
    throw "The authoritative product version source is not valid JSON: $productVersionPath"
}
if ($productVersion.schemaVersion -ne 1 -or
    $productVersion.applicationVersion -notmatch "^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z]+(\.[0-9A-Za-z]+)*)?$" -or
    $productVersion.releaseChannel -notmatch "^(stable|prerelease)$" -or
    $productVersion.windowsVersion -notmatch "^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$") {
    throw "The authoritative product version source has an unsupported version mapping."
}
$applicationIsPrerelease = $productVersion.applicationVersion -match "-"
if (($productVersion.releaseChannel -eq "stable") -eq $applicationIsPrerelease) {
    throw "The authoritative product version source has an inconsistent application version and release channel."
}
$windowsVersionCommasArgument = ConvertTo-AzzsWindowsVersionCommasArgument `
    -WindowsVersion $productVersion.windowsVersion
$evidenceName = "windows-$($Architecture.ToLowerInvariant())-release"
$logDirectory = Join-Path $repositoryRoot "out/logs"
$manifestDirectory = Join-Path $repositoryRoot "out/manifests"
$testResultDirectory = Join-Path $repositoryRoot "out/test-results"
$logPath = Join-Path $logDirectory "$evidenceName.log"
$msbuildLogPath = Join-Path $logDirectory "$evidenceName.msbuild.log"
$binlogPath = Join-Path $logDirectory "$evidenceName.binlog"
$manifestPath = Join-Path $manifestDirectory "$evidenceName.json"
$testResultPath = Join-Path $testResultDirectory "core-$($Architecture.ToLowerInvariant())-release.xml"

foreach ($directoryPath in @($logDirectory, $manifestDirectory, $testResultDirectory)) {
    Assert-PathChainWithoutReparsePoint `
        -Path $directoryPath `
        -Context "Build evidence directory '$directoryPath'"
}
foreach ($filePath in @($logPath, $msbuildLogPath, $binlogPath, $manifestPath, $testResultPath)) {
    Assert-PathChainWithoutReparsePoint `
        -Path $filePath `
        -Context "Build evidence file '$filePath'"
}

New-Item -ItemType Directory -Path $logDirectory, $manifestDirectory, $testResultDirectory -Force | Out-Null
New-Item -ItemType File -Path $logPath -Force | Out-Null

function Write-Log {
    param([Parameter(Mandatory = $true)][string]$Message)
    $Message | Tee-Object -FilePath $script:logPath -Append
}

function Remove-NativeContractExecutables {
    param(
        [Parameter(Mandatory = $true)]
        [string]$BinaryDirectory
    )

    $repositoryRootFullPath = [System.IO.Path]::GetFullPath($repositoryRoot).TrimEnd('\', '/')
    $binaryDirectoryFullPath = [System.IO.Path]::GetFullPath($BinaryDirectory).TrimEnd('\', '/')
    if (-not $binaryDirectoryFullPath.StartsWith(
            ($repositoryRootFullPath + [System.IO.Path]::DirectorySeparatorChar),
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Native contract cleanup path must remain below the repository root: $BinaryDirectory"
    }
    Assert-PathChainWithoutReparsePoint `
        -Path $binaryDirectoryFullPath `
        -Context "Native contract cleanup directory"
    if (-not (Test-Path -LiteralPath $binaryDirectoryFullPath -PathType Container)) {
        return
    }

    $testsDirectory = Join-Path $binaryDirectoryFullPath "tests"
    if (-not (Test-Path -LiteralPath $testsDirectory -PathType Container)) {
        return
    }
    Assert-NoReparsePointsBelow `
        -Path $testsDirectory `
        -Context "Native contract cleanup tests directory"
    $testExecutables = @(
        Get-ChildItem -LiteralPath $testsDirectory -File -Recurse -Filter *.exe
    )
    foreach ($testExecutable in $testExecutables) {
        Remove-Item -LiteralPath $testExecutable.FullName -Force
    }
    if ($testExecutables.Count -gt 0) {
        Write-Log ("Removed {0} stale native contract executable(s) from {1}." -f $testExecutables.Count, $testsDirectory)
    }
}

function Invoke-NativeCommand {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )

    Write-Log ("> {0} {1}" -f $FilePath, ($Arguments -join " "))
    & $FilePath @Arguments 2>&1 | Tee-Object -FilePath $script:logPath -Append
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        throw "Command failed with exit code ${exitCode}: $FilePath"
    }
}

$vswherePath = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio/Installer/vswhere.exe"
if (-not (Test-Path -LiteralPath $vswherePath)) {
    throw "vswhere.exe was not found. Install Visual Studio 2026 before building."
}

$selectVisualStudio = Join-Path $PSScriptRoot "select-visual-studio.ps1"
if (-not (Test-Path -LiteralPath $selectVisualStudio)) {
    throw "The Visual Studio selection helper was not found: $selectVisualStudio"
}
$visualStudioInstance = & $selectVisualStudio -VswherePath $vswherePath -Architecture $Architecture
$visualStudioPath = $visualStudioInstance.installationPath
$visualStudioVersion = $visualStudioInstance.catalog.productDisplayVersion
$msbuildPath = Join-Path $visualStudioPath "MSBuild/Current/Bin/MSBuild.exe"
if (-not (Test-Path -LiteralPath $msbuildPath)) {
    throw "MSBuild.exe was not found in the selected Visual Studio instance."
}

$cmakePath = Join-Path $visualStudioPath "Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
if (-not (Test-Path -LiteralPath $cmakePath)) {
    throw "Visual Studio CMake was not found. Install the C++ CMake tools component."
}
$ctestPath = Join-Path (Split-Path -Parent $cmakePath) "ctest.exe"
if (-not (Test-Path -LiteralPath $ctestPath)) {
    throw "ctest.exe was not found beside cmake.exe."
}
$cmakeVersion = (& $cmakePath --version | Select-Object -First 1).Trim()
if (-not $cmakeVersion.StartsWith("cmake version ")) {
    throw "The selected Visual Studio CMake executable did not report a usable version: '$cmakeVersion'."
}

$toolsetVersionPath = Join-Path $visualStudioPath "VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt"
if (-not (Test-Path -LiteralPath $toolsetVersionPath)) {
    throw "The default MSVC toolset version file was not found."
}
$toolsetVersion = (Get-Content -LiteralPath $toolsetVersionPath -Raw).Trim()
if ([string]::IsNullOrWhiteSpace($toolsetVersion)) {
    throw "The selected Visual Studio instance did not report a default MSVC toolset version."
}

$windowsSdkRelease = "10.0.28000.2526"
$windowsSdkProductVersion = "10.1.28000.2526"
$windowsSdkRoot = Join-Path ${env:ProgramFiles(x86)} "Windows Kits/10"
$windowsSdkTarget = "10.0.28000.0"
$windowsSdkRequiredPaths = @(
    (Join-Path $windowsSdkRoot "Include/$windowsSdkTarget/um/Windows.h"),
    (Join-Path $windowsSdkRoot "Include/$windowsSdkTarget/ucrt/stdlib.h"),
    (Join-Path $windowsSdkRoot "Lib/$windowsSdkTarget/ucrt/$Architecture/ucrt.lib"),
    (Join-Path $windowsSdkRoot "Lib/$windowsSdkTarget/um/$Architecture/kernel32.lib"),
    (Join-Path $windowsSdkRoot "bin/$windowsSdkTarget/x64/rc.exe")
)
$missingWindowsSdkPaths = @(
    $windowsSdkRequiredPaths | Where-Object {
        -not (Test-Path -LiteralPath $_)
    }
)
if ($missingWindowsSdkPaths.Count -gt 0) {
    throw "Windows SDK target $windowsSdkTarget is incomplete: $($missingWindowsSdkPaths -join ', ')"
}
$windowsSdkProducts = @(
    "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*",
    "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*"
) | ForEach-Object {
    Get-ItemProperty -Path $_ -ErrorAction SilentlyContinue
} | Where-Object {
    $displayVersion = $_.PSObject.Properties["DisplayVersion"]
    $displayName = $_.PSObject.Properties["DisplayName"]
    $null -ne $displayVersion -and $null -ne $displayName -and
        $displayVersion.Value -eq $windowsSdkProductVersion -and
        $displayName.Value -match "Windows.*(Software Development Kit|SDK)"
}
if (@($windowsSdkProducts).Count -eq 0) {
    throw "Windows SDK release $windowsSdkRelease was not found."
}

$writeManifest = Join-Path $PSScriptRoot "write-build-manifest.ps1"
$startupDiagnosticDeviceDataRootEnabled = [bool]$EnableStartupDiagnosticDeviceDataRoot
& $writeManifest -Architecture $Architecture -Result started -RepositoryRoot $repositoryRoot -VisualStudioPath $visualStudioPath -VisualStudioVersion $visualStudioVersion -MSBuildPath $msbuildPath -CMakePath $cmakePath -WindowsSdkRelease $windowsSdkRelease -OutputPath $manifestPath -StartupDiagnosticDeviceDataRootEnabled $startupDiagnosticDeviceDataRootEnabled

Push-Location $repositoryRoot
try {
    $presetArchitecture = $Architecture.ToLowerInvariant()
    $configurePreset = "windows-$presetArchitecture"
    $buildPreset = "$configurePreset-release"
    $startupDiagnosticDeviceDataRoot = if ($startupDiagnosticDeviceDataRootEnabled) { "ON" } else { "OFF" }
    $buildTesting = if ($RunCoreSmoke) { "ON" } else { "OFF" }
    $cmakeBinaryDirectoryByArchitecture = @{
        "x64" = "out/b/x"
        "ARM64" = "out/b/a"
    }
    $cmakeBinaryDirectory = Join-Path $repositoryRoot $cmakeBinaryDirectoryByArchitecture[$Architecture]
    $coreLibraryDirectory = Join-Path $cmakeBinaryDirectory "lib/Release"
    $winuiVersionedResourceDirectory = Join-Path $cmakeBinaryDirectory "generated/winui"
    if (-not $RunCoreSmoke) {
        Remove-NativeContractExecutables -BinaryDirectory $cmakeBinaryDirectory
        $historicalBinaryDirectory = Join-Path $repositoryRoot "out/build/windows-$presetArchitecture"
        if (Test-Path -LiteralPath $historicalBinaryDirectory -PathType Container) {
            Write-Log "Historical CMake output detected at $historicalBinaryDirectory; it is not used for this build. Only stale contract-test executables are removed."
        }
        Remove-NativeContractExecutables -BinaryDirectory $historicalBinaryDirectory
    }

    Write-Log "Restoring the locked C++/WinRT and XAML host packages."
    Invoke-NativeCommand -FilePath $msbuildPath -Arguments @(
        (Join-Path $repositoryRoot "Azzs.Windows.sln"),
        "/m",
        "/t:Restore",
        "/p:Configuration=Release",
        "/p:Platform=$Architecture",
        "/p:ContinuousIntegrationBuild=true"
    )

    Write-Log "Building $Architecture Release core and Windows adapter."
    Invoke-NativeCommand -FilePath $cmakePath -Arguments @(
        "--preset", $configurePreset,
        "-DCMAKE_GENERATOR_INSTANCE=$visualStudioPath",
        "-DAZZS_ENABLE_STARTUP_DIAGNOSTIC_DEVICE_DATA_ROOT=$startupDiagnosticDeviceDataRoot",
        "-DBUILD_TESTING=$buildTesting"
    )
    Invoke-NativeCommand -FilePath $cmakePath -Arguments @("--build", "--preset", $buildPreset)

    if ($RunCoreSmoke) {
        Write-Log "Running the x64 headless core smoke test."
        Invoke-NativeCommand -FilePath $ctestPath -Arguments @(
            "--preset", "windows-x64-release",
            "--no-tests=error",
            "--output-junit", $testResultPath
        )
    } elseif ($Architecture -eq "x64") {
        Write-Log "Core smoke tests were not requested; the default build does not generate native contract-test executables."
    } elseif ($Architecture -eq "ARM64") {
        Write-Log "ARM64 is compile-and-link only on the hosted x64 build machine; no ARM64 test is executed."
    }

    Write-Log "Building the C++/WinRT and XAML host."
    Invoke-NativeCommand -FilePath $msbuildPath -Arguments @(
        (Join-Path $repositoryRoot "Azzs.Windows.sln"),
        "/m",
        "/t:Build",
        "/p:Configuration=Release",
        "/p:Platform=$Architecture",
        "/p:AzzsCoreLibraryDirectory=$coreLibraryDirectory",
        "/p:AzzsGeneratedResourceDirectory=$winuiVersionedResourceDirectory",
        "/p:AzzsApplicationVersion=$($productVersion.applicationVersion)",
        "/p:AzzsWindowsVersion=$($productVersion.windowsVersion)",
        "/p:AzzsWindowsVersionCommas=$windowsVersionCommasArgument",
        "/p:ContinuousIntegrationBuild=true",
        "/bl:$binlogPath",
        "/fl",
        "/flp:logfile=$msbuildLogPath;verbosity=normal"
    )

    $executablePath = Join-Path $repositoryRoot "out/windows/$Architecture/Release/Azzs.WinUI.exe"
    if (-not (Test-Path -LiteralPath $executablePath)) {
        throw "The WinUI build completed without the expected executable: $executablePath"
    }

    $signingScript = Join-Path $PSScriptRoot "sign-release.ps1"
    $verificationScript = Join-Path $PSScriptRoot "verify-authenticode.ps1"
    $payloadDirectory = Join-Path $repositoryRoot "out/windows/$Architecture/Release"
    if ($RequireAuthenticodeSignature) {
        Write-Log "Signing the Windows payload with the supplied Authenticode certificate."
        & $signingScript -PayloadDirectory $payloadDirectory -AllowedUnsignedFileName "Azzs.WinUI.exe" -CertificateThumbprint $SigningCertificateThumbprint -TimestampUrl $TimestampUrl |
            Tee-Object -FilePath $script:logPath -Append
    } else {
        Write-Log "Development build: Authenticode signing was explicitly not requested."
    }
    if ($RequireAuthenticodeSignature) {
        & $verificationScript `
            -PayloadDirectory $payloadDirectory `
            -RequireRfc3161Timestamp `
            -ExpectedSignerThumbprint $SigningCertificateThumbprint `
            -ExpectedSignerFileName "Azzs.WinUI.exe" |
            Tee-Object -FilePath $script:logPath -Append
    }

    $runtimeCatalogLockContract = Join-Path $repositoryRoot "tests/runtime-catalog-lock-contract/check_runtime_catalog_lock.py"
    if (-not (Test-Path -LiteralPath $runtimeCatalogLockContract -PathType Leaf)) {
        throw "The runtime catalog lock contract was not found: $runtimeCatalogLockContract"
    }
    $pythonCommand = Get-Command python.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $pythonCommand) {
        throw "python.exe was not found. The runtime catalog lock contract requires Python 3.9 or later."
    }
    $runtimeCatalogPayloadRoot = Join-Path $repositoryRoot "out/windows/$Architecture/Release"
    Write-Log "Verifying the runtime catalog locks in the staged Windows payload."
    Invoke-NativeCommand -FilePath $pythonCommand.Source -Arguments @(
        $runtimeCatalogLockContract,
        "--repository-root", $repositoryRoot,
        "--payload-root", $runtimeCatalogPayloadRoot
    )

    & $writeManifest -Architecture $Architecture -Result succeeded -RepositoryRoot $repositoryRoot -VisualStudioPath $visualStudioPath -VisualStudioVersion $visualStudioVersion -MSBuildPath $msbuildPath -CMakePath $cmakePath -WindowsSdkRelease $windowsSdkRelease -OutputPath $manifestPath -StartupDiagnosticDeviceDataRootEnabled $startupDiagnosticDeviceDataRootEnabled
    Write-Log "Build evidence: $manifestPath"
} catch {
    $failureMessage = $_.Exception.Message
    & $writeManifest -Architecture $Architecture -Result failed -RepositoryRoot $repositoryRoot -VisualStudioPath $visualStudioPath -VisualStudioVersion $visualStudioVersion -MSBuildPath $msbuildPath -CMakePath $cmakePath -WindowsSdkRelease $windowsSdkRelease -OutputPath $manifestPath -StartupDiagnosticDeviceDataRootEnabled $startupDiagnosticDeviceDataRootEnabled -FailureMessage $failureMessage
    Write-Log "Build failed: $failureMessage"
    throw
} finally {
    Pop-Location
}
