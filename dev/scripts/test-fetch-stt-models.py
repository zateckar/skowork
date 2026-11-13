"""Offline regression checks; uses tiny isolated repositories, never real models."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import warnings
import zipfile

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "dev/scripts/fetch-stt-models.ps1"
FIXTURES = [f"fixtures/parakeet-{lang}.wav" for lang in ("en", "de", "es", "fr")]
FIXTURES += ["fixtures/whisper-0.wav", "fixtures/whisper-1.wav"]


class FetchTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="skowork-fetch-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "dev/scripts").mkdir(parents=True)
        shutil.copyfile(SCRIPT, self.root / "dev/scripts/fetch-stt-models.ps1")
        self.client = self.root / "client"
        (self.client / "tools").mkdir(parents=True)
        self.manifest = json.loads((ROOT / "client/tools/speech-manifest.json").read_text())
        self.contents = {}
        for entry in self.manifest["files"]:
            content = (entry["path"] + " original").encode()
            entry.update(bytes=len(content), sha256=hashlib.sha256(content).hexdigest())
            self.contents[entry["path"]] = content
            if entry["source"] == "runtime":
                target = self.client / "third_party/sherpa-onnx/x64" / Path(entry["path"]).name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(content)
        (self.client / "tools/speech-manifest.json").write_text(json.dumps(self.manifest))
        self.contents.update({name: b"local recording" for name in FIXTURES})
        self.contents["manifest.json"] = json.dumps(self.manifest).encode()
        self.archive = self.root / "input.zip"
        self.sentinel = self.client / self.manifest["files"][0]["path"]
        self.sentinel.parent.mkdir(parents=True)
        self.sentinel.write_bytes(b"existing model must survive rejection")

    def bundle(self, *, duplicate=False):
        with zipfile.ZipFile(self.archive, "w") as archive:
            for name, content in self.contents.items():
                archive.writestr(name, content)
            if duplicate:
                name = self.manifest["files"][0]["path"]
                with warnings.catch_warnings():
                    warnings.simplefilter("ignore", UserWarning)
                    archive.writestr(name.upper(), self.contents[name])
        descriptor = dict(schema=1, url="https://github.com/zateckar/skowork/releases/download/speech-assets-v1/Skowork-speech-build-assets-v1-win-x64.zip",
                          bytes=self.archive.stat().st_size,
                          sha256=hashlib.sha256(self.archive.read_bytes()).hexdigest())
        (self.client / "tools/speech-build-assets.json").write_text(json.dumps(descriptor))

    def run_fetch(self, *args):
        return subprocess.run(["pwsh", "-NoProfile", "-File", str(self.root / "dev/scripts/fetch-stt-models.ps1"), *args],
                              capture_output=True, text=True, timeout=30)

    def rejected(self, expected):
        result = self.run_fetch("-ArchivePath", str(self.archive))
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn(expected, result.stdout + result.stderr)
        self.assertEqual(self.sentinel.read_bytes(), b"existing model must survive rejection")
        self.assertFalse((self.client / "models/stt/fixtures").exists())

    def test_import_then_offline_verify_preserves_runtime_and_extra_fixture(self):
        self.bundle()
        extra = self.client / "models/stt/fixtures/parakeet-cs.wav"
        extra.parent.mkdir(parents=True)
        extra.write_bytes(b"private local fixture")
        result = self.run_fetch("-ArchivePath", str(self.archive))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for entry in self.manifest["files"]:
            if entry["source"] == "models":
                self.assertEqual((self.client / entry["path"]).read_bytes(), self.contents[entry["path"]])
        for name in FIXTURES:
            self.assertEqual((self.client / "models/stt" / name).read_bytes(), self.contents[name])
        self.assertEqual(extra.read_bytes(), b"private local fixture")
        self.archive.unlink()
        result = self.run_fetch("-VerifyOnly")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Verified 13", result.stdout)

    def test_bad_archive_hash(self):
        self.bundle()
        data = bytearray(self.archive.read_bytes())
        data[-1] ^= 1
        self.archive.write_bytes(data)
        self.rejected("Speech bundle failed size or SHA-256 verification")

    def test_bad_archive_size(self):
        self.bundle()
        with self.archive.open("ab") as stream:
            stream.write(b"trailing bytes")
        self.rejected("Speech bundle failed size or SHA-256 verification")

    def test_missing_model(self):
        del self.contents[self.manifest["files"][4]["path"]]
        self.bundle()
        self.rejected("Missing bundled asset")

    def test_wrong_model_hash_even_with_valid_bundle_digest(self):
        name = self.manifest["files"][4]["path"]
        self.contents[name] = bytes(len(self.contents[name]))
        self.bundle()
        self.rejected("Bundled asset failed size or SHA-256 verification")

    def test_missing_recording(self):
        del self.contents[FIXTURES[-1]]
        self.bundle()
        self.rejected("Missing bundled fixture")

    def test_case_insensitive_duplicate(self):
        self.bundle(duplicate=True)
        self.rejected("Duplicate bundled path")

    def test_wrong_vendored_runtime(self):
        self.bundle()
        (self.client / "third_party/sherpa-onnx/x64/onnxruntime.dll").write_bytes(b"incorrect runtime")
        self.rejected("Speech asset failed size or SHA-256 verification")

    def test_unlisted_archive_paths_are_never_extracted(self):
        self.contents["../../outside.txt"] = b"never extract arbitrary entries"
        self.contents["unlisted.txt"] = b"ignore unlisted files"
        self.bundle()
        result = self.run_fetch("-ArchivePath", str(self.archive))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse((self.client / "outside.txt").exists())
        self.assertFalse((self.root / "outside.txt").exists())
        self.assertFalse((self.client / "models/stt/unlisted.txt").exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
