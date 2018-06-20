#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <cstring>
#include <cassert>
#include <time.h>
#include <algorithm>
#include <libinput.h>
#include <linux/input-event-codes.h>

#include <iostream>

extern "C"
{
#include <wlr/backend/wayland.h>
#include <wlr/backend/libinput.h>
#include <wlr/backend/session.h>
#include <wlr/backend/multi.h>
#include <wlr/types/wlr_screenshooter.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_linux_dmabuf.h>
#include <wlr/types/wlr_gamma_control.h>
#include <wlr/types/wlr_xdg_output.h>
#include <wlr/types/wlr_seat.h>
}

#include "output/wayfire-shell.hpp"
#include "core.hpp"
#include "output.hpp"
#include "view.hpp"
#include "input-manager.hpp"
#include "workspace-manager.hpp"
#include "debug.hpp"
#include "render-manager.hpp"
#include "view/priv-view.hpp"

#ifdef BUILD_WITH_IMAGEIO
#include "img.hpp"
#endif

#include "signal-definitions.hpp"
#include "wayfire-shell-protocol.h"


/* Start input_manager */

namespace {
bool grab_start_finalized;
};

struct wf_gesture_recognizer {

    constexpr static int MIN_FINGERS = 3;
    constexpr static int MIN_SWIPE_DISTANCE = 100;
    constexpr static float MIN_PINCH_DISTANCE = 70;
    constexpr static int EDGE_SWIPE_THRESHOLD = 50;

    struct finger {
        int id;
        int sx, sy;
        int ix, iy;
    };

    std::map<int, finger> current;

    bool in_gesture = false, gesture_emitted = false;
    int start_sum_dist;

    void reset_gesture()
    {
        gesture_emitted = false;

        int cx = 0, cy = 0;
        for (auto f : current) {
            cx += f.second.sx;
            cy += f.second.sy;
        }

        cx /= current.size();
        cy /= current.size();

        start_sum_dist = 0;
        for (auto &f : current) {
            start_sum_dist += std::sqrt((cx - f.second.sx) * (cx - f.second.sx)
                    + (cy - f.second.sy) * (cy - f.second.sy));

            f.second.ix = f.second.sx;
            f.second.iy = f.second.sy;
        }
    }

    void start_new_gesture(int reason_id, int time)
    {
        in_gesture = true;
        reset_gesture();

        for (auto &f : current)
            if (f.first != reason_id)
                    core->input->handle_touch_up(time, f.first);
    }

    void stop_gesture()
    {
        in_gesture = gesture_emitted = false;
    }

    void continue_gesture(int id, int sx, int sy)
    {
        if (gesture_emitted)
            return;

        /* first case - consider swipe, we go through each
         * of the directions and check whether such swipe has occured */

        bool is_left_swipe = true, is_right_swipe = true,
             is_up_swipe = true, is_down_swipe = true;

        for (auto f : current) {
            int dx = f.second.sx - f.second.ix;
            int dy = f.second.sy - f.second.iy;

            if (-MIN_SWIPE_DISTANCE < dx)
                is_left_swipe = false;
            if (dx < MIN_SWIPE_DISTANCE)
                is_right_swipe = false;

            if (-MIN_SWIPE_DISTANCE < dy)
                is_up_swipe = false;
            if (dy < MIN_SWIPE_DISTANCE)
                is_down_swipe = false;
        }

        uint32_t swipe_dir = 0;
        if (is_left_swipe)
            swipe_dir |= GESTURE_DIRECTION_LEFT;
        if (is_right_swipe)
            swipe_dir |= GESTURE_DIRECTION_RIGHT;
        if (is_up_swipe)
            swipe_dir |= GESTURE_DIRECTION_UP;
        if (is_down_swipe)
            swipe_dir |= GESTURE_DIRECTION_DOWN;

        if (swipe_dir) {
            wayfire_touch_gesture gesture;
            gesture.type = GESTURE_SWIPE;
            gesture.finger_count = current.size();
            gesture.direction = swipe_dir;

            bool bottom_edge = false, upper_edge = false,
                 left_edge = false, right_edge = false;

            auto og = core->get_active_output()->get_full_geometry();

            for (auto f : current)
            {
                bottom_edge |= (f.second.iy >= og.y + og.height - EDGE_SWIPE_THRESHOLD);
                upper_edge  |= (f.second.iy <= og.y + EDGE_SWIPE_THRESHOLD);
                left_edge   |= (f.second.ix <= og.x + EDGE_SWIPE_THRESHOLD);
                right_edge  |= (f.second.ix >= og.x + og.width - EDGE_SWIPE_THRESHOLD);
            }

            uint32_t edge_swipe_dir = 0;
            if (bottom_edge)
                edge_swipe_dir |= GESTURE_DIRECTION_UP;
            if (upper_edge)
                edge_swipe_dir |= GESTURE_DIRECTION_DOWN;
            if (left_edge)
                edge_swipe_dir |= GESTURE_DIRECTION_RIGHT;
            if (right_edge)
                edge_swipe_dir |= GESTURE_DIRECTION_LEFT;

            if ((edge_swipe_dir & swipe_dir) == swipe_dir)
                gesture.type = GESTURE_EDGE_SWIPE;

            core->input->handle_gesture(gesture);
            gesture_emitted = true;
            return;
        }

        /* second case - this has been a pinch.
         * We calculate the central point of the fingers (cx, cy),
         * then we measure the average distance to the center. If it
         * is bigger/smaller above/below some threshold, then we emit the gesture */
        int cx = 0, cy = 0;
        for (auto f : current) {
            cx += f.second.sx;
            cy += f.second.sy;
        }

        cx /= current.size();
        cy /= current.size();

        int sum_dist = 0;
        for (auto f : current) {
            sum_dist += std::sqrt((cx - f.second.sx) * (cx - f.second.sx)
                    + (cy - f.second.sy) * (cy - f.second.sy));
        }

        bool inward_pinch  = (start_sum_dist - sum_dist >= MIN_PINCH_DISTANCE);
        bool outward_pinch = (start_sum_dist - sum_dist <= -MIN_PINCH_DISTANCE);

        if (inward_pinch || outward_pinch) {
            wayfire_touch_gesture gesture;
            gesture.type = GESTURE_PINCH;
            gesture.finger_count = current.size();
            gesture.direction =
                (inward_pinch ? GESTURE_DIRECTION_IN : GESTURE_DIRECTION_OUT);

            core->input->handle_gesture(gesture);
            gesture_emitted = true;
        }
    }

