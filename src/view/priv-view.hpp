#include <view.hpp>
#include <assert.h>

wayfire_surface_t* wf_surface_from_void(void *handle);
wayfire_view_t* wf_view_from_void(void *handle);
wlr_box get_scissor_box(wayfire_output *output, wlr_box *box);

void emit_view_map(wayfire_view view);
void emit_view_unmap(wayfire_view view);

void init_xdg_shell_v6();
void init_xwayland();
void init_layer_shell();

struct wlr_seat;
void xwayland_set_seat(wlr_seat *seat);
std::string xwayland_get_display();

void init_desktop_apis();

class decorator_base_t;
extern decorator_base_t *wf_decorator;
