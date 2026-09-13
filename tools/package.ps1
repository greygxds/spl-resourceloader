<#
.SYNOPSIS
    Builds Release, runs the tests and produces the release zip and its SHA-256 file.

.DESCRIPTION
    This is what CI's release packaging runs, and what a maintainer runs locally.
    Output goes to dist\package\:

        resourceLoader-<version>.zip          the install package
        resourceLoader-<version>.zip.sha256

    The version is taken from src\core\BuildVersion.h when CI (or build.ps1 -Stamp) wrote one,
    and is "local" otherwise.

.EXAMPLE
    tools\package.ps1
    tools\package.ps1 -SkipBuild -SkipTests     # package what dist\Release already holds (CI)
#>
[CmdletBinding()]
param(
    # Use the binaries already in dist\Release.
    [switch] $SkipBuild,

    # Do not run spl_tests.exe.
    [switch] $SkipTests,

    # Overrides the version read from src\core\BuildVersion.h.
    [string] $Version
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Common.ps1')

$root = Get-RepoRoot
$releaseDir = Join-Path $root 'dist\Release'
$packageRoot = Join-Path $root 'dist\package'

if (-not $Version)
{
    $versionHeader = Join-Path $root 'src\core\BuildVersion.h'
    $Version = 'local'
    if (Test-Path $versionHeader)
    {
        $match = Select-String -Path $versionHeader -Pattern 'SPL_VERSION_TEXT "Build ([^"]+)"'
        if ($match)
        {
            $Version = $match.Matches[0].Groups[1].Value
        }
    }
}

if (-not $SkipBuild)
{
    & (Join-Path $PSScriptRoot 'build.ps1') -Configuration Release -WarningsAsErrors
}

$asi = Join-Path $releaseDir 'resourceLoader.asi'
if (-not (Test-Path $asi))
{
    throw "$asi does not exist. Build Release first, or drop -SkipBuild."
}

if (-not $SkipTests)
{
    $tests = Join-Path $releaseDir 'spl_tests.exe'
    Write-Host "Running $tests..." -ForegroundColor Cyan
    & $tests
    if ($LASTEXITCODE -ne 0)
    {
        throw "Tests failed with exit code $LASTEXITCODE."
    }
}

$name = "resourceLoader-$Version"
$stage = Join-Path $packageRoot $name
if (Test-Path $packageRoot)
{
    Remove-Item -Recurse -Force $packageRoot
}
New-Item -ItemType Directory -Force (Join-Path $stage 'resourceLoader\resources') | Out-Null
New-Item -ItemType Directory -Force (Join-Path $stage 'resourceLoader\mods') | Out-Null

# The layout users unpack into the GTA V folder.
Copy-Item $asi $stage
Copy-Item (Join-Path $root 'README.md') (Join-Path $stage 'README.md')

# config.toml is the exact text the loader writes on a first launch, taken from DefaultConfig.cpp
# so the two cannot drift.
$defaultConfigSource = Get-Content -Raw (Join-Path $root 'src\config\DefaultConfig.cpp')
$configMatch = [regex]::Match($defaultConfigSource, 'R"\((.*?)\)";', 'Singleline')
if (-not $configMatch.Success)
{
    throw 'The default config raw string was not found in src\config\DefaultConfig.cpp.'
}
$configMatch.Groups[1].Value -replace "`r`n", "`n" |
    Set-Content -NoNewline -Encoding utf8NoBOM (Join-Path $stage 'resourceLoader\config.toml')

# A placeholder log. The loader truncates resourceLoader.log on every start, so a user who sends
# this text back has not run the game with the loader installed.
@'
This is an example log. If you see it, you either:

* did not actually launch the game after installing the mod
* installed the mod incorrectly.

Read installation instructions again.
'@ -replace "`r?`n", "`r`n" | Set-Content -Encoding utf8NoBOM (Join-Path $stage 'resourceLoader\resourceLoader.log')

if (Test-Path (Join-Path $root 'LICENSE'))
{
    Copy-Item (Join-Path $root 'LICENSE') (Join-Path $stage 'LICENSE.txt')
}
else
{
    Write-Warning 'There is no LICENSE file in the repository root, so the package ships without one.'
}

# THIRD_PARTY_LICENSES.txt, from the license files of what the .asi actually links. Catch2 is
# test-only and not shipped.
function Get-LuaLicense
{
    $header = Get-Content (Join-Path $root 'external\lua\lua.h')
    $start = ($header | Select-String -Pattern '^\* Copyright \(C\)' | Select-Object -Last 1).LineNumber
    if (-not $start)
    {
        throw 'The Lua copyright notice was not found at the end of external\lua\lua.h.'
    }
    $lines = @()
    foreach ($line in $header[($start - 1)..($header.Count - 1)])
    {
        if ($line -match '^\*{10,}/$') { break }
        $lines += ($line -replace '^\* ?', '')
    }
    return $lines -join "`r`n"
}

$notices = [ordered]@{
    'spdlog (https://github.com/gabime/spdlog)'            = Get-Content -Raw (Join-Path $root 'external\spdlog\LICENSE')
    'fmt, bundled with spdlog (https://github.com/fmtlib/fmt)' = Get-Content -Raw (Join-Path $root 'external\spdlog\include\spdlog\fmt\bundled\fmt.license.rst')
    'toml++ (https://github.com/marzer/tomlplusplus)'      = Get-Content -Raw (Join-Path $root 'external\tomlplusplus\LICENSE')
    'MinHook (https://github.com/TsudaKageyu/minhook)'     = Get-Content -Raw (Join-Path $root 'external\minhook\LICENSE.txt')
    'Lua (https://www.lua.org)'                            = Get-LuaLicense
    'miniz (https://github.com/richgel999/miniz)'          = Get-Content -Raw (Join-Path $root 'external\miniz\LICENSE')
    'ScriptHookV SDK (http://dev-c.com/gtav/scripthookv/)' = @'
resourceLoader.asi is built against the ScriptHookV SDK by Alexander Blade and needs
ScriptHookV.dll at runtime. Neither the SDK nor ScriptHookV itself is part of this package;
download ScriptHookV from http://dev-c.com/gtav/scripthookv/ under its author's terms.
'@
}
$text = @('Third-party software used by resourceLoader.asi', '')
foreach ($entry in $notices.GetEnumerator())
{
    $text += ('=' * 78)
    $text += $entry.Key
    $text += ('=' * 78)
    $text += ''
    $text += $entry.Value.TrimEnd()
    $text += ''
}
$text -join "`r`n" | Set-Content -Encoding utf8NoBOM (Join-Path $stage 'THIRD_PARTY_LICENSES.txt')

function New-ZipWithHash
{
    param([string] $Source, [string] $Destination)

    Compress-Archive -Path $Source -DestinationPath $Destination -CompressionLevel Optimal
    $hash = (Get-FileHash -Algorithm SHA256 $Destination).Hash.ToLowerInvariant()
    "$hash  $(Split-Path -Leaf $Destination)" | Set-Content -Encoding ascii "$Destination.sha256"
    Write-Host "$Destination ($hash)" -ForegroundColor Green
}

New-ZipWithHash -Source (Join-Path $stage '*') -Destination (Join-Path $packageRoot "$name.zip")