    void update_touch(int32_t time, int id, int sx, int sy)
    {
        current[id].sx = sx;
        current[id].sy = sy;

        if (in_gesture)
        {
            continue_gesture(id, sx, sy);
        } else
        {
            core->input->handle_touch_motion(time, id, sx, sy);
        }
    }

    timespec get_ctime()
    {
        timespec ts;
        timespec_get(&ts, TIME_UTC);

        return ts;
    }

    void register_touch(int time, int id, int sx, int sy)
    {
        log_info("touch_gesture: finger %d at %d@%d", id, sx, sy);
        current[id] = {id, sx, sy, sx, sy};
        if (in_gesture)
            reset_gesture();

        if (current.size() >= MIN_FINGERS && !in_gesture)
            start_new_gesture(id, time);

        if (!in_gesture)
            core->input->handle_touch_down(time, id, sx, sy);
    }

    void unregister_touch(int32_t time, int32_t id)
    {
        /* shouldn't happen, except possibly in nested(wayland/x11) backend */
        if (!current.count(id))
            return;

        log_info("touch_gesture: finger %d up", id);
        current.erase(id);
        if (in_gesture)
        {
            if (current.size() < MIN_FINGERS)
                stop_gesture();
            else
                reset_gesture();
        }
        else
        {
            core->input->handle_touch_up(time, id);
        }
    }
};

struct wf_touch
{
    wl_listener down, up, motion;
    wf_gesture_recognizer gesture_recognizer;
    wlr_cursor *cursor;

    wf_touch(wlr_cursor *cursor);
    void add_device(wlr_input_device *device);
};

static void handle_touch_down(wl_listener* listener, void *data)
{
    auto ev = static_cast<wlr_event_touch_down*> (data);
    auto touch = static_cast<wf_touch*> (ev->device->data);

    double lx, ly;
    wlr_cursor_absolute_to_layout_coords(core->input->cursor,
                                         ev->device, ev->x, ev->y, &lx, &ly);

    touch->gesture_recognizer.register_touch(ev->time_msec, ev->touch_id, lx, ly);
}

static void handle_touch_up(wl_listener* listener, void *data)
{
    auto ev = static_cast<wlr_event_touch_up*> (data);
    auto touch = static_cast<wf_touch*> (ev->device->data);

    touch->gesture_recognizer.unregister_touch(ev->time_msec, ev->touch_id);
}

static void handle_touch_motion(wl_listener* listener, void *data)
{
    auto ev = static_cast<wlr_event_touch_motion*> (data);
    auto touch = static_cast<wf_touch*> (ev->device->data);

    double lx, ly;
    wlr_cursor_absolute_to_layout_coords(core->input->cursor,
                                         ev->device, ev->x, ev->y, &lx, &ly);
    touch->gesture_recognizer.update_touch(ev->time_msec, ev->touch_id, lx, ly);
}

wf_touch::wf_touch(wlr_cursor *cursor)
{
    down.notify   = handle_touch_down;
    up.notify     = handle_touch_up;
    motion.notify = handle_touch_motion;

    wl_signal_add(&cursor->events.touch_up, &up);
    wl_signal_add(&cursor->events.touch_down, &down);
    wl_signal_add(&cursor->events.touch_motion, &motion);

    this->cursor = cursor;
}

void wf_touch::add_device(wlr_input_device *device)
{
    device->data = this;
    wlr_cursor_attach_input_device(cursor, device);
}

void input_manager::update_touch_focus(wayfire_surface_t *surface, uint32_t time, int id, int x, int y)
{
    if (surface)
    {
        wlr_seat_touch_point_focus(seat, surface->surface, time, id, x, y);
    } else
    {
        wlr_seat_touch_point_clear_focus(seat, time, id);
    }

    if (id == 0)
        touch_focus = surface;
}

wayfire_surface_t* input_manager::update_touch_position(uint32_t time, int32_t id, int32_t x, int32_t y, int &sx, int &sy)
{
    /* we have got a touch event, so our_touch must have been initialized */
    assert(our_touch);
    auto wo = core->get_output_at(x, y);
    auto og = wo->get_full_geometry();

    x -= og.x;
    y -= og.y;

    wayfire_surface_t *new_focus = NULL;
    wo->workspace->for_each_view(
        [&] (wayfire_view view)
        {
            if (new_focus) return;
            new_focus = view->map_input_coordinates(x, y, sx, sy);
        }, WF_ALL_LAYERS);

    update_touch_focus(new_focus, time, id, x, y);

    for (auto& icon : drag_icons)
        icon->update_output_position();

    return new_focus;
}

void input_manager::handle_touch_down(uint32_t time, int32_t id, int32_t x, int32_t y)
{
    int ox = x, oy = y;
    auto wo = core->get_output_at(x, y);
    auto og = wo->get_full_geometry();

    core->focus_output(wo);

    ox -= og.x; oy -= og.y;
    if (!active_grab)
    {
        int sx, sy;
        auto focused = update_touch_position(time, id, x, y, sx, sy);
        if (focused)
            wlr_seat_touch_notify_down(seat, focused->surface, time, id, sx, sy);
    }

    if (id < 1)
        core->input->check_touch_bindings(ox, oy);

    if (active_grab)
    {
        if (active_grab->callbacks.touch.down)
            active_grab->callbacks.touch.down(id, ox, oy);

        return;
    }

}

void input_manager::handle_touch_up(uint32_t time, int32_t id)
{
    if (active_grab)
    {
        if (active_grab->callbacks.touch.up)
            active_grab->callbacks.touch.up(id);

        return;
    }

    wlr_seat_touch_notify_up(seat, time, id);
}

void input_manager::handle_touch_motion(uint32_t time, int32_t id, int32_t x, int32_t y)
{
    if (active_grab)
    {
        auto wo = core->get_output_at(x, y);
        auto og = wo->get_full_geometry();
        if (active_grab->callbacks.touch.motion)
            active_grab->callbacks.touch.motion(id, x - og.x, y - og.y);

        return;
    }

    int sx, sy;
    update_touch_position(time, id, x, y, sx, sy);
    wlr_seat_touch_notify_motion(seat, time, id, sx, sy);
}

