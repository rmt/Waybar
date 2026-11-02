# gtk-layer-shell 0.10.1: false initial configure timeout

This patch is for **gtk-layer-shell**, not Waybar. It is a local diagnostic fix,
not an installed dependency or an automatic part of the Waybar build.

In 0.10.1, the initial configure wait checks its one-second deadline after
`wl_display_roundtrip()`, without checking whether that roundtrip received the
configure. Slow application callbacks dispatched by the roundtrip can push it
past the deadline. The library then warns and unmaps an already-configured window.
Waybar's force-commit workaround cannot remap that window.

The patch checks that the configure is still outstanding before timing out.
It retains the existing timeout for genuinely outstanding configures. It does
not address slow callbacks, nor turn the blocking roundtrip into a timed wait.

The included mock-compositor regression test dispatches a 1.2-second callback
during initial mapping. On unpatched 0.10.1, configure is received and acknowledged,
but the window is then incorrectly unmapped. With the patch it stays mapped.
The library suite passes with 42 successes and two expected failures.

Related report: https://github.com/Alexays/Waybar/issues/4978

## Reproduce the private build

From the Waybar checkout, with fresh destination directories:

```sh
git clone --depth 1 --branch v0.10.1 https://github.com/wmww/gtk-layer-shell.git \
    build/gtk-layer-shell-src
patch -p1 -d build/gtk-layer-shell-src \
    < contrib/gtk-layer-shell-timeout/fix-initial-configure-timeout.patch
meson setup build/gtk-layer-shell-test build/gtk-layer-shell-src \
    --buildtype=debugoptimized -Dtests=true -Dintrospection=false -Dvapi=false \
    -Ddocs=false -Dexamples=false
meson compile -C build/gtk-layer-shell-test
meson test -C build/gtk-layer-shell-test --no-rebuild --print-errorlogs -j 4
```

Stop the failed Waybar instance yourself, then launch the local binary with the
private library (no system installation required):

```sh
LD_LIBRARY_PATH="$PWD/build/gtk-layer-shell-test/src:/usr/local/lib" ./build/waybar
```

The `/usr/local/lib` entry is for the locally installed libcava; omit it if not
needed. All private dependency sources and binaries are under `build/` and are
removed by `just configure` / `just setup`. The patch here survives that reset.
