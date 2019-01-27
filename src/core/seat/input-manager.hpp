#ifndef INPUT_MANAGER_HPP
#define INPUT_MANAGER_HPP

#include <unordered_set>
#include <map>
#include <vector>

#include "seat.hpp"
#include "cursor.hpp"
#include "plugin.hpp"
#include "view.hpp"

extern "C"
{
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_idle.h>
#include <wlr/types/wlr_seat.h>
struct wlr_drag_icon;
}

struct wf_gesture_recognizer;
struct wlr_seat;

struct wf_touch;
struct wf_keyboard;

enum wf_binding_type
{
    WF_BINDING_KEY,
    WF_BINDING_BUTTON,
    WF_BINDING_AXIS,
    WF_BINDING_TOUCH,
    WF_BINDING_GESTURE,
    WF_BINDING_ACTIVATOR
};

struct wf_binding
{
    wf_option value;
    wf_binding_type type;
    wayfire_output *output;

    union {
        void *raw;
        key_callback *key;
        axis_callback *axis;
        touch_callback *touch;
        button_callback *button;
        gesture_callback *gesture;
        activator_callback *activator;
    } call;
};

using wf_binding_ptr = std::unique_ptr<wf_binding>;

/* TODO: most probably we want to split even more of input_manager's functionality into
 * wf_keyboard, wf_cursor and wf_touch */
class input_manager
{
    private:
        wayfire_grab_interface active_grab = nullptr;
        bool session_active = true;

        wl_listener input_device_created, new_drag_icon, request_set_cursor,
                    request_set_selection, request_set_primary_selection;


        signal_callback_t config_updated;

        int gesture_id;

        std::map<wf_binding_type, std::vector<std::unique_ptr<wf_binding>>> bindings;
        using binding_criteria = std::function<bool(wf_binding*)>;
        void rem_binding(binding_criteria criteria);

        bool is_touch_enabled();

        void create_seat();

        // returns the surface under the given global coordinates
        // if no such surface (return NULL), lx and ly are undefined
        wayfire_surface_t* input_surface_at(int x, int y,
            int& lx, int& ly);

        void set_touch_focus(wayfire_surface_t *surface, uint32_t time, int id, int lx, int ly);

        void update_drag_icons();

        std::vector<std::unique_ptr<wf_keyboard>> keyboards;
        std::vector<std::unique_ptr<wf_input_device>> input_devices;

        /* TODO: move this in a wf_keyboard struct,
         * This might not work with multiple keyboards */
        bool in_mod_binding = false;
        int count_other_inputs = 0;
        std::vector<std::function<void()>> match_keys(uint32_t mods, uint32_t key);

        wayfire_view keyboard_focus;

    public:

        input_manager();
        ~input_manager();

        void handle_new_input(wlr_input_device *dev);
        void handle_input_destroyed(wlr_input_device *dev);

        void update_cursor_position(uint32_t time_msec, bool real_update = true);
        void update_cursor_focus(wayfire_surface_t *surface, int lx, int ly);

        wl_client *exclusive_client = NULL;

        wlr_seat *seat = nullptr;
        std::unique_ptr<wf_cursor> cursor;

        wayfire_surface_t* cursor_focus = nullptr, *touch_focus = nullptr;
        signal_callback_t surface_map_state_changed;

        std::unique_ptr<wf_touch> our_touch;
        std::vector<std::unique_ptr<wf_drag_icon>> drag_icons;

        int pointer_count = 0, touch_count = 0;
        void update_capabilities();

        void set_keyboard_focus(wayfire_view view, wlr_seat *seat);

        bool grab_input(wayfire_grab_interface);
        void ungrab_input();
        bool input_grabbed();

        bool can_focus_surface(wayfire_surface_t *surface);
        void set_exclusive_focus(wl_client *client);

        void toggle_session();
        uint32_t get_modifiers();

        void free_output_bindings(wayfire_output *output);

        void handle_pointer_axis  (wlr_event_pointer_axis *ev);
        void handle_pointer_motion(wlr_event_pointer_motion *ev);
        void handle_pointer_motion_absolute(wlr_event_pointer_motion_absolute *ev);
        void handle_pointer_button(wlr_event_pointer_button *ev);
        void handle_pointer_frame();

        bool handle_keyboard_key(uint32_t key, uint32_t state);
        void handle_keyboard_mod(uint32_t key, uint32_t state);

        void handle_touch_down  (uint32_t time, int32_t id, int32_t x, int32_t y);
        void handle_touch_motion(uint32_t time, int32_t id, int32_t x, int32_t y);
        void handle_touch_up    (uint32_t time, int32_t id);

        void handle_gesture(wf_touch_gesture g);

        void check_touch_bindings(int32_t x, int32_t y);

        wf_binding* new_binding(wf_binding_type type, wf_option value, wayfire_output *output, void *callback);
        void rem_binding(void *callback);
        void rem_binding(wf_binding *binding);
};

#endif /* end of include guard: INPUT_MANAGER_HPP */
