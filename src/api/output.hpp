#ifndef OUTPUT_HPP
#define OUTPUT_HPP

#include "plugin.hpp"
#include "object.hpp"
#include "view.hpp"

#include <vector>
#include <unordered_map>
#include <unordered_set>

extern "C"
{
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_seat.h>
}

struct plugin_manager;

class workspace_manager;
class render_manager;

class wayfire_output : public wf_object_base
{
    friend class wayfire_core;

    private:
       std::unordered_map<std::string, std::vector<signal_callback_t*>> signals;
       std::unordered_multiset<wayfire_grab_interface> active_plugins;

       plugin_manager *plugin;
       wayfire_view active_view, last_active_toplevel;

       wl_listener destroy_listener;
       signal_callback_t unmap_view_cb;

       wf_option_callback config_mode_changed,
                          config_transform_changed,
                          config_position_changed,
                          config_scale_changed;

       wf_option mode_opt, scale_opt, transform_opt, position_opt;

       void set_initial_mode();
       void set_initial_scale();
       void set_initial_transform();
       void set_initial_position();

       void set_position(std::string pos);
       void refresh_custom_modes();
       void add_custom_mode(std::string mode);

    public:
       int id;
       bool destroyed = false;
       wlr_output* handle;
       std::tuple<int, int> get_screen_size();

       render_manager *render;
       workspace_manager *workspace;

       wayfire_output(wlr_output*, wayfire_config *config);
       ~wayfire_output();

       std::string to_string() const;

       /* output-local geometry of the output */
       wf_geometry get_relative_geometry();

       /* geometry with respect to the output-layout */
       wf_geometry get_full_geometry();

       void set_transform(wl_output_transform new_transform);
       wl_output_transform get_transform();

       /* return true if mode switch has succeeded */
       bool set_mode(std::string mode);
       bool set_mode(uint32_t width, uint32_t height, uint32_t refresh_mHz);

       void set_scale(double scale);
       void set_position(wf_point p);

       /* makes sure that the pointer is inside the output's geometry */
       void ensure_pointer();

       /* in output-local coordinates */
       std::tuple<int, int> get_cursor_position();

       /* @param break_fs - lower fullscreen windows if any */
       bool activate_plugin  (wayfire_grab_interface owner, bool lower_fs = true);
       bool deactivate_plugin(wayfire_grab_interface owner);
       bool is_plugin_active (owner_t owner_name);

       void activate();
       void deactivate();

       wayfire_view get_top_view();
       wayfire_view get_active_view() { return active_view; }
       wayfire_view get_view_at_point(int x, int y);

       void attach_view(wayfire_view v);
       void detach_view(wayfire_view v);

       /* TODO: unexport, or move to input_manager */
       void set_keyboard_focus(wlr_surface *surface, wlr_seat *seat);

       /* sets keyboard focus and active_view */
       void set_active_view(wayfire_view v, wlr_seat *seat = nullptr);

       /* same as set_active_view(), but will bring the view to the front */
       void focus_view(wayfire_view v, wlr_seat *seat = nullptr);

       void bring_to_front(wayfire_view v);

       /* force refocus the topmost view in one of the layers marked in layers
        * and which isn't skip_view */
       void refocus(wayfire_view skip_view, uint32_t layers);

       wf_binding *add_key(wf_option key, key_callback *);
       wf_binding *add_axis(wf_option axis, axis_callback *);
       wf_binding *add_touch(wf_option mod, touch_callback *);
       wf_binding *add_button(wf_option button, button_callback *);
       wf_binding *add_gesture(wf_option gesture, gesture_callback *);
       wf_binding *add_activator(wf_option activator, activator_callback *);

       /* remove the given binding, regardless of its type */
       void rem_binding(wf_binding *binding);

       /* remove all the bindings that have the given callback,
        * regardless of the type (key/button/etc) */
       void rem_binding(void *callback);

       /* send cancel to all active plugins, NOT API */
       void break_active_plugins();

       /* return an active wayfire_grab_interface on this output
        * which has grabbed the input. If none, then return nullptr
        * NOT API */
       wayfire_grab_interface get_input_grab_interface();
};
#endif /* end of include guard: OUTPUT_HPP */
