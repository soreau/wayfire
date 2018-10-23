#ifndef SIGNAL_DEFINITIONS_HPP
#define SIGNAL_DEFINITIONS_HPP

#include "output.hpp"

/* signal definitions */
/* convenience functions are provided to get some basic info from the signal */
struct _view_signal : public signal_data
{
    wayfire_view view;
};
wayfire_view get_signaled_view(signal_data *data);

using create_view_signal     = _view_signal;
using destroy_view_signal    = _view_signal;
using map_view_signal        = _view_signal;
using unmap_view_signal      = _view_signal;
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

struct _view_state_signal : public _view_signal
{
    bool state;
};
bool get_signaled_state(signal_data *data);

/* the view-fullscreen-request signal is sent on two ocassions:
 * 1. The app requests to be fullscreened
 * 2. Some plugin requests the view to be unfullscreened
 * callbacks for this signal can differentiate between the two cases
 * in the following way: when in case 1. then view->fullscreen != signal_data->state,
 * i.e the state hasn't been applied already. However, when some plugin etc.
 * wants to use this signal, then it should apply the state in advance */
using view_maximized_signal = _view_state_signal;
using view_fullscreen_signal = _view_state_signal;



/* same as both change_viewport_request and change_viewport_notify */
struct change_viewport_signal : public signal_data
{
    int old_vx, old_vy;
    int new_vx, new_vy;
};
using change_viewport_notify = change_viewport_signal;


/* sent when the workspace implementation actually reserves the workarea */
struct reserved_workarea_signal : public signal_data
{
    wf_geometry old_workarea;
    wf_geometry new_workarea;
};

// TODO: this is a private signal, maybe we should hide it? */
struct _surface_map_state_changed_signal : public signal_data
{
    wayfire_surface_t *surface;
};


/* Part 2: Signals from wayfire_core */
struct _output_signal : public signal_data
{
    wayfire_output *output;
};

wayfire_output *get_signaled_output(signal_data *data);

using output_added_signal = _output_signal;
using output_removed_signal = _output_signal;

#endif

