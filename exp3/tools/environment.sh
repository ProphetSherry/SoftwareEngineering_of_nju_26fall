#!/usr/bin/env bash
# Source this file before building or running with locally unpacked dependencies.
# Normal system installations need no environment changes.
campus_environment_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ -z "${CAMPUS_DEPS_ROOT:-}" && -x "$campus_environment_dir/build/deps/usr/bin/cmake" ]]; then
  CAMPUS_DEPS_ROOT="$campus_environment_dir/build/deps/usr"
fi

# The bundled Qt installation does not include system input-method plugins.
# Include the Qt 5 system plugin directory while preserving caller settings.
campus_system_qt_plugins=/usr/lib/x86_64-linux-gnu/qt5/plugins
if [[ -d "$campus_system_qt_plugins/platforminputcontexts" ]]; then
  case ":${QT_PLUGIN_PATH:-}:" in
    *":$campus_system_qt_plugins:"*) ;;
    *) export QT_PLUGIN_PATH="${QT_PLUGIN_PATH:+$QT_PLUGIN_PATH:}$campus_system_qt_plugins" ;;
  esac
  if command -v fcitx5 >/dev/null 2>&1 &&
      [[ -f "$campus_system_qt_plugins/platforminputcontexts/libfcitx5platforminputcontextplugin.so" ]]; then
    export QT_IM_MODULE="${QT_IM_MODULE:-fcitx}"
    export XMODIFIERS="${XMODIFIERS:-@im=fcitx}"
  fi
fi
if [[ -n "${CAMPUS_DEPS_ROOT:-}" ]]; then
  export PATH="$CAMPUS_DEPS_ROOT/bin:$PATH"
  export LD_LIBRARY_PATH="$CAMPUS_DEPS_ROOT/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  export QT_QPA_PLATFORM_PLUGIN_PATH="$CAMPUS_DEPS_ROOT/lib/x86_64-linux-gnu/qt5/plugins"
fi
