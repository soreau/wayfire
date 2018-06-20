#include <output.hpp>
#include <debug.hpp>
#include <opengl.hpp>
#include <core.hpp>
#include <render-manager.hpp>
#include <workspace-manager.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <animation.hpp>

/* TODO: this file should be included in some header maybe(plugin.hpp) */
#include <linux/input-event-codes.h>
#include "view-change-viewport-signal.hpp"


class wayfire_expo : public wayfire_plugin_t
{
    private:
        key_callback toggle_cb, press_cb, move_cb;
        touch_gesture_callback touch_toggle_cb;

        wf_option action_button;
        wf_option background_color, zoom_animation_duration;
        wf_option delimiter_offset;

        wf_duration zoom_animation;

        render_hook_t renderer;

        struct {
            bool active = false;
            bool moving = false;
            bool button_pressed = false;

            bool zoom_in = false;
        } state;
        int target_vx, target_vy;
        std::tuple<int, int> move_started_ws;

        std::vector<std::vector<wf_workspace_stream*>> streams;
        signal_callback_t resized_cb;

    public:
    void init(wayfire_config *config)
    {
        grab_interface->name = "expo";
        grab_interface->abilities_mask = WF_ABILITY_CONTROL_WM;

        auto section = config->get_section("expo");
        auto toggle_key = section->get_option("toggle", "<super> KEY_E");

        if (!toggle_key->as_key().valid())
            return;

        GetTuple(vw, vh, output->workspace->get_workspace_grid_size());
        streams.resize(vw);

        for (int i = 0; i < vw; i++) {
            for (int j = 0;j < vh; j++) {
                streams[i].push_back(new wf_workspace_stream);
                streams[i][j]->tex = streams[i][j]->fbuff = -1;
                streams[i][j]->ws = std::make_tuple(i, j);
            }
        }

        zoom_animation_duration = section->get_option("duration", "300");
        zoom_animation = wf_duration(zoom_animation_duration);

        delimiter_offset = section->get_option("offset", "10");

        toggle_cb = [=] (uint32_t key) {
            if (!state.active) {
                activate();
            } else {
                if (!zoom_animation.running() || state.zoom_in)
                    deactivate();
            }
        };

        touch_toggle_cb = [=] (wayfire_touch_gesture*) {
            if (state.active)
                deactivate();
            else
                activate();
        };

        output->add_key(toggle_key, &toggle_cb);

        wayfire_touch_gesture activate_gesture;
        activate_gesture.type = GESTURE_PINCH;
        activate_gesture.finger_count = 3;
        output->add_gesture(activate_gesture, &touch_toggle_cb);

        grab_interface->callbacks.pointer.button = [=] (uint32_t button, uint32_t state)
        {
            if (button != BTN_LEFT)
                return;

            GetTuple(x, y, output->get_cursor_position());
            handle_input_press(x, y, state);

        };
        grab_interface->callbacks.pointer.motion = [=] (int32_t x, int32_t y)
        {
            handle_input_move(x, y);
        };

        grab_interface->callbacks.touch.down = [=] (int32_t id, wl_fixed_t sx, wl_fixed_t sy)
        {
            if (id > 0) return;
            handle_input_press(sx, sy, WLR_BUTTON_PRESSED);
        };

        grab_interface->callbacks.touch.up = [=] (int32_t id)
        {
            if (id > 0) return;
            handle_input_press(0, 0, WLR_BUTTON_RELEASED);
        };

        grab_interface->callbacks.touch.motion = [=] (int32_t id, int32_t sx, int32_t sy)
        {
            if (id > 0) // we handle just the first finger
                return;

            handle_input_move(sx, sy);
        };

        renderer = [=] (uint32_t fb) { render(fb); };
        resized_cb = [=] (signal_data*) {
            for (int i = 0; i < vw; i++) {
                for (int j = 0; j < vh; j++) {
                    GL_CALL(glDeleteTextures(1, &streams[i][j]->tex));
                    GL_CALL(glDeleteFramebuffers(1, &streams[i][j]->fbuff));
                    streams[i][j]->tex = streams[i][j]->fbuff = -1;
                }
            }
        };

        output->connect_signal("output-resized", &resized_cb);

        background_color = section->get_option("background", "0 0 0 1");
    }

    void activate()
    {
        if (!output->activate_plugin(grab_interface))
            return;

        grab_interface->grab();

        state.active = true;
        state.button_pressed = false;
        state.moving = false;
        zoom_animation.start();

        GetTuple(vx, vy, output->workspace->get_current_workspace());

        target_vx = vx;
        target_vy = vy;
        calculate_zoom(true);

        output->render->set_renderer(renderer);
        output->render->auto_redraw(true);
    }

    void deactivate()
    {
        zoom_animation.start();
        state.moving = false;

        output->workspace->set_workspace(std::make_tuple(target_vx, target_vy));

        calculate_zoom(false);
        update_zoom();
    }

    wf_geometry get_grid_geometry()
    {
        GetTuple(vw, vh, output->workspace->get_workspace_grid_size());
        auto full_g = output->get_full_geometry();

        wf_geometry grid;
        grid.x = grid.y = 0;
        grid.width = full_g.width * vw;
        grid.height = full_g.height * vh;

        return grid;
    }

