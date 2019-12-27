#ifndef SEAT_HPP
#define SEAT_HPP

extern "C"
{
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/backend/libinput.h>
#include <wlr/types/wlr_switch.h>
}

#include "../../view/surface-impl.hpp"
#include "wayfire/output.hpp"
#include "wayfire/input-device.hpp"

struct wf_drag_icon : public wf::wlr_child_surface_base_t
{
    wlr_drag_icon *icon;
    wf::wl_listener_wrapper on_map, on_unmap, on_destroy;

    wf_drag_icon(wlr_drag_icon *icon);
    wf_point get_offset() override;

    void damage();
    void damage_surface_box(const wlr_box& rect) override;
};

class wf_input_device_internal : public wf::input_device_t
{
    public:
    wf_input_device_internal(wlr_input_device* dev);
    virtual ~wf_input_device_internal() = default;

    wf::wl_listener_wrapper on_destroy;
    virtual void update_options() {};
};

/** Convert the given point to a surface-local point */
wf_pointf get_surface_relative_coords(wf::surface_interface_t *surface,
    const wf_pointf& point);

#endif /* end of include guard: SEAT_HPP */
