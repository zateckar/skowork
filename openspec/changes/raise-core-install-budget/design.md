# Design

## Context

Measured on 2026-11-22 with GCC 16.2 (MinGW, winlibs), a Release build matching CI (`SKOWORK_TESTS=OFF`):

| Option | Skowork.exe | skowork-worker.exe | Install size (with pdfium.dll 7.24 MB) |
|---|---|---|---|
| `-O2` (current) | 21.59 MB | 2.21 MB | 31.04 MB |
| `-O2` + LTO | GCC internal compiler error in `binds_to_current_def_p` while linking Skowork.exe | 1.54 MB | n/a |
| `-Os` | 14.12 MB | 1.76 MB | 23.12 MB |

Release builds are already stripped (`-s`), linked with `--gc-sections`, and compiled with `-ffunction-sections -fdata-sections`. Resources are negligible (`.rsrc` 0.01 MB). The hardening flags of add-client-foundation D9 (stack protector, `_FORTIFY_SOURCE=3`, `_GLIBCXX_ASSERTIONS`, CET) cost some size and are security decisions.

## Decisions

### D1: Raise the budget, keep `-O2`

`-Os` would meet the 30 MB budget, but it makes CPU-bound code slower (typically 5–20%): the UI thread's layout and Markdown rendering, SQLite queries and the Teams cache decoders. LTO would keep `-O2` speed, but GCC crashes on it. So the build stays as it is, and the core-package budget goes to 40 MB.

40 MB leaves about 9 MB of growth over today's 31.04 MB. The package grew 2.15 MB between 2026-11-19 and 2026-11-22 with several large features.

### D2: Other budgets unchanged

The idle working set (60 MB), idle CPU, tray-ready time and the optional packages' budgets stay as they are. The idle working set on the hosted runner is 20.1 MB.

## Risks

- A larger download and install. 40 MB is still small for a desktop client, and the package is a 13 MB zip.
- If growth continues at this rate, the budget is reached again. LTO can be measured again with a later GCC, and `-Os` for libraries that are not CPU-bound remains an option.
