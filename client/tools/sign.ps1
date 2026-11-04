# Authenticode-signs the shipped executables with the organization code-signing certificate (add-client-foundation
# design D9). Used by the release workflow and by the dev build published from CI; the certificate comes from the
# environment and is removed again from disk.
#   SIGNING_CERT      base64 of the PFX
#   SIGNING_PASSWORD  its password
param(
    [Parameter(Mandatory)] [string] $BuildDir
)
$ErrorActionPreference = 'Stop'

if (-not $env:SIGNING_CERT) { throw 'SIGNING_CERT is not set' }
$pfx = Join-Path $env:RUNNER_TEMP 'sign.pfx'
try {
    [IO.File]::WriteAllBytes($pfx, [Convert]::FromBase64String($env:SIGNING_CERT))
    $signtool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\signtool.exe" | Sort-Object FullName | Select-Object -Last 1
    if (-not $signtool) { throw 'signtool.exe not found' }
    & $signtool.FullName sign /fd SHA256 /f $pfx /p $env:SIGNING_PASSWORD /tr http://timestamp.digicert.com /td SHA256 `
        (Join-Path $BuildDir 'bin/Workbench.exe') (Join-Path $BuildDir 'bin/workbench-worker.exe')
    if ($LASTEXITCODE -ne 0) { throw "signtool failed ($LASTEXITCODE)" }
} finally {
    Remove-Item $pfx -ErrorAction SilentlyContinue
}