void input_manager::check_touch_bindings(int x, int y)
{
    uint32_t mods = get_modifiers();
    std::vector<touch_callback*> calls;
    for (auto listener : touch_listeners)
    {
        if (listener.second.mod == mods &&
                listener.second.output == core->get_active_output())
        {
            calls.push_back(listener.second.call);
        }
    }

    for (auto call : calls)
        (*call)(x, y);
}

/* TODO: reorganize input-manager code, perhaps move it to another file */
struct wf_callback
{
    int id;
    wayfire_output *output;
};

struct key_callback_data : wf_callback
{
    key_callback *call;
    wf_option key;
};

struct axis_callback_data : wf_callback
{
    axis_callback *call;
    wf_option modifier;
};

struct button_callback_data : wf_callback
{
    button_callback *call;
    wf_option button;
};

/* TODO: inhibit idle */
static void handle_pointer_button_cb(wl_listener*, void *data)
{
    auto ev = static_cast<wlr_event_pointer_button*> (data);
    core->input->handle_pointer_button(ev);
        wlr_seat_pointer_notify_button(core->input->seat, ev->time_msec,
                                       ev->button, ev->state);
}

static void handle_pointer_motion_cb(wl_listener*, void *data)
{
    auto ev = static_cast<wlr_event_pointer_motion*> (data);
    core->input->handle_pointer_motion(ev);
}

static void handle_pointer_motion_absolute_cb(wl_listener*, void *data)
{
    auto ev = static_cast<wlr_event_pointer_motion_absolute*> (data);
    core->input->handle_pointer_motion_absolute(ev);
}

static void handle_pointer_axis_cb(wl_listener*, void *data)
{
    auto ev = static_cast<wlr_event_pointer_axis*> (data);
    core->input->handle_pointer_axis(ev);
}

static void handle_keyboard_key_cb(wl_listener* listener, void *data)
{
    auto ev = static_cast<wlr_event_keyboard_key*> (data);
    wf_keyboard *keyboard = wl_container_of(listener, keyboard, key);

    auto seat = core->get_current_seat();
    wlr_seat_set_keyboard(seat, keyboard->device);

    if (!core->input->handle_keyboard_key(ev->keycode, ev->state))
    {
        wlr_seat_keyboard_notify_key(core->input->seat, ev->time_msec,
                                     ev->keycode, ev->state);
    }
}

static uint32_t mod_from_key(uint32_t key)
{
    if (key == KEY_LEFTALT || key == KEY_RIGHTALT)
        return WLR_MODIFIER_ALT;
    if (key == KEY_LEFTCTRL || key == KEY_RIGHTCTRL)
        return WLR_MODIFIER_CTRL;
    if (key == KEY_LEFTSHIFT || key == KEY_RIGHTSHIFT)
        return WLR_MODIFIER_SHIFT;
    if (key == KEY_LEFTMETA || key == KEY_RIGHTMETA)
        return WLR_MODIFIER_LOGO;

    return 0;
}

static void handle_keyboard_mod_cb(wl_listener* listener, void* data)
{
    auto kbd = static_cast<wlr_keyboard*> (data);
    wf_keyboard *keyboard = wl_container_of(listener, keyboard, modifier);

    auto seat = core->get_current_seat();
    wlr_seat_set_keyboard(seat, keyboard->device);
    wlr_seat_keyboard_send_modifiers(core->input->seat, &kbd->modifiers);
}

static void handle_request_set_cursor(wl_listener*, void *data)
{
    auto ev = static_cast<wlr_seat_pointer_request_set_cursor_event*> (data);
    core->input->set_cursor(ev);
}

static void handle_drag_icon_map(wl_listener* listener, void *data)
{
    auto wlr_icon = (wlr_drag_icon*) data;
    auto icon = (wf_drag_icon*) wlr_icon->surface->data;

    icon->unmap();
}

static void handle_drag_icon_destroy(wl_listener* listener, void *data)
{
    auto wlr_icon = (wlr_drag_icon*) data;
    auto icon = (wf_drag_icon*) wlr_icon->surface->data;

    auto it = std::find_if(core->input->drag_icons.begin(),
                           core->input->drag_icons.end(),
                           [=] (const std::unique_ptr<wf_drag_icon>& ptr)
                                {return ptr.get() == icon;});

    /* we don't dec_keep_count() because the surface memory is
     * managed by the unique_ptr */
    assert(it != core->input->drag_icons.end());
    core->input->drag_icons.erase(it);
}

wf_drag_icon::wf_drag_icon(wlr_drag_icon *ic)
    : wayfire_surface_t(nullptr), icon(ic)
{
    map_ev.notify  = handle_drag_icon_map;
    destroy.notify = handle_drag_icon_destroy;

    wl_signal_add(&icon->events.map, &map_ev);
    wl_signal_add(&icon->events.destroy, &destroy);

    icon->surface->data = this;
    map(icon->surface);
}

wf_point wf_drag_icon::get_output_position()
{
    auto pos = icon->is_pointer ?
        core->get_cursor_position() : core->get_touch_position(icon->touch_id);

    GetTuple(x, y, pos);

    x += icon->sx;
    y += icon->sy;

    if (output)
    {
        auto og = output->get_full_geometry();
        x -= og.x;
        y -= og.y;
    }

    return {x, y};
}

void wf_drag_icon::damage(const wlr_box& box)
{
    if (!is_mapped())
        return;

    core->for_each_output([=] (wayfire_output *output)
    {
        auto output_geometry = output->get_full_geometry();
        if (rect_intersect(output_geometry, box))
        {
            auto local = box;
            local.x -= output_geometry.x;
            local.y -= output_geometry.y;

            output->render->damage(local);
        }
    });
}

static void handle_new_drag_icon_cb(wl_listener*, void *data)
{
    auto di = static_cast<wlr_drag_icon*> (data);

    auto icon = std::unique_ptr<wf_drag_icon>(new wf_drag_icon(di));
    core->input->drag_icons.push_back(std::move(icon));
}

static bool check_vt_switch(wlr_session *session, uint32_t key, uint32_t mods)
{
    if (!session)
        return false;
    if (mods ^ (WLR_MODIFIER_ALT | WLR_MODIFIER_CTRL))
        return false;

    if (key < KEY_F1 || key > KEY_F10)
        return false;

    int target_vt = key - KEY_F1 + 1;
    wlr_session_change_vt(session, target_vt);
    return true;
}

