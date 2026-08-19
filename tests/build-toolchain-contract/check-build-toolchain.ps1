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
    $helperPath = Join-Path (Resolve-Path -LiteralPath $RepositoryRoot).Path "eng/select-visual-studio.ps1"
    Require (Test-Path -LiteralPath $helperPath -PathType Leaf) "selection helper is missing"

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
