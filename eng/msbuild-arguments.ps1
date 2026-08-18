Set-StrictMode -Version Latest

function ConvertTo-AzzsWindowsVersionCommasArgument {
    [OutputType([string])]
    param(
        [Parameter(Mandatory = $true)]
        [ValidatePattern("^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$")]
        [string]$WindowsVersion
    )

    # MSBuild parses raw commas in /p values as property separators. Percent-escaping
    # keeps them literal until MSBuild has finished parsing the command line.
    return $WindowsVersion.Replace(".", "%2c")
}
