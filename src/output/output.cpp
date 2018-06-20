#include "debug.hpp"
#include "plugin-loader.hpp"
#include "output.hpp"
#include "view.hpp"
#include "core.hpp"
#include "signal-definitions.hpp"
#include "input-manager.hpp"
#include "render-manager.hpp"
#include "workspace-manager.hpp"
#include "wayfire-shell.hpp"

#include <linux/input.h>

#include <algorithm>
#include <assert.h>

#include <cstring>
#include <config.hpp>

void wayfire_output::connect_signal(std::string name, signal_callback_t* callback)
{
    signals[name].push_back(callback);
}

void wayfire_output::disconnect_signal(std::string name, signal_callback_t* callback)
{
    auto it = std::remove_if(signals[name].begin(), signals[name].end(),
    [=] (const signal_callback_t *call) {
        return call == callback;
    });

    signals[name].erase(it, signals[name].end());
}

void wayfire_output::emit_signal(std::string name, signal_data *data)
{
    std::vector<signal_callback_t> callbacks;
    for (auto x : signals[name])
        callbacks.push_back(*x);

    for (auto x : callbacks)
        x(data);
}

static wl_output_transform get_transform_from_string(std::string transform)
{
    if (transform == "normal")
        return WL_OUTPUT_TRANSFORM_NORMAL;
    else if (transform == "90")
        return WL_OUTPUT_TRANSFORM_90;
    else if (transform == "180")
        return WL_OUTPUT_TRANSFORM_180;
    else if (transform == "270")
        return WL_OUTPUT_TRANSFORM_270;
    else if (transform == "flipped")
        return WL_OUTPUT_TRANSFORM_FLIPPED;
    else if (transform == "180_flipped")
        return WL_OUTPUT_TRANSFORM_FLIPPED_180;
    else if (transform == "90_flipped")
        return WL_OUTPUT_TRANSFORM_FLIPPED_90;
    else if (transform == "270_flipped")
        return WL_OUTPUT_TRANSFORM_FLIPPED_270;

    log_error ("Bad output transform in config: %s", transform.c_str());
    return WL_OUTPUT_TRANSFORM_NORMAL;
}

std::pair<wlr_output_mode, bool> parse_output_mode(std::string modeline)
{
    wlr_output_mode mode;
    mode.refresh = 60;

    int read = std::sscanf(modeline.c_str(), "%d x %d @ %d", &mode.width, &mode.height, &mode.refresh);

    if (mode.refresh < 1000)
        mode.refresh *= 1000;

    if (read < 2 || mode.width <= 0 || mode.height <= 0 || mode.refresh <= 0)
        return {mode, false};

    return {mode, true};
}

wf_point parse_output_layout(std::string layout)
{
    wf_point pos;
    int read = std::sscanf(layout.c_str(), "%d @ %d", &pos.x, &pos.y);

    if (read < 2)
        pos.x = pos.y = 0;

    return pos;
}

wlr_output_mode *find_matching_mode(wlr_output *output, int32_t w, int32_t h, int32_t rr)
{
    wlr_output_mode *mode;
    wl_list_for_each(mode, &output->modes, link)
    {
        if (mode->width == w && mode->height == h && mode->refresh == rr)
            return mode;
    }

    return NULL;
}

bool wayfire_output::set_mode(uint32_t width, uint32_t height, uint32_t refresh_mHz)
{
    auto built_in = find_matching_mode(handle, width, height, refresh_mHz);
    if (built_in)
    {
        wlr_output_set_mode(handle, built_in);
        return true;
    } else
    {
        log_info("Couldn't find matching mode %dx%d@%f for output %s."
                 "Trying to use custom mode (might not work).",
                 width, height, refresh_mHz / 1000.0,
                 handle->name);

        return wlr_output_set_custom_mode(handle, width, height, refresh_mHz);
    }

    emit_signal("mode-changed", nullptr);
    emit_signal("output-resized", nullptr);
}

bool wayfire_output::set_mode(std::string mode)
{
    if (mode == "default")
    {
        if (wl_list_length(&handle->modes) > 0)
        {
            struct wlr_output_mode *mode =
                wl_container_of((&handle->modes)->prev, mode, link);

            set_mode(mode->width, mode->height, mode->refresh);
            return true;
        }

        return false;
    }

    auto target = parse_output_mode(mode);
    if (!target.second)
    {
        log_error ("Invalid mode config for output %s", handle->name);
        return false;
    } else
    {
        return set_mode(target.first.width, target.first.height,
                        target.first.refresh);
    }
}

