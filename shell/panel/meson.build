panel = executable('wayfire-shell-panel',
    ['panel.cpp', 'widgets.cpp', 'net.cpp', 'main.cpp'],
    dependencies: shell_deps + [gio, threads],
    include_directories: wayfire_conf_inc,
    install: true,
    install_dir: 'lib/wayfire')
