#include <wayfire/plugin.hpp>
#include <wayfire/output.hpp>
#include <wayfire/core.hpp>
#include <wayfire/debug.hpp>
#include <wayfire/view.hpp>
#include <wayfire/view-transform.hpp>
#include <wayfire/render-manager.hpp>
#include <wayfire/workspace-stream.hpp>
#include <wayfire/workspace-manager.hpp>
#include <wayfire/signal-definitions.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <wayfire/util/duration.hpp>

#include <cmath>
#include <utility>
#include "vswipe-processing.hpp"

class vswipe : public wf::plugin_interface_t
{
    private:
        struct {
            /* When the workspace is set to (-1, -1), means no such workspace */
            wf::workspace_stream_t prev, curr, next;
        } streams;

        enum swipe_direction_t
        {
            HORIZONTAL = 0,
            VERTICAL,
            UNKNOWN,
        };

        struct {
            bool swiping = false;
            bool animating = false;
            swipe_direction_t direction;

            wf::pointf_t initial_deltas;
            double gap = 0.0;

            double delta_prev = 0.0;
            double delta_last = 0.0;

            int vx = 0;
            int vy = 0;
            int vw = 0;
            int vh = 0;
        } state;

        wf::render_hook_t renderer;
        wf::option_wrapper_t<bool> enable_horizontal{"vswipe/enable_horizontal"};
        wf::option_wrapper_t<bool> enable_vertical{"vswipe/enable_vertical"};
        wf::option_wrapper_t<bool> smooth_transition{"vswipe/enable_smooth_transition"};

        wf::option_wrapper_t<wf::color_t> background_color{"vswipe/background"};
        wf::option_wrapper_t<int> animation_duration{"vswipe/duration"};
        wf::animation::simple_animation_t smooth_delta{animation_duration};

        wf::option_wrapper_t<int> fingers{"vswipe/fingers"};
        wf::option_wrapper_t<double> gap{"vswipe/gap"};
        wf::option_wrapper_t<double> threshold{"vswipe/threshold"};
        wf::option_wrapper_t<double> delta_threshold{"vswipe/delta_threshold"};
        wf::option_wrapper_t<double> speed_factor{"vswipe/speed_factor"};
        wf::option_wrapper_t<double> speed_cap{"vswipe/speed_cap"};

    public:

    void init() override
    {
        grab_interface->name = "vswipe";
        grab_interface->capabilities = wf::CAPABILITY_MANAGE_COMPOSITOR;
        grab_interface->callbacks.cancel = [=] () { finalize_and_exit(); };

        wf::get_core().connect_signal("pointer_swipe_begin", &on_swipe_begin);
        wf::get_core().connect_signal("pointer_swipe_update", &on_swipe_update);
        wf::get_core().connect_signal("pointer_swipe_end", &on_swipe_end);
        renderer = [=] (const wf::framebuffer_t& buffer) { render(buffer); };
    }

    /**
     * Get the translation matrix for a workspace at the given offset.
     * Depends on the swipe direction
     */
    glm::mat4 get_translation(double offset)
    {
        switch (state.direction)
        {
            case UNKNOWN:
                return glm::mat4(1.0);
            case HORIZONTAL:
                return glm::translate(glm::mat4(1.0),
                    glm::vec3(offset, 0.0, 0.0));
            case VERTICAL:
                return glm::translate(glm::mat4(1.0),
                    glm::vec3(0.0, -offset, 0.0));
        }

        assert(false); // not reached
    }

