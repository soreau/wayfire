subdir('wfwindowutil')

shell_deps = [wf_protos, wfconfig, wf_window_util,
      freetype2, cairo, wayland_client, wayland_cursor]

subdir('background')
#
#
#panel = executable('wayfire-shell-panel',
#    ['panel.cpp', 'widgets.cpp', 'net.cpp', 'main.cpp'],
#    dependencies: shell_deps + [gio, threads],
#    include_directories: wayfire_conf_inc,
#    install: true,
#    install_dir: 'lib/wayfire')
#

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
#if conf_data.get('BUILD_WITH_ALSA', false)
#  sound_popup = executable('wayfire-sound-popup',
#      ['sound-popup.cpp'],
#      dependencies: shell_deps + [alsa],
#      include_directories: wayfire_conf_inc,
#      install: true,
#      install_dir: 'lib/wayfire')
#endif
#
#
#
#
