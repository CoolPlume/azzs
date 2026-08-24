[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$ExecutablePath,

    [ValidateRange(1, 60)]
    [int]$StartupTimeoutSeconds = 20,

    [ValidateRange(1, 60)]
    [int]$NavigationTimeoutSeconds = 15,

    [switch]$RunAsInvoker
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$currentPrincipal = [Security.Principal.WindowsPrincipal]::new(
    [Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $currentPrincipal.IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Hardware navigation UIA smoke requires an elevated runner to inspect the elevated WinUI tree.'
}
Write-Output 'runner_integrity=administrator'

function Assert-ProcessAlive {
    param([System.Diagnostics.Process]$Process)

    $Process.Refresh()
    if ($Process.HasExited) {
        throw "Azzs.WinUI exited before the hardware page was projected; exit_code=$($Process.ExitCode)"
    }
}

function Find-TopLevelWindow {
    param([int]$ProcessId)

    $condition = [System.Windows.Automation.PropertyCondition]::new(
        [System.Windows.Automation.AutomationElement]::ProcessIdProperty,
        $ProcessId)
    return [System.Windows.Automation.AutomationElement]::RootElement.FindFirst(
        [System.Windows.Automation.TreeScope]::Children,
        $condition)
}

function Find-DescendantByAutomationId {
    param(
        [System.Windows.Automation.AutomationElement]$Root,
        [string]$AutomationId
    )

    $condition = [System.Windows.Automation.PropertyCondition]::new(
        [System.Windows.Automation.AutomationElement]::AutomationIdProperty,
        $AutomationId)
    return $Root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $condition)
}

function Get-ObservedAutomationIds {
    param([System.Windows.Automation.AutomationElement]$Root)

    $ids = [System.Collections.Generic.List[string]]::new()
    $elements = $Root.FindAll(
        [System.Windows.Automation.TreeScope]::Descendants,
        [System.Windows.Automation.Condition]::TrueCondition)
    foreach ($element in $elements) {
        try {
            $automationId = $element.Current.AutomationId
            if (-not [string]::IsNullOrWhiteSpace($automationId)) {
                $ids.Add($automationId)
            }
        }
        catch [System.Windows.Automation.ElementNotAvailableException] {
            continue
        }
    }

    return @($ids | Sort-Object -Unique | Select-Object -First 80)
}

function Wait-ForTopLevelWindow {
    param(
        [System.Diagnostics.Process]$Process,
        [int]$TimeoutSeconds
    )

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        Assert-ProcessAlive -Process $Process
        $window = Find-TopLevelWindow -ProcessId $Process.Id
        if ($null -ne $window) {
            return $window
        }
        Start-Sleep -Milliseconds 200
    }

    throw "Timed out waiting for the Azzs.WinUI top-level window after $TimeoutSeconds seconds"
}

function Wait-ForDescendant {
    param(
        [System.Diagnostics.Process]$Process,
        [System.Windows.Automation.AutomationElement]$Root,
        [string]$AutomationId,
        [int]$TimeoutSeconds
    )

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        Assert-ProcessAlive -Process $Process
        $element = Find-DescendantByAutomationId -Root $Root -AutomationId $AutomationId
        if ($null -ne $element) {
            return $element
        }
        Start-Sleep -Milliseconds 200
    }

    $observedIds = @(Get-ObservedAutomationIds -Root $Root)
    $observedText = if ($observedIds.Count -eq 0) { 'none' } else { $observedIds -join ',' }
    throw "Timed out waiting for AutomationId=$AutomationId after $TimeoutSeconds seconds; observed_automation_ids=$observedText"
}

function Get-ElementProjectionText {
    param([System.Windows.Automation.AutomationElement]$Element)

    try {
        $name = $Element.Current.Name
        if (-not [string]::IsNullOrWhiteSpace($name)) {
            return $name.Trim()
        }
    }
    catch [System.Windows.Automation.ElementNotAvailableException] {
        return $null
    }

    try {
        $valuePattern = $null
        if ($Element.TryGetCurrentPattern(
                [System.Windows.Automation.ValuePattern]::Pattern,
                [ref]$valuePattern)) {
            $value = ([System.Windows.Automation.ValuePattern]$valuePattern).Current.Value
            if (-not [string]::IsNullOrWhiteSpace($value)) {
                return $value.Trim()
            }
        }
    }
    catch [System.Windows.Automation.ElementNotAvailableException] {
        return $null
    }

    return $null
}