    int sx, sy;
    wayfire_view moving_view;
    void handle_input_move(wl_fixed_t x, wl_fixed_t y)
    {
        int cx = x;
        int cy = y;

        if (state.button_pressed && !zoom_animation.running())
        {
            start_move(cx, cy);
            state.button_pressed = false;
        }

        if (!state.moving || !moving_view)
            return;

        int global_x = cx, global_y = cy;
        input_coordinates_to_global_coordinates(global_x, global_y);

        auto grid = get_grid_geometry();
        if (!point_inside({global_x, global_y}, grid))
            return;

        GetTuple(vw, vh, output->workspace->get_workspace_grid_size());
        int max = std::max(vw, vh);

        auto g = moving_view->get_wm_geometry();
        moving_view->move(g.x + (cx - sx) * max, g.y + (cy - sy) * max);

        sx = cx;
        sy = cy;

        update_target_workspace(sx, sy);
    }

    void start_move(int x, int y)
    {
        /* target workspace has been updated on the last click
         * so it has accurate information about views' viewport */
        if (!moving_view)
            return;

        move_started_ws = std::tuple<int, int> {target_vx, target_vy};
        state.moving = true;
        output->bring_to_front(moving_view);

        moving_view->set_moving(true);

        if (moving_view->fullscreen)
            moving_view->fullscreen_request(moving_view->get_output(), false);
    }

    void end_move()
    {
        state.moving = false;

        if (moving_view)
        {
            view_change_viewport_signal data;
            data.view = moving_view;
            data.from = move_started_ws;
            data.to   = std::tuple<int, int> {target_vx, target_vy};

            output->emit_signal("view-change-viewport", &data);
            moving_view->set_moving(false);
        }
    }
    void input_coordinates_to_global_coordinates(int &sx, int &sy)
    {
        auto og = output->get_full_geometry();

        GetTuple(vw, vh, output->workspace->get_workspace_grid_size());

        float max = std::max(vw, vh);

        float grid_start_x = og.width * (max - vw) / float(max) / 2;
        float grid_start_y = og.height * (max - vh) / float(max) / 2;

        sx -= grid_start_x;
        sy -= grid_start_y;

        sx *= max;
        sy *= max;
    }

    wayfire_view find_view_at(int sx, int sy)
    {
        GetTuple(vx, vy, output->workspace->get_current_workspace());
        auto og = output->get_full_geometry();

        input_coordinates_to_global_coordinates(sx, sy);

        sx -= vx * og.width;
        sy -= vy * og.height;

        wayfire_view search = nullptr;
        output->workspace->for_each_view([&search, sx, sy] (wayfire_view v) {
            if (!search && point_inside({sx, sy}, v->get_wm_geometry()))
            search = v;
        }, WF_WM_LAYERS);

        return search;
    }

    void update_target_workspace(int x, int y) {
        auto og = output->get_full_geometry();

        input_coordinates_to_global_coordinates(x, y);

        auto grid = get_grid_geometry();
        if (!point_inside({x, y}, grid))
            return;

        target_vx = x / og.width;
        target_vy = y / og.height;
    }

    void handle_input_press(int32_t x, int32_t y, uint32_t state)
    {
        if (zoom_animation.running())
            return;

        if (state == WLR_BUTTON_RELEASED && !this->state.moving) {
            deactivate();
        } else if (state == WLR_BUTTON_RELEASED) {
            end_move();
        } else {
            this->state.button_pressed = true;
            sx = x;
            sy = y;

            moving_view = find_view_at(sx, sy);
            update_target_workspace(sx, sy);
        }
    }

    struct {
        float scale_x, scale_y,
              off_x, off_y,
              delimiter_offset;
    } render_params;

    void render(uint32_t target_fb)
    {
        GetTuple(vw, vh, output->workspace->get_workspace_grid_size());
        GetTuple(vx, vy, output->workspace->get_current_workspace());
        GetTuple(w,  h,  output->get_screen_size());

        OpenGL::use_default_program();

        float angle;
        switch(output->get_transform()) {
            case WL_OUTPUT_TRANSFORM_NORMAL:
                angle = 0;
                break;
            case WL_OUTPUT_TRANSFORM_90:
                angle = 3 * M_PI / 2;
                break;
            case WL_OUTPUT_TRANSFORM_180:
                angle = M_PI;
                break;
            case WL_OUTPUT_TRANSFORM_270:
                angle = M_PI / 2;
                break;
            default:
                angle = 0;
                break;
        }

        glm::mat4 matrix(1.0);
        auto rot       = glm::rotate(matrix, angle, glm::vec3(0, 0, 1));
        auto translate = glm::translate(matrix, glm::vec3(render_params.off_x, render_params.off_y, 0));
        auto scale     = glm::scale(matrix, glm::vec3(render_params.scale_x, render_params.scale_y, 1));
        matrix = rot * translate * scale;

        OpenGL::use_device_viewport();
        auto vp = OpenGL::get_device_viewport();
        GL_CALL(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target_fb));
        GL_CALL(glScissor(vp.x, vp.y, vp.width, vp.height));

