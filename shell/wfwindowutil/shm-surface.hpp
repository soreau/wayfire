#ifndef SHM_SURFACE_HPP
#define SHM_SURFACE_HPP

#include "window.hpp"

/* Once we create the window, we should wait until we get a zxdg_configure event, afterwards we can draw/whatever */
wayfire_window* create_shm_window(wayfire_display *display,
                                  uint32_t width, uint32_t height,
                                  std::function<void()> configured);

#endif /* end of include guard: SHM_SURFACE_HPP */
