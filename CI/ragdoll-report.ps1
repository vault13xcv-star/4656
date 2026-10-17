param(
    [Parameter(Mandatory)][string] $Stage,
    [Parameter(Mandatory)][ValidateSet('running', 'success', 'failure', 'cancelled', 'skipped')][string] $Result
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if (-not $env:GITHUB_SHA -or -not $env:GITHUB_RUN_ID -or -not $env:GITHUB_TOKEN) {
    throw 'This report publisher requires a GitHub Actions job with contents: write.'
}
$source = $env:GITHUB_WORKSPACE
$attempt = $env:GITHUB_RUN_ATTEMPT
$branch = "build-results/ragdoll-$($env:GITHUB_SHA)-$($env:GITHUB_RUN_ID)-$attempt"
$reports = Join-Path $env:RUNNER_TEMP "ragdoll-results-$($env:GITHUB_RUN_ID)-$attempt"
$runUrl = "https://github.com/$($env:GITHUB_REPOSITORY)/actions/runs/$($env:GITHUB_RUN_ID)"

function Invoke-ReportGit([string[]] $Arguments) {
    & git.exe -C $reports @Arguments
    if ($LASTEXITCODE -ne 0) { throw "Git status publication failed ($LASTEXITCODE)." }
}

if (-not (Test-Path (Join-Path $reports '.git'))) {
    New-Item -ItemType Directory -Force $reports | Out-Null
    Invoke-ReportGit @('init', '--initial-branch', $branch)
    Invoke-ReportGit @('config', 'user.name', 'github-actions[bot]')
    Invoke-ReportGit @('config', 'user.email', '41898282+github-actions[bot]@users.noreply.github.com')
    Invoke-ReportGit @('config', 'core.autocrlf', 'false')
    Invoke-ReportGit @('remote', 'add', 'origin', "https://github.com/$($env:GITHUB_REPOSITORY).git")
}
# Credentials live only in the disposable repository's local Git config. Never
# put a token in a remote URL, a report, an artifact, or a logged command.
$auth = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes("x-access-token:$($env:GITHUB_TOKEN)"))
Invoke-ReportGit @('config', 'http.https://github.com/.extraheader', "AUTHORIZATION: basic $auth")

$status = [ordered]@{
    schema = 1
    sourceCommit = $env:GITHUB_SHA
    sourceBranch = $env:GITHUB_REF_NAME
    runId = $env:GITHUB_RUN_ID
    runAttempt = $attempt
    runUrl = $runUrl
    resultsBranch = $branch
    stage = $Stage
    result = $Result
    updatedUtc = [DateTime]::UtcNow.ToString('o')
    releaseUrl = $null
    package = $null
}
$metadataPath = Join-Path $source 'dist/package.json'
if (Test-Path $metadataPath) {
    $metadata = Get-Content $metadataPath -Raw | ConvertFrom-Json
    $status.package = $metadata
    if ($Stage -eq 'completed') {
        $zip = Join-Path $source "dist/$($metadata.file)"
        # GitHub rejects individual files above 100 MiB. Keep some margin; a
        # release asset and an Actions artifact also carry the complete ZIP.
        if ((Test-Path $zip) -and (Get-Item $zip).Length -lt 95MB) {
            Copy-Item $zip $reports -Force
            $status.package | Add-Member -NotePropertyName gitDownload -NotePropertyValue "https://github.com/$($env:GITHUB_REPOSITORY)/raw/refs/heads/$branch/$($metadata.file)" -Force
        }
    }
}
$releasePath = Join-Path $source 'dist/release-url.txt'
if (Test-Path $releasePath) { $status.releaseUrl = (Get-Content $releasePath -Raw).Trim() }
$status | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $reports 'status.json') -Encoding utf8
@"
# Windows x64 ragdoll build

Status: **$Result**, phase: **$Stage**.

Source commit: ``$($env:GITHUB_SHA)``.

[Build logs]($runUrl)

This branch contains generated build reports. It does not replace the source branch.
Read status.json for package checksums, a successful release URL, and an optional Git download.
"@ | Set-Content (Join-Path $reports 'README.md') -Encoding utf8

$logDir = Join-Path $source 'ragdoll-logs'
if (Test-Path $logDir) {
    $reportLogs = Join-Path $reports 'logs'
    New-Item -ItemType Directory -Force $reportLogs | Out-Null
    foreach ($log in Get-ChildItem $logDir -File) {
        # Output only bounded compiler/test diagnostics, not environment dumps.
        Get-Content $log.FullName -Tail 180 | Set-Content (Join-Path $reportLogs $log.Name) -Encoding utf8
    }
}
Invoke-ReportGit @('add', '--', 'status.json', 'README.md')
if (Test-Path (Join-Path $reports 'logs')) { Invoke-ReportGit @('add', '--', 'logs') }
foreach ($zip in Get-ChildItem $reports -Filter '*.zip' -File) { Invoke-ReportGit @('add', '--', $zip.Name) }
Invoke-ReportGit @('commit', '--allow-empty', '-m', "Build $Stage`: $Result")
# Each run/attempt owns a new branch. Updates are ordinary fast-forward pushes.
Invoke-ReportGit @('push', 'origin', "HEAD:refs/heads/$branch")
Write-Output "Build status: $branch ($Result / $Stage)"