void wayfire_output::set_initial_mode()
{
    auto section = core->config->get_section(handle->name);
    const auto default_mode = "default";

    mode_opt = section->get_option("mode", default_mode);

    /* we capture the shared_ptr, but as config will outlive us anyway,
     * and the lambda will be destroyed as soon as the wayfire_output is
     * destroyed, the circular dependency will be broken */
    config_mode_changed = [this] ()
    { set_mode(mode_opt->as_string()); };

    mode_opt->updated.push_back(&config_mode_changed);

    if (!set_mode(mode_opt->as_string()))
    {
        log_error ("Couldn't set the requested in config mode for output %s", handle->name);
        if (!set_mode(default_mode))
            log_error ("Couldn't set any mode for output %s", handle->name);
    }
}

static void handle_output_layout_changed(wl_listener*, void *)
{
    core->for_each_output([] (wayfire_output *wo)
    {
        wo->workspace->for_each_view([=] (wayfire_view view)
        {
            auto wm = view->get_wm_geometry();
            view->move(wm.x, wm.y, false);
        }, WF_LAYER_WORKSPACE);
    });
}

wayfire_output::wayfire_output(wlr_output *handle, wayfire_config *c)
{
    this->handle = handle;

    set_initial_mode();
    set_initial_scale();
    set_initial_transform();
    set_initial_position();


    render = new render_manager(this);

    core->set_default_cursor();
    plugin = new plugin_manager(this, c,
                                *c->get_section("core")->get_option("plugins", "default"));

    unmap_view_cb = [=] (signal_data *data)
    {
        auto view = get_signaled_view(data);

        if (view == active_view)
            refocus(active_view, workspace->get_view_layer(view));

        wayfire_shell_unmap_view(view);
    };

    connect_signal("unmap-view", &unmap_view_cb);
}

void wayfire_output::refocus(wayfire_view skip_view, uint32_t layers)
{
    wayfire_view next_focus = nullptr;
    auto views = workspace->get_views_on_workspace(workspace->get_current_workspace(), layers);

    for (auto v : views)
    {
        if (v != skip_view && v->is_mapped())
        {
            next_focus = v;
            break;
        }
    }

    set_active_view(next_focus);
}

workspace_manager::~workspace_manager()
{ }

wayfire_output::~wayfire_output()
{
    core->input->free_output_bindings(this);

    delete workspace;
    delete plugin;
    delete render;

    wl_list_remove(&destroy_listener.link);
}

wf_geometry wayfire_output::get_relative_geometry()
{
    wf_geometry g;
    g.x = g.y = 0;
    wlr_output_effective_resolution(handle, &g.width, &g.height);

    return g;
}

wf_geometry wayfire_output::get_full_geometry()
{
    wf_geometry g;
    g.x = handle->lx; g.y = handle->ly;
    wlr_output_effective_resolution(handle, &g.width, &g.height);

    return g;
}

void wayfire_output::set_transform(wl_output_transform new_tr)
{
    wlr_output_set_transform(handle, new_tr);

    emit_signal("output-resized", nullptr);
    emit_signal("transform-changed", nullptr);
}

wl_output_transform wayfire_output::get_transform()
{
    return (wl_output_transform)handle->transform;
}

void wayfire_output::set_initial_transform()
{
    transform_opt = (*core->config)[handle->name]->get_option("transform", "normal");

    config_transform_changed = [this] ()
    { set_transform(get_transform_from_string(transform_opt->as_string())); };

    transform_opt->updated.push_back(&config_transform_changed);
    wlr_output_set_transform(handle, get_transform_from_string(transform_opt->as_string()));
}

void wayfire_output::set_scale(double scale)
{
    wlr_output_set_scale(handle, scale);

    emit_signal("output-resized", nullptr);
    emit_signal("scale-changed", nullptr);
}

void wayfire_output::set_initial_scale()
{
    scale_opt = (*core->config)[handle->name]->get_option("scale", "1");

    config_scale_changed = [this] ()
    { set_scale(scale_opt->as_double()); };

    scale_opt->updated.push_back(&config_scale_changed);

    set_scale(scale_opt->as_double());
}

void wayfire_output::set_position(wf_point p)
{
    wlr_output_layout_remove(core->output_layout, handle);
    wlr_output_layout_add(core->output_layout, handle, p.x, p.y);

    emit_signal("output-position-changed", nullptr);
    emit_signal("output-resized", nullptr);
}

