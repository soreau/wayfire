#include <wayfire/util.hpp>

#include <cmath>

static inline double vswipe_process_delta(const double delta,
        const double accumulated_dx,
        const int vx, const int vw,
        const double speed_cap = 0.5,
        const double speed_factor = 256)
{
    // The slowdown below must be applied differently for going out of bounds.
    double sdx_offset = accumulated_dx;
    if (vx == 0 && accumulated_dx > 0.0)
        sdx_offset = accumulated_dx + 1.0;
    if (vx == vw - 1 && accumulated_dx < 0.0)
        sdx_offset = accumulated_dx - 1.0;

    // To achieve a "rubberband" resistance effect when going too far, ease-in
    // of the whole swiped distance is used as a slowdown factor for the current delta.
    const double ease = 1.0 - std::pow(std::abs(sdx_offset) - 0.025, 4.0);

    // If we're moving further in the limit direction, slow down all the way
    // to extremely slow, but reversing the direction should be easier.
    const double slowdown = clamp(ease,
            std::signbit(delta) == std::signbit(sdx_offset) ? 0.005 : 0.2, 1.0);

    return clamp(delta / speed_factor, -speed_cap, speed_cap) * slowdown;
}

static inline int vswipe_finish_target(const double accumulated_dx,
        const int vx, const int vw,
        const double last_deltas = 0,
        const double move_threshold = 0.35,
        const double fast_threshold = 24)
{
    int target_dx = 0;

    const bool too_left = vx == 0 && (accumulated_dx > 0.0 || last_deltas > 0.0);
    const bool too_right = vx == vw - 1 && (accumulated_dx < 0.0 || last_deltas < 0.0);

    if (!too_left && (accumulated_dx > move_threshold || last_deltas > fast_threshold))
        target_dx = 1;
    else if (!too_right && (accumulated_dx < -move_threshold || last_deltas < -fast_threshold))
        target_dx = -1;

    return target_dx;
}
