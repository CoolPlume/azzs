[CmdletBinding()]
param(
    [ValidateSet("x64", "ARM64")]
    [string]$Architecture = "x64",

    [switch]$SkipCoreSmoke
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$evidenceName = "windows-$($Architecture.ToLowerInvariant())-release"
$logDirectory = Join-Path $repositoryRoot "out/logs"
$manifestDirectory = Join-Path $repositoryRoot "out/manifests"
$testResultDirectory = Join-Path $repositoryRoot "out/test-results"
$logPath = Join-Path $logDirectory "$evidenceName.log"
$msbuildLogPath = Join-Path $logDirectory "$evidenceName.msbuild.log"
$binlogPath = Join-Path $logDirectory "$evidenceName.binlog"
$manifestPath = Join-Path $manifestDirectory "$evidenceName.json"
$testResultPath = Join-Path $testResultDirectory "core-$($Architecture.ToLowerInvariant())-release.xml"

New-Item -ItemType Directory -Path $logDirectory, $manifestDirectory, $testResultDirectory -Force | Out-Null
New-Item -ItemType File -Path $logPath -Force | Out-Null

function Write-Log {
    param([Parameter(Mandatory = $true)][string]$Message)
    $Message | Tee-Object -FilePath $script:logPath -Append
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

$instanceJson = & $vswherePath -latest -products * -version "[18.8,18.9)" -requires Microsoft.Component.MSBuild -format json -utf8
if ($LASTEXITCODE -ne 0 -or -not $instanceJson) {
    throw "Visual Studio 2026 Stable 18.8.2 was not found."
}
$visualStudioInstance = @(ConvertFrom-Json ($instanceJson -join [Environment]::NewLine))[0]
$visualStudioPath = $visualStudioInstance.installationPath
$visualStudioVersion = $visualStudioInstance.catalog.productDisplayVersion
if ($visualStudioVersion -ne "18.8.2") {
    throw "Visual Studio 2026 Stable 18.8.2 is required; found '$visualStudioVersion'."
}
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
if ($cmakeVersion -ne "cmake version 4.3.1-msvc1") {
    throw "CMake 4.3.1-msvc1 is required by the Windows build baseline; found '$cmakeVersion'."
}

$toolsetVersionPath = Join-Path $visualStudioPath "VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt"
if (-not (Test-Path -LiteralPath $toolsetVersionPath)) {
    throw "The default MSVC toolset version file was not found."
}
$toolsetVersion = (Get-Content -LiteralPath $toolsetVersionPath -Raw).Trim()
if ($toolsetVersion -ne "14.51.36231") {
    throw "MSVC 14.51.36231 is required; the selected Visual Studio instance defaults to '$toolsetVersion'."
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
& $writeManifest -Architecture $Architecture -Result started -RepositoryRoot $repositoryRoot -VisualStudioPath $visualStudioPath -VisualStudioVersion $visualStudioVersion -MSBuildPath $msbuildPath -CMakePath $cmakePath -WindowsSdkRelease $windowsSdkRelease -OutputPath $manifestPath

Push-Location $repositoryRoot
try {
    $presetArchitecture = $Architecture.ToLowerInvariant()
    $configurePreset = "windows-$presetArchitecture"
    $buildPreset = "$configurePreset-release"
    $coreLibraryDirectory = Join-Path $repositoryRoot "out/build/$configurePreset/lib/Release"

    Write-Log "Building $Architecture Release core and Windows adapter."
    Invoke-NativeCommand -FilePath $cmakePath -Arguments @(
        "--preset", $configurePreset,
        "-DCMAKE_GENERATOR_INSTANCE=$visualStudioPath"
    )
    Invoke-NativeCommand -FilePath $cmakePath -Arguments @("--build", "--preset", $buildPreset)

    if ($Architecture -eq "x64" -and -not $SkipCoreSmoke) {
        Write-Log "Running the x64 headless core smoke test."
        Invoke-NativeCommand -FilePath $ctestPath -Arguments @(
            "--preset", "windows-x64-release",
            "--no-tests=error",
            "--output-junit", $testResultPath
        )
    } elseif ($Architecture -eq "ARM64") {
        Write-Log "ARM64 is compile-and-link only on the hosted x64 build machine; no ARM64 test is executed."
    }

    Write-Log "Building the C++/WinRT and XAML host."
    Invoke-NativeCommand -FilePath $msbuildPath -Arguments @(
        (Join-Path $repositoryRoot "Azzs.Windows.sln"),
        "/restore",
        "/m",
        "/t:Build",
        "/p:Configuration=Release",
        "/p:Platform=$Architecture",
        "/p:AzzsCoreLibraryDirectory=$coreLibraryDirectory",
        "/p:ContinuousIntegrationBuild=true",
        "/bl:$binlogPath",
        "/fl",
        "/flp:logfile=$msbuildLogPath;verbosity=normal"
    )

    $executablePath = Join-Path $repositoryRoot "out/windows/$Architecture/Release/Azzs.WinUI.exe"
    if (-not (Test-Path -LiteralPath $executablePath)) {
        throw "The WinUI build completed without the expected executable: $executablePath"
    }

    & $writeManifest -Architecture $Architecture -Result succeeded -RepositoryRoot $repositoryRoot -VisualStudioPath $visualStudioPath -VisualStudioVersion $visualStudioVersion -MSBuildPath $msbuildPath -CMakePath $cmakePath -WindowsSdkRelease $windowsSdkRelease -OutputPath $manifestPath
    Write-Log "Build evidence: $manifestPath"
} catch {
    $failureMessage = $_.Exception.Message
    & $writeManifest -Architecture $Architecture -Result failed -RepositoryRoot $repositoryRoot -VisualStudioPath $visualStudioPath -VisualStudioVersion $visualStudioVersion -MSBuildPath $msbuildPath -CMakePath $cmakePath -WindowsSdkRelease $windowsSdkRelease -OutputPath $manifestPath -FailureMessage $failureMessage
    Write-Log "Build failed: $failureMessage"
    throw
} finally {
    Pop-Location
}
