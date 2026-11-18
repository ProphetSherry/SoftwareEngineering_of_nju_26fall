#!/usr/bin/env bash
set -euo pipefail
campus_project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
source "$campus_project_dir/tools/environment.sh"
if [[ -z "${DISPLAY:-}" ]]; then
  printf 'This test needs a real X11 / WSLg display.\n' >&2
  exit 1
fi
if [[ ! -x "$campus_project_dir/build/campus_gui_tests" ]]; then
  "$campus_project_dir/tools/build.sh"
fi
"$campus_project_dir/tools/start-input-method.sh"
exec /usr/bin/python3 "$campus_project_dir/tools/test_real_ime.py"