        auto clear_color = background_color->as_cached_color();
        glClearColor(clear_color.r, clear_color.g, clear_color.b, clear_color.a);
        glClear(GL_COLOR_BUFFER_BIT);

        auto vp_geometry = OpenGL::get_device_viewport();

        for(int j = 0; j < vh; j++) {
            for(int i = 0; i < vw; i++) {
                if (!streams[i][j]->running) {
                    output->render->workspace_stream_start(streams[i][j]);
                } else {
                    output->render->workspace_stream_update(streams[i][j],
                            render_params.scale_x, render_params.scale_y);
                }

                float tlx = (i - vx) * w + render_params.delimiter_offset;
                float tly = (j - vy) * h + render_params.delimiter_offset;

                float brx = tlx + w - 2 * render_params.delimiter_offset;
                float bry = tly + h - 2 * render_params.delimiter_offset;

                gl_geometry out_geometry = {
                    2.0f * tlx / w - 1.0f, 1.0f - 2.0f * tly / h,
                    2.0f * brx / w - 1.0f, 1.0f - 2.0f * bry / h};

                gl_geometry texg;
                texg.x1 = 0;
                texg.y1 = 0;
                texg.x2 = streams[i][j]->scale_x;
                texg.y2 = streams[i][j]->scale_y;

                GL_CALL(glEnable(GL_SCISSOR_TEST));
                GL_CALL(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target_fb));

                GL_CALL(glScissor(vp_geometry.x, vp_geometry.y,
                                  vp_geometry.width, vp_geometry.height));

                OpenGL::render_transformed_texture(streams[i][j]->tex, out_geometry, texg, matrix,
                        glm::vec4(1), TEXTURE_TRANSFORM_USE_DEVCOORD |
                        TEXTURE_USE_TEX_GEOMETRY | TEXTURE_TRANSFORM_INVERT_Y);

                GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, 0));
                GL_CALL(glDisable(GL_SCISSOR_TEST));
            }
        }

        update_zoom();
    }

    struct tup {
        float begin, end;
    };

    struct {
        tup scale_x, scale_y,
            off_x, off_y;
        wf_transition delimiter_offset;
    } zoom_target;

    void calculate_zoom(bool zoom_in)
    {
        GetTuple(vw, vh, output->workspace->get_workspace_grid_size());

        int max = std::max(vw, vh);

        float diff_w = (max - vw) / (1. * max);
        float diff_h = (max - vh) / (1. * max);

        vw = vh = max;

        float center_w = vw / 2.f;
        float center_h = vh / 2.f;

        if (zoom_in) {
            render_params.scale_x = render_params.scale_y = 1;
        } else {
            render_params.scale_x = 1.f / vw;
            render_params.scale_y = 1.f / vh;
        }

        zoom_target.scale_x = {1, 1.f / vw};
        zoom_target.scale_y = {1, 1.f / vh};

        zoom_target.off_x   = {0, ((target_vx - center_w) * 2.f + 1.f) / vw + diff_w};
        zoom_target.off_y   = {0, ((center_h - target_vy) * 2.f - 1.f) / vh - diff_h};

        zoom_target.delimiter_offset = {0, (float)delimiter_offset->as_cached_int()};

        if (!zoom_in)
        {
            std::swap(zoom_target.scale_x.begin, zoom_target.scale_x.end);
            std::swap(zoom_target.scale_y.begin, zoom_target.scale_y.end);
            std::swap(zoom_target.off_x.begin, zoom_target.off_x.end);
            std::swap(zoom_target.off_y.begin, zoom_target.off_y.end);
            std::swap(zoom_target.delimiter_offset.start,
                      zoom_target.delimiter_offset.end);
        }

        state.zoom_in = zoom_in;
        zoom_animation.start();
    }

    void update_zoom()
    {

        render_params.scale_x = zoom_animation.progress(zoom_target.scale_x.begin,
                                                        zoom_target.scale_x.end);
        render_params.scale_y = zoom_animation.progress(zoom_target.scale_y.begin,
                                                        zoom_target.scale_y.end);

        render_params.off_x = zoom_animation.progress(zoom_target.off_x.begin,
                                                      zoom_target.off_x.end);
        render_params.off_y = zoom_animation.progress(zoom_target.off_y.begin,
                                                      zoom_target.off_y.end);

        render_params.delimiter_offset = zoom_animation.progress(zoom_target.delimiter_offset);

        if (!zoom_animation.running() && !state.zoom_in)
            finalize_and_exit();
    }

    void finalize_and_exit()
    {
        state.active = false;
        output->deactivate_plugin(grab_interface);
        grab_interface->ungrab();

        GetTuple(vw, vh, output->workspace->get_workspace_grid_size());

        for (int i = 0; i < vw; i++) {
            for (int j = 0; j < vh; j++) {
                output->render->workspace_stream_stop(streams[i][j]);
            }
        }

        output->render->reset_renderer();
        output->render->auto_redraw(false);
    }
};

extern "C" {
    wayfire_plugin_t *newInstance() {
        return new wayfire_expo();
    }
}
