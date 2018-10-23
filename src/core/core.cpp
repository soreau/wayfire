extern "C"
{
#include <wlr/types/wlr_screenshooter.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_idle.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_input_inhibitor.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_gamma_control.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
}

#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>

#include "output.hpp"
#include "core.hpp"
#include "signal-definitions.hpp"
#include "workspace-manager.hpp"
#include "seat/input-manager.hpp"
#include "seat/input-inhibit.hpp"
#include "seat/touch.hpp"
#include "../output/wayfire-shell.hpp"
#include "view/priv-view.hpp"
#include "config.h"

#ifdef BUILD_WITH_IMAGEIO
#include "img.hpp"
#endif

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

/* decorations impl */
struct wf_server_decoration
{
    wlr_surface *surface;
    wl_listener mode_set, destroy;
};

static void handle_decoration_mode(wl_listener*, void *data)
{
    auto decor = (wlr_server_decoration*) data;
    auto wd = (wf_server_decoration*) decor->data;

    log_info("set decoration mode %d", decor->mode);

    bool use_csd = decor->mode == WLR_SERVER_DECORATION_MANAGER_MODE_CLIENT;
    core->uses_csd[wd->surface] = use_csd;

    auto wf_surface = wf_surface_from_void(wd->surface->data);
    if (wf_surface)
        wf_surface->has_client_decoration = use_csd;
}

static void handle_decoration_destroyed(wl_listener*, void *data)
{
    auto decor = (wlr_server_decoration*) data;
    auto wd = (wf_server_decoration*) decor->data;

    wl_list_remove(&wd->mode_set.link);
    wl_list_remove(&wd->destroy.link);

    core->uses_csd.erase(wd->surface);
    delete wd;
}

static void handle_decoration_created(wl_listener*, void *data)
{
    auto decor = (wlr_server_decoration*) data;

    auto wf_decor = new wf_server_decoration;
    wf_decor->mode_set.notify = handle_decoration_mode;
    wf_decor->destroy.notify  = handle_decoration_destroyed;
    wf_decor->surface = decor->surface;
    decor->data = wf_decor;

    handle_decoration_mode(NULL, data);
}

/* virtual keyboard */
static void handle_virtual_keyboard(wl_listener*, void *data)
{
    auto kbd = (wlr_virtual_keyboard_v1*) data;
    core->input->handle_new_input(&kbd->input_device);
}

/* input-inhibit impl */
static void handle_input_inhibit_activated(wl_listener*, void *data)
{
    auto manager = (wlr_input_inhibit_manager*) data;
    log_info("set exclusive focus");
    core->input->set_exclusive_focus(manager->active_client);
}

static void handle_input_inhibit_deactivated(wl_listener*, void*)
{
    core->input->set_exclusive_focus(nullptr);
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
    protocols.screencopy = wlr_screencopy_manager_v1_create(display);
    protocols.gamma = wlr_gamma_control_manager_create(display);
    protocols.linux_dmabuf = wlr_linux_dmabuf_v1_create(display, renderer);
    protocols.export_dmabuf = wlr_export_dmabuf_manager_v1_create(display);

    protocols.decorator_manager = wlr_server_decoration_manager_create(display);
    wlr_server_decoration_manager_set_default_mode(protocols.decorator_manager,
                                                   WLR_SERVER_DECORATION_MANAGER_MODE_CLIENT);

    input_inhibit_activated.notify = handle_input_inhibit_activated;
    input_inhibit_deactivated.notify = handle_input_inhibit_deactivated;
    protocols.input_inhibit = wlr_input_inhibit_manager_create(display);
    wl_signal_add(&protocols.input_inhibit->events.activate, &input_inhibit_activated);
    wl_signal_add(&protocols.input_inhibit->events.deactivate, &input_inhibit_deactivated);

    decoration_created.notify = handle_decoration_created;
    wl_signal_add(&protocols.decorator_manager->events.new_decoration, &decoration_created);

    protocols.output_manager = wlr_xdg_output_manager_v1_create(display, output_layout);

    protocols.vkbd_manager = wlr_virtual_keyboard_manager_v1_create(display);
    vkbd_created.notify = handle_virtual_keyboard;
    wl_signal_add(&protocols.vkbd_manager->events.new_virtual_keyboard, &vkbd_created);

    protocols.idle = wlr_idle_create(display);
    protocols.idle_inhibit = wlr_idle_inhibit_v1_create(display);

    protocols.wf_shell = wayfire_shell_create(display);

#ifdef BUILD_WITH_IMAGEIO
    image_io::init();
#endif
}

