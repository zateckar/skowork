# Freezes verified product speech inputs plus the existing development recordings.
param([Parameter(Mandatory)] [string] $OutDir, [ValidatePattern('^[A-Za-z0-9_-]+$')] [string] $Version = '1')
$ErrorActionPreference = 'Stop'
$client = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../../client')).Path
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$out = (Resolve-Path -LiteralPath $OutDir).Path
$archive = Join-Path $out "Skowork-speech-build-assets-v$Version-win-x64.zip"
if (Test-Path -LiteralPath $archive) { throw "Build bundle already exists: $archive" }
$work = Join-Path $out ('.speech-inputs-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
try {
    & (Join-Path $client 'tools/package-speech.ps1') -OutDir $work
    $product = @(Get-ChildItem -LiteralPath $work -Filter '*.zip' -File)
    if ($product.Count -ne 1) { throw 'Expected one verified product speech package' }
    $manifest = Get-Content -LiteralPath (Join-Path $client 'tools/speech-manifest.json') -Raw | ConvertFrom-Json
    $checks = @($manifest.files | ForEach-Object { @{ Path = [string]$_.path; Hash = [string]$_.sha256; Bytes = [long]$_.bytes } })
    $fixtures = @('parakeet-en.wav', 'parakeet-de.wav', 'parakeet-es.wav', 'parakeet-fr.wav', 'whisper-0.wav', 'whisper-1.wav')
    $zip = [IO.Compression.ZipFile]::Open($product[0].FullName, [IO.Compression.ZipArchiveMode]::Update)
    try {
        foreach ($name in $fixtures) {
            $source = Join-Path $client "models/stt/fixtures/$name"
            $hash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
            $bytes = (Get-Item -LiteralPath $source).Length
            if ($bytes -le 0) { throw "Empty development recording: $name" }
            $checks += @{ Path = "fixtures/$name"; Hash = $hash; Bytes = $bytes }
            $entry = $zip.CreateEntry("fixtures/$name", [IO.Compression.CompressionLevel]::NoCompression)
            $entry.LastWriteTime = [DateTimeOffset]::Parse('2020-01-01T00:00:00Z')
            $inputStream = [IO.File]::OpenRead($source)
            $outputStream = $entry.Open()
            try { $inputStream.CopyTo($outputStream) } finally { $outputStream.Dispose(); $inputStream.Dispose() }
        }
    } finally { $zip.Dispose() }
    $zip = [IO.Compression.ZipFile]::OpenRead($product[0].FullName)
    try {
        foreach ($check in $checks) {
            $entry = $zip.GetEntry($check.Path)
            if (!$entry -or $entry.Length -ne $check.Bytes) { throw "Invalid bundled input: $($check.Path)" }
            $stream = $entry.Open()
            $sha = [Security.Cryptography.SHA256]::Create()
            try { $hash = [Convert]::ToHexString($sha.ComputeHash($stream)) } finally { $sha.Dispose(); $stream.Dispose() }
            if ($hash -ne $check.Hash) { throw "Invalid bundled SHA-256: $($check.Path)" }
        }
    } finally { $zip.Dispose() }
    [IO.File]::Move($product[0].FullName, $archive)
    $digest = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    "$digest  $([IO.Path]::GetFileName($archive))" | Set-Content -LiteralPath "$archive.sha256" -NoNewline -Encoding ascii
    Write-Host "Build bundle: $archive ($((Get-Item -LiteralPath $archive).Length) bytes), sha256 $digest"
} finally {
    $resolvedWork = [IO.Path]::GetFullPath($work)
    if ([IO.Path]::GetDirectoryName($resolvedWork) -ne $out -or
        [IO.Path]::GetFileName($resolvedWork) -notmatch '^\.speech-inputs-[a-f0-9]{32}$') { throw 'Unsafe bundle cleanup path' }
    Remove-Item -LiteralPath $resolvedWork -Recurse -Force
}
