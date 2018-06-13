#include "window.hpp"
#include "shm-surface.hpp"
#include <cstring>
#include <algorithm>
#include <map>
#include <wayland-cursor.h>
#include <unistd.h>

wayfire_window* current_touch_window = nullptr, *current_pointer_window = nullptr;
size_t current_window_touch_points = 0;

int pointer_x, pointer_y;
void pointer_enter(void *data, struct wl_pointer *wl_pointer,
    uint32_t serial, struct wl_surface *surface, wl_fixed_t surface_x, wl_fixed_t surface_y)
{
    /* possibly an event for a surface we just destroyed */
    if (!surface)
        return;

    pointer_x = wl_fixed_to_int(surface_x);
    pointer_y = wl_fixed_to_int(surface_y);

    auto window = (wayfire_window*) wl_surface_get_user_data(surface);
    if (window && window->pointer_enter)
        window->pointer_enter(wl_pointer, serial,
                pointer_x * window->scale, pointer_y * window->scale);
    if (window)
    {
        current_pointer_window = window;
        window->has_pointer_focus = true;
    }
}

void pointer_leave(void *data, struct wl_pointer *wl_pointer, uint32_t serial,
    struct wl_surface *surface)
{
    /* possibly an event for a surface we just destroyed */
    if (!surface)
        return;

    auto window = (wayfire_window*) wl_surface_get_user_data(surface);
    if (window && window->pointer_leave)
    {
        window->pointer_leave();
        window->has_pointer_focus = false;
    }

    current_pointer_window = nullptr;
}

void pointer_motion(void *data, struct wl_pointer *wl_pointer, uint32_t time,
    wl_fixed_t surface_x, wl_fixed_t surface_y)
{
    pointer_x = wl_fixed_to_int(surface_x);
    pointer_y = wl_fixed_to_int(surface_y);
    if (current_pointer_window && current_pointer_window->pointer_move)
        current_pointer_window->pointer_move(pointer_x * current_pointer_window->scale,
                                             pointer_y * current_pointer_window->scale);
}

void pointer_button(void *data, struct wl_pointer *wl_pointer, uint32_t serial,
    uint32_t time, uint32_t button, uint32_t state)
{
    if (current_pointer_window && current_pointer_window->pointer_button)
        current_pointer_window->pointer_button(button, state,
                                               pointer_x * current_pointer_window->scale,
                                               pointer_y * current_pointer_window->scale);
}

void pointer_axis(void *data, struct wl_pointer *wl_pointer, uint32_t time,
    uint32_t axis, wl_fixed_t value)
{
}
void pointer_frame(void *data, struct wl_pointer *wl_pointer) {}
void pointer_axis_source(void *data, struct wl_pointer *wl_pointer,
        uint32_t axis_source) {}
void pointer_axis_stop(void *data, struct wl_pointer *wl_pointer, uint32_t time,
        uint32_t axis) {}
void pointer_axis_discrete(void *data, struct wl_pointer *wl_pointer, uint32_t axis,
        int32_t discrete) {}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete
};

static void touch_down(void *data, struct wl_touch *wl_touch,
                       uint32_t serial, uint32_t time, struct wl_surface *surface,
                       int32_t id, wl_fixed_t x, wl_fixed_t y)
{
    auto window = (wayfire_window*) wl_surface_get_user_data(surface);

    if (current_touch_window != window)
        current_window_touch_points = 0;

    current_touch_window = window;
    current_window_touch_points++;

    if (window->touch_down)
        window->touch_down(time, id,
                           wl_fixed_to_int(x) * window->scale,
                           wl_fixed_to_int(y) * window->scale);
}

static void
touch_up(void *data, struct wl_touch *wl_touch,
         uint32_t serial, uint32_t time, int32_t id)
{
    if (current_touch_window && current_touch_window->touch_down)
        current_touch_window->touch_up(id);

    current_window_touch_points--;
    if (!current_window_touch_points)
        current_touch_window = nullptr;
}

static void
touch_motion(void *data, struct wl_touch *wl_touch,
             uint32_t time, int32_t id, wl_fixed_t x, wl_fixed_t y)
{
    if (current_touch_window->touch_motion)
        current_touch_window->touch_motion(id,
                                           wl_fixed_to_int(x) * current_touch_window->scale,
                                           wl_fixed_to_int(y) * current_touch_window->scale);
}

static void
touch_frame(void *data, wl_touch *) {}

static void
touch_cancel(void *data, wl_touch *) {}

static const struct wl_touch_listener touch_listener = {
    touch_down,
    touch_up,
    touch_motion,
    touch_frame,
    touch_cancel,
    NULL,
    NULL
};


static void update_wl_output(wayfire_output *output)
{
    /* TODO: connect to xdg output manager and take info */
}

