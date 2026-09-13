# Shared helpers for the tools/*.ps1 scripts.
# Every script locates Visual Studio through vswhere, so no Developer Prompt is needed.

Set-StrictMode -Version Latest

function Get-RepoRoot
{
    return (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
}

function Get-VsWherePath
{
    $vsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vsWhere))
    {
        throw "vswhere.exe not found at '$vsWhere'. Install Visual Studio 2022 (or Build Tools 2022) with the 'Desktop development with C++' workload."
    }
    return $vsWhere
}

function Get-VsInstallPath
{
    $path = & (Get-VsWherePath) -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $path)
    {
        throw "No Visual Studio installation with the C++ toolset was found. Install the 'Desktop development with C++' workload."
    }
    return $path
}

function Get-MSBuildPath
{
    $msBuild = & (Get-VsWherePath) -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
    if (-not $msBuild)
    {
        throw 'MSBuild.exe was not found. Install Visual Studio 2022 with the "Desktop development with C++" workload.'
    }
    return $msBuild
}

function Get-ClangToolPath
{
    param([Parameter(Mandatory)][string] $Name)   # 'clang-format.exe'

    $tool = Join-Path (Get-VsInstallPath) "VC\Tools\Llvm\x64\bin\$Name"
    if (-not (Test-Path $tool))
    {
        throw "$Name was not found at '$tool'. Install the Visual Studio component 'C++ Clang tools for Windows'."
    }
    return $tool
}

# Makes sure the pinned submodules are checked out. A fresh clone without
# --recurse-submodules would otherwise fail deep inside the build.
function Initialize-Submodules
{
    $root = Get-RepoRoot
    $markers = @{
        'external/spdlog'       = 'include/spdlog/spdlog.h'
        'external/tomlplusplus' = 'include/toml++/toml.hpp'
        'external/minhook'      = 'include/MinHook.h'
        'external/Catch2'       = 'extras/catch_amalgamated.cpp'
    }

    foreach ($entry in $markers.GetEnumerator())
    {
        if (Test-Path (Join-Path $root "$($entry.Key)/$($entry.Value)"))
        {
            continue
        }

        Write-Host "Checking out submodules (missing: $($entry.Key))..." -ForegroundColor Yellow
        & git -C $root submodule update --init --recursive
        if ($LASTEXITCODE -ne 0)
        {
            throw 'git submodule update failed.'
        }
        return
    }
}

# Source files the formatter cares about: our code only, never external/.
function Get-SourceFiles
{
    $root = Get-RepoRoot
    return Get-ChildItem -Path (Join-Path $root 'src'), (Join-Path $root 'tests') `
        -Include '*.h', '*.cpp' -Recurse -File | Sort-Object FullName
}
