background_deps = shell_deps

if conf_data.get('BUILD_WITH_PIXBUF', false)
  background_deps += [gdk, pixbuf]
endif

background = executable('wayfire-shell-background',
    ['background.cpp'],
    dependencies: background_deps,
    include_directories: wayfire_conf_inc,
    install: true,
    install_dir: 'lib/wayfire')
