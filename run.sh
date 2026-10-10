#!/usr/bin/env bash
set -euo pipefail

anyps5_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
exec "$anyps5_dir/build/examples/bomberman/run.sh" "$@"
