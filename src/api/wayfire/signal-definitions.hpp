#ifndef SIGNAL_DEFINITIONS_HPP
#define SIGNAL_DEFINITIONS_HPP

#include "wayfire/view.hpp"
#include "wayfire/output.hpp"

/* signal definitions */
/* convenience functions are provided to get some basic info from the signal */
struct _view_signal : public wf::signal_data_t
{
    wayfire_view view;
};
wayfire_view get_signaled_view(wf::signal_data_t *data);

using create_view_signal     = _view_signal;
using destroy_view_signal    = _view_signal;
using unmap_view_signal      = _view_signal;
using pre_unmap_view_signal  = _view_signal;

struct map_view_signal : public _view_signal
{
    /* Indicates whether the position already has its initial posittion */
    bool is_positioned = false;
};

/* Indicates the view is no longer available, for ex. it has been minimized
 * or unmapped */
using view_disappeared_signal = _view_signal;

using focus_view_signal      = _view_signal;
using view_set_parent_signal = _view_signal;
using move_request_signal    = _view_signal;
using title_changed_signal   = _view_signal;
using app_id_changed_signal  = _view_signal;

struct resize_request_signal : public _view_signal
{
    uint32_t edges;
};

/* sent when the view geometry changes */
struct view_geometry_changed_signal : public _view_signal
{
    wf_geometry old_geometry;
};

struct view_tiled_signal : public _view_signal
{
    uint32_t edges;
    bool carried_out = false;
    wf_geometry desired_size;
};

struct view_fullscreen_signal : public _view_signal
{
    bool state;
    bool carried_out = false;
    wf_geometry desired_size;
};

struct view_minimize_request_signal : public _view_signal
{
    bool state;
    /* If some plugin wants to delay the (un)minimize of the view, it needs to
     * listen for the view-minimize-request and set carried_out to true.
     * It is a hint to core that the action will be (or already was) performed
     * by a plugin, and minimized state shouldn't be further changed by core */
    bool carried_out = false;
};

/* same as both change_viewport_request and change_viewport_notify */
struct change_viewport_signal : public wf::signal_data_t
{
    bool carried_out;
    wf_point old_viewport, new_viewport;
};
using change_viewport_notify = change_viewport_signal;

/* sent when the workspace implementation actually reserves the workarea */
struct reserved_workarea_signal : public wf::signal_data_t
{
    wf_geometry old_workarea;
    wf_geometry new_workarea;
};

// TODO: this is a private signal, maybe we should hide it? */
struct _surface_map_state_changed_signal : public wf::signal_data_t
{
    wf::surface_interface_t *surface;
};

/* Part 2: Signals from wf::output_layout_t */
struct _output_signal : public wf::signal_data_t
{
    wf::output_t *output;
};

wf::output_t *get_signaled_output(wf::signal_data_t *data);

using output_added_signal = _output_signal;
using output_removed_signal = _output_signal;

namespace wf
{
    class input_device_t;
    /* Used in the tablet-mode and lid-state signals from core */
    struct switch_signal : public wf::signal_data_t
    {
        nonstd::observer_ptr<input_device_t> device;
        bool state;
    };

    /* in input-device-added and input-device-removed signals from core */
    struct input_device_signal : public signal_data_t
    {
        nonstd::observer_ptr<input_device_t> device;
    };

    /**
     * Used for the following events:
     *
     * pointer_motion, pointer_motion_abs, pointer_button, pointer_axis,
     * pointer_swipe_begin, pointer_swipe_update, pointer_swipe_end,
     * pointer_pinch_begin, pointer_pinch_update, pointer_pinch_end,
     *
     * keyboard_key,
     *
     * touch_down, touch_up, touch_motion,
     *
     * tablet_proximity, tablet_axis, tablet_button, tablet_tip
     *
     * The template parameter is the corresponding type of wlr events.
     *
     * The input event signals are sent from core whenever a new input from an
     * input device arrives. The events are sent before any processing is done,
     * and they are independent of plugin input grabs and other wayfire input
     * mechanisms.
     *
     * The event data can be modified by plugins, and then the modified event
     * will be used instead. However plugins which modify the event must ensure
     * that subsequent events are adjusted accordingly as well.
     */
    template<class wlr_event_t>
    struct input_event_signal : public wf::signal_data_t
    {
        /* The event as it has arrived from wlroots */
        wlr_event_t *event;
    };
}

#endif

