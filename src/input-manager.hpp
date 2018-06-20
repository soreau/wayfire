#ifndef INPUT_MANAGER_HPP
#define INPUT_MANAGER_HPP

#include <unordered_set>
#include <map>
#include <vector>

#include "plugin.hpp"
#include "view.hpp"

extern "C"
{
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>

struct wlr_drag_icon;
}


struct wf_gesture_recognizer;
struct key_callback_data;
struct axis_callback_data;
struct button_callback_data;
struct wlr_seat;
struct wf_touch;

struct wf_keyboard
{
    wlr_keyboard *handle;
    wlr_input_device *device;
    wl_listener key, modifier, destroy;
    wf_keyboard(wlr_input_device *keyboard, wayfire_config *config);
};

struct wf_drag_icon : public wayfire_surface_t
{
    wlr_drag_icon *icon;
    wl_listener map_ev, destroy;

    wf_drag_icon(wlr_drag_icon *icon);

    bool is_subsurface() { return true ;}
    wf_point get_output_position();
    void damage(const wlr_box& rect);
};

class input_manager
{
    friend void handle_new_input_cb       (wl_listener*, void*);
    friend void handle_keyboard_destroy_cb(wl_listener*, void*);

    private:
        wayfire_grab_interface active_grab = nullptr;
        bool session_active = true;

        wl_listener input_device_created, new_drag_icon,
                    button, motion, motion_absolute, axis,
                    request_set_cursor,
                    touch_down, touch_up, touch_motion;

        int gesture_id;
        struct wf_gesture_listener
        {
            wayfire_touch_gesture gesture;
            touch_gesture_callback* call;
            wayfire_output *output;
        };

        struct touch_listener {
            uint32_t mod;
            touch_callback* call;
            wayfire_output *output;
        };

        std::map<int, int> mods_count;
        std::map<int, wf_gesture_listener> gesture_listeners;
        std::map<int, touch_listener> touch_listeners;
        std::map<int, key_callback_data*> key_bindings;
        std::map<int, axis_callback_data*> axis_bindings;
        std::map<int, button_callback_data*> button_bindings;

        bool is_touch_enabled();

        void create_seat();
        void handle_new_input(wlr_input_device *dev);
        void handle_input_destroyed(wlr_input_device *dev);

        void update_cursor_focus(wayfire_surface_t *focus, int x, int y);
        void update_cursor_position(uint32_t time_msec, bool real_update = true);

        void update_touch_focus(wayfire_surface_t *focus,
                                uint32_t time, int id, int x, int y);
        wayfire_surface_t* update_touch_position(uint32_t time, int id, int x, int y,
                                                 int &sx, int &sy);

        std::vector<std::unique_ptr<wf_keyboard>> keyboards;

        /* TODO: move this in a wf_keyboard struct,
         * This might not work with multiple keyboards */
        bool in_mod_binding = false;
        int count_other_inputs = 0;
        std::vector<key_callback*> match_keys(uint32_t mods, uint32_t key);

    public:

        input_manager();
        wlr_seat *seat = nullptr;
        wlr_cursor *cursor = NULL;
        wlr_xcursor_manager *xcursor;

        wayfire_surface_t* cursor_focus = nullptr, *touch_focus = nullptr;
        signal_callback_t surface_destroyed;

        std::unique_ptr<wf_touch> our_touch;
        std::vector<std::unique_ptr<wf_drag_icon>> drag_icons;

        int pointer_count = 0, touch_count = 0;
        void update_capabilities();
        void set_cursor(wlr_seat_pointer_request_set_cursor_event *ev);

        bool grab_input(wayfire_grab_interface);
        void ungrab_input();
        bool input_grabbed();

        void toggle_session();
        uint32_t get_modifiers();

        void free_output_bindings(wayfire_output *output);

        void handle_pointer_axis  (wlr_event_pointer_axis *ev);
        void handle_pointer_motion(wlr_event_pointer_motion *ev);
        void handle_pointer_motion_absolute(wlr_event_pointer_motion_absolute *ev);
        void handle_pointer_button(wlr_event_pointer_button *ev);

        bool handle_keyboard_key(uint32_t key, uint32_t state);
        void handle_keyboard_mod(uint32_t key, uint32_t state);

        void handle_touch_down  (uint32_t time, int32_t id, int32_t x, int32_t y);
        void handle_touch_motion(uint32_t time, int32_t id, int32_t x, int32_t y);
        void handle_touch_up    (uint32_t time, int32_t id);

        void handle_gesture(wayfire_touch_gesture g);

        void check_touch_bindings(int32_t x, int32_t y);

        int  add_key(wf_option, key_callback *, wayfire_output *output);
        void rem_key(int);
        void rem_key(key_callback *callback);

        int  add_axis(wf_option, axis_callback *, wayfire_output *output);
        void rem_axis(int);
        void rem_axis(axis_callback * callback);

        int  add_button(wf_option, button_callback *, wayfire_output *output);
        void rem_button(int);
        void rem_button(button_callback *callback);

        int  add_touch(uint32_t mod, touch_callback*, wayfire_output *output);
        void rem_touch(int32_t id);
        void rem_touch(touch_callback*);

        int add_gesture(const wayfire_touch_gesture& gesture,
                        touch_gesture_callback* callback, wayfire_output *output);
        void rem_gesture(int id);
        void rem_gesture(touch_gesture_callback*);
};

#endif /* end of include guard: INPUT_MANAGER_HPP */