    void render(const wf::framebuffer_t &fb)
    {
        if (!smooth_delta.running() && !state.swiping)
            finalize_and_exit();

        update_stream(streams.prev);
        update_stream(streams.curr);
        update_stream(streams.next);

        OpenGL::render_begin(fb);
        OpenGL::clear(background_color);
        fb.scissor(fb.framebuffer_box_from_geometry_box(fb.geometry));

        gl_geometry out_geometry = {
            .x1 = -1,
            .y1 = 1,
            .x2 = 1,
            .y2 = -1,
        };

        auto swipe = get_translation(smooth_delta * 2);
        /* Undo rotation of the workspace */
        auto workspace_transform = glm::inverse(fb.transform);
        swipe = swipe * workspace_transform;

        if (streams.prev.ws.x >= 0)
        {
            auto prev = get_translation(-2.0 - state.gap * 2.0);
            OpenGL::render_transformed_texture(streams.prev.buffer.tex,
                out_geometry, {}, fb.transform * prev * swipe);
        }

        OpenGL::render_transformed_texture(streams.curr.buffer.tex,
            out_geometry, {}, fb.transform * swipe);

        if (streams.next.ws.x >= 0)
        {
            auto next = get_translation(2.0 + state.gap * 2.0);
            OpenGL::render_transformed_texture(streams.next.buffer.tex,
                out_geometry, {}, fb.transform * next * swipe);
        }

        GL_CALL(glUseProgram(0));
        OpenGL::render_end();
    }

    inline void update_stream(wf::workspace_stream_t& s)
    {
        if (s.ws.x < 0 || s.ws.y < 0)
            return;

        if (!s.running)
            output->render->workspace_stream_start(s);
        else
            output->render->workspace_stream_update(s);
    }

    template<class wlr_event> using event = wf::input_event_signal<wlr_event>;
    wf::signal_callback_t on_swipe_begin = [=] (wf::signal_data_t *data)
    {
        if (!enable_horizontal && !enable_vertical)
            return;

        if (output->is_plugin_active(grab_interface->name))
            return;

        auto ev = static_cast<
            event<wlr_event_pointer_swipe_begin>*> (data)->event;
        if (static_cast<int>(ev->fingers) != fingers)
            return;

        // Plugins are per output, swipes are global, so we need to handle
        // the swipe only when the cursor is on *our* (plugin instance's) output
        if (!(output->get_relative_geometry() & output->get_cursor_position()))
            return;

        state.swiping = true;
        state.direction = UNKNOWN;
        state.initial_deltas = {0.0, 0.0};
        smooth_delta.set(0, 0);

        state.delta_last = 0;
        state.delta_prev = 0;

        state.gap = gap / output->get_screen_size().width;

        // We switch the actual workspace before the finishing animation,
        // so the rendering of the animation cannot dynamically query current
        // workspace again, so it's stored here
        auto grid = output->workspace->get_workspace_grid_size();
        auto ws = output->workspace->get_current_workspace();
        state.vw = grid.width;
        state.vh = grid.height;
        state.vx = ws.x;
        state.vy = ws.y;

        /* Invalid in the beginning, because we want a few swipe events to
         * determine whether swipe is horizontal or vertical */
        streams.prev.ws = {-1, -1};
        streams.next.ws = {-1, -1};
        streams.curr.ws = wf::point_t {ws.x, ws.y};
    };

    void start_swipe(swipe_direction_t direction)
    {
        assert(direction != UNKNOWN);
        state.direction = direction;

        bool was_active = output->is_plugin_active(grab_interface->name);
        if (!output->activate_plugin(grab_interface))
            return;

        grab_interface->grab();
        wf::get_core().focus_output(output);

        output->render->set_renderer(renderer);
        if (!was_active)
            output->render->set_redraw_always();

        auto ws = output->workspace->get_current_workspace();
        auto grid = output->workspace->get_workspace_grid_size();
        if (direction == HORIZONTAL)
        {
            if (ws.x > 0)
                streams.prev.ws = wf::point_t{ws.x - 1, ws.y};
            if (ws.x < grid.width - 1)
                streams.next.ws = wf::point_t{ws.x + 1, ws.y};
        } else //if (direction == VERTICAL)
        {
            if (ws.y > 0)
                streams.prev.ws = wf::point_t{ws.x, ws.y - 1};
            if (ws.y < grid.height - 1)
                streams.next.ws = wf::point_t{ws.x, ws.y + 1};
        }
    }

