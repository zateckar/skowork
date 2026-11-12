## Context

The upstream Parakeet archive now hashes to 1495e91b59d7fc53ddca91f3431758ec99f8fa253fdb0e1e851be0f72b14b0f9 instead of the pinned 5793d0fd397c5778d2cf2126994d58e9d56b1be7c04d13c7a15bb1b4eafb16bf. Its encoder, decoder and joiner also differ from the accepted manifest. Existing local assets pass fetch-stt-models.ps1 -VerifyOnly (13 files). Preserve these bytes.

## Decisions

### D1: Versioned repository build input

Use a dedicated speech-assets-v1 prerelease, separate from the rolling dev release and v*.*.* product release triggers. Build its ZIP from the already verified package-speech.ps1 output, retaining its manifest and license notices, and add the six recordings fetched by the previous developer script. The Czech recording is a separate local fixture and is not added. The final product speech ZIP continues to exclude recordings. Never overwrite this release asset; future changes require another version. Pin URL, bytes and SHA-256 in client/tools/speech-build-assets.json. The digest rejects replacements even if a repository administrator changes the release; GitHub platform immutability is not assumed.

### D2: Validate before copying

The fetch script verifies the archive digest and size, rejects duplicate ZIP paths, checks all 13 manifest files inside the archive, requires the six fixture entries, and verifies the two existing vendored runtime files before copying any models or recordings. It opens only named entries, without extracting arbitrary archive paths. Runtime DLLs remain sourced from the vendored tree. An optional -ArchivePath accepts an offline copy with the same pinned archive verification. -VerifyOnly remains offline and validates the existing 13 local assets.

### D3: Verification and limits

Use isolated tiny fixture repositories to exercise successful import and rejection of corrupted archives, missing/duplicate entries, incorrect model hashes and runtime mismatch before local models change. Build and verify the real bundle, publish it without clobbering, download it back, and run the fetcher plus final speech packager in an isolated repository layout. Validate OpenSpec and workflows before commit. Push main and inspect its actual Actions result and published desktop, optional runtime, speech and container artifacts. Local checks alone do not establish remote publication. No model upgrade or new speech quality claim is made.

## Risks

The bundle depends on GitHub release availability and is approximately 1.1 GB. The pinned archive and per-file hashes protect integrity, not availability. Keep the input release separate from rolling publication. Audio fixtures remain developer inputs rather than product payloads. Licensing notices are carried forward from the existing verified package.

## Verification evidence

- All 13 retained source assets pass -VerifyOnly. The build bundle is 1,097,703,495 bytes with SHA-256 7529e35b097060faa17b4fc1fd94faeed07e7d537d123c07dd24cae5d67a1914.
- Nine isolated fetch regression checks failed against the previous fetcher (which ignored the unsupported offline argument and attempted upstream download), then passed after implementation. Coverage includes successful import/offline verification, archive hash and size, missing model/recording, duplicate path, model hash, vendored runtime and non-extraction of arbitrary entries.
- Importing the full bundle into an empty isolated model tree and packaging it again reproduces the original 1,096,195,563-byte product ZIP exactly: SHA-256 7f68642e69f89792936be93cbb67d186f3e1815dcaed64d8965e1a7e20bfd814. This verifies unchanged product bytes, including notices and omission of recordings.
- Independent read-only code review found no critical or important implementation issue. Its minor traversal-test destination finding was corrected, and the test now also checks that an ordinary unlisted file is never extracted. Runtime speech quality and interruption rollback remain outside this build-input change.
