<#
.SYNOPSIS
    Runs the Visual Studio bundled clang-format over src/ and tests/.

.DESCRIPTION
    Everyone on the same Visual Studio version gets the same formatter, so the version is
    printed. -Check is the authoritative formatting gate in CI.

.EXAMPLE
    tools\format.ps1
    tools\format.ps1 -Check
#>
[CmdletBinding()]
param(
    # Report badly formatted files and fail instead of rewriting them.
    [switch] $Check
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Common.ps1')

$clangFormat = Get-ClangToolPath -Name 'clang-format.exe'
& $clangFormat --version

$files = Get-SourceFiles
if (-not $files)
{
    Write-Host 'No source files found.' -ForegroundColor Yellow
    exit 0
}

$paths = $files | ForEach-Object { $_.FullName }

if ($Check)
{
    & $clangFormat --dry-run --Werror @paths
    if ($LASTEXITCODE -ne 0)
    {
        throw 'Formatting check failed. Run tools\format.ps1 to fix it.'
    }
    Write-Host "Formatting is clean ($($files.Count) files)." -ForegroundColor Green
    exit 0
}

& $clangFormat -i @paths
if ($LASTEXITCODE -ne 0)
{
    throw "clang-format failed with exit code $LASTEXITCODE."
}
Write-Host "Formatted $($files.Count) files." -ForegroundColor Green
