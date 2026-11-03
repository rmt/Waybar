#!/usr/bin/env python3
"""Private GTK windows for taskbar startup/lifetime regression coverage."""

import sys
from pathlib import Path

import gi

from gi.repository import GLib

GLib.set_prgname("waybar-startup-probe")
gi.require_version("Gtk", "3.0")
from gi.repository import Gtk

control = Path(sys.argv[1])
windows = []
for index in range(3):
    window = Gtk.Window(title=f"Waybar startup probe {index}")
    window.set_default_size(240, 100)
    window.add(Gtk.Label(label=f"Startup probe {index}"))
    window.show_all()
    windows.append(window)


def ready():
    (control / "ready").touch()
    return False


def poll_control():
    if (control / "close-one").exists() and not (control / "closed-one").exists():
        windows[1].destroy()
        windows[0].set_title("Latest startup probe title")
        (control / "closed-one").touch()
    if (control / "quit").exists():
        for window in windows:
            window.destroy()
        Gtk.main_quit()
        return False
    return True


GLib.idle_add(ready)
GLib.timeout_add(50, poll_control)
Gtk.main()