std::vector<key_callback*> input_manager::match_keys(uint32_t mod_state, uint32_t key)
{
    std::vector<key_callback*> callbacks;

    for (auto& pair : key_bindings)
    {
        auto& binding = pair.second;
        const auto keyb = binding->key->as_cached_key();
        if (binding->output == core->get_active_output() &&
            mod_state == keyb.mod && key == keyb.keyval)
            callbacks.push_back(binding->call);
    }

    return callbacks;
}

bool input_manager::handle_keyboard_key(uint32_t key, uint32_t state)
{
    if (active_grab && active_grab->callbacks.keyboard.key)
        active_grab->callbacks.keyboard.key(key, state);

    auto mod = mod_from_key(key);
    if (mod)
        handle_keyboard_mod(mod, state);

    std::vector<key_callback*> callbacks;
    auto kbd = wlr_seat_get_keyboard(seat);

    log_info("in modifier binding %d", in_mod_binding);
    if (state == WLR_KEY_PRESSED)
    {
        if (check_vt_switch(wlr_multi_get_session(core->backend), key, get_modifiers()))
            return true;

        /* as long as we have pressed only modifiers, we should check for modifier bindings on release */
        if (mod)
        {
            bool modifiers_only = !count_other_inputs;
            for (size_t i = 0; i < kbd->num_keycodes; i++)
                if (!mod_from_key(kbd->keycodes[i]))
                    modifiers_only = false;

            if (modifiers_only)
                in_mod_binding = true;
            else
                in_mod_binding = false;
        } else
        {
            in_mod_binding = false;
        }

        callbacks = match_keys(get_modifiers(), key);
    } else
    {
        if (in_mod_binding)
            callbacks = match_keys(get_modifiers() | mod, 0);

        in_mod_binding = false;
    }

    for (auto call : callbacks)
        (*call) (key);

    return active_grab || !callbacks.empty();
}

void input_manager::handle_keyboard_mod(uint32_t modifier, uint32_t state)
{
    if (active_grab && active_grab->callbacks.keyboard.mod)
        active_grab->callbacks.keyboard.mod(modifier, state);
}

void input_manager::handle_pointer_button(wlr_event_pointer_button *ev)
{
    /* TODO: do we need this?
    if (ev->state == WLR_BUTTON_RELEASED)
    {
        cursor_focus = nullptr;
        update_cursor_position(ev->time_msec, false);
    } */

    log_info("got button %d", ev->button);
    in_mod_binding = false;

    if (ev->state == WLR_BUTTON_PRESSED)
    {
        count_other_inputs++;

        GetTuple(gx, gy, core->get_cursor_position());
        auto output = core->get_output_at(gx, gy);
        core->focus_output(output);

        std::vector<button_callback*> callbacks;

        auto mod_state = get_modifiers();
        for (auto& pair : button_bindings)
        {
            auto& binding = pair.second;
            const auto button = binding->button->as_cached_button();
            if (binding->output == core->get_active_output() &&
                mod_state == button.mod && ev->button == button.button)
                callbacks.push_back(binding->call);
        }

        GetTuple(ox, oy, core->get_active_output()->get_cursor_position());
        for (auto call : callbacks)
            (*call) (ev->button, ox, oy);
    } else
    {
        count_other_inputs--;
    }

    if (active_grab && active_grab->callbacks.pointer.button)
        active_grab->callbacks.pointer.button(ev->button, ev->state);
}

void input_manager::update_cursor_focus(wayfire_surface_t *focus, int x, int y)
{
    cursor_focus = focus;
    if (focus)
    {
        wlr_seat_pointer_notify_enter(seat, focus->surface, x, y);
    } else
    {
        wlr_seat_pointer_clear_focus(seat);
    }
}

void input_manager::update_cursor_position(uint32_t time_msec, bool real_update)
{
    auto output = core->get_output_at(cursor->x, cursor->y);
    assert(output);

    if (input_grabbed() && real_update)
    {
        GetTuple(sx, sy, core->get_active_output()->get_cursor_position());
        if (active_grab->callbacks.pointer.motion)
            active_grab->callbacks.pointer.motion(sx, sy);
        return;
    }

    GetTuple(px, py, output->get_cursor_position());
    int sx, sy;
    wayfire_surface_t *new_focus = NULL;

    output->workspace->for_each_view(
        [&] (wayfire_view view)
        {
            if (new_focus) return;
            new_focus = view->map_input_coordinates(px, py, sx, sy);
        }, WF_ALL_LAYERS);

    update_cursor_focus(new_focus, sx, sy);
    wlr_seat_pointer_notify_motion(core->input->seat, time_msec, sx, sy);

    for (auto& icon : drag_icons)
        icon->update_output_position();
}

void input_manager::handle_pointer_motion(wlr_event_pointer_motion *ev)
{
    wlr_cursor_move(cursor, ev->device, ev->delta_x, ev->delta_y);
    update_cursor_position(ev->time_msec);
}

void input_manager::handle_pointer_motion_absolute(wlr_event_pointer_motion_absolute *ev)
{
    wlr_cursor_warp_absolute(cursor, ev->device, ev->x, ev->y);
    update_cursor_position(ev->time_msec);;
}

void input_manager::handle_pointer_axis(wlr_event_pointer_axis *ev)
{
    std::vector<axis_callback*> callbacks;

    auto mod_state = get_modifiers();

    for (auto& pair : axis_bindings)
    {
        auto& binding = pair.second;
        const auto mod = binding->modifier->as_cached_key().mod;

        if (binding->output == core->get_active_output() &&
            mod_state == mod)
            callbacks.push_back(binding->call);
    }

    for (auto call : callbacks)
        (*call) (ev);

    /* reset modifier bindings */
    in_mod_binding = false;
    if (active_grab)
    {
        if (active_grab->callbacks.pointer.axis)
            active_grab->callbacks.pointer.axis(ev);

        return;
    }


    wlr_seat_pointer_notify_axis(seat, ev->time_msec, ev->orientation,
                                 ev->delta, ev->delta_discrete, ev->source);
}

void input_manager::set_cursor(wlr_seat_pointer_request_set_cursor_event *ev)
{
    auto focused_surface = ev->seat_client->seat->pointer_state.focused_surface;
    auto client = focused_surface ? wl_resource_get_client(focused_surface->resource) : NULL;

    if (ev->surface && client == ev->seat_client->client && !input_grabbed())
        wlr_cursor_set_surface(cursor, ev->surface, ev->hotspot_x, ev->hotspot_y);
}

