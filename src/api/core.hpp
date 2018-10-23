#ifndef CORE_HPP
#define CORE_HPP

#include "object.hpp"

#include <functional>
#include <memory>
#include <vector>
#include <map>
#include <nonstd/observer_ptr.h>

extern "C"
{
#include <wlr/backend.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_output_layout.h>

#define static
#include <wlr/types/wlr_compositor.h>
#undef static

struct wlr_data_device_manager;
struct wlr_linux_dmabuf_v1;
struct wlr_gamma_control_manager;
struct wlr_screenshooter;
struct wlr_xdg_output_manager_v1;
struct wlr_export_dmabuf_manager_v1;
struct wlr_server_decoration_manager;
struct wlr_input_inhibit_manager;
struct wayfire_shell;
struct wlr_virtual_keyboard_manager_v1;
struct wlr_idle;
struct wlr_idle_inhibit_manager_v1;
struct wlr_screencopy_manager_v1;

#include <wayland-server.h>
}

class decorator_base_t;
class input_manager;
class wayfire_config;
class wayfire_output;
class wayfire_view_t;
class wayfire_surface_t;

using wayfire_view = nonstd::observer_ptr<wayfire_view_t>;
using output_callback_proc = std::function<void(wayfire_output *)>;

struct wf_server_decoration;

class wayfire_core : public wf_object_base
{
        friend struct plugin_manager;
        friend class wayfire_output;

        wl_listener output_layout_changed;
        wl_listener decoration_created;
        wl_listener vkbd_created;

        wl_listener input_inhibit_activated, input_inhibit_deactivated;

        wayfire_output *active_output;
        std::map<wlr_output*, wayfire_output*> outputs;

        std::vector<std::unique_ptr<wayfire_view_t>> views;

        void configure(wayfire_config *config);

        int times_wake = 0;
        uint32_t focused_layer = 0;

    public:
        std::map<wlr_surface*, uint32_t> uses_csd;

        std::vector<wl_resource*> shell_clients;
        wayfire_config *config;

        wl_display *display;
        wl_event_loop *ev_loop;
        wlr_backend *backend;
        wlr_renderer *renderer;
        wlr_output_layout *output_layout;
        wlr_compositor *compositor;

        struct
        {
            wlr_data_device_manager *data_device;
            wlr_gamma_control_manager *gamma;
            wlr_screenshooter *screenshooter;
            wlr_screencopy_manager_v1 *screencopy;
            wlr_linux_dmabuf_v1 *linux_dmabuf;
            wlr_export_dmabuf_manager_v1 *export_dmabuf;
            wlr_server_decoration_manager *decorator_manager;
            wlr_xdg_output_manager_v1 *output_manager;
            wlr_virtual_keyboard_manager_v1 *vkbd_manager;
            wlr_input_inhibit_manager *input_inhibit;
            wlr_idle *idle;
            wlr_idle_inhibit_manager_v1 *idle_inhibit;
            wayfire_shell *wf_shell;
        } protocols;

        std::string wayland_display, xwayland_display;

        input_manager *input;

        std::string to_string() const { return "wayfire-core"; }

        void init(wayfire_config *config);
        void wake();
        void sleep();
        void refocus_active_output_active_view();

        wlr_seat *get_current_seat();

        uint32_t get_keyboard_modifiers();

        void set_cursor(std::string name);

        /* no such coordinate will ever realistically be used for input */
        static const int invalid_coordinate = -123456789;

        /* in output-layout-local coordinates */
        std::tuple<int, int> get_cursor_position();

        /* in output-layout-local coordinates */
        std::tuple<int, int> get_touch_position(int id);

        wayfire_surface_t *get_cursor_focus();
        wayfire_surface_t *get_touch_focus();

        void add_view(std::unique_ptr<wayfire_view_t> view);

        wayfire_view find_view(wayfire_surface_t *);
        wayfire_view find_view(uint32_t id);

        /* completely destroy a view */
        void erase_view(wayfire_view view);

        /* brings the view to the top
         * and also focuses its output */
        void focus_view(wayfire_view win, wlr_seat *seat);
        void move_view_to_output(wayfire_view v, wayfire_output *new_output);

        void add_output(wlr_output *output);
        wayfire_output *get_output(wlr_output *output);
        wayfire_output *get_output(std::string name);

        void focus_output(wayfire_output *o);
        void remove_output(wayfire_output *o);

        wayfire_output *get_active_output();
        wayfire_output *get_next_output(wayfire_output *output);
        wayfire_output *get_output_at(int x, int y);
        size_t          get_num_outputs();

        void for_each_output(output_callback_proc);

        void focus_layer(uint32_t layer);
        uint32_t get_focused_layer();

        void run(const char *command);

        int vwidth, vheight;

        std::string shadersrc;
        bool run_panel;
};

extern wayfire_core *core;
#endif // CORE_HPP