void refocus_idle_cb(void *data)
{
    core->refocus_active_output_active_view();
}

void wayfire_core::wake()
{
    wl_event_loop_add_idle(ev_loop, refocus_idle_cb, 0);

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

uint32_t wayfire_core::get_keyboard_modifiers()
{
    return input->get_modifiers();
}

static void output_destroyed_callback(wl_listener *, void *data)
{
    core->remove_output(core->get_output((wlr_output*) data));
}

void wayfire_core::set_cursor(std::string name)
{
    if (name == "default")
        name = "left_ptr";

    if (input->cursor)
        wlr_xcursor_manager_set_cursor_image(input->xcursor, name.c_str(), input->cursor);
}

const int wayfire_core::invalid_coordinate;
std::tuple<int, int> wayfire_core::get_cursor_position()
{
    if (input->cursor)
        return std::tuple<int, int> (input->cursor->x, input->cursor->y);
    else
        return std::tuple<int, int> (invalid_coordinate, invalid_coordinate);
}

std::tuple<int, int> wayfire_core::get_touch_position(int id)
{
    if (!input->our_touch)
        return std::make_tuple(invalid_coordinate, invalid_coordinate);

    auto it = input->our_touch->gesture_recognizer.current.find(id);
    if (it != input->our_touch->gesture_recognizer.current.end())
        return std::make_tuple(it->second.sx, it->second.sy);

    return std::make_tuple(invalid_coordinate, invalid_coordinate);
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
void wayfire_core::add_output(wlr_output *output)
{
    log_info("add new output: %s", output->name);
    if (outputs.find(output) != outputs.end())
    {
        log_info("old output");
        return;
    }

    wayfire_output *wo = outputs[output] = new wayfire_output(output, config);
    wo->id = _last_output_id++;
    focus_output(wo);

    wo->destroy_listener.notify = output_destroyed_callback;
    wl_signal_add(&wo->handle->events.destroy, &wo->destroy_listener);

    wo->connect_signal("_surface_mapped", &input->surface_map_state_changed);
    wo->connect_signal("_surface_unmapped", &input->surface_map_state_changed);

    output_added_signal data;
    data.output = wo;
    emit_signal("output-added", &data);

    if (input->exclusive_client)
        inhibit_output(wo);
}

void wayfire_core::remove_output(wayfire_output *output)
{
    log_info("removing output: %s", output->handle->name);

    output->destroyed = true;
    outputs.erase(output->handle);

    output_removed_signal data;
    data.output = output;
    emit_signal("output-removed", &data);

    /* we have no outputs, simply quit */
    if (outputs.empty())
        std::exit(0);

    if (output == active_output)
        focus_output(outputs.begin()->second);

    /* first move each desktop view(e.g windows) to another output */
    std::vector<wayfire_view> views;
    output->workspace->for_each_view_reverse([&views] (wayfire_view view) { views.push_back(view); }, WF_WM_LAYERS);

    for (auto& view : views)
        output->detach_view(view);

    for (auto& view : views)
    {
        active_output->attach_view(view);
        active_output->focus_view(view);

        if (view->maximized)
            view->maximize_request(true);

        if (view->fullscreen)
            view->fullscreen_request(active_output, true);
    }

    /* just remove all other views - backgrounds, panels, etc.
     * desktop views have been removed by the previous cycle */
    output->workspace->for_each_view([] (wayfire_view view)
    {
        view->set_output(nullptr);
        view->close();
    }, WF_ALL_LAYERS);

    /* FIXME: this is a hack, but depends on #46 */
    input->surface_map_state_changed(NULL);

    if (input->exclusive_client)
        uninhibit_output(output);

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
