<#
.SYNOPSIS
    Builds the game-independent projects and runs the Catch2 test executable.

.EXAMPLE
    tools\test.ps1
    tools\test.ps1 -Configuration Release -CatchArguments '[manifest]'
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Dev', 'Release')]
    [string] $Configuration = 'Debug',

    # Passed straight to spl_tests.exe, e.g. a Catch2 tag filter such as '[manifest]'.
    [string[]] $CatchArguments = @()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Common.ps1')

$root = Get-RepoRoot

& (Join-Path $PSScriptRoot 'build.ps1') -Configuration $Configuration -Core
if ($LASTEXITCODE -ne 0)
{
    throw "Build failed with exit code $LASTEXITCODE."
}

$tests = Join-Path $root "dist\$Configuration\spl_tests.exe"
if (-not (Test-Path $tests))
{
    throw "$tests was not produced by the build."
}

Write-Host "Running $tests..." -ForegroundColor Cyan
& $tests @CatchArguments
$exitCode = $LASTEXITCODE
if ($exitCode -ne 0)
{
    throw "Tests failed with exit code $exitCode."
}

Write-Host 'Tests passed.' -ForegroundColor Green
