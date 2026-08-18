Set-StrictMode -Version Latest

function ConvertTo-AzzsWindowsVersionCommasArgument {
    [OutputType([string])]
    param(
        [Parameter(Mandatory = $true)]
        [string]$WindowsVersion
    )

    if ([string]::IsNullOrWhiteSpace($WindowsVersion) -or
        $WindowsVersion -notmatch "^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$") {
        throw "WindowsVersion must contain exactly four numeric dot-separated segments."
    }

    # MSBuild parses raw commas in /p values as property separators. Percent-escaping
    # keeps them literal until MSBuild has finished parsing the command line.
    return $WindowsVersion.Replace(".", "%2c")
}