    wf::signal_callback_t on_swipe_update = [&] (wf::signal_data_t *data)
    {
        if (!state.swiping)
            return;

        auto ev = static_cast<
            event<wlr_event_pointer_swipe_update>*> (data)->event;

        if (state.direction == UNKNOWN)
        {
            auto grid = output->workspace->get_workspace_grid_size();

            // XXX: how to determine this??
            static constexpr double initial_direction_threshold = 0.05;
            state.initial_deltas.x +=
                std::abs(ev->dx) / speed_factor;
            state.initial_deltas.y +=
                std::abs(ev->dy) / speed_factor;

            bool horizontal =
                state.initial_deltas.x > initial_direction_threshold;
            bool vertical =
                state.initial_deltas.y > initial_direction_threshold;

            horizontal &= state.initial_deltas.x > state.initial_deltas.y;
            vertical &= state.initial_deltas.y > state.initial_deltas.x;

            if (horizontal && grid.width > 1 && enable_horizontal)
            {
                start_swipe(HORIZONTAL);
            }
            else if (vertical && grid.height > 1 && enable_vertical)
            {
                start_swipe(VERTICAL);
            }

            if (state.direction == UNKNOWN)
                return;
        }

        const double cap = speed_cap;
        const double fac = speed_factor;

        state.delta_prev = state.delta_last;
        double current_delta_processed;
        if (state.direction == HORIZONTAL)
        {
            current_delta_processed = vswipe_process_delta(ev->dx,
                smooth_delta, state.vx, state.vw, cap, fac);
            state.delta_last = ev->dx;
        } else
        {
            current_delta_processed = vswipe_process_delta(ev->dy,
                smooth_delta, state.vy, state.vh, cap, fac);
            state.delta_last = ev->dy;
        }

        double new_delta_end = smooth_delta.end + current_delta_processed;
        double new_delta_start = smooth_transition ?  smooth_delta : new_delta_end;
        smooth_delta.animate(new_delta_start, new_delta_end);
    };

    wf::signal_callback_t on_swipe_end = [=] (wf::signal_data_t *data)
    {
        if (!state.swiping || !output->is_plugin_active(grab_interface->name))
        {
            state.swiping = false;
            return;
        }

        state.swiping = false;
        const double move_threshold = wf::clamp((double)threshold, 0.0, 1.0);
        const double fast_threshold = wf::clamp((double)delta_threshold, 0.0, 1000.0);

        int target_delta = 0;
        wf::point_t target_workspace = {state.vx, state.vy};

        switch (state.direction)
        {
            case UNKNOWN:
                target_delta = 0;
                break;
            case HORIZONTAL:
                target_delta = vswipe_finish_target(smooth_delta.end,
                    state.vx, state.vw, state.delta_prev + state.delta_last,
                    move_threshold, fast_threshold);
                target_workspace.x -= target_delta;
                break;
            case VERTICAL:
                target_delta = vswipe_finish_target(smooth_delta.end,
                    state.vy, state.vh, state.delta_prev + state.delta_last,
                    move_threshold, fast_threshold);
                target_workspace.y -= target_delta;
                break;
        }

        smooth_delta.animate(target_delta + state.gap * target_delta);
        output->workspace->set_workspace(target_workspace);
        state.animating = true;
    };

    void finalize_and_exit()
    {
        state.swiping = false;
        grab_interface->ungrab();

        if (output->is_plugin_active(grab_interface->name))
            output->render->set_redraw_always(false);

        output->deactivate_plugin(grab_interface);

        if (streams.prev.running)
            output->render->workspace_stream_stop(streams.prev);
        output->render->workspace_stream_stop(streams.curr);
        if (streams.next.running)
            output->render->workspace_stream_stop(streams.next);

        output->render->set_renderer(nullptr);
        state.animating = false;
    }

    void fini() override
    {
        if (state.swiping)
            finalize_and_exit();

        OpenGL::render_begin();
        streams.prev.buffer.release();
        streams.curr.buffer.release();
        streams.next.buffer.release();
        OpenGL::render_end();

        wf::get_core().disconnect_signal("pointer_swipe_begin", &on_swipe_begin);
        wf::get_core().disconnect_signal("pointer_swipe_update", &on_swipe_update);
        wf::get_core().disconnect_signal("pointer_swipe_end", &on_swipe_end);
    }
};

DECLARE_WAYFIRE_PLUGIN(vswipe);
