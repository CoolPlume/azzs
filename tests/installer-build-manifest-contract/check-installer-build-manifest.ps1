[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$RepositoryRoot,

    [Parameter(Mandatory = $true)]
    [string]$TemporaryRoot
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
        throw "installer build manifest contract: $Message"
    }
}

function Write-JsonFile {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Value,

        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    New-Item -ItemType Directory -Path (Split-Path -Parent $Path) -Force | Out-Null
    $Value | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $Path -Encoding UTF8
}

function Get-BuildManifestPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FixtureRoot
    )

    return Join-Path $FixtureRoot "out/manifests/windows-x64-release.json"
}

function Write-ValidBuildManifest {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FixtureRoot
    )

    $commit = (& git -C $FixtureRoot rev-parse HEAD).Trim()
    Require ($LASTEXITCODE -eq 0) "fixture Git commit could not be resolved"
    Write-JsonFile -Value ([ordered]@{
            schemaVersion = 1
            result = "succeeded"
            source = [ordered]@{
                commit = $commit
                dirty = $false
            }
            target = [ordered]@{
                architecture = "x64"
                configuration = "Release"
            }
            buildOptions = [ordered]@{
                startupDiagnosticDeviceDataRoot = $false
            }
            artifacts = @(
                [ordered]@{
                    path = "Azzs.WinUI.exe"
                    bytes = (Get-Item -LiteralPath (Join-Path $FixtureRoot "out/windows/x64/Release/Azzs.WinUI.exe")).Length
                    sha256 = (Get-FileHash -LiteralPath (Join-Path $FixtureRoot "out/windows/x64/Release/Azzs.WinUI.exe") -Algorithm SHA256).Hash.ToLowerInvariant()
                }
            )
        }) -Path (Get-BuildManifestPath -FixtureRoot $FixtureRoot)
}

function New-FixtureRoot {
    $fixtureRoot = Join-Path $TemporaryRoot ("azzs-installer-build-manifest-contract-" + [Guid]::NewGuid().ToString("N"))
    $fixtureRoots.Add($fixtureRoot)
    New-Item -ItemType Directory -Path $fixtureRoot -Force | Out-Null

    $sourceRoot = (Resolve-Path -LiteralPath $RepositoryRoot).Path
    Copy-Item -LiteralPath (Join-Path $sourceRoot ".gitignore") -Destination (Join-Path $fixtureRoot ".gitignore")
    New-Item -ItemType Directory -Path (Join-Path $fixtureRoot "eng") -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $fixtureRoot "release") -Force | Out-Null
    foreach ($scriptName in @("package-installer.ps1", "portable-artifact-content.ps1")) {
        Copy-Item -LiteralPath (Join-Path $sourceRoot "eng/$scriptName") -Destination (Join-Path $fixtureRoot "eng/$scriptName")
    }
    Copy-Item -LiteralPath (Join-Path $sourceRoot "release/product-version.json") -Destination (Join-Path $fixtureRoot "release/product-version.json")

    $payloadDirectory = Join-Path $fixtureRoot "out/windows/x64/Release"
    New-Item -ItemType Directory -Path $payloadDirectory -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $payloadDirectory "Azzs.WinUI.exe") -Value "contract payload" -Encoding ASCII

    & git -C $fixtureRoot init --quiet
    Require ($LASTEXITCODE -eq 0) "fixture Git repository could not be initialized"
    & git -C $fixtureRoot config user.email "installer-contract@example.invalid"
    Require ($LASTEXITCODE -eq 0) "fixture Git email could not be configured"
    & git -C $fixtureRoot config user.name "installer build manifest contract"
    Require ($LASTEXITCODE -eq 0) "fixture Git name could not be configured"
    & git -C $fixtureRoot add .
    Require ($LASTEXITCODE -eq 0) "fixture files could not be staged"
    & git -C $fixtureRoot commit --quiet -m "fixture"
    Require ($LASTEXITCODE -eq 0) "fixture commit could not be created"
    Write-ValidBuildManifest -FixtureRoot $fixtureRoot

    return $fixtureRoot
}

function Require-NoInstallerCandidate {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FixtureRoot,

        [Parameter(Mandatory = $true)]
        [string]$Scenario
    )

    foreach ($candidatePath in @(
            (Join-Path $FixtureRoot "out/staging/installer/x64"),
            (Join-Path $FixtureRoot "out/obj/installer/x64"),
            (Join-Path $FixtureRoot "out/packages/Azzs-standard-x64-machine.msi"),
            (Join-Path $FixtureRoot "out/manifests/package-standard-x64-machine.json")
        )) {
        Require (-not (Test-Path -LiteralPath $candidatePath)) "$Scenario created an installer staging, intermediate, package, or manifest candidate"
    }
}

function Seed-StaleInstallerOutputs {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FixtureRoot
    )

    $stagingDirectory = Join-Path $FixtureRoot "out/staging/installer/x64"
    New-Item -ItemType Directory -Path $stagingDirectory -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $stagingDirectory "stale.txt") -Value "stale" -Encoding ASCII

    $packagePath = Join-Path $FixtureRoot "out/packages/Azzs-standard-x64-machine.msi"
    New-Item -ItemType Directory -Path (Split-Path -Parent $packagePath) -Force | Out-Null
    Set-Content -LiteralPath $packagePath -Value "stale" -Encoding ASCII

    $manifestPath = Join-Path $FixtureRoot "out/manifests/package-standard-x64-machine.json"
    New-Item -ItemType Directory -Path (Split-Path -Parent $manifestPath) -Force | Out-Null
    Set-Content -LiteralPath $manifestPath -Value "{}" -Encoding ASCII
}

