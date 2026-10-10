#!/usr/bin/env bash
set -euo pipefail

anyps5_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
exec python3 -B "$anyps5_dir/tools/run_with_memory_guard.py" -- "$anyps5_dir/build/examples/bomberman/run.sh" "$@"
