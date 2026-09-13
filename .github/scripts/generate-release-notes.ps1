$ErrorActionPreference = 'Stop'

$repository = $env:GITHUB_REPOSITORY
$currentSha = $env:GITHUB_SHA
$releaseTag = $env:RELEASE_TAG

if (-not $repository -or -not $currentSha -or -not $releaseTag) {
    throw 'GITHUB_REPOSITORY, GITHUB_SHA, and RELEASE_TAG are required'
}

$previousTag = @(
    git tag --merged $currentSha --list 'build-*' |
        ForEach-Object {
            if ($_ -match '^build-(\d{4}\.\d{2}\.\d{2})_(\d+)$' -and $_ -ne $releaseTag) {
                [pscustomobject]@{
                    Name = $_
                    Date = $matches[1]
                    Number = [int]$matches[2]
                }
            }
            elseif ($_ -match '^build-(\d+)$' -and $_ -ne $releaseTag) {
                [pscustomobject]@{
                    Name = $_
                    Date = '0000.00.00'
                    Number = [int]$matches[1]
                }
            }
        } |
        Sort-Object Date, Number |
        Select-Object -Last 1
).Name

$range = if ($previousTag) { "$previousTag..$currentSha" } else { $currentSha }
$heading = if ($previousTag) {
    "Changes since [$previousTag](https://github.com/$repository/releases/tag/$previousTag)"
} else {
    'Changes since the beginning of the repository'
}

$categories = [ordered]@{
    'Features' = @()
    'Bug fixes' = @()
    'Improvements' = @()
    'Documentation' = @()
    'Maintenance' = @()
    'Other changes' = @()
}

foreach ($commit in @(git log --no-merges --no-decorate --format="%H%x09%s" $range)) {
    $parts = $commit -split "`t", 2
    if ($parts.Count -ne 2) {
        continue
    }

    $sha = $parts[0]
    $subject = $parts[1]
    $category = 'Other changes'

    if ($subject -match '^(?<type>[A-Za-z]+)(\([^)]*\))?!?:\s*(?<summary>.+)$') {
        $subject = $matches.summary
        switch ($matches.type.ToLowerInvariant()) {
            { $_ -in @('feat', 'feature') } { $category = 'Features'; break }
            { $_ -in @('fix', 'bugfix') } { $category = 'Bug fixes'; break }
            { $_ -in @('refactor', 'perf', 'improvement') } { $category = 'Improvements'; break }
            'docs' { $category = 'Documentation'; break }
            { $_ -in @('chore', 'build', 'ci', 'test', 'style', 'revert') } { $category = 'Maintenance'; break }
        }
    }

    $shortSha = $sha.Substring(0, 7)
    $link = ('[`{0}`](https://github.com/{1}/commit/{2})' -f $shortSha, $repository, $sha)
    $categories[$category] += "- $link $subject"
}

$notes = [System.Collections.Generic.List[string]]::new()
$notes.Add("## $heading")
$notes.Add('')

foreach ($category in $categories.Keys) {
    if ($categories[$category].Count -eq 0) {
        continue
    }

    $notes.Add("### $category")
    $notes.AddRange([string[]]$categories[$category])
    $notes.Add('')
}

$notes | Set-Content -Encoding utf8 release-notes.md
