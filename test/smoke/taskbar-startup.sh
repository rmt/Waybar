#!/usr/bin/env bash
# Regress cold taskbar lookups during gtk-layer-shell's initial configure wait.
# No cache flushing, display access or system installation is required.
# Dependencies: sway, dbus-run-session, Python GTK3, a C compiler, grim.
# Usage: WAYBAR_BIN=$PWD/build/waybar SWAY_BIN=/usr/bin/sway bash test/smoke/taskbar-startup.sh
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$DIR/../.." && pwd)"
if [ "${1:-}" != --private-session ]; then
    exec dbus-run-session -- bash "$0" --private-session
fi

work="$(mktemp -d /tmp/waybar-taskbar-startup.XXXXXX)"
export XDG_RUNTIME_DIR="$work/runtime"
mkdir -m 700 "$XDG_RUNTIME_DIR"
export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1
export LIBGL_ALWAYS_SOFTWARE=1 GDK_BACKEND=wayland GIO_USE_VFS=local NO_AT_BRIDGE=1
unset WAYLAND_DISPLAY SWAYSOCK
bar_pid=""; client_pid=""; comp_pid=""
cleanup() {
    local result=$?
    trap - EXIT
    for pid in "$bar_pid" "$client_pid" "$comp_pid"; do
        if [ -n "$pid" ]; then
            kill -TERM "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
        fi
    done
    if [ "$result" = 0 ] && [ "${KEEP_ARTIFACTS:-0}" != 1 ]; then
        rm -rf "$work"
    else
        echo "Taskbar startup artifacts: $work"
    fi
    exit "$result"
}
trap cleanup EXIT

wait_for() {
    local description="$1"; shift
    for _ in $(seq 1 150); do
        if "$@"; then return 0; fi
        sleep 0.1
    done
    echo "Timed out: $description" >&2
    return 1
}

"${CC:-cc}" -shared -fPIC "$DIR/taskbar-startup-delay.c" -pthread -ldl -o "$work/delay.so"
printf 'output HEADLESS-1 mode 1280x720\nseat seat0 fallback true\n' > "$work/sway.conf"
"${SWAY_BIN:-sway}" -c "$work/sway.conf" > "$work/sway.log" 2>&1 &
comp_pid=$!
wait_for "Wayland socket" bash -c 'find "$XDG_RUNTIME_DIR" -maxdepth 1 -type s -name "wayland-*" | grep -q .'
export WAYLAND_DISPLAY
WAYLAND_DISPLAY="$(find "$XDG_RUNTIME_DIR" -maxdepth 1 -type s -name 'wayland-*' -printf '%f\n' | head -1)"
[ -n "$WAYLAND_DISPLAY" ]
wait_for "Sway IPC socket" bash -c 'find "$XDG_RUNTIME_DIR" -maxdepth 1 -type s -name "sway-ipc.*.sock" | grep -q .'
export SWAYSOCK
SWAYSOCK="$(find "$XDG_RUNTIME_DIR" -maxdepth 1 -type s -name 'sway-ipc.*.sock' -print -quit)"

if [ -x "$ROOT/build/test/async_icons_test" ]; then
    "$ROOT/build/test/async_icons_test"
fi

python3 "$DIR/taskbar-startup-client.py" "$work" > "$work/client.log" 2>&1 &
client_pid=$!
wait_for "client windows" test -f "$work/ready"
cat > "$work/config.jsonc" <<'EOF'
{
  "layer": "top", "height": 32,
  "modules-left": ["wlr/taskbar"], "modules-right": ["custom/marker"],
  "wlr/taskbar": {"format": "{icon} {name}", "icon-size": 16},
  "custom/marker": {"format": "Taskbar startup test"}
}
EOF
cat > "$work/style.css" <<'EOF'
* { font-family: sans-serif; font-size: 14px; }
window#waybar { background: #202020; color: white; }
#taskbar button { padding: 0 8px; color: white; }
EOF
LD_PRELOAD="$work/delay.so${LD_PRELOAD:+:$LD_PRELOAD}" \
    "${WAYBAR_BIN:-$ROOT/build/waybar}" -l trace -c "$work/config.jsonc" -s "$work/style.css" \
    > "$work/waybar.log" 2>&1 &
bar_pid=$!
wait_for "deliberately slow desktop search" grep -q TASKBAR_TEST_SEARCH_DELAY_BEGIN "$work/waybar.log"
# These events arrive while a worker is blocked. The UI must process them before
# the delayed search returns, and reload must cancel old completion callbacks.
touch "$work/close-one"
wait_for "client closure/title change" test -f "$work/closed-one"
kill -USR2 "$bar_pid"
wait_for "second bar after reload" bash -c '[ "$(grep -c "metadata enabled after first draw" "$1")" -ge 2 ]' _ "$work/waybar.log"
sleep 1
swaymsg 'output HEADLESS-1 scale 2' >/dev/null
sleep 1
kill -0 "$bar_pid"
grim "$work/render.png"
kill -INT "$bar_pid"
wait "$bar_pid"; bar_pid=""