void wayfire_output::set_position(std::string p)
{
    wlr_output_layout_remove(core->output_layout, handle);

    if (p == "default" || p.empty())
    {
        wlr_output_layout_add_auto(core->output_layout, handle);
    } else
    {
        auto pos = parse_output_layout(p);
        wlr_output_layout_add(core->output_layout, handle, pos.x, pos.y);
    }

    emit_signal("output-position-changed", nullptr);
    emit_signal("output-resized", nullptr);
}

void wayfire_output::set_initial_position()
{
    position_opt = (*core->config)[handle->name]->get_option("layout", "default");

    config_position_changed = [this] ()
    { set_position(position_opt->as_string()); };

    position_opt->updated.push_back(&config_position_changed);
    set_position(position_opt->as_string());
}

std::tuple<int, int> wayfire_output::get_screen_size()
{
    int w, h;
    wlr_output_effective_resolution(handle, &w, &h);
    return std::make_tuple(w, h);
}

/* TODO: is this still relevant? */
void wayfire_output::ensure_pointer()
{
    /*
    auto ptr = weston_seat_get_pointer(core->get_current_seat());
    if (!ptr) return;

    int px = wl_fixed_to_int(ptr->x), py = wl_fixed_to_int(ptr->y);

    auto g = get_full_geometry();
    if (!point_inside({px, py}, g)) {
        wl_fixed_t cx = wl_fixed_from_int(g.x + g.width / 2);
        wl_fixed_t cy = wl_fixed_from_int(g.y + g.height / 2);

        weston_pointer_motion_event ev;
        ev.mask |= WESTON_POINTER_MOTION_ABS;
        ev.x = wl_fixed_to_double(cx);
        ev.y = wl_fixed_to_double(cy);

        weston_pointer_move(ptr, &ev);
    } */
}

std::tuple<int, int> wayfire_output::get_cursor_position()
{
    GetTuple(x, y, core->get_cursor_position());
    auto og = get_full_geometry();

    return std::make_tuple(x - og.x, y - og.y);
}

void wayfire_output::activate()
{
}

void wayfire_output::deactivate()
{
    // TODO: what do we do?
}

void wayfire_output::attach_view(wayfire_view v)
{
    v->set_output(this);
    workspace->add_view_to_layer(v, WF_LAYER_WORKSPACE);

    _view_signal data;
    data.view = v;
    emit_signal("attach-view", &data);
}

void wayfire_output::detach_view(wayfire_view v)
{
    _view_signal data;
    data.view = v;
    emit_signal("detach-view", &data);

    workspace->add_view_to_layer(v, 0);

    wayfire_view next = nullptr;
    auto views = workspace->get_views_on_workspace(workspace->get_current_workspace(),
                                                   WF_WM_LAYERS);
    for (auto wview : views)
    {
        if (wview->is_mapped())
        {
            next = wview;
            break;
        }
    }

    if (next == nullptr)
    {
        active_view = nullptr;
    }
    else
    {
        focus_view(next);
    }
}

void wayfire_output::bring_to_front(wayfire_view v) {
    assert(v);

    workspace->add_view_to_layer(v, -1);
    v->damage();
}

void wayfire_output::set_keyboard_focus(wlr_surface *surface, wlr_seat *seat)
{
    auto kbd = wlr_seat_get_keyboard(seat);
    if (kbd != NULL) {
        wlr_seat_keyboard_notify_enter(seat, surface,
                                       kbd->keycodes, kbd->num_keycodes,
                                       &kbd->modifiers);                                                                                                            
    } else
    {
        wlr_seat_keyboard_notify_enter(seat, surface, NULL, 0, NULL);
    }
}

void wayfire_output::set_active_view(wayfire_view v, wlr_seat *seat)
{
    if (v && !v->is_mapped())
        return set_active_view(nullptr, seat);

    if (seat == nullptr)
        seat = core->get_current_seat();

    bool refocus = (active_view == v);

    if (active_view && active_view->is_mapped() && !refocus)
        active_view->activate(false);

    active_view = v;
    if (active_view)
    {
        set_keyboard_focus(active_view->get_keyboard_focus_surface(), seat);

        if (!refocus)
            active_view->activate(true);
    } else
    {
        set_keyboard_focus(NULL, seat);
    }
}


void wayfire_output::focus_view(wayfire_view v, wlr_seat *seat)
{
    if (v && workspace->get_view_layer(v) < core->get_focused_layer())
    {
        log_info("Denying focus request for a view from a lower layer than the focused layer");
        return;
    }

    if (v && v->is_mapped())
    {
        if (v->get_keyboard_focus_surface())
        {
            set_active_view(v, seat);
            bring_to_front(v);

            focus_view_signal data;
            data.view = v;
            emit_signal("focus-view", &data);
        }
    }
    else
    {
        set_active_view(nullptr, seat);
        if (v)
            bring_to_front(v);
    }
}

