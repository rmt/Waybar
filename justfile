default: build

# Configure an existing build directory as needed, then compile.
build:
    meson setup build
    meson compile -C build

# Start with a fresh Meson build directory.
configure:
    rm -rf build
    meson setup build

# Alias for configure.
setup: configure