bool input_manager::is_touch_enabled()
{
    return touch_count > 0;
}

/* TODO: possibly add more input options which aren't available right now */
namespace device_config
{
    int touchpad_tap_enabled;
    int touchpad_dwl_enabled;
    int touchpad_natural_scroll_enabled;

    std::string drm_device;

    wayfire_config *config;

    void load(wayfire_config *conf)
    {
        config = conf;

        auto section = (*config)["input"];
        touchpad_tap_enabled            = *section->get_option("tap_to_click", "1");
        touchpad_dwl_enabled            = *section->get_option("disable_while_typing", "0");
        touchpad_natural_scroll_enabled = *section->get_option("naturall_scroll", "0");

        drm_device = (*config)["core"]->get_option("drm_device", "default")->raw_value;
    }
}

void configure_input_device(libinput_device *device)
{
    assert(device);
    /* we are configuring a touchpad */
    if (libinput_device_config_tap_get_finger_count(device) > 0)
    {
        libinput_device_config_tap_set_enabled(device,
                device_config::touchpad_tap_enabled ?
                    LIBINPUT_CONFIG_TAP_ENABLED : LIBINPUT_CONFIG_TAP_DISABLED);
        libinput_device_config_dwt_set_enabled(device,
                device_config::touchpad_dwl_enabled ?
                LIBINPUT_CONFIG_DWT_ENABLED : LIBINPUT_CONFIG_DWT_DISABLED);

        if (libinput_device_config_scroll_has_natural_scroll(device) > 0)
        {
            libinput_device_config_scroll_set_natural_scroll_enabled(device,
                    device_config::touchpad_natural_scroll_enabled);
        }
    }
}

void input_manager::update_capabilities()
{
    uint32_t cap = 0;
    if (pointer_count)
        cap |= WL_SEAT_CAPABILITY_POINTER;
    if (keyboards.size())
        cap |= WL_SEAT_CAPABILITY_KEYBOARD;
    if (touch_count)
        cap |= WL_SEAT_CAPABILITY_TOUCH;

    wlr_seat_set_capabilities(seat, cap);
}

void handle_new_input_cb(wl_listener*, void *data)
{
    auto dev = static_cast<wlr_input_device*> (data);
    assert(dev);
    core->input->handle_new_input(dev);
}

void handle_keyboard_destroy_cb(wl_listener *listener, void*)
{
    wf_keyboard *keyboard = wl_container_of(listener, keyboard, destroy);
    core->input->handle_input_destroyed(keyboard->device);
}

wf_keyboard::wf_keyboard(wlr_input_device *dev, wayfire_config *config)
    : handle(dev->keyboard), device(dev)
{
    auto ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

    auto section = config->get_section("input");

    std::string model   = *section->get_option("xkb_model", "");
    std::string variant = *section->get_option("xkb_variant", "");
    std::string layout  = *section->get_option("xkb_layout", "");
    std::string options = *section->get_option("xkb_option", "");
    std::string rules   = *section->get_option("xkb_rule", "");

    xkb_rule_names names;
    names.rules   = strdup(rules.c_str());
    names.model   = strdup(model.c_str());
    names.layout  = strdup(layout.c_str());
    names.variant = strdup(variant.c_str());
    names.options = strdup(options.c_str());

    auto keymap = xkb_map_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);

    wlr_keyboard_set_keymap(dev->keyboard, keymap);
    xkb_keymap_unref(keymap);
    xkb_context_unref(ctx);

    int repeat_rate  = *section->get_option("kb_repeat_rate", "40");
    int repeat_delay = *section->get_option("kb_repeat_delay", "400");
    wlr_keyboard_set_repeat_info(dev->keyboard, repeat_rate, repeat_delay);

    key.notify      = handle_keyboard_key_cb;
    modifier.notify = handle_keyboard_mod_cb;
    destroy.notify  = handle_keyboard_destroy_cb;

    wl_signal_add(&dev->keyboard->events.key, &key);
    wl_signal_add(&dev->keyboard->events.modifiers, &modifier);
    wl_signal_add(&dev->events.destroy, &destroy);

    wlr_seat_set_keyboard(core->get_current_seat(), dev);
}

void input_manager::handle_new_input(wlr_input_device *dev)
{
    if (!cursor)
        create_seat();

    if (dev->type == WLR_INPUT_DEVICE_KEYBOARD)
        keyboards.push_back(std::unique_ptr<wf_keyboard> (new wf_keyboard(dev, core->config)));

    if (dev->type == WLR_INPUT_DEVICE_POINTER)
    {
        wlr_cursor_attach_input_device(cursor, dev);
        pointer_count++;
    }

    if (dev->type == WLR_INPUT_DEVICE_TOUCH)
    {
        touch_count++;
        if (!our_touch)
            our_touch = std::unique_ptr<wf_touch> (new wf_touch(cursor));

        log_info("has touch devi with output %s", dev->output_name);

        our_touch->add_device(dev);
    }

    if (wlr_input_device_is_libinput(dev))
        configure_input_device(wlr_libinput_get_device_handle(dev));

    auto section = core->config->get_section(nonull(dev->name));
    auto mapped_output = section->get_option("output", nonull(dev->output_name))->raw_value;

    auto wo = core->get_output(mapped_output);
    if (wo)
        wlr_cursor_map_input_to_output(cursor, dev, wo->handle);

    update_capabilities();
}

void input_manager::handle_input_destroyed(wlr_input_device *dev)
{
    log_info("add new input: %s", dev->name);
    if (dev->type == WLR_INPUT_DEVICE_KEYBOARD)
    {
        auto it = std::remove_if(keyboards.begin(), keyboards.end(),
                                 [=] (const std::unique_ptr<wf_keyboard>& kbd) { return kbd->device == dev; });

        keyboards.erase(it, keyboards.end());
    }

    if (dev->type == WLR_INPUT_DEVICE_POINTER)
    {
        wlr_cursor_detach_input_device(cursor, dev);
        pointer_count--;
    }

    if (dev->type == WLR_INPUT_DEVICE_TOUCH)
        touch_count--;

    update_capabilities();
}

