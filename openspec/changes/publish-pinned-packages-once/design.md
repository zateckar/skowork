# Design

## Context

Current pipeline:

- `ci.yml` runs on every push to main. It fetches the 1,097,703,495-byte build-input bundle from the `speech-assets-v1` release, packages the 1,096,195,563-byte speech zip, uploads it as an Actions artifact, and then replaces the rolling `dev` pre-release with it.
- `release.yml` runs on every version tag and repeats the steps. It attaches the speech zip to the GitHub release.
- `fetch-node-runtime.ps1` downloads the pinned Node.js zip on every run, and `package.ps1` repackages `node.exe` into the 100,261,283-byte app runtime package.

The pinned inputs do not change between runs: `client/tools/speech-build-assets.json` pins the bundle, `client/tools/speech-manifest.json` pins the 13 files, and `fetch-node-runtime.ps1` pins the Node.js version and SHA-256. Repackaging the speech zip from the verified inputs reproduces the same bytes (`pin-speech-build-assets` design, decision D2).

The speech catalog design in `improve-skowork-and-knowledge-control` says the zip is "built and published with each release". This change supersedes that decision.

## Decisions

### D1: The speech package is a pinned, immutable release asset

Pin the product zip in `client/tools/speech-package.json`, with `schema`, `version`, `url`, `bytes` and `sha256`. Publish it once as release `speech-package-v<version>`. Operators point `policy.speech.package.url` and `policy.speech.package.sha256` at that asset, so the client's existing digest check covers it.

Alternatives:

- *Keep attaching it to every release.* Rejected: about 1.1 GB per main push and per tag, for an asset that rarely changes.
- *Cache the speech zip in Actions.* Rejected: each run still uploads the artifact, and the cache is capped at 10 GB per repository with eviction.
- *Only skip the steps when files are unchanged (path filters).* Rejected: the dev pre-release is still replaced with the zip whenever the client builds.

### D2: A dedicated workflow builds the speech package

`speech-package.yml` runs on `workflow_dispatch` and on pushes to main that change `client/tools/speech-package.json`, `client/tools/package-speech.ps1`, `client/tools/speech-manifest.json` or `client/tools/speech-build-assets.json`. It fetches the pinned build-input bundle, runs `package-speech.ps1`, checks the size and SHA-256 against the pin, and creates the release only if the tag does not exist. It never replaces a release.

### D3: The dev pre-release and the product release carry no speech package

The runtime attachment decision below is superseded by `streamline-release-pipeline`.

The `dev` pre-release and the version release contain the Skowork zip, its checksum and the app runtime package. Testers and administrators take the speech package from its release, using the pinned URL and digest.

### D4: The app runtime package is cached, not published once (superseded)

Superseded by `streamline-release-pipeline`: the runtime is independently published once per input identity.

The runtime is 100 MB, and product installs need it for Local apps, so it stays in each dev and product release. `ci.yml` and `release.yml` restore `client/runtime/node` from `actions/cache`, keyed on the hash of `dev/scripts/fetch-node-runtime.ps1`. The upstream download runs only on a cache miss. The SHA-256 and signature checks stay in the script.

Alternative: publish the runtime as an immutable asset like the speech package. Possible later, if the 100 MB upload per release becomes a problem.

## Open questions

1. **Superseded decision.** Does the user confirm that the speech package is no longer built and published with each release (`improve-skowork-and-knowledge-control` design, 2026-11-12)?
2. **Runtime.** Is the cache enough (D4), or should the runtime also become an immutable asset?
3. **Dev pre-release.** Is it acceptable that the `dev` pre-release has no speech package? Testers would import it from its release.

## Risks

- Availability: the speech package release becomes a separate dependency. The digest check refuses any replaced file, and the build-input bundle remains the source.
- Cache eviction: unused caches are evicted after seven days. A cache miss only costs a 100 MB download.
- Release immutability: GitHub does not guarantee it. The pinned SHA-256 is what protects clients.

## Migration

1. Publish `speech-package-v1` from the existing verified zip, and check its digest against the pin.
2. Change `ci.yml` and `release.yml`, add `speech-package.yml` and the runtime cache, and update the release notes.
3. Set the dev policy's `speech.package.url` and `speech.package.sha256`.
4. Confirm a main push and a test tag produce no speech zip and reuse the runtime cache.
