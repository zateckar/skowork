## Decisions

Product CI retains strict OpenSpec validation, client release compilation, manifest and footprint checks, signing when configured, and the control-plane image build (which type-checks and builds the admin UI). Fuzzing, control-plane tests, policy tests and Graph mock tests remain local and do not delay product publication. Build and publish the image once. Tagged releases follow the same product-build principle.

The Node package includes node.exe, its license and app-host.js. A tag derived from the Git blob identities of the fetch script, app-host.js and packaging script identifies its inputs. The dedicated workflow builds only when those inputs change or manually dispatched, checks the pinned upstream archive, executable hash and signature, and creates a release without replacing an existing one. Product release notes link to that tag. A missing release is created; an API error fails closed. A changed packager requires a new tag. This supersedes the app runtime cache decision in publish-pinned-packages-once.

The speech release remains keyed by the explicit product pin and is never replaced. The release check must handle a genuine HTTP 404 as absent, while other errors fail the job. An existing release is accepted only when both expected assets exist. Both optional workflows serialize per ref to avoid duplicate publishers. Publication of optional assets is independent of product publication, so links may temporarily point to an asset still being built; failed workflows require attention.

## Verification

Exercise release lookup with present, absent and error responses; validate workflow syntax, strict OpenSpec and packaging with fixtures. Verify actual GitHub runs and release assets when credentials permit.

The speech pin is updated to the digest of the current verified package and its actual case-sensitive asset name; the manifest checkout is fixed to LF so package bytes do not depend on Git's platform line-ending setting. The model file hashes and versions are unchanged.
