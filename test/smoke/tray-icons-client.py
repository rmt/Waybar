#!/usr/bin/env python3
"""Private SNI fixtures and pixel assertions for asynchronous tray icon loading."""

import re
import sys
from pathlib import Path

import gi

from gi.repository import Gio, GLib

if sys.argv[1] == "--check-image":
    gi.require_version("GdkPixbuf", "2.0")
    from gi.repository import GdkPixbuf

    image = GdkPixbuf.Pixbuf.new_from_file(sys.argv[2])
    pixels = image.get_pixels()
    colors = {(255, 0, 0): 0, (0, 255, 0): 0, (0, 0, 255): 0,
              (255, 255, 0): 0, (255, 0, 255): 0, (0, 255, 255): 0}
    for y in range(min(96, image.get_height())):
        for x in range(image.get_width()):
            offset = y * image.get_rowstride() + x * image.get_n_channels()
            rgb = tuple(pixels[offset:offset + 3])
            if rgb in colors:
                colors[rgb] += 1
    red, green, blue, yellow, magenta, cyan = (colors[c] for c in colors)
    mode = sys.argv[3]
    area_scale = int(sys.argv[4]) ** 2 if len(sys.argv) > 4 else 1
    assert blue > 200 * area_scale and yellow > 900 * area_scale and green > 900 * area_scale, colors
    if mode == "normal":
        assert red > 100, colors
    elif mode == "attention":
        assert red == 0 and magenta == 0 and yellow > 1400 * area_scale, colors
    elif mode == "symbolic":
        assert red == 0 and green > 1400 * area_scale, colors
    elif mode == "symbolic-cyan":
        assert red == 0 and cyan > 700 * area_scale, colors
    elif mode == "pixmap":
        assert red == 0 and magenta > 100, colors
    print(f"Tray pixels {mode}: {colors}")
    sys.exit(0)

root = Path(sys.argv[1])
assets = root / "assets"
assets.mkdir()
for name, color in [("red", "#ff0000"), ("blue", "#0000ff"),
                    ("yellow", "#ffff00"), ("probe-symbolic", "#bebebe")]:
    (assets / f"{name}.svg").write_text(
        f'<svg xmlns="http://www.w3.org/2000/svg" width="32" height="32">'
        f'<rect width="32" height="32" fill="{color}"/></svg>')

interface_name = "org.kde.StatusNotifierItem"
xml = (Path(__file__).resolve().parents[2] / "protocol/dbus-status-notifier-item.xml").read_text()
# The fixture has no menu; do not make libdbusmenu fetch a nonexistent one.
xml = re.sub(r"\s*<property name='Menu'.*?/>", "", xml)
interface = Gio.DBusNodeInfo.new_for_xml(xml).interfaces[0]
connection = Gio.bus_get_sync(Gio.BusType.SESSION, None)
loop = GLib.MainLoop()
mode = "normal"


def pixmap(red, green, blue):
    return [(32, 32, bytes([255, red, green, blue]) * (32 * 32))]


class Item:
    def __init__(self, identifier, path):
        self.identifier = identifier
        self.path = path
        connection.register_object(path, interface, self.method, self.get_property, None)

    def method(self, conn, sender, path, iface, name, params, invocation):
        invocation.return_value(None)

    def get_property(self, conn, sender, path, iface, name):
        ordinary = self.identifier == "probe"
        values = {
            "Category": ("s", "ApplicationStatus"),
            "Id": ("s", self.identifier), "Title": ("s", self.identifier),
            "Status": ("s", "NeedsAttention" if ordinary and mode == "attention" else "Active"),
            "IconThemePath": ("s", str(assets)),
            "IconName": ("s", str(assets / "red.svg") if ordinary else "missing-remote-icon"),
            "IconPixmap": ("a(iiay)", [] if ordinary else pixmap(0, 255, 0)),
            "OverlayIconName": ("s", str(assets / "blue.svg") if ordinary else ""),
            "OverlayIconPixmap": ("a(iiay)", []),
            "AttentionIconName": ("s", str(assets / "yellow.svg")),
            "AttentionIconPixmap": ("a(iiay)", []), "AttentionMovieName": ("s", ""),
            "ToolTip": ("(sa(iiay)ss)", ("", [], self.identifier, "")),
            "ItemIsMenu": ("b", False),
        }
        if ordinary and mode == "symbolic":
            values["IconName"] = ("s", "probe-symbolic")
        if ordinary and mode == "pixmap":
            values["IconName"] = ("s", "missing-base-icon")
            values["IconPixmap"] = ("a(iiay)", pixmap(255, 0, 255))
            values["OverlayIconName"] = ("s", "missing-overlay-icon")
            values["OverlayIconPixmap"] = ("a(iiay)", pixmap(0, 0, 255))
        signature, value = values[name]
        return GLib.Variant(signature, value)

    def update(self):
        for signal in ["NewIcon", "NewAttentionIcon", "NewOverlayIcon"]:
            connection.emit_signal(None, self.path, interface_name, signal, None)
        status = "NeedsAttention" if mode == "attention" else "Active"
        connection.emit_signal(None, self.path, interface_name, "NewStatus",
                               GLib.Variant("(s)", (status,)))


items = [Item("probe", "/Probe"), Item("custom", "/Custom"), Item("missing-custom", "/Missing")]


def appeared(conn, name, owner):
    for item in items:
        connection.call("org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
                        "org.kde.StatusNotifierWatcher", "RegisterStatusNotifierItem",
                        GLib.Variant("(s)", (item.path,)), None,
                        Gio.DBusCallFlags.NONE, -1, None, None)


watch = Gio.bus_watch_name_on_connection(connection, "org.kde.StatusNotifierWatcher",
                                        Gio.BusNameWatcherFlags.NONE, appeared, None)


def poll():
    global mode
    if (root / "quit").exists():
        connection.close_sync(None)
        loop.quit()
        return False
    requested = (root / "mode").read_text().strip() if (root / "mode").exists() else "normal"
    if requested != mode:
        mode = requested
        items[0].update()
    return True


(root / "ready").touch()
GLib.timeout_add(20, poll)
loop.run()
