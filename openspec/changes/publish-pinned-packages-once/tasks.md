# Tasks

## 1. Specs and decisions

- [x] 1.1 Get the user's confirmation that the 2026-11-12 speech catalog decision is superseded (confirmed by the user)
- [x] 1.2 Amend `improve-skowork-and-knowledge-control` design: the sentence now refers to `speech-package-v<version>` and this change
- [x] 1.3 Amend the `pin-speech-build-assets` delta scenario "Publish product speech artifacts" to name the speech package workflow
- [x] 1.4 Run `openspec validate --all --strict` (54 items passed)

## 2. Speech package

- [x] 2.1 Add `client/tools/speech-package.json` with the pinned URL, byte length and SHA-256 (version 1, 1,096,195,563 bytes)
- [x] 2.2 Add `.github/workflows/speech-package.yml`: builds from the pinned build-input bundle, checks the pin, and creates `speech-package-v<version>` only if it does not exist
- [ ] 2.3 Publish `speech-package-v1` once: push, run the workflow (`workflow_dispatch`), then download the asset and check its digest
- [x] 2.4 Remove the speech steps and the speech zip from `ci.yml` and `release.yml`; the release notes link the speech release and its digest
- [ ] 2.5 Set `policy.speech.package.url` and `policy.speech.package.sha256` for the dev policy, after 2.3

## 3. App runtime package

- [x] 3.1 `actions/cache` for `client/runtime/node` in `ci.yml` and `release.yml`, keyed on the hash of `dev/scripts/fetch-node-runtime.ps1`
- [x] 3.2 Fetch only on a cache miss; `fetch-node-runtime.ps1 -VerifyOnly` checks VERSION, the pinned `node.exe` SHA-256 and the Authenticode signature on every run (the full fetch was run locally and passed)

## 4. Verification

- [ ] 4.1 Lint the changed workflows with actionlint (not installed here; the three workflow files parse as YAML)
- [ ] 4.2 Push a main change: no speech zip is uploaded, and the runtime comes from the cache; record artifact sizes before and after
- [ ] 4.3 Run a version-tag release (or a `workflow_dispatch` dry run): the release has the Skowork zip and the runtime package, and no speech zip
- [ ] 4.4 Run strict OpenSpec validation, then archive this change once 2.3, 2.5, 4.2 and 4.3 are done