static void handle_wl_output_geometry(void *data, struct wl_output *wl_output,
                                   int32_t, int32_t, int32_t, int32_t, int32_t, const char*, const char*, int32_t)
{ }

static void handle_wl_output_mode(void *data, struct wl_output *wl_output, uint32_t flags, int32_t, int32_t, int32_t)
{ }

static void handle_wl_output_done(void *data, struct wl_output *wl_output)
{ }

static void handle_wl_output_scale(void *data, struct wl_output *wl_output, int32_t factor)
{
    auto wo = (wayfire_output*) wl_output_get_user_data(wl_output);
    wo->set_scale(factor);
}

const wl_output_listener output_listener =
{
    handle_wl_output_geometry,
    handle_wl_output_mode,
    handle_wl_output_done,
    handle_wl_output_scale
};
void handle_zxdg_ping (void *, zxdg_shell_v6 *shell, uint32_t serial)
{ zxdg_shell_v6_pong(shell, serial); }
const zxdg_shell_v6_listener zxdg_listener = { handle_zxdg_ping };

// listeners
void registry_add_object(void *data, struct wl_registry *registry, uint32_t name,
        const char *interface, uint32_t version)
{
    auto display = (wayfire_display*) data;

    if (strcmp(interface, wl_compositor_interface.name) == 0)
    {
        display->compositor = (wl_compositor*) wl_registry_bind(registry, name,
                                                                &wl_compositor_interface,
                                                                std::min(version, 3u));
    }
    else if (strcmp(interface, zxdg_shell_v6_interface.name) == 0)
    {
        display->zxdg_shell = (zxdg_shell_v6*) wl_registry_bind(registry, name, &zxdg_shell_v6_interface, std::min(version, 1u));
        zxdg_shell_v6_add_listener(display->zxdg_shell, &zxdg_listener, NULL);
    }
    else if (strcmp(interface, wl_seat_interface.name) == 0 && display->seat == NULL)
    {
        display->seat = (wl_seat*) wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 2u));
        display->pointer = wl_seat_get_pointer(display->seat);

        auto touch = wl_seat_get_touch(display->seat);

        if (display->pointer)
            wl_pointer_add_listener(display->pointer, &pointer_listener, NULL);

        if (touch)
            wl_touch_add_listener(touch, &touch_listener, NULL);
    }
    else if (strcmp(interface, wl_shm_interface.name) == 0)
    {
        display->shm = (wl_shm*) wl_registry_bind(registry, name, &wl_shm_interface, std::min(version, 1u));
    }
    else if (strcmp(interface, zwf_shell_manager_v1_interface.name) == 0)
    {
        std::cout << "bind zwf shell" << std::endl;
        display->zwf_shell_manager =
            (zwf_shell_manager_v1*) wl_registry_bind(registry, name,
                                                     &zwf_shell_manager_v1_interface,
                                                     std::min(version, 1u));
    }
    else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0)
    {
        display->zxdg_output_manager = (zxdg_output_manager_v1*)
            wl_registry_bind(registry, name, &zxdg_output_manager_v1_interface,
                             std::min(version, 2u));
    }
    else if (strcmp(interface, wl_output_interface.name) == 0)
    {
        std::cout << "bind wl_output" << std::endl;
        auto output = (wl_output*) wl_registry_bind(registry, name, &wl_output_interface,
                                                    std::min(version, 1u));
        // XXX: are we sure that the zwf_shell_manager will be created before the wl_output?
        display->name_to_wayfire_output[name] = new wayfire_output(display, output);
    }
}

void registry_remove_object(void *data, struct wl_registry *registry, uint32_t name)
{
    auto display = (wayfire_display*) data;
    if (display->name_to_wayfire_output.count(name))
    {
        delete display->name_to_wayfire_output[name];
        display->name_to_wayfire_output.erase(name);
    }
}

static struct wl_registry_listener registry_listener =
{
    &registry_add_object,
    &registry_remove_object
};

/* wayfire_display implementation */
wayfire_display::wayfire_display(std::function<void(wayfire_output*)> new_output_cb)
{
    this->new_output_callback = new_output_cb;
    display = wl_display_connect(NULL);

    if (!display)
    {
        std::cerr << "Failed to connect to display!" << std::endl;
        std::exit(-1);
    }

    wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, this);
    wl_display_roundtrip(display);

    if (!load_cursor())
    {
        std::cerr << "Failed to load cursor!" << std::endl;
        std::exit(-1);
    }
}

wayfire_display::~wayfire_display()
{
    zxdg_output_manager_v1_destroy(zxdg_output_manager);

    // TODO: we should also fix up all kinds of shells,
    // registry, etc. here
    wl_display_disconnect(display);
}

