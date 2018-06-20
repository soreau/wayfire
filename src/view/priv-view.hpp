#ifndef PRIV_VIEW_HPP
#define PRIV_VIEW_HPP

#include <view.hpp>
#include <decorator.hpp>
#include <assert.h>

wayfire_surface_t* wf_surface_from_void(void *handle);
wayfire_view_t* wf_view_from_void(void *handle);
wlr_box wlr_box_from_pixman_box(const pixman_box32_t& box);

void emit_view_map(wayfire_view view);
void emit_view_unmap(wayfire_view view);

void init_xdg_shell_v6();
void init_xdg_shell();
void init_xwayland();
void init_layer_shell();

struct wlr_seat;
void xwayland_set_seat(wlr_seat *seat);
std::string xwayland_get_display();

void init_desktop_apis();

extern decorator_base_t *wf_decorator;

#endif /* end of include guard: PRIV_VIEW_HPP */

