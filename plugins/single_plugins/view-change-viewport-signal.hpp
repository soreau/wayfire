#ifndef VIEW_CHANGE_VIEWPORT_CPP
#define VIEW_CHANGE_VIEWPORT_CPP

#include <wayfire/signal-definitions.hpp>

struct view_change_viewport_signal : public _view_signal
{
    wf_point from, to;
};

#endif /* end of include guard: VIEW_CHANGE_VIEWPORT_CPP */
