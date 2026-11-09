# Fetches the speech-to-text models package (add-meeting-capture design D1) into client/models/stt: Parakeet-TDT-0.6B-v3
# int8, Whisper small int8, Silero VAD, pyannote segmentation 3.0 int8 and the WeSpeaker ResNet34-LM embedding, all
# from the k2-fsa/sherpa-onnx GitHub releases, about 1.2 GB to download and 1.0 GB on disk. Each archive and each kept
# file is checked against client/tools/speech-manifest.json. The runtime DLLs are vendored in
# client/third_party/sherpa-onnx and copied next to the worker by the build. The test recordings shipped with the
# models go to client/models/stt/fixtures for the MeetingSherpa tests.
#   pwsh dev/scripts/fetch-stt-models.ps1 [-VerifyOnly]
param([switch] $VerifyOnly)
$ErrorActionPreference = 'Stop'
$client = Resolve-Path (Join-Path $PSScriptRoot '../../client')
$models = Join-Path $client 'models/stt'
$manifest = Get-Content -LiteralPath (Join-Path $client 'tools/speech-manifest.json') -Raw | ConvertFrom-Json
if ($manifest.schema -ne 1 -or $manifest.package -ne 'skowork-speech' -or $manifest.architecture -ne 'windows-x64' -or
    $manifest.files.Count -ne 13) {
    throw 'Unsupported speech manifest'
}
$expected = @{}
foreach ($file in $manifest.files) {
    $path = [string]$file.path
    if ($path -notmatch '^models/stt/[A-Za-z0-9_.-]+(/[A-Za-z0-9_.-]+)*$' -or ($path -split '/') -contains '..' -or
        $expected.ContainsKey($path) -or $file.sha256 -notmatch '^[a-fA-F0-9]{64}$' -or [long]$file.bytes -le 0 -or
        $file.source -notin @('models', 'runtime')) { throw "Invalid speech manifest entry: $path" }
    $expected[$path] = $file
}

$base = 'https://github.com/k2-fsa/sherpa-onnx/releases/download'
$downloads = @(
    @{ url = "$base/asr-models/sherpa-onnx-nemo-parakeet-tdt-0.6b-v3-int8.tar.bz2"; sha = '5793D0FD397C5778D2CF2126994D58E9D56B1BE7C04D13C7A15BB1B4EAFB16BF' },
    @{ url = "$base/asr-models/sherpa-onnx-whisper-small.tar.bz2"; sha = '486A46AFBB7BA798507190FFE02FEA2DD726049AF212E774537EFAC6AFB210A6' },
    @{ url = "$base/asr-models/silero_vad.onnx"; sha = '9E2449E1087496D8D4CABA907F23E0BD3F78D91FA552479BB9C23AC09CBB1FD6' },
    @{ url = "$base/speaker-segmentation-models/sherpa-onnx-pyannote-segmentation-3-0.tar.bz2"; sha = '24615EE884C897D9D2BA09BB4D30DA6BB1B15E685065962DB5B02E76E4996488' },
    @{ url = "$base/speaker-recongition-models/wespeaker_en_voxceleb_resnet34_LM.onnx"; sha = 'E9848563DA86F263117134DFD7AD63C92355B37DE492B55E325400C9D9C39012' }
)
# Kept file (relative to client/models/stt) <- path in the extracted archives.
$files = @(
    @('parakeet-tdt-0.6b-v3-int8/encoder.int8.onnx', 'sherpa-onnx-nemo-parakeet-tdt-0.6b-v3-int8/encoder.int8.onnx'),
    @('parakeet-tdt-0.6b-v3-int8/decoder.int8.onnx', 'sherpa-onnx-nemo-parakeet-tdt-0.6b-v3-int8/decoder.int8.onnx'),
    @('parakeet-tdt-0.6b-v3-int8/joiner.int8.onnx', 'sherpa-onnx-nemo-parakeet-tdt-0.6b-v3-int8/joiner.int8.onnx'),
    @('parakeet-tdt-0.6b-v3-int8/tokens.txt', 'sherpa-onnx-nemo-parakeet-tdt-0.6b-v3-int8/tokens.txt'),
    @('whisper-small-int8/small-encoder.int8.onnx', 'sherpa-onnx-whisper-small/small-encoder.int8.onnx'),
    @('whisper-small-int8/small-decoder.int8.onnx', 'sherpa-onnx-whisper-small/small-decoder.int8.onnx'),
    @('whisper-small-int8/small-tokens.txt', 'sherpa-onnx-whisper-small/small-tokens.txt'),
    @('pyannote-segmentation-3-0/model.int8.onnx', 'sherpa-onnx-pyannote-segmentation-3-0/model.int8.onnx'),
    @('pyannote-segmentation-3-0/LICENSE', 'sherpa-onnx-pyannote-segmentation-3-0/LICENSE'),
    @('silero_vad.onnx', 'silero_vad.onnx'),
    @('wespeaker_en_voxceleb_resnet34_LM.onnx', 'wespeaker_en_voxceleb_resnet34_LM.onnx')
)
$fixtures = @(
    @('fixtures/parakeet-en.wav', 'sherpa-onnx-nemo-parakeet-tdt-0.6b-v3-int8/test_wavs/en.wav'),
    @('fixtures/parakeet-de.wav', 'sherpa-onnx-nemo-parakeet-tdt-0.6b-v3-int8/test_wavs/de.wav'),
    @('fixtures/parakeet-es.wav', 'sherpa-onnx-nemo-parakeet-tdt-0.6b-v3-int8/test_wavs/es.wav'),
    @('fixtures/parakeet-fr.wav', 'sherpa-onnx-nemo-parakeet-tdt-0.6b-v3-int8/test_wavs/fr.wav'),
    @('fixtures/whisper-0.wav', 'sherpa-onnx-whisper-small/test_wavs/0.wav'),
    @('fixtures/whisper-1.wav', 'sherpa-onnx-whisper-small/test_wavs/1.wav')
)