bool wayfire_display::load_cursor()
{
    auto cursor_theme = wl_cursor_theme_load(NULL, 16, shm);
    if (!cursor_theme) {
        std::cout << "failed to load cursor theme" << std::endl;
        return false;
    }

    const char* alternatives[] = {
        "left_ptr", "default",
        "top_left_arrow", "left-arrow"
    };

    cursor = NULL;
    for (int i = 0; i < 4 && !cursor; i++)
        cursor = wl_cursor_theme_get_cursor(cursor_theme, alternatives[i]);

    cursor_surface = wl_compositor_create_surface(compositor);
    if (!cursor || !cursor_surface) {
        std::cout << "failed to load cursor" << std::endl;
        return false;
    }

    return true;
}

void wayfire_display::show_default_cursor(uint32_t serial)
{
    auto image = cursor->images[0];
    auto buffer = wl_cursor_image_get_buffer(image);

    wl_surface_attach(cursor_surface, buffer, 0, 0);
    wl_surface_damage(cursor_surface, 0, 0, image->width, image->height);
    wl_surface_commit(cursor_surface);

    if (pointer)
    {
        wl_pointer_set_cursor(pointer, serial, cursor_surface,
                              image->hotspot_x, image->hotspot_y);
    }
}

/* zxdg_output impl */
static void zxdg_output_logical_position(void *data, struct zxdg_output_v1 *zxdg_output_v1,
                                         int32_t x, int32_t y)
{ }

static void zxdg_output_logical_size(void *data, struct zxdg_output_v1 *zxdg_output_v1,
                                     int32_t width, int32_t height)
{
    auto wo = (wayfire_output*) data;
    if (wo->resized_callback)
        wo->resized_callback(wo, width, height);
}

static void zxdg_output_done(void *data, struct zxdg_output_v1 *zxdg_output_v1) { }
static void zxdg_output_name(void *data, struct zxdg_output_v1 *zxdg_output_v1, const char *name) {}
static void zxdg_output_description(void *data, struct zxdg_output_v1 *zxdg_output_v1, const char *description) {}


const struct zxdg_output_v1_listener zxdg_output_v1_impl =
{
    zxdg_output_logical_position,
    zxdg_output_logical_size,
    zxdg_output_done,
    zxdg_output_name,
    zxdg_output_description
};


/* wayfire_output implementation */
wayfire_output::wayfire_output(wayfire_display *display, wl_output *output)
{
    this->display = display;
    this->handle = output;

    zxdg_output = zxdg_output_manager_v1_get_xdg_output(display->zxdg_output_manager, handle);
    zxdg_output_v1_add_listener(zxdg_output, &zxdg_output_v1_impl, this);

    zwf = zwf_shell_manager_v1_get_wf_output(display->zwf_shell_manager, output);

    if (display->new_output_callback)
        display->new_output_callback(this);
}

wayfire_output::~wayfire_output()
{
    if (destroyed_callback)
        destroyed_callback(this);

    zxdg_output_v1_destroy(zxdg_output);
}

void wayfire_output::set_scale(int scale)
{
    this->scale = scale;
}

wayfire_window* wayfire_output::create_window(int width, int height,
                                              std::function<void()> configured)
{
    auto window = create_shm_window(display, width * scale, height * scale,
                                    configured);

    window->output = this;
    window->set_scale(scale);

    return window;
}

/* wayfire_window impl */
wayfire_window::wayfire_window()
{
}

wayfire_window::~wayfire_window()
{
    if (current_pointer_window == this)
        current_pointer_window = nullptr;

    zwf_wm_surface_v1_destroy(zwf);
    zxdg_toplevel_v6_destroy(toplevel);
    zxdg_surface_v6_destroy(xdg_surface);
    wl_surface_destroy(surface);
    cairo_surface_destroy(cairo_surface);
}

void wayfire_window::set_scale(int scale)
{
    this->scale = scale;
    wl_surface_set_buffer_scale(surface, scale);
}

/* utility functions */
void render_rounded_rectangle(cairo_t *cr, int x, int y, int width, int height,
        double radius, double r, double g, double b, double a)
{
    double degrees = M_PI / 180.0;

    cairo_new_sub_path (cr);
    cairo_arc (cr, x + width - radius, y + radius, radius, -90 * degrees, 0 * degrees);
    cairo_arc (cr, x + width - radius, y + height - radius, radius, 0 * degrees, 90 * degrees);
    cairo_arc (cr, x + radius, y + height - radius, radius, 90 * degrees, 180 * degrees);
    cairo_arc (cr, x + radius, y + radius, radius, 180 * degrees, 270 * degrees);
    cairo_close_path (cr);

    cairo_set_source_rgba(cr, r, g, b, a);
    cairo_fill_preserve(cr);
}

cairo_surface_t *cairo_try_load_png(const char *path)
{
    if (access(path, F_OK) != -1) {
        return cairo_image_surface_create_from_png(path);
    } else {
        return NULL;
    }
}

