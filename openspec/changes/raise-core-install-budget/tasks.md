# Tasks

## 1. Budget

- [x] 1.1 Set the default `-MaxInstallMB` of `client/tools/check-footprint.ps1` to 40 (design D1). Verify that `check-footprint.ps1 -BuildDir build/release-local -InstallOnly` passes for a CI-equivalent Release build (31.04 MB).

## 2. Verification

- [x] 2.1 Run `openspec validate raise-core-install-budget --strict`.