function Wait-ForHardwareProjection {
    param(
        [System.Diagnostics.Process]$Process,
        [System.Windows.Automation.AutomationElement]$Root,
        [int]$TimeoutSeconds
    )

    # WinUI does not expose the Page or Border roots consistently through UIA.
    # The localized model label and populated model value are stable descendants.
    $null = Wait-ForDescendant -Process $Process -Root $Root -AutomationId 'AzzsHardwareModelSummary' -TimeoutSeconds $TimeoutSeconds

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        Assert-ProcessAlive -Process $Process
        $model = Find-DescendantByAutomationId -Root $Root -AutomationId 'AzzsHardwareModel'
        if ($null -ne $model) {
            $modelText = Get-ElementProjectionText -Element $model
            if (-not [string]::IsNullOrWhiteSpace($modelText)) {
                return [PSCustomObject]@{
                    ModelTextLength = $modelText.Length
                }
            }
        }

        Start-Sleep -Milliseconds 200
    }

    throw "Timed out waiting for non-empty hardware model projection after $TimeoutSeconds seconds"
}

function Wait-ForOverviewProjection {
    param(
        [System.Diagnostics.Process]$Process,
        [System.Windows.Automation.AutomationElement]$Root,
        [int]$TimeoutSeconds
    )

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        Assert-ProcessAlive -Process $Process
        $overviewPage = Find-DescendantByAutomationId -Root $Root -AutomationId 'AzzsOverviewPage'
        if ($null -ne $overviewPage) {
            return 'AzzsOverviewPage'
        }

        # See the hardware-page note above: retain a semantic page marker for
        # WinUI projections that omit the Page root from the automation tree.
        $overviewStages = Find-DescendantByAutomationId -Root $Root -AutomationId 'AzzsGuidedInitializationStages'
        if ($null -ne $overviewStages) {
            return 'AzzsGuidedInitializationStages'
        }

        Start-Sleep -Milliseconds 200
    }

    $observedIds = @(Get-ObservedAutomationIds -Root $Root)
    $observedText = if ($observedIds.Count -eq 0) { 'none' } else { $observedIds -join ',' }
    throw "Timed out waiting for overview projection after $TimeoutSeconds seconds; observed_automation_ids=$observedText"
}

function Invoke-Navigation {
    param(
        [System.Windows.Automation.AutomationElement]$NavigationItem,
        [string]$AutomationId
    )

    $pattern = $null
    if ($NavigationItem.TryGetCurrentPattern(
            [System.Windows.Automation.SelectionItemPattern]::Pattern,
            [ref]$pattern)) {
        ([System.Windows.Automation.SelectionItemPattern]$pattern).Select()
        return 'SelectionItemPattern.Select'
    }

    if ($NavigationItem.TryGetCurrentPattern(
            [System.Windows.Automation.InvokePattern]::Pattern,
            [ref]$pattern)) {
        ([System.Windows.Automation.InvokePattern]$pattern).Invoke()
        return 'InvokePattern.Invoke'
    }

    throw "$AutomationId exposes neither SelectionItemPattern nor InvokePattern"
}

function Get-ApplicationFailureSignals {
    param(
        [int]$ProcessId,
        [DateTime]$StartedAt
    )

    $hexProcessId = ('0x{0:x}' -f $ProcessId)
    $signals = @()
    try {
        $events = @(Get-WinEvent -FilterHashtable @{
            LogName = 'Application'
            StartTime = $StartedAt
            Id = 1000, 1001, 1026
        } -ErrorAction SilentlyContinue)

        foreach ($event in $events) {
            $xml = $event.ToXml()
            if ($xml.Contains('Azzs.WinUI.exe') -or $xml.Contains($hexProcessId)) {
                $signals += [PSCustomObject]@{
                        Provider = $event.ProviderName
                        Id = $event.Id
                        Level = $event.LevelDisplayName
                        TimeUtc = $event.TimeCreated.ToUniversalTime().ToString('o')
                    }
            }
        }

        return [PSCustomObject]@{
            Available = $true
            FailureType = $null
            Signals = @($signals)
        }
    }
    catch {
        return [PSCustomObject]@{
            Available = $false
            FailureType = $_.Exception.GetType().Name
            Signals = @()
        }
    }
}

$runStartedAt = Get-Date
$process = $null
$window = $null
$scriptExitCode = 0