void input_manager::create_seat()
{
    cursor = wlr_cursor_create();

    wlr_cursor_attach_output_layout(cursor, core->output_layout);
    wlr_cursor_map_to_output(cursor, NULL);
    wlr_cursor_warp(cursor, NULL, cursor->x, cursor->y);

    xcursor = wlr_xcursor_manager_create(NULL, 32);
    wlr_xcursor_manager_load(xcursor, 1);

    core->set_default_cursor();

    wl_signal_add(&cursor->events.button, &button);
    wl_signal_add(&cursor->events.motion, &motion);
    wl_signal_add(&cursor->events.motion_absolute, &motion_absolute);
    wl_signal_add(&cursor->events.axis, &axis);

    wl_signal_add(&seat->events.request_set_cursor, &request_set_cursor);
    wl_signal_add(&seat->events.new_drag_icon, &new_drag_icon);
}

input_manager::input_manager()
{
    input_device_created.notify = handle_new_input_cb;
    seat = wlr_seat_create(core->display, "default");

    wl_signal_add(&core->backend->events.new_input,
                  &input_device_created);

    button.notify             = handle_pointer_button_cb;
    motion.notify             = handle_pointer_motion_cb;
    motion_absolute.notify    = handle_pointer_motion_absolute_cb;
    axis.notify               = handle_pointer_axis_cb;
    request_set_cursor.notify = handle_request_set_cursor;
    new_drag_icon.notify      = handle_new_drag_icon_cb;

    surface_destroyed = [=] (signal_data *data)
    {
        auto conv = static_cast<_surface_unmapped_signal*> (data);
        assert(conv);

        if (conv->surface == cursor_focus)
            update_cursor_focus(nullptr, 0, 0);
        if (conv->surface == touch_focus)
            update_touch_focus(nullptr, 0, 0, 0, 0);
    };

    /*
    if (is_touch_enabled())
    {

        auto touch = weston_seat_get_touch(core->get_current_seat());
        tgrab.interface = &touch_grab_interface;
        tgrab.touch = touch;
        weston_touch_start_grab(touch, &tgrab);

        using namespace std::placeholders;
        gr = new wf_gesture_recognizer(touch,
                                       std::bind(std::mem_fn(&input_manager::handle_gesture),
                                                 this, _1));
    }

    session_listener.notify = session_signal_handler;
    wl_signal_add(&core->ec->session_signal, &session_listener);
    */
}

uint32_t input_manager::get_modifiers()
{
    uint32_t mods = 0;
    auto keyboard = wlr_seat_get_keyboard(seat);
    if (keyboard)
        mods = wlr_keyboard_get_modifiers(keyboard);

    return mods;
}

bool input_manager::grab_input(wayfire_grab_interface iface)
{
    if (!iface || !iface->grabbed || !session_active)
        return false;

    assert(!active_grab); // cannot have two active input grabs!

    if (our_touch)
        for (const auto& f : our_touch->gesture_recognizer.current)
            handle_touch_up(0, f.first);

    active_grab = iface;

    auto kbd = wlr_seat_get_keyboard(seat);
    auto mods = kbd->modifiers;
    mods.depressed = 0;
    wlr_seat_keyboard_send_modifiers(seat, &mods);

    iface->output->set_keyboard_focus(NULL, seat);
    update_cursor_focus(nullptr, 0, 0);
    core->set_default_cursor();
    return true;
}

void input_manager::ungrab_input()
{
    if (active_grab)
        active_grab->output->set_active_view(active_grab->output->get_active_view());
    active_grab = nullptr;

    /*
    if (is_touch_enabled())
        gr->end_grab();
        */
}

bool input_manager::input_grabbed()
{
    return active_grab || !session_active;
}

void input_manager::toggle_session()
{

    session_active ^= 1;
    if (!session_active)
    {
        if (active_grab)
        {
            auto grab = active_grab;
            ungrab_input();
            active_grab = grab;
        }
    } else
    {
        if (active_grab)
        {
            auto grab = active_grab;
            active_grab = nullptr;
            grab_input(grab);
        }
    }

}
static int _last_id = 0;

