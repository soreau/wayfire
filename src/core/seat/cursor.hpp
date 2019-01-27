#ifndef CURSOR_HPP
#define CURSOR_HPP

#include "seat.hpp"

extern "C"
{
#include <wlr/types/wlr_xcursor_manager.h>
}


struct wf_cursor
{
    wf_cursor();
    ~wf_cursor();

    void attach_device(wlr_input_device *device);
    void detach_device(wlr_input_device *device);

    void set_cursor(wlr_seat_pointer_request_set_cursor_event *ev);
    void set_cursor(std::string name);

    void hide_cursor();
    void warp_cursor(int x, int y);

    void init_xcursor();

    wl_listener button, motion, motion_absolute, axis, frame;
    signal_callback_t config_reloaded;

    wlr_cursor *cursor = NULL;
    wlr_xcursor_manager *xcursor = NULL;
};

#endif /* end of include guard: CURSOR_HPP */
