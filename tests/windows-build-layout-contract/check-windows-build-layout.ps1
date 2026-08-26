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

$presetsPath = Join-Path $RepositoryRoot "CMakePresets.json"
$presets = Get-Content -LiteralPath $presetsPath -Raw | ConvertFrom-Json
$binaryDirectoryByPreset = @{}
$buildTestingByPreset = @{}
foreach ($preset in $presets.configurePresets) {
    $binaryDirectoryByPreset[$preset.name] = $preset.binaryDir
    $buildTestingByPreset[$preset.name] = $preset.cacheVariables.BUILD_TESTING
}

Require ($binaryDirectoryByPreset["windows-x64"] -eq '${sourceDir}/out/b/x') "windows-x64 must use the short MSBuild binary directory."
Require ($binaryDirectoryByPreset["windows-arm64"] -eq '${sourceDir}/out/b/a') "windows-arm64 must use the short MSBuild binary directory."
Require ($buildTestingByPreset["windows-x64"] -eq "OFF") "The default Windows x64 build must not generate native contract-test executables."
Require ($buildTestingByPreset["windows-arm64"] -eq "OFF") "The default Windows ARM64 build must not generate native contract-test executables."

$buildScript = Get-Content -LiteralPath (Join-Path $RepositoryRoot "eng/build.ps1") -Raw
Require ($buildScript -match '"x64"\s*=\s*"out/b/x"') "build.ps1 must use the x64 short CMake binary directory."
Require ($buildScript -match '"ARM64"\s*=\s*"out/b/a"') "build.ps1 must use the ARM64 short CMake binary directory."
Require ($buildScript -match '\[switch\]\$RunCoreSmoke') "build.ps1 must make native core smoke tests opt-in."
Require ($buildScript -match '-DBUILD_TESTING=\$buildTesting') "build.ps1 must enable native contract tests only through its explicit smoke-test mode."
Require ($buildScript -match 'if \(\$RunCoreSmoke\)') "build.ps1 must run CTest only when the explicit smoke-test mode is selected."
Require ($buildScript -match 'Remove-NativeContractExecutables') "build.ps1 must remove stale native contract executables before a non-smoke build."
Require ($buildScript -match 'out/build/windows-\$presetArchitecture') "build.ps1 must clean the historical Windows build directory that can retain scanned test executables."

$longestStatePath = Join-Path $RepositoryRoot "out/b/x/src/application/software-optimization-batch-runner/azzs_software_optimization_batch_runner_application.dir/Release/azzs_sof.1CFA3AB9.tlog/azzs_software_optimization_batch_runner_application.lastbuildstate"
Require ($longestStatePath.Length -lt 260) "The Windows CMake binary directory leaves the MSBuild state path at or above the classic path limit."

Write-Output "windows build layout contract: PASS"
