#!/usr/bin/env bash
set -euo pipefail
campus_project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
source "$campus_project_dir/tools/environment.sh"
if [[ ! -x "$campus_project_dir/build/campus_gui" ]]; then
  "$campus_project_dir/tools/build.sh"
fi
# Start the input daemon before Qt can request the incompatible default
# D-Bus activation command on WSL. Headless checks need no input daemon.
if [[ "${QT_IM_MODULE:-}" == fcitx && -n "${DISPLAY:-}" &&
      "${QT_QPA_PLATFORM:-}" != offscreen && "${QT_QPA_PLATFORM:-}" != minimal ]]; then
  if ! "$campus_project_dir/tools/start-input-method.sh"; then
    printf '中文输入法启动失败，请检查上方日志提示。\n' >&2
  fi
fi
if [[ $# -gt 0 ]]; then
  exec "$campus_project_dir/build/campus_gui" "$@"
fi
# Keep the demonstration database inside the ignored build directory.
if [[ ! -f "$campus_project_dir/build/demo.db" ]]; then
  exec "$campus_project_dir/build/campus_gui" \
    --db "$campus_project_dir/build/demo.db" --demo
fi
exec "$campus_project_dir/build/campus_gui" --db "$campus_project_dir/build/demo.db"
