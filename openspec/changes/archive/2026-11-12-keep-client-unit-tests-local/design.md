## Context

Both Windows workflow builds inherit SKOWORK_TESTS=ON from the release preset and invoke CTest. CI also has a portable-sanitizers job that executes the client unit suite. The separate fuzz job already builds with unit tests disabled.

## Decisions

Override SKOWORK_TESTS=OFF on the two workflow configure commands rather than changing CMakePresets.json. Remove Windows CTest calls, display setup, test mock installs and failed golden-test uploads. Keep the pinned optional app runtime fetch because it supplies distributed artifacts. Remove portable-sanitizers and its needs entries, retaining parser fuzzing with sanitizers. D9 references the new owning capability for this policy.

Alternatives: removing only the Windows CTest command still compiles unused tests and leaves Linux client unit tests in CI; changing the release preset disables convenient local tests. Neither satisfies the requested boundary as clearly.

## Risks / Trade-offs

Client unit-test regressions will be detected locally rather than by GitHub Actions. Product compilation, manifests, footprint, packaging and fuzz checks remain automated. A workflow lint and configure inspection verify the new boundary; the actual pushed SHA's pipeline supplies remote evidence.

## Local Verification (2026-11-12)

Actionlint 1.7.12 passes both workflows. An isolated release-preset configure with SKOWORK_TESTS=OFF succeeds and CTest lists zero tests. The existing debug build still has SKOWORK_TESTS=ON, and local test sources and CMakePresets.json are unchanged. Strict OpenSpec validation passes all 51 items before archival. The remote pipeline is verified after pushing the workflow commit.
