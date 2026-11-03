# Tracing a slow or invisible Waybar startup

Build with `just`, then stop your previous Waybar instance yourself. From the
checkout, capture the **local** binary rather than the installed `waybar`:

```sh
LD_LIBRARY_PATH="$PWD/build/gtk-layer-shell-test/src:/usr/local/lib" \
    ./build/waybar -l trace 2>&1 | tee /tmp/waybar-startup.log
```

The private library prefix uses the local timeout fix described in
[`gtk-layer-shell-timeout/README.md`](gtk-layer-shell-timeout/README.md). To
reproduce with the system gtk-layer-shell, use `LD_LIBRARY_PATH=/usr/local/lib`
instead. The libcava path can be omitted if your system loader finds it already.
Do not launch both copies against the same configuration at once.

## Deferred taskbar metadata

The taskbar no longer resolves a synthetic `unknown` app ID while a toplevel is
being constructed. App-ID/title events record the current state and mark metadata
dirty. After the bar's first draw, idle work submits desktop-file discovery to
GIO's worker pool, with at most two current requests per taskbar. Each worker
owns copies of the app ID/title and lookup paths; it never accesses a Task or GTK
object. It tries the real app ID before a title-based fallback.

Results return to the main context. Changed IDs/titles, closed tasks and teardown
cancel obsolete requests, suppressing late completions without waiting for the
worker. Cancellation is checked between lookup steps; an already-running GIO
search may finish in the background because that API has no cancellation hook.

GTK icon-theme access and widget updates remain on the main thread. The taskbar
and tray now use GTK's native asynchronous icon APIs to read, render and scale
selected icons. File candidates are checked asynchronously before decoding.
Theme lookup itself can still perform synchronous work, so it has a separate
`Icon theme lookup` timing. GTK initialization is unchanged.

Icon requests are cancelled when superseded, resized or destroyed. Native async
operations are always finished to release their results, but cancelled requests
cannot call a destroyed receiver or overwrite a newer icon. Taskbar theme order,
SNI attention/normal/pixmap fallback, custom file/name overrides, symbolic colors,
overlay composition and HiDPI scaling are retained. Existing icons remain visible
while their replacements load. The legacy synchronous IconLoader API used by
other modules is not converted by this change.

A standalone test injects a two-second desktop lookup without flushing any
system caches. It verifies worker/UI thread separation and requires reload and
clean exit to succeed while the slow search is still running, as well as
exercising task closure and icon scale changes:

```sh
WAYBAR_BIN="$PWD/build/waybar" SWAY_BIN=/usr/bin/sway \
    bash test/smoke/taskbar-startup.sh
```

It requires Python GTK3, a C compiler, Sway, D-Bus and grim. The test uses a private
headless compositor/session and can run against the stock gtk-layer-shell.
It also runs `build/test/async_icons_test`, which tests file/name/pixmap fallbacks,
corrupt inputs, symbolic colors, aspect ratio, scaling, cancellation and stale
results. That suite skips when run without a display by ordinary `meson test`.

A separate real-SNI fixture checks attention icons, overlays, custom overrides,
missing-file/pixmap fallbacks, symbolic colors, scale changes, reload and removal
using screenshot pixel assertions:

```sh
WAYBAR_BIN="$PWD/build/waybar" SWAY_BIN=/usr/bin/sway \
    bash test/smoke/tray-icons.sh
```

## Reading the timings

`-l trace` enables `[timing]` messages and thread IDs. Normal log levels remain
quiet. Scope entries are flushed before the operation begins; a matching exit
reports `elapsed_ms` measured with a monotonic clock. Scope IDs correlate nested
or interleaved operations. `unwinding=true` means an exception escaped the scope.
Durations include nested operations: do not add parent and child timings.

The instrumentation covers:

- GTK initialization, config/CSS loading, portal construction and appearance callbacks;
- the Wayland registry roundtrip, output detection and bar construction;
- individual module setup and dispatched updates, including group children;
- `window.show_all()`, map/unmap/configure callbacks and the post-show idle callback;
- taskbar toplevel/app-ID/title callbacks, desktop-file candidate prefixes,
  `g_desktop_app_info_search()` fallback searches and icon loading;
- `Task desktop lookup worker` spans on worker threads and
  `Task desktop lookup completed` spans on the UI thread;
- `Icon theme lookup`, `Icon async decode scheduled` and `Icon async decode finished`
  spans on the UI thread; `Icon async decode latency_ms` reports the asynchronous
  wait, not time spent blocking the UI;
- `Task icon applied` and `SNI icon applied` spans for main-thread widget updates;
- tray watcher/proxy callbacks, icon updates and menu construction;
- synchronous Bluetooth manager creation and battery `ReadValue` calls.

Window snapshots include GTK visible/mapped/realized flags, mode, opacity and size.
An initial configure message alone does not prove the window is mapped. For
example, `show_all returned ... visible=true mapped=false` after the timeout
indicates that GTK still considers the widget visible but it has been unmapped.
A mapped GTK window is not proof that the compositor has presented a frame.

Extract the main timing and configure messages with:

```sh
grep -E '\[timing\]|Timed out|Bar configured' /tmp/waybar-startup.log
```

Find the long `elapsed_ms` spans and the innermost active operation during the
pause. These trace durations measure synchronous work, not the full latency of
an outstanding asynchronous request. If a long `window.show_all()` has no slow
nested Waybar scope, the pause is still inside GTK/Wayland or an uninstrumented
callback; the traces alone do not identify which.

For protocol-level correlation, repeat with `WAYLAND_DEBUG=client` added to the
environment. That records outgoing surface commits, incoming configure events,
and acknowledgements alongside the timings. Tracing changes timing and can
perturb startup races; compare traced and untraced runs.

Trace/debug output can include application titles, paths and other metadata.
Review logs before sharing them. No startup recovery or timeout behavior is
changed by the tracing itself.