# A fresh process gets a fresh injected delay. Clean exit must not wait for an
# uninterruptible desktop search, nor leave a late callback accessing the destroyed taskbar.
LD_PRELOAD="$work/delay.so${LD_PRELOAD:+:$LD_PRELOAD}" \
    "${WAYBAR_BIN:-$ROOT/build/waybar}" -l trace -c "$work/config.jsonc" -s "$work/style.css" \
    > "$work/early-exit.log" 2>&1 &
bar_pid=$!
wait_for "second slow lookup" grep -q TASKBAR_TEST_SEARCH_DELAY_BEGIN "$work/early-exit.log"
kill -INT "$bar_pid"
wait "$bar_pid"; bar_pid=""
if grep -q TASKBAR_TEST_SEARCH_DELAY_END "$work/early-exit.log"; then
    echo "UI exit waited for the delayed desktop search" >&2
    exit 1
fi
touch "$work/quit"
wait "$client_pid"; client_pid=""

python3 - "$work/waybar.log" <<'PY'
import re
import sys
from pathlib import Path

lines = Path(sys.argv[1]).read_text().splitlines()
active = {}
draw_seen = False
main_thread = None
searches = 0
icons = 0
completed = 0
closed = set()
for line in lines:
    assert not re.search(r'Timed out waiting|Gtk-CRITICAL|AddressSanitizer|Segmentation fault', line), line
    thread_match = re.search(r'\[thread (\d+)\]', line)
    thread = thread_match[1] if thread_match else None
    if 'runtime GTK' in line:
        main_thread = thread
    if 'metadata enabled after first draw' in line:
        draw_seen = True
    closed_task = re.search(r'Task \((\d+)\).* closed$', line)
    if closed_task:
        closed.add(closed_task[1])
    begin = re.search(r'\[timing\] begin #(\d+) (.*)', line)
    if begin:
        scope_id, label = begin.groups()
        if label.startswith(('IconLoader::', 'g_desktop_app_info_search', 'Desktop-file candidates')):
            assert draw_seen, f'Lookup before first draw: {label}'
            if label.startswith(('IconLoader::image_load_icon', 'IconLoader::load_icon_async')):
                assert thread == main_thread, f'GTK icon operation on worker: {label}'
            else:
                assert thread != main_thread, f'Desktop discovery on UI thread: {label}'
            assert not any(t == thread and s.startswith(('Bar::window.show_all', 'Task::handle_', 'Taskbar::handle_toplevel_create'))
                           for t, s in active.values()), f'Lookup within protocol/map callback: {label}'
        if label.startswith(('Icon theme lookup', 'Icon async decode', 'Task icon applied', 'SNI icon applied')):
            assert thread == main_thread, f'GTK icon operation on worker: {label}'
        if label.startswith('Task icon applied'):
            icons += 1
        if label.startswith('Task desktop lookup completed'):
            assert thread == main_thread, 'Completion did not return to the UI thread'
            task_id = re.search(r'task=(\d+)', label)[1]
            assert task_id not in closed, 'Late completion for a closed task'
            completed += 1
        if label.startswith('g_desktop_app_info_search'):
            assert not label.endswith('app_id=unknown'), line
            searches += 1
        active[scope_id] = (thread, label)
    end = re.search(r'\[timing\] end #(\d+)', line)
    if end:
        active.pop(end[1], None)
assert searches > 0, 'Test never exercised desktop search'
assert icons > 0 and completed > 0, 'No completed metadata/icon application'
assert closed, 'Task closure was not exercised'
draws = [i for i, line in enumerate(lines) if 'metadata enabled after first draw' in line]
assert len(draws) >= 2, 'Reload was not exercised'
delay_begin = next(i for i, line in enumerate(lines) if 'TASKBAR_TEST_SEARCH_DELAY_BEGIN' in line)
delay_end = next(i for i, line in enumerate(lines) if 'TASKBAR_TEST_SEARCH_DELAY_END' in line)
assert delay_begin < draws[1] < delay_end, 'Reload waited for desktop search instead of remaining responsive'
assert any('scale factor changed, scheduling icon reload' in line for line in lines), 'Scale change was not exercised'
print('Taskbar background lookup passed: UI responsive during slow search; close/reload/scale/early exit safe; GTK stays on UI thread.')
PY
