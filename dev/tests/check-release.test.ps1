$ErrorActionPreference = 'Stop'
$global:releaseTestMode = 'missing'
function gh {
    if ($global:releaseTestMode -eq 'missing') { $global:LASTEXITCODE = 1; return 'HTTP/2.0 404 Not Found', '', '{"message":"Not Found"}' }
    if ($global:releaseTestMode -eq 'error') { $global:LASTEXITCODE = 1; return 'HTTP/2.0 403 Forbidden', '', '{"message":"Forbidden"}' }
    $global:LASTEXITCODE = 0
    return 'HTTP/2.0 200 OK', '', '{"assets":[{"name":"pkg.zip"},{"name":"pkg.zip.sha256"}]}'
}
$script = Join-Path $PSScriptRoot '../scripts/check-release.ps1'
if ((& $script -Repository 'owner/repo' -Tag 'pkg' -Asset 'pkg.zip') -ne 'false') { throw 'Missing release must return false' }
$global:releaseTestMode = 'present'
if ((& $script -Repository 'owner/repo' -Tag 'pkg' -Asset 'pkg.zip') -ne 'true') { throw 'Complete release must return true' }
try { & $script -Repository 'owner/repo' -Tag 'pkg' -Asset 'other.zip'; throw 'Incomplete release accepted' } catch { if ($_.Exception.Message -eq 'Incomplete release accepted') { throw } }
$global:releaseTestMode = 'error'
try { & $script -Repository 'owner/repo' -Tag 'pkg' -Asset 'pkg.zip'; throw 'API failure accepted' } catch { if ($_.Exception.Message -eq 'API failure accepted') { throw } }
Write-Host 'Release lookup checks passed'
