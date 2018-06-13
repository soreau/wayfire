libwfwindowutil = static_library('wfwindow',
    ['window.cpp', 'shm-surface.cpp'],
    dependencies: [cairo, wf_protos],
    include_directories: wayfire_conf_inc)


wfwinheaders = include_directories('.')
wf_window_util = declare_dependency(link_with: libwfwindowutil, include_directories: wfwinheaders)

