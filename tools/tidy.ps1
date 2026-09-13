<#
.SYNOPSIS
    Builds with Visual Studio's clang-tidy code analysis enabled.

.DESCRIPTION
    The VS integration picks up the repository's .clang-tidy automatically. Analysis is off in
    normal builds for speed (props/Common.props), and this script turns it on.

.EXAMPLE
    tools\tidy.ps1
    tools\tidy.ps1 -WithMsvcAnalysis      # also run MSVC's own C++ Core Check rules
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Dev', 'Release')]
    [string] $Configuration = 'Debug',

    # Analyze only the game-independent projects.
    [switch] $Core,

    # Run MSVC code analysis alongside clang-tidy, as a second opinion.
    [switch] $WithMsvcAnalysis
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Common.ps1')

Initialize-Submodules

$root = Get-RepoRoot
$target = if ($Core) { 'SpResourceLoader.Core.slnf' } else { 'SpResourceLoader.sln' }
$msvcAnalysis = if ($WithMsvcAnalysis) { 'true' } else { 'false' }

# MSBuild looks for clang-tidy under VC\Tools\Llvm\bin, which only exists when the 32-bit
# Clang component is installed. The x64 component is the one we require (Get-ClangToolPath),
# so point the analysis at it explicitly.
$clangTidyDirectory = Split-Path (Get-ClangToolPath 'clang-tidy.exe')

$arguments = @(
    (Join-Path $root $target),
    '/m',
    '/nologo',
    '/t:Rebuild',
    "/p:Configuration=$Configuration",
    '/p:Platform=x64',
    '/p:RunCodeAnalysis=true',
    '/p:EnableClangTidyCodeAnalysis=true',
    "/p:ClangTidyToolPath=$clangTidyDirectory",
    "/p:EnableMicrosoftCodeAnalysis=$msvcAnalysis"
)

Write-Host "Analyzing $target ($Configuration|x64)..." -ForegroundColor Cyan
& (Get-MSBuildPath) @arguments
if ($LASTEXITCODE -ne 0)
{
    throw "Analysis failed with exit code $LASTEXITCODE."
}
Write-Host 'Analysis finished. Findings are reported as build warnings.' -ForegroundColor Green
