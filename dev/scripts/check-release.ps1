param([Parameter(Mandatory)][string] $Repository, [Parameter(Mandatory)][string] $Tag, [Parameter(Mandatory)][string] $Asset)
$ErrorActionPreference = 'Stop'
# gh returns a non-zero status for 404. Inspect the HTTP status and clear that status before returning.
$response = @(gh api --include "repos/$Repository/releases/tags/$Tag" 2>&1 | ForEach-Object { "$_" })
$exitCode = $LASTEXITCODE
$global:LASTEXITCODE = 0
$status = ($response | Where-Object { $_ -match '^HTTP/\S+\s+(\d{3})' } | Select-Object -Last 1)
if ($status -notmatch '^HTTP/\S+\s+(\d{3})') { throw "Release lookup failed without HTTP status: $($response -join ' ')" }
$code = [int]$Matches[1]
if ($code -eq 404) { return 'false' }
if ($exitCode -ne 0 -or $code -ne 200) { throw "Release lookup failed: $($response -join ' ')" }
$json = ($response | Where-Object { $_ -match '^\{' } | Select-Object -Last 1) | ConvertFrom-Json
$names = @($json.assets | ForEach-Object { $_.name })
if ($Asset -notin $names -or "$Asset.sha256" -notin $names) { throw "Release $Tag is missing expected assets" }
return 'true'
