# Fetches the exact versioned speech build bundle; model hashes remain in speech-manifest.json.
# -ArchivePath imports an offline copy with the same pinned size and SHA-256 checks.
param([switch] $VerifyOnly, [string] $ArchivePath)
$ErrorActionPreference = 'Stop'
$client = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../../client')).Path
$models = Join-Path $client 'models/stt'
$manifest = Get-Content -LiteralPath (Join-Path $client 'tools/speech-manifest.json') -Raw | ConvertFrom-Json
if ($manifest.schema -ne 1 -or $manifest.package -ne 'skowork-speech' -or $manifest.architecture -ne 'windows-x64' -or
    $manifest.files.Count -ne 13) { throw 'Unsupported speech manifest' }
$expected = @{}
foreach ($file in $manifest.files) {
    $path = [string]$file.path
    if ($path -notmatch '^models/stt/[A-Za-z0-9_.-]+(/[A-Za-z0-9_.-]+)*$' -or ($path -split '/') -contains '..' -or
        $expected.ContainsKey($path) -or $file.sha256 -notmatch '^[a-fA-F0-9]{64}$' -or [long]$file.bytes -le 0 -or
        $file.source -notin @('models', 'runtime')) { throw "Invalid speech manifest entry: $path" }
    $expected[$path] = $file
}
$runtimeEntries = @($manifest.files | Where-Object source -eq 'runtime')
if ($runtimeEntries.Count -ne 2 -or $expected['models/stt/sherpa-onnx-c-api.dll'].source -ne 'runtime' -or
    $expected['models/stt/onnxruntime.dll'].source -ne 'runtime') { throw 'Invalid speech runtime manifest' }
function Get-AssetPath($file) {
    $relative = ([string]$file.path).Substring('models/stt/'.Length).Replace('/', [IO.Path]::DirectorySeparatorChar)
    if ($file.source -eq 'models') { return Join-Path $models $relative }
    if ($relative -match '[\\/]') { throw "Invalid runtime path: $relative" }
    return Join-Path (Join-Path $client 'third_party/sherpa-onnx/x64') $relative
}
function Test-Asset($file) {
    $source = Get-AssetPath $file
    $info = Get-Item -LiteralPath $source
    if ($info.Length -ne [long]$file.bytes -or (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ne $file.sha256) {
        throw "Speech asset failed size or SHA-256 verification: $($file.path)"
    }
}
if ($VerifyOnly) {
    if ($ArchivePath) { throw 'Use either -VerifyOnly or -ArchivePath' }
    foreach ($file in $manifest.files) { Test-Asset $file }
    Write-Host "Verified $($manifest.files.Count) speech assets against the manifest"
    return
}
$pin = Get-Content -LiteralPath (Join-Path $client 'tools/speech-build-assets.json') -Raw | ConvertFrom-Json
if ($pin.schema -ne 1 -or $pin.url -notmatch '^https://github\.com/zateckar/skowork/releases/download/speech-assets-[A-Za-z0-9_-]+/[A-Za-z0-9_.-]+\.zip$' -or
    $pin.sha256 -notmatch '^[a-fA-F0-9]{64}$' -or [long]$pin.bytes -le 0) { throw 'Invalid speech build bundle pin' }
$fixtures = @('fixtures/parakeet-en.wav', 'fixtures/parakeet-de.wav', 'fixtures/parakeet-es.wav',
    'fixtures/parakeet-fr.wav', 'fixtures/whisper-0.wav', 'fixtures/whisper-1.wav')
$tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$work = Join-Path $tempRoot ('workbench-stt-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
try {
    if ($ArchivePath) { $archive = (Resolve-Path -LiteralPath $ArchivePath).Path }
    else {
        $archive = Join-Path $work 'speech-build-assets.zip'
        Write-Host "Downloading pinned speech build bundle: $($pin.url)"
        # Invoke-WebRequest uses the platform certificate store and keeps TLS verification enabled.
        Invoke-WebRequest -Uri $pin.url -OutFile $archive -MaximumRetryCount 3
    }
    if ((Get-Item -LiteralPath $archive).Length -ne [long]$pin.bytes -or
        (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $pin.sha256) {
        throw 'Speech bundle failed size or SHA-256 verification'
    }
    Add-Type -AssemblyName System.IO.Compression
    $zip = [IO.Compression.ZipFile]::OpenRead($archive)
    try {
        $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        foreach ($entry in $zip.Entries) {
            if (!$seen.Add($entry.FullName)) { throw "Duplicate bundled path: $($entry.FullName)" }
        }
        foreach ($file in $manifest.files) {
            $entry = $zip.GetEntry([string]$file.path)
            if (!$entry) { throw "Missing bundled asset: $($file.path)" }
            if ($entry.Length -ne [long]$file.bytes) { throw "Bundled asset failed size or SHA-256 verification: $($file.path)" }
            $stream = $entry.Open()
            $sha = [Security.Cryptography.SHA256]::Create()
            try { $hash = [Convert]::ToHexString($sha.ComputeHash($stream)) } finally { $sha.Dispose(); $stream.Dispose() }
            if ($hash -ne $file.sha256) { throw "Bundled asset failed size or SHA-256 verification: $($file.path)" }
        }
        foreach ($name in $fixtures) {
            $entry = $zip.GetEntry($name)
            if (!$entry -or $entry.Length -le 0) { throw "Missing bundled fixture: $name" }
        }
        foreach ($file in $runtimeEntries) { Test-Asset $file }
        # No destination changes until the entire input and existing runtime have passed verification.
        $copies = @($manifest.files | Where-Object source -eq 'models' | ForEach-Object {
            @{ Entry = [string]$_.path; Destination = (Get-AssetPath $_) }
        })
        $copies += @($fixtures | ForEach-Object { @{ Entry = $_; Destination = (Join-Path $models $_) } })
        foreach ($copy in $copies) {
            New-Item -ItemType Directory -Force -Path (Split-Path $copy.Destination) | Out-Null
            $inputStream = $zip.GetEntry($copy.Entry).Open()
            try {
                $outputStream = [IO.File]::Create($copy.Destination)
                try { $inputStream.CopyTo($outputStream) } finally { $outputStream.Dispose() }
            } finally { $inputStream.Dispose() }
        }
    } finally { $zip.Dispose() }
    Write-Host "client/models/stt ready: 13 verified speech assets and 6 development recordings"
} finally {
    $resolvedWork = [IO.Path]::GetFullPath($work)
    if ([IO.Path]::GetDirectoryName($resolvedWork) -ne $tempRoot.TrimEnd([IO.Path]::DirectorySeparatorChar) -or
        [IO.Path]::GetFileName($resolvedWork) -notmatch '^workbench-stt-[a-f0-9]{32}$') { throw 'Unsafe temporary cleanup path' }
    Remove-Item -LiteralPath $resolvedWork -Recurse -Force
}