#define id_deleter(type) \
\
void input_manager::rem_ ##type (int id) \
{ \
    auto it = type ## _bindings.find(id); \
    if (it != type ## _bindings.end()) \
    { \
        delete it->second; \
        type ## _bindings.erase(it); \
    } \
}

#define callback_deleter(type) \
void input_manager::rem_ ##type (type ## _callback *cb) \
{ \
    auto it = type ## _bindings.begin(); \
\
    while(it != type ## _bindings.end()) \
    { \
        if (it->second->call == cb) \
        { \
            delete it->second; \
            it = type ## _bindings.erase(it); \
        } else \
            ++it; \
    } \
}

int input_manager::add_key(wf_option option, key_callback *call, wayfire_output *output)
{
    auto kcd = new key_callback_data;
    kcd->call = call;
    kcd->output = output;
    kcd->key = option;
    kcd->id = ++_last_id;

    key_bindings[_last_id] = kcd;
    return _last_id;
}

id_deleter(key);
callback_deleter(key);

int input_manager::add_axis(wf_option option, axis_callback *call, wayfire_output *output)
{
    auto acd = new axis_callback_data;
    acd->call = call;
    acd->output = output;
    acd->modifier = option;
    acd->id = ++_last_id;

    axis_bindings[_last_id] = acd;
    return _last_id;
}

id_deleter(axis);
callback_deleter(axis);

int input_manager::add_button(wf_option option, button_callback *call, wayfire_output *output)
{
    auto bcd = new button_callback_data;
    bcd->call = call;
    bcd->output = output;
    bcd->button = option;
    bcd->id = ++_last_id;

    button_bindings[_last_id] = bcd;
    return _last_id;
}

id_deleter(button);
callback_deleter(button);

/* */

int input_manager::add_touch(uint32_t mods, touch_callback* call, wayfire_output *output)
{
    int sz = 0;
    if (!touch_listeners.empty())
        sz = (--touch_listeners.end())->first + 1;

    touch_listeners[sz] = {mods, call, output};
    return sz;
}

void input_manager::rem_touch(int id)
{
    touch_listeners.erase(id);
}

void input_manager::rem_touch(touch_callback *tc)
{
    std::vector<int> ids;
    for (const auto& x : touch_listeners)
        if (x.second.call == tc)
            ids.push_back(x.first);

    for (auto x : ids)
        rem_touch(x);
}

int input_manager::add_gesture(const wayfire_touch_gesture& gesture,
        touch_gesture_callback *callback, wayfire_output *output)
{
    gesture_listeners[gesture_id] = {gesture, callback, output};
    gesture_id++;
    return gesture_id - 1;
}

void input_manager::rem_gesture(int id)
{
    gesture_listeners.erase(id);
}

void input_manager::rem_gesture(touch_gesture_callback *cb)
{
    std::vector<int> ids;
    for (const auto& x : gesture_listeners)
        if (x.second.call == cb)
            ids.push_back(x.first);

    for (auto x : ids)
        rem_gesture(x);
}

void input_manager::free_output_bindings(wayfire_output *output)
{
    std::vector<int> bindings;
    for (auto kcd : key_bindings)
        if (kcd.second->output == output)
            bindings.push_back(kcd.second->id);

    for (auto x : bindings)
        rem_key(x);

    bindings.clear();
    for (auto bcd : button_bindings)
        if (bcd.second->output == output)
            bindings.push_back(bcd.second->id);

    for (auto x : bindings)
        rem_button(x);

    std::vector<int> ids;
    for (const auto& x : touch_listeners)
        if (x.second.output == output)
            ids.push_back(x.first);
    for (auto x : ids)
        rem_touch(x);

    ids.clear();
    for (const auto& x : gesture_listeners)
        if (x.second.output == output)
            ids.push_back(x.first);
    for (auto x : ids)
        rem_gesture(x);
}

void input_manager::handle_gesture(wayfire_touch_gesture g)
{
    for (const auto& listener : gesture_listeners) {
        if (listener.second.gesture.type == g.type &&
            listener.second.gesture.finger_count == g.finger_count &&
            core->get_active_output() == listener.second.output)
        {
            (*listener.second.call)(&g);
        }
    }
}

/* End input_manager */

void wayfire_core::configure(wayfire_config *config)
{
    this->config = config;
    auto section = config->get_section("core");

    vwidth  = *section->get_option("vwidth", "3");
    vheight = *section->get_option("vheight", "3");

    shadersrc   = section->get_option("shadersrc", INSTALL_PREFIX "/share/wayfire/shaders")->as_string();
    run_panel   = section->get_option("run_panel", "1")->as_int();

}

static void handle_output_layout_changed(wl_listener*, void *)
{
    core->for_each_output([] (wayfire_output *wo)
    {
        wo->emit_signal("output-resized", nullptr);
    });
}

void wayfire_core::init(wayfire_config *conf)
{
    configure(conf);
    device_config::load(conf);

    protocols.data_device = wlr_data_device_manager_create(display);
    wlr_renderer_init_wl_display(renderer, display);

    output_layout = wlr_output_layout_create();
    output_layout_changed.notify = handle_output_layout_changed;
    wl_signal_add(&output_layout->events.change, &output_layout_changed);

    core->compositor = wlr_compositor_create(display, wlr_backend_get_renderer(backend));
    init_desktop_apis();
    input = new input_manager();

    protocols.screenshooter = wlr_screenshooter_create(display);
    protocols.gamma = wlr_gamma_control_manager_create(display);
    protocols.linux_dmabuf = wlr_linux_dmabuf_create(display, renderer);
    protocols.output_manager = wlr_xdg_output_manager_create(display, output_layout);
    protocols.wf_shell = wayfire_shell_create(display);

#ifdef BUILD_WITH_IMAGEIO
    image_io::init();
#endif
}

bool wayfire_core::set_decorator(decorator_base_t *decor)
{
    if (wf_decorator)
        return false;

    return (wf_decorator = decor);
}

void refocus_idle_cb(void *data)
{
    core->refocus_active_output_active_view();
}

void wayfire_core::wake()
{
    if (times_wake == 0 && run_panel)
        run(INSTALL_PREFIX "/lib/wayfire/wayfire-shell-client");

    for (auto o : pending_outputs)
        add_output(o);
    pending_outputs.clear();

    auto loop = wl_display_get_event_loop(display);
    wl_event_loop_add_idle(loop, refocus_idle_cb, 0);

    if (times_wake > 0)
    {
        for_each_output([] (wayfire_output *output)
                        { output->emit_signal("wake", nullptr); });
    }

    ++times_wake;
}

void wayfire_core::sleep()
{
    for_each_output([] (wayfire_output *output)
            { output->emit_signal("sleep", nullptr); });
}

wlr_seat* wayfire_core::get_current_seat()
{ return input->seat; }

static void output_destroyed_callback(wl_listener *, void *data)
{
    core->remove_output(core->get_output((wlr_output*) data));
}

void wayfire_core::set_default_cursor()
{
    if (input->cursor)
        wlr_xcursor_manager_set_cursor_image(input->xcursor, "left_ptr", input->cursor);
}

std::tuple<int, int> wayfire_core::get_cursor_position()
{
    if (input->cursor)
        return std::tuple<int, int> (input->cursor->x, input->cursor->y);
    else
        return std::tuple<int, int> (0, 0);
}

std::tuple<int, int> wayfire_core::get_touch_position(int id)
{
    if (!input->our_touch)
        return std::make_tuple(0, 0);

    auto it = input->our_touch->gesture_recognizer.current.find(id);
    if (it != input->our_touch->gesture_recognizer.current.end())
        return std::make_tuple(it->second.sx, it->second.sy);

    return std::make_tuple(0, 0);
}

wayfire_surface_t *wayfire_core::get_cursor_focus()
{
    return input->cursor_focus;
}

wayfire_surface_t *wayfire_core::get_touch_focus()
{
    return input->touch_focus;
}

static int _last_output_id = 0;
/* TODO: remove pending_outputs, they are no longer necessary */
void wayfire_core::add_output(wlr_output *output)
{
    log_info("add new output: %s", output->name);
    if (outputs.find(output) != outputs.end())
    {
        log_info("old output");
        return;
    }

    if (!input) {
        pending_outputs.push_back(output);
        return;
    }

    wayfire_output *wo = outputs[output] = new wayfire_output(output, config);
    wo->id = _last_output_id++;
    focus_output(wo);

    wo->destroy_listener.notify = output_destroyed_callback;
    wl_signal_add(&wo->handle->events.destroy, &wo->destroy_listener);

    wo->connect_signal("_surface_unmapped", &input->surface_destroyed);
    wayfire_shell_handle_output_created(wo);
}

void wayfire_core::remove_output(wayfire_output *output)
{
    log_info("removing output: %s", output->handle->name);

    outputs.erase(output->handle);
    wayfire_shell_handle_output_destroyed(output);

    /* we have no outputs, simply quit */
    if (outputs.empty())
        std::exit(0);

    if (output == active_output)
        focus_output(outputs.begin()->second);

 //   auto og = output->get_full_geometry();
  //  auto ng = active_output->get_full_geometry();
   // int dx = ng.x - og.x, dy = ng.y - og.y;

    /* first move each desktop view(e.g windows) to another output */
    output->workspace->for_each_view_reverse([=] (wayfire_view view)
    {
        view->set_output(nullptr);
        output->workspace->add_view_to_layer(view, 0);

        active_output->attach_view(view);
        active_output->focus_view(view);
    }, WF_WM_LAYERS);

    /* just remove all other views - backgrounds, panels, etc.
     * desktop views have been removed by the previous cycle */
    output->workspace->for_each_view([output] (wayfire_view view)
    {
        view->set_output(nullptr);
        output->workspace->add_view_to_layer(view, 0);
    }, WF_ALL_LAYERS);

    delete output;
}

void wayfire_core::refocus_active_output_active_view()
{
    if (!active_output)
        return;

    auto view = active_output->get_active_view();
    if (view) {
        active_output->focus_view(nullptr);
        active_output->focus_view(view);
    }
}

void wayfire_core::focus_output(wayfire_output *wo)
{
    assert(wo);
    if (active_output == wo)
        return;

    wo->ensure_pointer();

    wayfire_grab_interface old_grab = nullptr;

    if (active_output)
    {
        old_grab = active_output->get_input_grab_interface();
        active_output->focus_view(nullptr);
    }

    active_output = wo;
    if (wo)
        log_debug("focus output: %s", wo->handle->name);

    /* invariant: input is grabbed only if the current output
     * has an input grab */
    if (input->input_grabbed())
    {
        assert(old_grab);
        input->ungrab_input();
    }

    wayfire_grab_interface iface = wo->get_input_grab_interface();

    /* this cannot be recursion as active_output will be equal to wo,
     * and wo->active_view->output == wo */
    if (!iface)
        refocus_active_output_active_view();
    else
        input->grab_input(iface);

    if (active_output)
    {
        wlr_output_schedule_frame(active_output->handle);
        active_output->emit_signal("output-gain-focus", nullptr);
    }
}

wayfire_output* wayfire_core::get_output(wlr_output *handle)
{
    auto it = outputs.find(handle);
    if (it != outputs.end()) {
        return it->second;
    } else {
        return nullptr;
    }
}

wayfire_output* wayfire_core::get_output(std::string name)
{
    for (const auto& wo : outputs)
        if (wo.first->name == name)
            return wo.second;

    return nullptr;
}

wayfire_output* wayfire_core::get_active_output()
{
    return active_output;
}

wayfire_output* wayfire_core::get_output_at(int x, int y)
{
    wayfire_output *target = nullptr;
    for_each_output([&] (wayfire_output *output)
    {
        if (point_inside({x, y}, output->get_full_geometry()) &&
                target == nullptr)
        {
            target = output;
        }
    });

    return target;
}

wayfire_output* wayfire_core::get_next_output(wayfire_output *output)
{
    if (outputs.empty())
        return output;
    auto id = output->handle;
    auto it = outputs.find(id);
    ++it;

    if (it == outputs.end()) {
        return outputs.begin()->second;
    } else {
        return it->second;
    }
}

size_t wayfire_core::get_num_outputs()
{
    return outputs.size();
}

void wayfire_core::for_each_output(output_callback_proc call)
{
    for (auto o : outputs)
        call(o.second);
}

void wayfire_core::focus_layer(uint32_t layer)
{
    if (get_focused_layer() == layer)
        return;

    focused_layer = layer;
    active_output->refocus(nullptr, wf_all_layers_not_below(layer));
}

uint32_t wayfire_core::get_focused_layer()
{
    return focused_layer;
}

void wayfire_core::add_view(std::unique_ptr<wayfire_view_t> view)
{
    views.push_back(std::move(view));
    assert(active_output);
}

wayfire_view wayfire_core::find_view(wayfire_surface_t *handle)
{
    auto view = dynamic_cast<wayfire_view_t*> (handle);
    if (!view)
        return nullptr;

    return nonstd::make_observer(view);
}

wayfire_view wayfire_core::find_view(uint32_t id)
{
    for (auto& v : views)
        if (v->get_id() == id)
            return nonstd::make_observer(v.get());

    return nullptr;
}

void wayfire_core::focus_view(wayfire_view v, wlr_seat *seat)
{
    if (!v)
        return;

    if (v->get_output() != active_output)
        focus_output(v->get_output());

    active_output->focus_view(v, seat);
}

void wayfire_core::erase_view(wayfire_view v)
{
    if (!v) return;

    if (v->get_output())
        v->get_output()->detach_view(v);

    auto it = std::find_if(views.begin(), views.end(),
                           [&v] (const std::unique_ptr<wayfire_view_t>& k)
                           { return k.get() == v.get(); });

    views.erase(it);
}

void wayfire_core::run(const char *command)
{
    pid_t pid = fork();

    /* The following is a "hack" for disowning the child processes,
     * otherwise they will simply stay as zombie processes */
    if (!pid) {
        if (!fork()) {
            setenv("WAYLAND_DISPLAY", wayland_display.c_str(), 1);
            auto xdisp = ":" + xwayland_get_display();
            setenv("DISPLAY", xdisp.c_str(), 1);

            int dev_null = open("/dev/null", O_WRONLY);
            dup2(dev_null, 1);
            dup2(dev_null, 2);

            exit(execl("/bin/sh", "/bin/bash", "-c", command, NULL));
        } else {
            exit(0);
        }
    } else {
        int status;
        waitpid(pid, &status, 0);
    }
}

void wayfire_core::move_view_to_output(wayfire_view v, wayfire_output *new_output)
{
    assert(new_output);
    if (v->get_output())
        v->get_output()->detach_view(v);

    new_output->attach_view(v);
    new_output->focus_view(v);
}

wayfire_core *core;
