# Fetches the app runtime package (add-local-apps design D2) into client/runtime/node: node.exe and its LICENSE from the
# official Node.js 26 LTS win-x64 zip, pinned by version and SHA-256 (from the release's SHASUMS256.txt). npm, npx and
# corepack are left out: Local apps use built-in modules only. Downloads with Invoke-WebRequest, because on the
# workstation only signed executables reach the internet. -VerifyOnly checks an existing client/runtime/node without
# downloading; CI runs it after restoring the runtime from its cache.
#   pwsh dev/scripts/fetch-node-runtime.ps1
#   pwsh dev/scripts/fetch-node-runtime.ps1 -VerifyOnly
param([switch] $VerifyOnly)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$version = 'v26.13.0'
$sha = 'DCC60D59A0746999F99064A01C020445D54D84228C691CDBF25B49AC3B59150A'
$exeSha = '14319838B3F355E0398C13569A90270BCE4FC68F25189F58C59B03DADAE0E6D5'
$name = "node-$version-win-x64"
$client = Resolve-Path (Join-Path $PSScriptRoot '../../client')
$runtime = Join-Path $client 'runtime/node'

function Test-Runtime {
    if ((Get-Content (Join-Path $runtime 'VERSION') -Raw) -ne $version) { throw "client/runtime/node is not $version" }
    $exe = Join-Path $runtime 'node.exe'
    $actual = (Get-FileHash $exe -Algorithm SHA256).Hash
    if ($actual -ne $exeSha) { throw "node.exe has SHA-256 $actual, expected $exeSha" }
    $sig = Get-AuthenticodeSignature $exe
    if ($sig.Status -ne 'Valid') { throw "node.exe signature is $($sig.Status)" }
    return $sig
}

if ($VerifyOnly) {
    $sig = Test-Runtime
    Write-Host "client/runtime/node verified: $version, signed by $($sig.SignerCertificate.Subject)"
    return
}

$work = Join-Path ([IO.Path]::GetTempPath()) ("skowork-node-" + [Guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $work, $runtime | Out-Null
try {
    $zip = Join-Path $work "$name.zip"
    Write-Host "downloading $name.zip"
    Invoke-WebRequest "https://nodejs.org/dist/$version/$name.zip" -OutFile $zip
    $h = (Get-FileHash $zip -Algorithm SHA256).Hash
    if ($h -ne $sha) { throw "$name.zip has SHA-256 $h, expected $sha" }
    Expand-Archive $zip -DestinationPath $work
    foreach ($f in 'node.exe', 'LICENSE') { Copy-Item (Join-Path $work "$name/$f") (Join-Path $runtime $f) -Force }
    Set-Content (Join-Path $runtime 'VERSION') $version -NoNewline
    $sig = Test-Runtime
    $mb = (Get-ChildItem $runtime -File | Where-Object Name -ne '.gitignore' | Measure-Object Length -Sum).Sum / 1MB
    Write-Host ("client/runtime/node ready: {0} {1:N1} MB, signed by {2}" -f $version, $mb, $sig.SignerCertificate.Subject)
} finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
