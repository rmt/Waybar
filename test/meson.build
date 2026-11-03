test_inc = include_directories('../include')

test_dep = [
    catch2,
    fmt,
    gtkmm,
    jsoncpp,
    spdlog,
]

test_src = files(
    'main.cpp',
    'config.cpp',
    'taskbar_dnd.cpp',
    'fit_label.cpp',
    '../src/config.cpp',
)

waybar_test = executable(
    'waybar_test',
    test_src,
    dependencies: test_dep,
    include_directories: test_inc,
)

test(
    'waybar',
    waybar_test,
    workdir: meson.project_source_root(),
)

async_icons_test = executable(
    'async_icons_test',
    'async_icons.cpp',
    '../src/util/async_icon.cpp',
    '../src/util/gtk_icon.cpp',
    dependencies: [gtkmm, fmt, spdlog],
    include_directories: test_inc,
)

test('async-icons', async_icons_test, workdir: meson.project_source_root())

subdir('utils')
subdir('hyprland')
