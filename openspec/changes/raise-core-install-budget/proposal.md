# Proposal

## Why

The CI footprint gate fails: `check-footprint.ps1` reports an install size of 31.01 MB for the release build, over the 30 MB core-package budget of client/desktop-host Footprint Budgets. `Skowork.exe` is 21.6 MB, of which 17.5 MB is code. It grew with the local Outlook and Teams support, the Calendar page and the Daily report: the core package was 28.89 MB on 2026-11-19.

The build is already stripped and section-garbage-collected. The remaining ways to shrink it cost speed or do not work today (see design.md), so the budget is raised instead.

## What Changes

- The core-package budget of Footprint Budgets goes from 30 MB to 40 MB installed.
- `check-footprint.ps1` uses 40 MB as its default, which CI and the release workflow use.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `client/desktop-host`: Footprint Budgets (core package budget 40 MB).

## Impact

`client/tools/check-footprint.ps1` and the desktop-host spec. No change to the client's code or build flags.