try {
    $existing = @(Get-Process -Name 'Azzs.WinUI' -ErrorAction SilentlyContinue)
    if ($existing.Count -gt 0) {
        $existingIds = ($existing | ForEach-Object Id) -join ','
        throw "Refusing to test while pre-existing Azzs.WinUI process(es) exist: $existingIds"
    }

    $resolvedExecutablePath = (Resolve-Path -LiteralPath $ExecutablePath).Path
    $previousCompatLayer = [Environment]::GetEnvironmentVariable('__COMPAT_LAYER', 'Process')
    try {
        if ($RunAsInvoker) {
            [Environment]::SetEnvironmentVariable('__COMPAT_LAYER', 'RUNASINVOKER', 'Process')
        }
        $process = Start-Process -FilePath $resolvedExecutablePath -WorkingDirectory (Split-Path -Parent $resolvedExecutablePath) -PassThru
    }
    finally {
        [Environment]::SetEnvironmentVariable('__COMPAT_LAYER', $previousCompatLayer, 'Process')
    }
    Write-Output "pid=$($process.Id)"
    Write-Output "launch_mode=$(if ($RunAsInvoker) { 'RUNASINVOKER' } else { 'manifest-default' })"

    $window = Wait-ForTopLevelWindow -Process $process -TimeoutSeconds $StartupTimeoutSeconds
    $null = Wait-ForDescendant -Process $process -Root $window -AutomationId 'AzzsPrimaryNavigation' -TimeoutSeconds $StartupTimeoutSeconds
    $driversItem = Wait-ForDescendant -Process $process -Root $window -AutomationId 'AzzsNavigationDrivers' -TimeoutSeconds $StartupTimeoutSeconds
    $firstDriverActivation = Invoke-Navigation -NavigationItem $driversItem -AutomationId 'AzzsNavigationDrivers'
    $firstProjection = Wait-ForHardwareProjection -Process $process -Root $window -TimeoutSeconds $NavigationTimeoutSeconds

    $overviewItem = Wait-ForDescendant -Process $process -Root $window -AutomationId 'AzzsNavigationOverview' -TimeoutSeconds $NavigationTimeoutSeconds
    $overviewActivation = Invoke-Navigation -NavigationItem $overviewItem -AutomationId 'AzzsNavigationOverview'
    $overviewProjection = Wait-ForOverviewProjection -Process $process -Root $window -TimeoutSeconds $NavigationTimeoutSeconds

    $driversItem = Wait-ForDescendant -Process $process -Root $window -AutomationId 'AzzsNavigationDrivers' -TimeoutSeconds $NavigationTimeoutSeconds
    $secondDriverActivation = Invoke-Navigation -NavigationItem $driversItem -AutomationId 'AzzsNavigationDrivers'
    $secondProjection = Wait-ForHardwareProjection -Process $process -Root $window -TimeoutSeconds $NavigationTimeoutSeconds
    Start-Sleep -Milliseconds 1000
    Assert-ProcessAlive -Process $process

    Write-Output 'result=PASS'
    Write-Output 'driver_entries=2'
    Write-Output 'overview_returns=1'
    Write-Output "first_driver_activation=$firstDriverActivation"
    Write-Output "overview_activation=$overviewActivation"
    Write-Output "second_driver_activation=$secondDriverActivation"
    Write-Output "overview_projection=$overviewProjection"
    Write-Output 'projection=AzzsHardwareModelSummary,AzzsHardwareModel'
    Write-Output "first_model_text_length=$($firstProjection.ModelTextLength)"
    Write-Output "second_model_text_length=$($secondProjection.ModelTextLength)"
}
catch {
    $scriptExitCode = 1
    Write-Output 'result=FAIL'
    Write-Output "failure_type=$($_.Exception.GetType().FullName)"
    Write-Output "failure=$($_.Exception.Message)"
}
finally {
    if ($null -ne $process) {
        try {
            Start-Sleep -Milliseconds 750
            $eventLogResult = Get-ApplicationFailureSignals -ProcessId $process.Id -StartedAt $runStartedAt
            if (-not $eventLogResult.Available) {
                Write-Output "event_log=unavailable:$($eventLogResult.FailureType)"
            }
            elseif ($eventLogResult.Signals.Count -eq 0) {
                Write-Output 'application_failure_events=none'
            }
            else {
                foreach ($signal in $eventLogResult.Signals) {
                    Write-Output "application_failure_event=provider:$($signal.Provider);id:$($signal.Id);level:$($signal.Level);time_utc:$($signal.TimeUtc)"
                }
            }
        }
        catch {
            Write-Output "event_log=unavailable:$($_.Exception.GetType().Name)"
        }

        try {
            $process.Refresh()
            if ($process.HasExited) {
                Write-Output "observed_exit_code=$($process.ExitCode)"
            }
            else {
                $windowPattern = $null
                if (($null -ne $window) -and $window.TryGetCurrentPattern(
                        [System.Windows.Automation.WindowPattern]::Pattern,
                        [ref]$windowPattern)) {
                    ([System.Windows.Automation.WindowPattern]$windowPattern).Close()
                    $process.WaitForExit(5000) | Out-Null
                }

                $process.Refresh()
                if (-not $process.HasExited) {
                    Stop-Process -Id $process.Id -ErrorAction Stop
                    $process.WaitForExit(5000) | Out-Null
                    Write-Output 'cleanup=Stop-Process:self-started-pid'
                }
                else {
                    Write-Output 'cleanup=WindowPattern.Close:self-started-pid'
                }
            }
        }
        catch {
            Write-Output "cleanup=failed:$($_.Exception.GetType().Name)"
            $scriptExitCode = 1
        }
    }
}

exit $scriptExitCode
