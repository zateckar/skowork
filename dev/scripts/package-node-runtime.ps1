param([Parameter(Mandatory)][string] $OutDir, [Parameter(Mandatory)][string] $Id)
$ErrorActionPreference = 'Stop'
if ($Id -notmatch '^[a-f0-9]{20}$') { throw 'Invalid runtime identity' }
$root = Resolve-Path (Join-Path $PSScriptRoot '../..')
& (Join-Path $PSScriptRoot 'fetch-node-runtime.ps1') -VerifyOnly
$name = "Skowork-app-runtime-$Id-win-x64"
$stage = Join-Path $OutDir $name
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
$dest = Join-Path $stage 'runtime/node'
New-Item -ItemType Directory -Force $dest | Out-Null
Copy-Item (Join-Path $root 'client/runtime/node/node.exe'), (Join-Path $root 'client/runtime/node/LICENSE'), (Join-Path $root 'client/src/apps/app-host.js') $dest
$zip = Join-Path $OutDir "$name.zip"
Remove-Item $zip -Force -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -CompressionLevel Optimal
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLowerInvariant()
"$hash  $name.zip" | Set-Content "$zip.sha256" -NoNewline -Encoding ascii
