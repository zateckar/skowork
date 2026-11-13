# Tasks

## 1. Specs and decisions

- [ ] 1.1 Get the user's confirmation that the 2026-11-12 speech catalog decision is superseded (design open question 1)
- [ ] 1.2 Amend `improve-skowork-and-knowledge-control` design: replace "built and published with each release like the Local apps runtime" with a reference to this change
- [ ] 1.3 Amend the `pin-speech-build-assets` delta scenario "Publish product speech artifacts" to name `speech-package.yml`
- [ ] 1.4 Run `openspec validate --all --strict`

## 2. Speech package

- [ ] 2.1 Add `client/tools/speech-package.json` with the pinned URL, byte length and SHA-256
- [ ] 2.2 Add `.github/workflows/speech-package.yml`: builds from the pinned build-input bundle, checks the pin, and creates `speech-package-v<version>` only if it does not exist
- [ ] 2.3 Publish `speech-package-v1` once, download it and check the digest
- [ ] 2.4 Remove the speech steps and the speech zip from `ci.yml` and `release.yml`; link the speech release from the release notes
- [ ] 2.5 Set `policy.speech.package.url` and `policy.speech.package.sha256` for the dev policy

## 3. App runtime package

- [ ] 3.1 Add `actions/cache` for `client/runtime/node`, keyed on the hash of `dev/scripts/fetch-node-runtime.ps1`
- [ ] 3.2 Run `fetch-node-runtime.ps1` only on a cache miss, keeping its SHA-256 and signature checks

## 4. Verification

- [ ] 4.1 Lint the changed workflows with actionlint
- [ ] 4.2 Push a main change: no speech zip is uploaded, and the runtime comes from the cache; record artifact sizes before and after
- [ ] 4.3 Run a version-tag release (or a `workflow_dispatch` dry run): the release has the Skowork zip and the runtime package, and no speech zip
- [ ] 4.4 Run strict OpenSpec validation, then archive this change
