# Builds the pinned, optional speech artifact. This does not install or activate it.
param([Parameter(Mandatory)] [string] $OutDir)
$ErrorActionPreference = 'Stop'
$client = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$manifestPath = Join-Path $PSScriptRoot 'speech-manifest.json'
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($manifest.schema -ne 1 -or $manifest.package -ne 'skowork-speech' -or $manifest.architecture -ne 'windows-x64' -or
    $manifest.version -notmatch '^[A-Za-z0-9_-]+$' -or $manifest.files.Count -ne 13) {
    throw 'Unsupported speech manifest'
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$out = (Resolve-Path -LiteralPath $OutDir).Path
$archive = Join-Path $out ("Skowork-speech-v{0}-win-x64.zip" -f $manifest.version)
if (Test-Path -LiteralPath $archive) { throw "Speech artifact already exists: $archive" }
$pending = Join-Path $out ('.speech-' + [Guid]::NewGuid().ToString('N') + '.tmp')
$seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$entries = @()
foreach ($file in $manifest.files) {
    $path = [string]$file.path
    if ($path -notmatch '^models/stt/[A-Za-z0-9_.-]+(/[A-Za-z0-9_.-]+)*$' -or ($path -split '/') -contains '..' -or !$seen.Add($path)) {
        throw "Invalid or duplicate manifest path: $path"
    }
    $relative = $path.Substring('models/stt/'.Length).Replace('/', [IO.Path]::DirectorySeparatorChar)
    if ($file.source -eq 'models') { $source = Join-Path (Join-Path $client 'models/stt') $relative }
    elseif ($file.source -eq 'runtime' -and $relative -notmatch '[\\/]') { $source = Join-Path (Join-Path $client 'third_party/sherpa-onnx/x64') $relative }
    else { throw "Invalid source for $path" }
    $info = Get-Item -LiteralPath $source
    if ($info.Length -ne [long]$file.bytes -or (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ne $file.sha256) {
        throw "Speech asset failed size or SHA-256 verification: $path"
    }
    $entries += @{ Path = $path; Source = $source; Hash = [string]$file.sha256 }
}
$licenses = @(
    @('licenses/THIRD-PARTY.md', (Join-Path $client 'third_party/README.md')),
    @('licenses/sherpa-onnx-Apache-2.0.txt', (Join-Path $client 'third_party/sherpa-onnx/LICENSE')),
    @('licenses/onnxruntime-MIT.txt', (Join-Path $client 'third_party/sherpa-onnx/LICENSE.onnxruntime')),
    @('licenses/NOTICE.txt', (Join-Path $client 'third_party/speech-model-licenses/NOTICE.txt')),
    @('licenses/CC-BY-4.0.txt', (Join-Path $client 'third_party/speech-model-licenses/CC-BY-4.0.txt')),
    @('licenses/whisper-MIT.txt', (Join-Path $client 'third_party/speech-model-licenses/whisper-MIT.txt')),
    @('licenses/silero-vad-MIT.txt', (Join-Path $client 'third_party/speech-model-licenses/silero-vad-MIT.txt')),
    @('manifest.json', $manifestPath)
)
foreach ($pair in $licenses) { $entries += @{ Path = $pair[0]; Source = $pair[1]; Hash = '' } }
try {
    Add-Type -AssemblyName System.IO.Compression
    $stream = [IO.File]::Open($pending, [IO.FileMode]::CreateNew, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    try {
        $zip = [IO.Compression.ZipArchive]::new($stream, [IO.Compression.ZipArchiveMode]::Create, $true)
        try {
            foreach ($entry in ($entries | Sort-Object Path)) {
                $item = $zip.CreateEntry($entry.Path, [IO.Compression.CompressionLevel]::NoCompression)
                $item.LastWriteTime = [DateTimeOffset]::Parse('2020-01-01T00:00:00Z')
                $inputStream = [IO.File]::OpenRead($entry.Source)
                $entryStream = $item.Open()
                try { $inputStream.CopyTo($entryStream) } finally { $entryStream.Dispose(); $inputStream.Dispose() }
            }
        } finally { $zip.Dispose() }
    } finally { $stream.Dispose() }
    # Verify the bytes inside the archive, not only the source files read before packaging.
    $zip = [IO.Compression.ZipFile]::OpenRead($pending)
    try {
        foreach ($entry in $entries | Where-Object Hash) {
            $item = $zip.GetEntry($entry.Path)
            if (!$item) { throw "Missing packaged asset: $($entry.Path)" }
            $inputStream = $item.Open()
            $sha = [Security.Cryptography.SHA256]::Create()
            try { $actual = [Convert]::ToHexString($sha.ComputeHash($inputStream)) } finally { $sha.Dispose(); $inputStream.Dispose() }
            if ($actual -ne $entry.Hash) { throw "Packaged asset failed SHA-256 verification: $($entry.Path)" }
        }
    } finally { $zip.Dispose() }
    [IO.File]::Move($pending, $archive)
    $digest = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    "$digest  $([IO.Path]::GetFileName($archive))" | Set-Content -LiteralPath "$archive.sha256" -NoNewline -Encoding ascii
    Write-Host "Packaged $archive ($((Get-Item -LiteralPath $archive).Length) bytes), sha256 $digest"
} finally {
    if (Test-Path -LiteralPath $pending) { Remove-Item -LiteralPath $pending -Force }
}
