<#
.SYNOPSIS
    Builds the solution with MSBuild.

.EXAMPLE
    tools\build.ps1 -Configuration Release
    tools\build.ps1 -Core              # only the game-independent projects
    tools\build.ps1 -Stamp             # stamp src/core/BuildVersion.h from the local git state
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Dev', 'Release')]
    [string] $Configuration = 'Dev',

    # Build SpResourceLoader.Core.slnf: the projects that do not touch the game.
    [switch] $Core,

    # Rebuild from scratch.
    [switch] $Rebuild,

    # Write src/core/BuildVersion.h from the local git state, for a build handed to someone else.
    [switch] $Stamp,

    # Delete dist/ and the stamped src/core/BuildVersion.h, then exit.
    [switch] $Clean,

    # Fail the build on any warning, the way CI does.
    [switch] $WarningsAsErrors
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Common.ps1')

$root = Get-RepoRoot
$versionHeader = Join-Path $root 'src\core\BuildVersion.h'

if ($Clean)
{
    foreach ($path in @((Join-Path $root 'dist'), $versionHeader))
    {
        if (Test-Path $path)
        {
            Remove-Item -Recurse -Force $path
            Write-Host "Removed $path"
        }
    }
    exit 0
}

if ($Stamp)
{
    $sha = (& git -C $root rev-parse --short HEAD).Trim()
    $date = (Get-Date).ToUniversalTime().ToString('yyyy.MM.dd')
    # Local builds are not CI builds, so they keep build number 0 and a "local-" prefix.
    @(
        '#pragma once',
        '#define SPL_BUILD_NUMBER 0',
        "#define SPL_VERSION_TEXT `"Build local-${date}_${sha}`"",
        "#define SPL_COMMIT_SHA `"$sha`""
    ) | Set-Content -Path $versionHeader -Encoding UTF8
    Write-Host "Stamped $versionHeader (local-${date}_${sha})" -ForegroundColor Green
}

Initialize-Submodules

$target = if ($Core) { 'SpResourceLoader.Core.slnf' } else { 'SpResourceLoader.sln' }
$msBuildTarget = if ($Rebuild) { 'Rebuild' } else { 'Build' }

$arguments = @(
    (Join-Path $root $target),
    '/m',
    '/nologo',
    "/t:$msBuildTarget",
    "/p:Configuration=$Configuration",
    '/p:Platform=x64'
)
if ($WarningsAsErrors)
{
    $arguments += '/p:SplWarningsAsErrors=true'
}

Write-Host "Building $target ($Configuration|x64)..." -ForegroundColor Cyan
& (Get-MSBuildPath) @arguments
if ($LASTEXITCODE -ne 0)
{
    throw "Build failed with exit code $LASTEXITCODE."
}

Write-Host "Output: $(Join-Path $root "dist\$Configuration")" -ForegroundColor Green
