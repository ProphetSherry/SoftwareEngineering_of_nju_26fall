#!/usr/bin/env bash
# Start Fcitx5 on the same session bus as Qt. WSLg rejects its Wayland IM addon.
set -euo pipefail
if [[ ! -x /usr/bin/fcitx5 ]]; then
  printf 'Fcitx5 is not installed.\n' >&2
  exit 1
fi
campus_fcitx_running() {
  local campus_bus_reply
  campus_bus_reply="$(/usr/bin/timeout 2s /usr/bin/dbus-send --session --print-reply \
    --dest=org.freedesktop.DBus /org/freedesktop/DBus \
    org.freedesktop.DBus.NameHasOwner string:org.fcitx.Fcitx5 2>/dev/null || true)"
  [[ "$campus_bus_reply" == *"boolean true"* ]]
}
if campus_fcitx_running; then
  exit 0
fi
campus_fcitx_args=(-d)
if [[ -r /proc/sys/kernel/osrelease ]] &&
    [[ "$(</proc/sys/kernel/osrelease)" == *microsoft* ]]; then
  campus_fcitx_args+=(--disable wayland,waylandim)
fi
campus_fcitx_log="${TMPDIR:-/tmp}/campus-fcitx5-${UID}.log"
/usr/bin/env -u LD_LIBRARY_PATH -u QT_PLUGIN_PATH \
  -u QT_QPA_PLATFORM_PLUGIN_PATH PATH=/usr/bin:/bin \
  /usr/bin/fcitx5 "${campus_fcitx_args[@]}" > "$campus_fcitx_log" 2>&1
for campus_attempt in {1..20}; do
  /usr/bin/sleep 0.1
  if campus_fcitx_running; then
    exit 0
  fi
done
printf 'Fcitx5 did not connect to the session bus. Log: %s\n' "$campus_fcitx_log" >&2
exit 1