wayfire_view wayfire_output::get_top_view()
{
    wayfire_view view = nullptr;
    workspace->for_each_view([&view] (wayfire_view v) {
        if (!view)
            view = v;
    }, WF_LAYER_WORKSPACE);

    return view;
}

wayfire_view wayfire_output::get_view_at_point(int x, int y)
{
    wayfire_view chosen = nullptr;

    workspace->for_each_view([x, y, &chosen] (wayfire_view v) {
        if (v->is_visible() && point_inside({x, y}, v->get_wm_geometry())) {
            if (chosen == nullptr)
                chosen = v;
        }
    }, WF_WM_LAYERS);

    return chosen;
}

bool wayfire_output::activate_plugin(wayfire_grab_interface owner, bool lower_fs)
{
    if (!owner)
        return false;

    if (core->get_active_output() != this)
        return false;

    if (active_plugins.find(owner) != active_plugins.end())
    {
        log_debug("output %s: activate plugin %s again", handle->name, owner->name.c_str());
        active_plugins.insert(owner);
        return true;
    }

    for(auto act_owner : active_plugins)
    {
        bool compatible = (act_owner->abilities_mask & owner->abilities_mask) == 0;
        if (!compatible)
            return false;
    }

    /* _activation_request is a special signal,
     * used to specify when a plugin is activated. It is used only internally, plugins
     * shouldn't listen for it */
    if (lower_fs && active_plugins.empty())
        emit_signal("_activation_request", (signal_data*)1);

    active_plugins.insert(owner);
    log_debug("output %s: activate plugin %s", handle->name, owner->name.c_str());
    return true;
}

bool wayfire_output::deactivate_plugin(wayfire_grab_interface owner)
{
    auto it = active_plugins.find(owner);
    if (it == active_plugins.end())
        return true;

    active_plugins.erase(it);
    log_debug("output %s: deactivate plugin %s", handle->name, owner->name.c_str());

    if (active_plugins.count(owner) == 0)
    {
        owner->ungrab();
        active_plugins.erase(owner);

        if (active_plugins.empty())
            emit_signal("_activation_request", nullptr);

        return true;
    }


    return false;
}

bool wayfire_output::is_plugin_active(owner_t name)
{
    for (auto act : active_plugins)
        if (act && act->name == name)
            return true;

    return false;
}

wayfire_grab_interface wayfire_output::get_input_grab_interface()
{
    for (auto p : active_plugins)
        if (p && p->is_grabbed())
            return p;

    return nullptr;
}

/* simple wrappers for core->input, as it isn't exposed to plugins */

int wayfire_output::add_key(wf_option key, key_callback* callback)
{
    return core->input->add_key(key, callback, this);
}

void wayfire_output::rem_key(key_callback* callback)
{
    core->input->rem_key(callback);
}

void wayfire_output::rem_key(int callback)
{
    core->input->rem_key(callback);
}

int wayfire_output::add_axis(wf_option mod, axis_callback* callback)
{
    return core->input->add_axis(mod, callback, this);
}

void wayfire_output::rem_axis(axis_callback* callback)
{
    core->input->rem_axis(callback);
}

void wayfire_output::rem_axis(int callback)
{
    core->input->rem_axis(callback);
}

int wayfire_output::add_button(wf_option button, button_callback* callback)
{
    return core->input->add_button(button, callback, this);
}

void wayfire_output::rem_button(button_callback* callback)
{
    core->input->rem_button(callback);
}

void wayfire_output::rem_button(int callback)
{
    core->input->rem_button(callback);
}

int wayfire_output::add_touch(uint32_t mod, touch_callback* callback)
{
    return core->input->add_touch(mod, callback, this);
}

void wayfire_output::rem_touch(int32_t id)
{
    core->input->rem_touch(id);
}

int wayfire_output::add_gesture(const wayfire_touch_gesture& gesture,
                                touch_gesture_callback* callback)
{
    return core->input->add_gesture(gesture, callback, this);
}

void wayfire_output::rem_gesture(int id)
{
    core->input->rem_gesture(id);
}

uint32_t wf_all_layers_not_below(uint32_t layer)
{
    uint32_t mask = 0;
    for (int i = 0; i < WF_TOTAL_LAYERS; i++)
    {
        if ((1 << i) >= layer)
            mask |= (1 << i);
    }

    return mask;
}
