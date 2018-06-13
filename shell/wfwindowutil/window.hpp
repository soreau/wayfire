#ifndef COMMON_HPP
#define COMMON_HPP

#include <wayland-client.h>
#include "xdg-shell-unstable-v6-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#include "wayfire-shell-client-protocol.h"
#include <iostream>
#include <functional>
#include <vector>
#include <map>
#include <cairo.h>
#include "config.h"

struct wayfire_output;
struct wl_cursor;
struct wayfire_display
{
    wl_compositor *compositor = nullptr;
    wl_display    *display = nullptr;
    wl_shm        *shm = nullptr;

    wl_seat       *seat = nullptr;
    wl_pointer    *pointer = nullptr;

    zxdg_shell_v6          *zxdg_shell = nullptr;
    zwf_shell_manager_v1   *zwf_shell_manager = nullptr;
    zxdg_output_manager_v1 *zxdg_output_manager = nullptr;

    wayfire_display(std::function<void(wayfire_output*)> new_output_cb);
    ~wayfire_display();

    std::map<uint32_t, wayfire_output*> name_to_wayfire_output;

    wl_cursor *cursor = nullptr;
    wl_surface *cursor_surface = nullptr;

    bool load_cursor();
    void show_default_cursor(uint32_t serial);

    std::function<void(wayfire_output*)> new_output_callback;
};

struct wayfire_window;
struct wayfire_output
{
    wayfire_display *display;

    wl_output *handle = nullptr;
    zxdg_output_v1 *zxdg_output = nullptr;
    zwf_output_v1 *zwf = nullptr;

    std::function<void(wayfire_output*)> destroyed_callback;
    std::function<void(wayfire_output*, int32_t, int32_t)> resized_callback;

    std::vector<wayfire_window*> windows;

    wayfire_output(wayfire_display *display, wl_output *);
    ~wayfire_output();

    int scale = 1;
    void set_scale(int scale);

    /* configured is called when the window is first configured,
     * so the rendering process for the window can start then */
    wayfire_window* create_window(int width, int height,
                                  std::function<void()> configured);
};

struct wayfire_window
{
	wl_surface        *surface = nullptr;
    zxdg_surface_v6   *xdg_surface = nullptr;
    zxdg_toplevel_v6  *toplevel = nullptr;
    zwf_wm_surface_v1 *zwf = nullptr;

    int scale = 1;
    void set_scale(int scale);

    std::function<void(wl_pointer*, uint32_t, int x, int y)> pointer_enter;
    std::function<void()> pointer_leave;
    std::function<void(int x, int y)> pointer_move;
    std::function<void(uint32_t button, uint32_t state, int x, int y)> pointer_button;

    std::function<void(uint32_t time, int32_t id, uint32_t x, uint32_t y)> touch_down;
    std::function<void(int32_t id, uint32_t x, uint32_t y)> touch_motion;
    std::function<void(int32_t id)> touch_up;

    wayfire_output *output;

    cairo_surface_t *cairo_surface;

    bool configured = false;
    std::function<void()> first_configure;

    bool has_pointer_focus = false;

    wayfire_window();
    ~wayfire_window();

    void damage_commit();
};

/* the focused windows */
extern wayfire_window *current_touch_window, *current_pointer_window;

void render_rounded_rectangle(cairo_t *cr, int x, int y, int width, int height, double radius,
        double r, double g, double b, double a);
cairo_surface_t *cairo_try_load_png(const char *path);

#endif /* end of include guard: COMMON_HPP */