function Require-InstallerFailure {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FixtureRoot,

        [Parameter(Mandatory = $true)]
        [string]$Scenario,

        [Parameter(Mandatory = $true)]
        [string]$ExpectedMessage
    )

    $failure = $null
    Push-Location -LiteralPath $FixtureRoot
    try {
        try {
            # The contract intentionally never accepts the WiX EULA.
            & (Join-Path $FixtureRoot "eng/package-installer.ps1") -Architecture x64 -SkipBuild
        }
        catch {
            $failure = $_
        }
    }
    finally {
        Pop-Location
    }

    Require ($null -ne $failure) "$Scenario unexpectedly continued past the installer entry point"
    Require ($failure.Exception.Message -like $ExpectedMessage) "$Scenario returned '$($failure.Exception.Message)' instead of '$ExpectedMessage'"
    Require-NoInstallerCandidate -FixtureRoot $FixtureRoot -Scenario $Scenario
}

New-Item -ItemType Directory -Path $TemporaryRoot -Force | Out-Null
try {
    $staleCommitFixture = New-FixtureRoot
    $staleManifest = Get-Content -LiteralPath (Get-BuildManifestPath -FixtureRoot $staleCommitFixture) -Raw | ConvertFrom-Json
    $staleManifest.source.commit = "0" * 40
    Write-JsonFile -Value $staleManifest -Path (Get-BuildManifestPath -FixtureRoot $staleCommitFixture)
    Require-InstallerFailure -FixtureRoot $staleCommitFixture -Scenario "stale build manifest" -ExpectedMessage "*clean, succeeded x64 Release build manifest for the current commit*"

    $dirtyManifestFixture = New-FixtureRoot
    $dirtyManifest = Get-Content -LiteralPath (Get-BuildManifestPath -FixtureRoot $dirtyManifestFixture) -Raw | ConvertFrom-Json
    $dirtyManifest.source.dirty = $true
    Write-JsonFile -Value $dirtyManifest -Path (Get-BuildManifestPath -FixtureRoot $dirtyManifestFixture)
    Require-InstallerFailure -FixtureRoot $dirtyManifestFixture -Scenario "dirty build manifest" -ExpectedMessage "*clean, succeeded x64 Release build manifest for the current commit*"

    $diagnosticBuildFixture = New-FixtureRoot
    $diagnosticBuildManifest = Get-Content -LiteralPath (Get-BuildManifestPath -FixtureRoot $diagnosticBuildFixture) -Raw | ConvertFrom-Json
    $diagnosticBuildManifest.buildOptions.startupDiagnosticDeviceDataRoot = $true
    Write-JsonFile -Value $diagnosticBuildManifest -Path (Get-BuildManifestPath -FixtureRoot $diagnosticBuildFixture)
    Require-InstallerFailure -FixtureRoot $diagnosticBuildFixture -Scenario "startup diagnostic build manifest" -ExpectedMessage "*clean, succeeded x64 Release build manifest for the current commit*"

    $missingDiagnosticBuildOptionFixture = New-FixtureRoot
    $missingDiagnosticBuildOptionManifest = Get-Content -LiteralPath (Get-BuildManifestPath -FixtureRoot $missingDiagnosticBuildOptionFixture) -Raw | ConvertFrom-Json
    $missingDiagnosticBuildOptionManifest.buildOptions.PSObject.Properties.Remove("startupDiagnosticDeviceDataRoot")
    Write-JsonFile -Value $missingDiagnosticBuildOptionManifest -Path (Get-BuildManifestPath -FixtureRoot $missingDiagnosticBuildOptionFixture)
    Require-InstallerFailure -FixtureRoot $missingDiagnosticBuildOptionFixture -Scenario "missing startup diagnostic build option" -ExpectedMessage "*Portable build manifest buildOptions is missing 'startupDiagnosticDeviceDataRoot'.*"

    $dirtyWorkspaceFixture = New-FixtureRoot
    Set-Content -LiteralPath (Join-Path $dirtyWorkspaceFixture "untracked-payload-marker.txt") -Value "dirty after build" -Encoding ASCII
    Require-InstallerFailure -FixtureRoot $dirtyWorkspaceFixture -Scenario "dirty repository after build" -ExpectedMessage "*clean current Git index, worktree, and untracked-file state*"

    $cleanFixture = New-FixtureRoot
    Seed-StaleInstallerOutputs -FixtureRoot $cleanFixture
    Require-InstallerFailure -FixtureRoot $cleanFixture -Scenario "clean manifest without WiX EULA" -ExpectedMessage "WiX Toolset 7 requires an explicit terms decision*"

    Write-Host "installer build manifest contract: PASS"
}
finally {
    foreach ($fixtureRoot in $fixtureRoots) {
        if (Test-Path -LiteralPath $fixtureRoot) {
            Remove-Item -LiteralPath $fixtureRoot -Recurse -Force
        }
    }
}
