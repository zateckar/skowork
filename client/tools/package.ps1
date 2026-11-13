<#
.SYNOPSIS
  Packages a client build as the desktop zip: Skowork.exe, the compatibility launcher, skowork-worker.exe, pdfium.dll and the third-party
  licences, plus a SHA-256 checksum file (add-client-foundation task 2.4). When the build holds the app runtime, also
  the optional app runtime package (add-local-apps) with its own checksum file.
#>
param(
    [Parameter(Mandatory)] [string] $BuildDir,
    [Parameter(Mandatory)] [string] $OutDir,
    [Parameter(Mandatory)] [string] $Version
)
$ErrorActionPreference = 'Stop'
$client = Resolve-Path (Join-Path $PSScriptRoot '..')
$bin = Join-Path $BuildDir 'bin'
$name = "Skowork-$Version-win-x64"
$stage = Join-Path $OutDir $name
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $stage, (Join-Path $stage 'licenses') | Out-Null

foreach ($f in 'Skowork.exe', 'Skowork-legacy.exe', 'skowork-worker.exe', 'pdfium.dll') {
    $src = Join-Path $bin $f
    if (-not (Test-Path $src)) { throw "missing $src" }
    Copy-Item $src $stage
}
# Licences of the vendored components (client/third_party/README.md is the manifest).
Copy-Item (Join-Path $client 'third_party\README.md') (Join-Path $stage 'licenses\THIRD-PARTY.md')
foreach ($dir in Get-ChildItem (Join-Path $client 'third_party') -Directory) {
    foreach ($lic in Get-ChildItem $dir.FullName -File -Recurse -Depth 1 -Filter 'LICENSE*') {
        Copy-Item $lic.FullName (Join-Path $stage "licenses\$($dir.Name)-$($lic.Name)") -Force
    }
}
# PDFium bundles further libraries, each with its own licence.
foreach ($lic in Get-ChildItem (Join-Path $client 'third_party\pdfium\licenses') -File) {
    Copy-Item $lic.FullName (Join-Path $stage "licenses\pdfium-$($lic.Name)") -Force
}

function Write-Zip([string] $From, [string] $ZipName) {
    $zip = Join-Path $OutDir "$ZipName.zip"
    Remove-Item $zip -Force -ErrorAction SilentlyContinue
    Compress-Archive -Path (Join-Path $From '*') -DestinationPath $zip -CompressionLevel Optimal
    $hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $ZipName.zip" | Set-Content (Join-Path $OutDir "$ZipName.zip.sha256") -NoNewline -Encoding ascii
    Write-Host "Packaged $zip ($([math]::Round((Get-Item $zip).Length / 1MB, 2)) MB), sha256 $hash"
}
Write-Zip $stage $name

# The optional app runtime package (add-local-apps D2): runtime\node with node.exe, its LICENSE and app-host.js, to be
# extracted next to Skowork.exe. Built only when the build laid out node.exe (dev/scripts/fetch-node-runtime.ps1).
$runtime = Join-Path $bin 'runtime\node'
if (Test-Path (Join-Path $runtime 'node.exe')) {
    $rtName = "Skowork-$Version-app-runtime-win-x64"
    $rtStage = Join-Path $OutDir $rtName
    Remove-Item $rtStage -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force (Join-Path $rtStage 'runtime\node') | Out-Null
    foreach ($f in 'node.exe', 'LICENSE', 'app-host.js') {
        $src = Join-Path $runtime $f
        if (-not (Test-Path $src)) { throw "missing $src" }
        Copy-Item $src (Join-Path $rtStage 'runtime\node')
    }
    Write-Zip $rtStage $rtName
} else {
    Write-Host "No app runtime in ${runtime}: the app runtime package is not built"
}
