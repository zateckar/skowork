<#
.SYNOPSIS
  Starts (or stops) the local Workbench control plane from the repository for development and the client end-to-end
  tests. It uses the organization Keycloak test realm and the real organization LLM API from the repository's .env. Nothing is
  installed outside the repository: dependencies go to control-plane/node_modules, state to dev/.stack.

.EXAMPLE
  ./dev/scripts/dev-stack.ps1                       # control plane on :8080
  ./dev/scripts/dev-stack.ps1 -CpPort 8081 -PublicBaseUrl http://localhost:8080   # behind the e2e test proxy
  ./dev/scripts/dev-stack.ps1 -Stop

.NOTES
  -BootstrapAdmins names the user names that hold platform-admin (default: the current Windows user name).
  -TestIssuerJwksFile lets automated tests add a locally signed test issuer (never in production).
#>
param(
    [int] $CpPort = 8080,
    [string] $PublicBaseUrl = '',
    [string] $BootstrapAdmins = $env:USERNAME,
    [string] $TestIssuer = 'https://e2e.workbench.test/realms/e2e',
    [string] $TestIssuerJwksFile = '',
    [switch] $Fresh,
    [switch] $Stop
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$cp = Join-Path $repo 'control-plane'
$state = Join-Path $repo 'dev\.stack'
$pidFile = Join-Path $state 'cp.pid'

function Stop-Stack {
    if (Test-Path $pidFile) {
        Stop-Process -Id ([int](Get-Content $pidFile)) -Force -ErrorAction SilentlyContinue
        Remove-Item $pidFile -Force
    }
}

if ($Stop) { Stop-Stack; return }

New-Item -ItemType Directory -Force $state | Out-Null
Stop-Stack
if ($Fresh) { Remove-Item (Join-Path $state 'workbench.db*') -Force -ErrorAction SilentlyContinue }
if (-not (Test-Path (Join-Path $cp 'node_modules'))) { Push-Location $cp; npm ci --no-audit --no-fund | Out-Null; Pop-Location }
if (-not (Test-Path (Join-Path $repo '.env'))) { throw 'The repository .env (OIDC_* and OPENAI_* settings) is missing.' }

$env:WORKBENCH_ENV = 'development'
$env:PORT = "$CpPort"
$env:HOST = '127.0.0.1,::1'   # loopback only, both families: "localhost" resolves to ::1 first on Windows
$env:WORKBENCH_DB = Join-Path $state 'workbench.db'
$env:WORKBENCH_PUBLIC_BASE_URL = if ($PublicBaseUrl) { $PublicBaseUrl } else { "http://localhost:$CpPort" }
$env:WORKBENCH_BOOTSTRAP_ADMINS = $BootstrapAdmins
# The mock MCP servers of the end-to-end tests (dev/mcp-test-server) listen on plain-http loopback.
$env:WORKBENCH_MCP_ALLOW_INSECURE_HTTP = '1'
if ($TestIssuerJwksFile) {
    $env:WORKBENCH_TEST_ISSUER = $TestIssuer
    $env:WORKBENCH_TEST_ISSUER_JWKS = Get-Content $TestIssuerJwksFile -Raw
}
$node = (Get-Command node -CommandType Application | Select-Object -First 1).Source
$proc = Start-Process $node -ArgumentList '--env-file=../.env', 'src/main.ts' -WorkingDirectory $cp -PassThru -WindowStyle Hidden `
    -RedirectStandardOutput (Join-Path $state 'cp.log') -RedirectStandardError (Join-Path $state 'cp.err')
$proc.Id | Set-Content $pidFile

$deadline = (Get-Date).AddSeconds(30)
while ($true) {
    try { if ((Invoke-WebRequest "http://localhost:$CpPort/readyz" -TimeoutSec 2).StatusCode -eq 200) { break } } catch { }
    $alive = [bool](Get-Process -Id $proc.Id -ErrorAction SilentlyContinue)
    if (-not $alive -or (Get-Date) -gt $deadline) { Get-Content (Join-Path $state 'cp.err') -Tail 20; throw 'The control plane did not start.' }
    Start-Sleep -Milliseconds 300
}
Write-Host "Workbench control plane is up on http://localhost:$CpPort (logs in $state)"
