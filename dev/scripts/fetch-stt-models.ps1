# Fetches the speech-to-text models package (add-meeting-capture design D1) into client/models/stt: Parakeet-TDT-0.6B-v3
# int8, Whisper small int8, Silero VAD, pyannote segmentation 3.0 int8 and the WeSpeaker ResNet34-LM embedding, all
# from the k2-fsa/sherpa-onnx GitHub releases, about 1.2 GB to download and 1.0 GB on disk. Each archive and each kept
# file is checked against the SHA-256 in client/third_party/README.md. The runtime DLLs are vendored in
# client/third_party/sherpa-onnx and copied next to the worker by the build. The test recordings shipped with the
# models go to client/models/stt/fixtures for the MeetingSherpa tests.
#   pwsh dev/scripts/fetch-stt-models.ps1
$ErrorActionPreference = 'Stop'
$client = Resolve-Path (Join-Path $PSScriptRoot '../../client')
$models = Join-Path $client 'models/stt'
$work = Join-Path ([IO.Path]::GetTempPath()) ("workbench-stt-" + [Guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $work, $models | Out-Null

$base = 'https://github.com/k2-fsa/sherpa-onnx/releases/download'
$downloads = @(
    @{ url = "$base/asr-models/sherpa-onnx-nemo-parakeet-tdt-0.6b-v3-int8.tar.bz2"; sha = '5793D0FD397C5778D2CF2126994D58E9D56B1BE7C04D13C7A15BB1B4EAFB16BF' },
    @{ url = "$base/asr-models/sherpa-onnx-whisper-small.tar.bz2"; sha = '486A46AFBB7BA798507190FFE02FEA2DD726049AF212E774537EFAC6AFB210A6' },
    @{ url = "$base/asr-models/silero_vad.onnx"; sha = '9E2449E1087496D8D4CABA907F23E0BD3F78D91FA552479BB9C23AC09CBB1FD6' },
    @{ url = "$base/speaker-segmentation-models/sherpa-onnx-pyannote-segmentation-3-0.tar.bz2"; sha = '24615EE884C897D9D2BA09BB4D30DA6BB1B15E685065962DB5B02E76E4996488' },
    @{ url = "$base/speaker-recongition-models/wespeaker_en_voxceleb_resnet34_LM.onnx"; sha = 'E9848563DA86F263117134DFD7AD63C92355B37DE492B55E325400C9D9C39012' }
)
# Kept file (relative to client/models/stt) <- path in the extracted archives, with its SHA-256.
$files = @(
    @('parakeet-tdt-0.6b-v3-int8/encoder.int8.onnx', 'sherpa-onnx-nemo-parakeet-tdt-0.6b-v3-int8/encoder.int8.onnx', 'ACFC2B4456377E15D04F0243AF540B7FE7C992F8D898D751CF134C3A55FD2247'),
    @('parakeet-tdt-0.6b-v3-int8/decoder.int8.onnx', 'sherpa-onnx-nemo-parakeet-tdt-0.6b-v3-int8/decoder.int8.onnx', '179E50C43D1A9DE79C8A24149A2F9BAC6EB5981823F2A2ED88D655B24248DB4E'),
    @('parakeet-tdt-0.6b-v3-int8/joiner.int8.onnx', 'sherpa-onnx-nemo-parakeet-tdt-0.6b-v3-int8/joiner.int8.onnx', '3164C13FC2821009440D20FCB5FDC78BFF28B4DB2F8D0F0B329101719C0948B3'),
    @('parakeet-tdt-0.6b-v3-int8/tokens.txt', 'sherpa-onnx-nemo-parakeet-tdt-0.6b-v3-int8/tokens.txt', 'D58544679EA4BC6AC563D1F545EB7D474BD6CFA467F0A6E2C1DC1C7D37E3C35D'),
    @('whisper-small-int8/small-encoder.int8.onnx', 'sherpa-onnx-whisper-small/small-encoder.int8.onnx', '4CBE7B22FA9026B843B60A68640C747DE05BAFB1A11B57EDC0E66C232D9F33A9'),
    @('whisper-small-int8/small-decoder.int8.onnx', 'sherpa-onnx-whisper-small/small-decoder.int8.onnx', 'ACAD50B5C782696E91B55914CC5AB4F756F1532F76E22AA6FC615F39FB69A8EE'),
    @('whisper-small-int8/small-tokens.txt', 'sherpa-onnx-whisper-small/small-tokens.txt', 'B34B360DBB493E781E479794586D661700670D65564001F23024971D1F2FA126'),
    @('pyannote-segmentation-3-0/model.int8.onnx', 'sherpa-onnx-pyannote-segmentation-3-0/model.int8.onnx', 'D582F4B4C6B48205DE7E0643C57DF0DF5615A3C176189BE3FC461E9D18827B5D'),
    @('pyannote-segmentation-3-0/LICENSE', 'sherpa-onnx-pyannote-segmentation-3-0/LICENSE', '14D7016AD68E7394D6E6B78D96CC2AE431C905287B89674CFDF021E79E62B8BA'),
    @('silero_vad.onnx', 'silero_vad.onnx', '9E2449E1087496D8D4CABA907F23E0BD3F78D91FA552479BB9C23AC09CBB1FD6'),
    @('wespeaker_en_voxceleb_resnet34_LM.onnx', 'wespeaker_en_voxceleb_resnet34_LM.onnx', 'E9848563DA86F263117134DFD7AD63C92355B37DE492B55E325400C9D9C39012')
)
$fixtures = @(
    @('fixtures/parakeet-en.wav', 'sherpa-onnx-nemo-parakeet-tdt-0.6b-v3-int8/test_wavs/en.wav'),
    @('fixtures/parakeet-de.wav', 'sherpa-onnx-nemo-parakeet-tdt-0.6b-v3-int8/test_wavs/de.wav'),
    @('fixtures/parakeet-es.wav', 'sherpa-onnx-nemo-parakeet-tdt-0.6b-v3-int8/test_wavs/es.wav'),
    @('fixtures/parakeet-fr.wav', 'sherpa-onnx-nemo-parakeet-tdt-0.6b-v3-int8/test_wavs/fr.wav'),
    @('fixtures/whisper-0.wav', 'sherpa-onnx-whisper-small/test_wavs/0.wav'),
    @('fixtures/whisper-1.wav', 'sherpa-onnx-whisper-small/test_wavs/1.wav')
)

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
        if ($h -ne $f[2]) { throw "$($f[1]) has SHA-256 $h, expected $($f[2])" }
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
