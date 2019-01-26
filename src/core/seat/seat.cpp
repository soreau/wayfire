#include "seat.hpp"
#include "core.hpp"
#include "input-manager.hpp"
#include "render-manager.hpp"
#include "../../view/priv-view.hpp"

extern "C"
{
#include <wlr/backend/libinput.h>
}

static void handle_drag_icon_map(wl_listener* listener, void *data)
{
    auto wlr_icon = (wlr_drag_icon*) data;
    auto icon = wf_surface_from_void(wlr_icon->data);
    icon->map(wlr_icon->surface);
}

static void handle_drag_icon_unmap(wl_listener* listener, void *data)
{
    auto wlr_icon = (wlr_drag_icon*) data;
    auto icon = wf_surface_from_void(wlr_icon->data);
    icon->unmap();
}

static void handle_drag_icon_destroy(wl_listener* listener, void *data)
{
    auto wlr_icon = (wlr_drag_icon*) data;
    auto icon = (wf_drag_icon*) wlr_icon->data;

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
    map_ev.notify   = handle_drag_icon_map;
    unmap_ev.notify = handle_drag_icon_unmap;
    destroy.notify  = handle_drag_icon_destroy;

    wl_signal_add(&icon->events.map, &map_ev);
    wl_signal_add(&icon->events.unmap, &unmap_ev);
    wl_signal_add(&icon->events.destroy, &destroy);

    icon->data = this;
}

wf_point wf_drag_icon::get_output_position()
{
    auto pos = icon->is_pointer ?
        core->get_cursor_position() : core->get_touch_position(icon->touch_id);

    GetTuple(x, y, pos);

    if (is_mapped())
    {
        x += icon->surface->sx;
        y += icon->surface->sy;
    }

    if (output)
    {
        auto og = output->get_layout_geometry();
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
        auto output_geometry = output->get_layout_geometry();
        if (output_geometry & box)
        {
            auto local = box;
            local.x -= output_geometry.x;
            local.y -= output_geometry.y;

            const auto& fb = output->render->get_target_framebuffer();
            output->render->damage(fb.damage_box_from_geometry_box(local));
        }
    });
}

static void handle_new_drag_icon_cb(wl_listener*, void *data)
{
    auto di = static_cast<wlr_drag_icon*> (data);

    auto icon = std::unique_ptr<wf_drag_icon>(new wf_drag_icon(di));
    core->input->drag_icons.push_back(std::move(icon));
}

static void handle_request_set_cursor(wl_listener*, void *data)
{
    auto ev = static_cast<wlr_seat_pointer_request_set_cursor_event*> (data);
    core->input->cursor->set_cursor(ev);
}

static void handle_request_set_selection_cb(wl_listener*, void *data)
{
    auto ev = static_cast<wlr_seat_request_set_selection_event*> (data);
    wlr_seat_set_selection(core->get_current_seat(), ev->source, ev->serial);
}

static void handle_request_set_primary_selection_cb(wl_listener*, void *data)
{
    auto ev = static_cast<wlr_seat_request_set_primary_selection_event*> (data);
    wlr_seat_set_primary_selection(core->get_current_seat(), ev->source, ev->serial);
}

void input_manager::update_drag_icons()
{
    for (auto& icon : drag_icons)
    {
        if (icon->is_mapped())
            icon->update_output_position();
    }
}

void input_manager::create_seat()
{
    cursor = nonstd::make_unique<wf_cursor> ();

    request_set_cursor.notify = handle_request_set_cursor;
    wl_signal_add(&seat->events.request_set_cursor, &request_set_cursor);

    new_drag_icon.notify      = handle_new_drag_icon_cb;
    wl_signal_add(&seat->events.new_drag_icon, &new_drag_icon);

    request_set_selection.notify = handle_request_set_selection_cb;
    wl_signal_add(&seat->events.request_set_selection, &request_set_selection);

    request_set_primary_selection.notify = handle_request_set_primary_selection_cb;
    wl_signal_add(&seat->events.request_set_primary_selection, &request_set_primary_selection);
}

wf_input_device::config_t wf_input_device::config;
void wf_input_device::config_t::load(wayfire_config *config)
{
    auto section = (*config)["input"];
    touchpad_tap_enabled            = section->get_option("tap_to_click", "1");
    touchpad_dwt_enabled            = section->get_option("disable_while_typing", "0");
    touchpad_natural_scroll_enabled = section->get_option("natural_scroll", "0");
}

static void handle_device_destroy_cb(wl_listener *listener, void*)
{
    wf_input_device::wlr_wrapper *wrapper = wl_container_of(listener, wrapper, destroy);
    core->input->handle_input_destroyed(wrapper->self->device);
}

wf_input_device::wf_input_device(wlr_input_device *dev)
{
    device = dev;
    update_options();

    _wrapper.self = this;
    _wrapper.destroy.notify = handle_device_destroy_cb;

    wl_signal_add(&dev->events.destroy, &_wrapper.destroy);
}

wf_input_device::~wf_input_device()
{
    wl_list_remove(&_wrapper.destroy.link);
}

void wf_input_device::update_options()
{
    /* We currently support options only for libinput devices */
    if (!wlr_input_device_is_libinput(device))
        return;

    auto dev = wlr_libinput_get_device_handle(device);
    assert(dev);

    /* we are configuring a touchpad */
    if (libinput_device_config_tap_get_finger_count(dev) > 0)
    {
        libinput_device_config_tap_set_enabled(dev,
            config.touchpad_tap_enabled->as_cached_int() ?
            LIBINPUT_CONFIG_TAP_ENABLED : LIBINPUT_CONFIG_TAP_DISABLED);

        libinput_device_config_dwt_set_enabled(dev,
            config.touchpad_dwt_enabled->as_cached_int() ?
            LIBINPUT_CONFIG_DWT_ENABLED : LIBINPUT_CONFIG_DWT_DISABLED);

        if (libinput_device_config_scroll_has_natural_scroll(dev) > 0)
        {
            libinput_device_config_scroll_set_natural_scroll_enabled(dev,
                    (bool)config.touchpad_natural_scroll_enabled->as_cached_int());
        }
    }
}
