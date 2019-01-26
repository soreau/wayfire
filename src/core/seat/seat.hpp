#ifndef SEAT_HPP
#define SEAT_HPP

extern "C"
{
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/backend/libinput.h>
}

#include "output.hpp"

struct wf_drag_icon : public wayfire_surface_t
{
    wlr_drag_icon *icon;
    wl_listener map_ev, unmap_ev, destroy;

    wf_drag_icon(wlr_drag_icon *icon);

    bool is_subsurface() { return true ;}
    wf_point get_output_position();
    void damage(const wlr_box& rect);
};

struct wf_input_device
{
    wlr_input_device *device;
    wf_input_device(wlr_input_device* dev);
    ~wf_input_device();

    struct wlr_wrapper {
        wl_listener destroy;
        wf_input_device* self;
    } _wrapper;

    void update_options();

    static struct config_t
    {
        wf_option touchpad_tap_enabled;
        wf_option touchpad_dwt_enabled;
        wf_option touchpad_natural_scroll_enabled;

        void load(wayfire_config *config);
    } config;

};

#endif /* end of include guard: SEAT_HPP */
