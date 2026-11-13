Pinned speech build inputs for Skowork. This release is retained independently of the rolling dev release and must not be overwritten.

The bundle preserves all 13 model/runtime files from client/tools/speech-manifest.json at source commit 757992e05debf4e16d5a6107094feb615ea65ebf, with their license notices. It also includes the six existing Parakeet and Whisper development recordings. It is a build input; product speech packages exclude those recordings.

- Archive: Skowork-speech-build-assets-v1-win-x64.zip
- Bytes: 1097703495
- SHA-256: 7529e35b097060faa17b4fc1fd94faeed07e7d537d123c07dd24cae5d67a1914

Created using dev/scripts/package-speech-build-assets.ps1 from locally retained assets after verifying every model/runtime file against the unchanged manifest. No model conversion or upgrade was performed. The product-only archive rebuilt from these inputs hashes to 7f68642e69f89792936be93cbb67d186f3e1815dcaed64d8965e1a7e20bfd814.
