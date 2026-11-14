#!/usr/bin/env bash
set -euo pipefail
git ls-files -s -- dev/scripts/fetch-node-runtime.ps1 dev/scripts/package-node-runtime.ps1 dev/scripts/runtime-id.sh client/src/apps/app-host.js | sha256sum | cut -c1-20
