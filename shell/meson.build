subdir('wfwindowutil')

shell_deps = [wf_protos, wfconfig, wf_window_util,
      freetype2, cairo, wayland_client, wayland_cursor]

subdir('background')
subdir('panel')

## TODO: do we need vkeyboard anymore?
## we should support layer-shell and virtual-keyboard protocols,
## maybe an external keyboard would provide much more features?
#
##vkeyboard = executable('wayfire-virtual-keyboard',
##    ['vkeyboard.cpp'],
##    dependencies: shell_deps + [libevdev],
##    include_directories: wayfire_conf_inc,
##    install: true,
##    install_dir: 'lib/wayfire')
##

