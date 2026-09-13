<#
.SYNOPSIS
    Verifies that every source file under src/ and tests/ belongs to exactly one project.

.DESCRIPTION
    Source lists in the .vcxproj files are explicit, so a newly added
    .cpp that nobody listed would silently never be compiled. This check runs in CI.
#>
[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Common.ps1')

$root = Get-RepoRoot

# src/Pch.cpp is deliberately compiled by every C++ project, each into its own .pch. The
# signature files are compiled by spl_game and by spl_sigcheck, which must not link the game.
$sharedFiles = @(
    'src\Pch.cpp',
    'src\rage\AddressResolver.cpp',
    'src\rage\GameBuild.cpp',
    'src\rage\signatures\Signatures.cpp'
)

# Which project each .cpp is listed in. A path can appear more than once only if it is shared.
$owners = @{}

foreach ($project in Get-ChildItem (Join-Path $root 'projects') -Filter '*.vcxproj' -Recurse)
{
    $xml = [xml](Get-Content $project.FullName)
    $namespaces = New-Object System.Xml.XmlNamespaceManager($xml.NameTable)
    $namespaces.AddNamespace('ms', 'http://schemas.microsoft.com/developer/msbuild/2003')

    foreach ($item in $xml.SelectNodes('//ms:ClCompile[@Include]', $namespaces))
    {
        $include = $item.GetAttribute('Include') -replace '\$\(SplRoot\)', ''
        if ($include -like 'external\*')
        {
            continue    # third-party sources are listed on purpose
        }

        if (-not $owners.ContainsKey($include))
        {
            $owners[$include] = @()
        }
        $owners[$include] += $project.BaseName
    }
}

$problems = @()

# 1. Every source file on disk must be listed.
foreach ($file in Get-ChildItem -Path (Join-Path $root 'src'), (Join-Path $root 'tests') -Include '*.cpp' -Recurse -File)
{
    $relative = $file.FullName.Substring($root.Length + 1)
    if (-not $owners.ContainsKey($relative))
    {
        $problems += "$relative is in no .vcxproj, so it is never compiled."
    }
}

# 2. No source file may be listed twice, except the shared precompiled-header source.
foreach ($entry in $owners.GetEnumerator())
{
    $path = Join-Path $root $entry.Key
    if (-not (Test-Path $path))
    {
        $problems += "$($entry.Key) is listed in $($entry.Value -join ', ') but does not exist."
        continue
    }
    if ($entry.Value.Count -gt 1 -and $sharedFiles -notcontains $entry.Key)
    {
        $problems += "$($entry.Key) is listed in more than one project: $($entry.Value -join ', ')."
    }
}

if ($problems)
{
    foreach ($problem in $problems)
    {
        Write-Host "ERROR: $problem" -ForegroundColor Red
    }
    throw "$($problems.Count) project/source inconsistency/inconsistencies found."
}

Write-Host "Project files are consistent ($($owners.Count) source files)." -ForegroundColor Green
