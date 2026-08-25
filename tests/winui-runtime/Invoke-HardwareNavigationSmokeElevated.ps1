[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$ExecutablePath,

    [Parameter(Mandatory = $true)]
    [string]$LogPath,

    [switch]$Elevated
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$logDirectory = Split-Path -Parent $LogPath
New-Item -ItemType Directory -Force -Path $logDirectory | Out-Null
$harnessPath = Join-Path $PSScriptRoot 'Invoke-HardwareNavigationSmoke.ps1'
$currentPrincipal = [Security.Principal.WindowsPrincipal]::new(
    [Security.Principal.WindowsIdentity]::GetCurrent())
$isAdministrator = $currentPrincipal.IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)

function Quote-Argument {
    param([Parameter(Mandatory = $true)][string]$Value)

    return '"' + $Value.Replace('"', '\"') + '"'
}

if (-not $isAdministrator) {
    if ($Elevated) {
        @(
            'result=FAIL'
            'failure_type=ElevationFailure'
            'failure=The elevated UIA runner did not receive administrator integrity.'
        ) | Out-File -LiteralPath $LogPath -Encoding utf8
        exit 1
    }

    try {
        $currentHost = (Get-Process -Id $PID).Path
        $arguments = @(
            '-NoProfile'
            '-NonInteractive'
            '-ExecutionPolicy'
            'Bypass'
            '-File'
            (Quote-Argument $PSCommandPath)
            '-ExecutablePath'
            (Quote-Argument $ExecutablePath)
            '-LogPath'
            (Quote-Argument $LogPath)
            '-Elevated'
        ) -join ' '
        $elevatedProcess = Start-Process -FilePath $currentHost -Verb RunAs `
            -ArgumentList $arguments -Wait -PassThru
        exit $elevatedProcess.ExitCode
    }
    catch {
        @(
            'result=FAIL'
            "failure_type=$($_.Exception.GetType().FullName)"
            "failure=$($_.Exception.Message)"
        ) | Out-File -LiteralPath $LogPath -Encoding utf8
        exit 1
    }
}

try {
    & $harnessPath -ExecutablePath $ExecutablePath *>&1 | Out-File -LiteralPath $LogPath -Encoding utf8
    exit $LASTEXITCODE
}
catch {
    @(
        'result=FAIL'
        "failure_type=$($_.Exception.GetType().FullName)"
        "failure=$($_.Exception.Message)"
    ) | Out-File -LiteralPath $LogPath -Encoding utf8
    exit 1
}
