#!/usr/bin/env bash
set -euo pipefail
campus_project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
source "$campus_project_dir/tools/environment.sh"
campus_cmake_args=()
if [[ -n "${CAMPUS_DEPS_ROOT:-}" ]]; then
  campus_cmake_args+=("-DCMAKE_PREFIX_PATH=$CAMPUS_DEPS_ROOT")
  campus_cmake_args+=("-DSQLite3_INCLUDE_DIR=$CAMPUS_DEPS_ROOT/include")
  campus_cmake_args+=("-DSQLite3_LIBRARY=$CAMPUS_DEPS_ROOT/lib/x86_64-linux-gnu/libsqlite3.a")
fi
cmake -S "$campus_project_dir" -B "$campus_project_dir/build" \
  "${campus_cmake_args[@]}" "$@"
cmake --build "$campus_project_dir/build" -j "${CAMPUS_BUILD_JOBS:-4}"
ctest --test-dir "$campus_project_dir/build" --output-on-failure