$modelEntries = @($manifest.files | Where-Object source -eq 'models')
$runtimeEntries = @($manifest.files | Where-Object source -eq 'runtime')
if ($files.Count -ne $modelEntries.Count -or $runtimeEntries.Count -ne 2 -or
    !$expected.ContainsKey('models/stt/sherpa-onnx-c-api.dll') -or !$expected.ContainsKey('models/stt/onnxruntime.dll') -or
    $expected['models/stt/sherpa-onnx-c-api.dll'].source -ne 'runtime' -or $expected['models/stt/onnxruntime.dll'].source -ne 'runtime') {
    throw 'Speech manifest and fetched file list differ'
}
$listed = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($f in $files) {
    $path = 'models/stt/' + $f[0]
    if (!$listed.Add($path) -or !$expected.ContainsKey($path) -or $expected[$path].source -ne 'models') {
        throw "Speech manifest does not match fetched file: $path"
    }
}
if ($VerifyOnly) {
    foreach ($file in $manifest.files) {
        $relative = ([string]$file.path).Substring('models/stt/'.Length).Replace('/', [IO.Path]::DirectorySeparatorChar)
        if ($file.source -eq 'models') { $src = Join-Path $models $relative }
        elseif ($relative -notmatch '[\\/]') { $src = Join-Path (Join-Path $client 'third_party/sherpa-onnx/x64') $relative }
        else { throw "Invalid runtime path: $relative" }
        $info = Get-Item -LiteralPath $src
        if ($info.Length -ne [long]$file.bytes -or (Get-FileHash -LiteralPath $src -Algorithm SHA256).Hash -ne $file.sha256) {
            throw "Speech asset failed size or SHA-256 verification: $($file.path)"
        }
    }
    Write-Host "Verified $($manifest.files.Count) speech assets against the manifest"
    return
}
$work = Join-Path ([IO.Path]::GetTempPath()) ("workbench-stt-" + [Guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $work, $models | Out-Null

try {
    foreach ($d in $downloads) {
        $name = Split-Path $d.url -Leaf
        $file = Join-Path $work $name
        Write-Host "downloading $name"
        curl.exe -sSL --fail --retry 3 -o $file $d.url
        if ($LASTEXITCODE -ne 0) { throw "download failed: $($d.url)" }
        $h = (Get-FileHash $file -Algorithm SHA256).Hash
        if ($h -ne $d.sha) { throw "$name has SHA-256 $h, expected $($d.sha)" }
        if ($name.EndsWith('.tar.bz2')) { tar -xjf $file -C $work; if ($LASTEXITCODE -ne 0) { throw "cannot extract $name" } }
    }
    foreach ($f in $files) {
        $src = Join-Path $work $f[1]
        $h = (Get-FileHash $src -Algorithm SHA256).Hash
        $entry = $expected['models/stt/' + $f[0]]
        if ((Get-Item -LiteralPath $src).Length -ne [long]$entry.bytes -or $h -ne $entry.sha256) {
            throw "$($f[1]) does not match the speech manifest"
        }
        $dst = Join-Path $models $f[0]
        New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
        Copy-Item $src $dst -Force
    }
    foreach ($f in $fixtures) {
        $dst = Join-Path $models $f[0]
        New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
        Copy-Item (Join-Path $work $f[1]) $dst -Force
    }
    $mb = (Get-ChildItem $models -Recurse -File | Where-Object { $_.FullName -notmatch '\\fixtures\\' } | Measure-Object Length -Sum).Sum / 1MB
    Write-Host ("client/models/stt ready: {0:N0} MB" -f $mb)
} finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
