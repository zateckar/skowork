## 1. Workflow

- [x] 1.1 Remove the `specs` job and the `OPENSPEC_TELEMETRY` variable from `.github/workflows/ci.yml`, and drop `specs` from the `needs` of `publish-image` and `publish-packages`. Verify: `.github/workflows` has no reference to OpenSpec or to a `specs` job, and the workflow file parses as YAML.
- [ ] 1.2 Push to main. Verify: the `ci` run has no OpenSpec validation job, and the image and dev packages publish.

## 2. Archive

- [ ] 2.1 Once 1.2 is checked, run `openspec archive remove-ci-spec-validation --yes`, then reread `openspec/specs/client/build-pipeline/spec.md`. Verify: the archive succeeds and the spec no longer requires validation in the pipeline.
