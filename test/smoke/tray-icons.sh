#!/usr/bin/env bash
# End-to-end SNI icons: async files/names, symbolic colors, attention/overlay,
# pixmap fallback, custom overrides, scale changes and reload/removal teardown.
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$DIR/../.." && pwd)"
if [ "${1:-}" != --private-session ]; then
    exec dbus-run-session -- bash "$0" --private-session
fi
# shellcheck source=lib.sh
export WAYBAR_BIN="${WAYBAR_BIN:-$ROOT/build/waybar}"
COMPOSITOR=sway
SCALE=1
source "$DIR/lib.sh"
work="$(mktemp -d /tmp/waybar-tray-icons.XXXXXX)"
client_pid=""; runtime=""
cleanup() {
    local result=$?
    trap - EXIT
    [ -z "$client_pid" ] || kill -TERM "$client_pid" 2>/dev/null || true
    [ -z "$client_pid" ] || wait "$client_pid" 2>/dev/null || true
    for pid in "$WAYBAR_PID" "$COMP_PID"; do
        if [ -n "$pid" ]; then
            kill -TERM "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
        fi
    done
    [ -z "$runtime" ] || rm -rf "$runtime"
    if [ "$result" = 0 ] && [ "${KEEP_ARTIFACTS:-0}" != 1 ]; then
        rm -rf "$work"
    else
        echo "Tray icon artifacts: $work"
    fi
    exit "$result"
}
trap cleanup EXIT
export GDK_BACKEND=wayland GIO_USE_VFS=local NO_AT_BRIDGE=1
unset WAYLAND_DISPLAY SWAYSOCK
smoke::setup
runtime="$XDG_RUNTIME_DIR"
smoke::start_compositor
python3 "$DIR/tray-icons-client.py" "$work" > "$work/client.log" 2>&1 &
client_pid=$!
for _ in $(seq 1 100); do [ ! -f "$work/ready" ] || break; sleep 0.1; done
[ -f "$work/ready" ]
python3 - "$work" <<'PY'
import json
import sys
from pathlib import Path
root = Path(sys.argv[1])
(root / 'config.jsonc').write_text(json.dumps({
    'layer': 'top', 'height': 32, 'modules-left': ['tray'], 'reload_style_on_change': True,
    'tray': {'icon-size': 32, 'spacing': 8, 'icons': {
        'custom': str(root / 'assets/yellow.svg'),
        'missing-custom': str(root / 'assets/nonexistent.svg')}}}))
PY
printf '* { color: #00ff00; } window#waybar { background: #202020; }\n' > "$work/style.css"
SMOKE_LOG="$work/waybar.log"
"${WAYBAR_BIN:-$ROOT/build/waybar}" -c "$work/config.jsonc" -s "$work/style.css" \
    > "$SMOKE_LOG" 2>&1 &
WAYBAR_PID=$!

output_scale=1
capture() {
    local mode="$1" requested="${2:-$1}"
    printf '%s\n' "$requested" > "$work/mode"
    for _ in $(seq 1 70); do
        smoke::assert_alive >/dev/null
        grim "$work/$mode.png"
        if python3 "$DIR/tray-icons-client.py" --check-image "$work/$mode.png" "$mode" "$output_scale" \
                > "$work/pixels.log" 2>&1; then
            echo "Tray pixels correct: $mode"
            return 0
        fi
        sleep 0.1
    done
    echo "Tray pixel assertion failed: $mode (see $work/pixels.log)" >&2
    return 1
}

capture normal
capture attention
capture symbolic
# Recolor solely through a live CSS change: no SNI property notification should be needed.
printf '* { color: #00ffff; } window#waybar { background: #202020; }\n' > "$work/style.css"
capture symbolic-cyan symbolic
printf '* { color: #00ff00; } window#waybar { background: #202020; }\n' > "$work/style.css"
capture symbolic
capture pixmap
# Supersede several requests before completion; only the final normal icon may win.
for mode in symbolic pixmap attention normal; do
    printf '%s\n' "$mode" > "$work/mode"
    sleep 0.03
done
capture normal
swaymsg 'output HEADLESS-1 scale 2' >/dev/null
output_scale=2
sleep 0.3
capture normal
smoke::reload 1
capture normal
# Drop all bus registrations while new image requests may still be pending.
printf 'symbolic\n' > "$work/mode"
touch "$work/quit"
wait "$client_pid"; client_pid=""
sleep 0.3
smoke::assert_alive
smoke::assert_clean_exit
echo 'Tray icon fallback/composition, symbolic CSS updates, HiDPI and lifecycle tests passed.'
