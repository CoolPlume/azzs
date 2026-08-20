[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$RepositoryRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$fixtureRoots = [System.Collections.Generic.List[string]]::new()

function Require {
    param(
        [Parameter(Mandatory = $true)]
        [bool]$Condition,

        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    if (-not $Condition) {
        throw "build toolchain contract: $Message"
    }
}

function New-VswhereFixture {
    $fixtureRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("azzs-build-toolchain-contract-" + [Guid]::NewGuid().ToString("N"))
    $fixtureRoots.Add($fixtureRoot)
    New-Item -ItemType Directory -Path $fixtureRoot -Force | Out-Null

    $jsonPath = Join-Path $fixtureRoot "instance.json"
    $instances = @(
        [ordered]@{
            installationPath = (Join-Path $fixtureRoot "visual-studio")
            catalog = [ordered]@{
                productDisplayVersion = "18.8.3"
            }
        }
    )
    ConvertTo-Json -InputObject $instances -Depth 8 | Set-Content -LiteralPath $jsonPath -Encoding ASCII
    Require ((Get-Content -LiteralPath $jsonPath -Raw).TrimStart().StartsWith("[")) `
        "vswhere fixture must preserve the JSON array response shape"

    $argumentsPath = Join-Path $fixtureRoot "vswhere-arguments.txt"
    $vswherePath = Join-Path $fixtureRoot "vswhere.cmd"
    @'
@echo off
> "%AZZS_VSWHERE_ARGUMENTS_FILE%" echo %*
if /I "%AZZS_VSWHERE_MODE%"=="missing-x64" (
  findstr /L /C:"Microsoft.VisualStudio.Component.VC.Tools.x86.x64" "%AZZS_VSWHERE_ARGUMENTS_FILE%" >nul
  if not errorlevel 1 exit /b 0
)
type "%AZZS_VSWHERE_JSON_FILE%"
exit /b 0
'@ | Set-Content -LiteralPath $vswherePath -Encoding ASCII

    return [pscustomobject]@{
        Root = $fixtureRoot
        JsonPath = $jsonPath
        ArgumentsPath = $argumentsPath
        VswherePath = $vswherePath
    }
}

function Invoke-VisualStudioSelection {
    param(
        [Parameter(Mandatory = $true)]
        [string]$HelperPath,

        [Parameter(Mandatory = $true)]
        [pscustomobject]$Fixture,

        [Parameter(Mandatory = $true)]
        [ValidateSet("x64", "ARM64")]
        [string]$Architecture,

        [Parameter(Mandatory = $true)]
        [string]$Mode
    )

    $env:AZZS_VSWHERE_ARGUMENTS_FILE = $Fixture.ArgumentsPath
    $env:AZZS_VSWHERE_JSON_FILE = $Fixture.JsonPath
    $env:AZZS_VSWHERE_MODE = $Mode
    return & $HelperPath -VswherePath $Fixture.VswherePath -Architecture $Architecture
}

try {
    $resolvedRepositoryRoot = (Resolve-Path -LiteralPath $RepositoryRoot).Path
    $helperPath = Join-Path $resolvedRepositoryRoot "eng/select-visual-studio.ps1"
    Require (Test-Path -LiteralPath $helperPath -PathType Leaf) "selection helper is missing"

    $rootCmake = Get-Content -LiteralPath (Join-Path $resolvedRepositoryRoot "CMakeLists.txt") -Raw
    $presets = Get-Content -LiteralPath (Join-Path $resolvedRepositoryRoot "CMakePresets.json") -Raw | ConvertFrom-Json
    $buildScript = Get-Content -LiteralPath (Join-Path $resolvedRepositoryRoot "eng/build.ps1") -Raw
    $manifestScript = Get-Content -LiteralPath (Join-Path $resolvedRepositoryRoot "eng/write-build-manifest.ps1") -Raw
    $diagnosticHeader = Get-Content -LiteralPath (Join-Path $resolvedRepositoryRoot "cmake/startup_diagnostic_config.hpp.in") -Raw
    $winuiProject = Get-Content -LiteralPath (Join-Path $resolvedRepositoryRoot "src/adapters/ui/winui/Azzs.WinUI.vcxproj") -Raw
    $compositionRoot = Get-Content -LiteralPath (Join-Path $resolvedRepositoryRoot "src/composition/windows/composition_root.cpp") -Raw
    $deviceEnvironmentHeader = Get-Content -LiteralPath (Join-Path $resolvedRepositoryRoot "src/adapters/windows/include/azzs/adapters/windows/windows_device_data_environment.hpp") -Raw
    $deviceEnvironmentSource = Get-Content -LiteralPath (Join-Path $resolvedRepositoryRoot "src/adapters/windows/src/windows_device_data_environment.cpp") -Raw
    $diagnosticOption = "AZZS_ENABLE_STARTUP_DIAGNOSTIC_DEVICE_DATA_ROOT"
    Require ($rootCmake -match "(?s)option\(\s*$diagnosticOption\s+.*?\s+OFF\s*\)") "the startup diagnostic root CMake option must default to OFF"
    foreach ($preset in $presets.configurePresets) {
        $presetValue = $preset.cacheVariables.PSObject.Properties[$diagnosticOption]
        Require ($null -ne $presetValue -and $presetValue.Value -eq "OFF") "configure preset '$($preset.name)' must default the startup diagnostic root to OFF"
    }
    Require ($diagnosticHeader.Contains("#define $diagnosticOption @AZZS_STARTUP_DIAGNOSTIC_DEVICE_DATA_ROOT_ENABLED@")) "the CMake option must generate the compile-time diagnostic guard"
    Require ($buildScript.Contains('"-DAZZS_ENABLE_STARTUP_DIAGNOSTIC_DEVICE_DATA_ROOT=$startupDiagnosticDeviceDataRoot"')) "the build entry point must explicitly set the diagnostic CMake option"
    Require ($buildScript.Contains('-StartupDiagnosticDeviceDataRootEnabled $startupDiagnosticDeviceDataRootEnabled')) "the build entry point must record the diagnostic mode in every build manifest"
    Require ($manifestScript.Contains('StartupDiagnosticDeviceDataRootEnabled = $false') -and
        $manifestScript.Contains('startupDiagnosticDeviceDataRoot = $StartupDiagnosticDeviceDataRootEnabled')) "the build manifest must record the diagnostic mode"
    $generatedDiagnosticInclude = '$(MSBuildThisFileDirectory)..\..\..\..\out\obj\winui\generated;'
    Require ($winuiProject.Contains($generatedDiagnosticInclude)) "the WinUI host must include the CMake-generated diagnostic guard through the controlled intermediate directory"
    Require ($rootCmake.Contains('out/obj/winui/generated') -and
        $rootCmake.Contains('AZZS_STARTUP_DIAGNOSTIC_CONFIG_DIRECTORY')) "CMake must generate the diagnostic guard in the controlled intermediate directory"
    Require ($compositionRoot.Contains("#if $diagnosticOption") -and
        $compositionRoot.Contains("AZZS_STARTUP_DIAGNOSTIC_DEVICE_DATA_ROOT") -and
        $compositionRoot.Contains("startup_diagnostic_device_data_options()") -and
        $compositionRoot.Contains("WindowsDeviceDataEnvironment::prepare()")) "the composition root must keep diagnostic and production roots compile-time separated"
    Require ($deviceEnvironmentHeader.Contains("diagnostic_root_utf8") -and
        $deviceEnvironmentHeader.Contains("std::optional<std::string> diagnostic_root_utf8;") -and
        $deviceEnvironmentHeader.Contains("DeviceDataEnvironmentOptions options = {}") -and
        -not $deviceEnvironmentHeader.Contains("root_override_utf8") -and
        -not $deviceEnvironmentHeader.Contains("subject_override") -and
        -not $deviceEnvironmentHeader.Contains("uses_test_root") -and
        $deviceEnvironmentSource.Contains("prepare_device_data_impl(") -and
        $deviceEnvironmentSource.Contains("return prepare_device_data_impl(options.diagnostic_root_utf8);") -and
        $deviceEnvironmentSource.Contains("auto subject = resolve_interactive_subject();") -and
        -not $deviceEnvironmentSource.Contains("root_override_utf8") -and
        -not $deviceEnvironmentSource.Contains("subject_override") -and
        $deviceEnvironmentSource.Contains("is_d_drive_path") -and
        $deviceEnvironmentSource.Contains("diagnostic device data root must be on drive D:")) "diagnostic roots must use the production identity contract without exposing test overrides"
    Require ($compositionRoot.Contains("static_cast<std::size_t>(required) + 1")) "diagnostic environment reads must reserve the terminating buffer element"

    $fixture = New-VswhereFixture
    $x64Instance = @(Invoke-VisualStudioSelection `
            -HelperPath $helperPath `
            -Fixture $fixture `
            -Architecture x64 `
            -Mode success)
    Require ($x64Instance.Count -eq 1) "x64 selection did not return one instance"
    Require ([string]$x64Instance[0].catalog.productDisplayVersion -eq "18.8.3") "a compatible newer patch version was not accepted"

    $x64Arguments = Get-Content -LiteralPath $fixture.ArgumentsPath -Raw
    Require ($x64Arguments.Contains("-version [18.8.2,19.0)")) "x64 selection did not use the compatible Stable 18 version range"
    Require ($x64Arguments.Contains("Microsoft.Component.MSBuild")) "x64 selection omitted MSBuild"
    Require ($x64Arguments.Contains("Microsoft.VisualStudio.Component.VC.Tools.x86.x64")) "x64 selection omitted x64 C++ tools"
    Require ($x64Arguments.Contains("Microsoft.VisualStudio.Component.VC.CMake.Project")) "x64 selection omitted CMake tools"
    Require ($x64Arguments.Contains("Microsoft.VisualStudio.ComponentGroup.UWP.VC")) "x64 selection omitted UWP C++ tools"
    Require (-not $x64Arguments.Contains("Microsoft.VisualStudio.Component.VC.Tools.ARM64")) "x64 selection must not require ARM64 tools"

    $arm64Instance = @(Invoke-VisualStudioSelection `
            -HelperPath $helperPath `
            -Fixture $fixture `
            -Architecture ARM64 `
            -Mode success)
    Require ($arm64Instance.Count -eq 1) "ARM64 selection did not return one instance"
    $arm64Arguments = Get-Content -LiteralPath $fixture.ArgumentsPath -Raw
    Require ($arm64Arguments.Contains("Microsoft.VisualStudio.Component.VC.Tools.ARM64")) "ARM64 selection omitted ARM64 C++ tools"

    $missingX64Failed = $false
    try {
        Invoke-VisualStudioSelection `
            -HelperPath $helperPath `
            -Fixture $fixture `
            -Architecture x64 `
            -Mode missing-x64 | Out-Null
    }
    catch {
        $missingX64Failed = $true
    }
    Require $missingX64Failed "an instance missing x64 C++ tools was not rejected"

    Write-Host "build toolchain contract: PASS"
}
finally {
    Remove-Item Env:AZZS_VSWHERE_ARGUMENTS_FILE -ErrorAction SilentlyContinue
    Remove-Item Env:AZZS_VSWHERE_JSON_FILE -ErrorAction SilentlyContinue
    Remove-Item Env:AZZS_VSWHERE_MODE -ErrorAction SilentlyContinue
    foreach ($fixtureRoot in $fixtureRoots) {
        if (Test-Path -LiteralPath $fixtureRoot) {
            Remove-Item -LiteralPath $fixtureRoot -Recurse -Force
        }
    }
}
