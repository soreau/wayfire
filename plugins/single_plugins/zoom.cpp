#include <plugin.hpp>
#include <output.hpp>
#include <opengl.hpp>
#include <debug.hpp>
#include <render-manager.hpp>
#include <animation.hpp>

class wayfire_zoom_screen : public wayfire_plugin_t
{

    post_hook_t hook;
    axis_callback axis;

    wf_option speed, modifier, smoothing_duration;

    GLuint program, posID, uvID;

    float target_zoom = 1.0;
    bool hook_set = false;
    wf_duration duration;

    public:
        void init(wayfire_config *config)
        {
            hook = [=] (uint32_t fb, uint32_t tex, uint32_t target)
            {
                render(fb, tex, target);
            };

            axis = [=] (wlr_event_pointer_axis* ev)
            {
                if (ev->orientation == WLR_AXIS_ORIENTATION_VERTICAL)
                    update_zoom_target(ev->delta);
            };

            auto section = config->get_section("zoom");
            modifier = section->get_option("modifier", "<super>");
            output->add_axis(modifier, &axis);

            speed    = section->get_option("speed", "0.005");
            smoothing_duration = section->get_option("smoothing_duration", "300");

            duration = wf_duration(smoothing_duration, wf_animation::linear);
            duration.start(1, 1); // so that the first value we get is correct
        }

        void update_zoom_target(float delta)
        {
            const float last_target = target_zoom;

            target_zoom -= target_zoom * delta * speed->as_cached_double();
            target_zoom = std::max(target_zoom, 1.0f);
            target_zoom = std::min(target_zoom, 50.0f);


            if (last_target != target_zoom)
            {
                auto current = duration.progress();
                duration.start(current, target_zoom);

                if (!hook_set)
                {
                    hook_set = true;
                    output->render->add_post(&hook);
                }
            }
        }

        void render(uint32_t fb, uint32_t tex, uint32_t target)
        {
            auto w = output->handle->width;
            auto h = output->handle->height;
            GetTuple(x, y, output->get_cursor_position());

            /* get rotation & scale */
            wlr_box box;
            box.x = x;
            box.y = y;
            box.width = box.height = 1;
            box = output_transform_box(output, box);

            x = box.x;
            y = h - box.y;

            const float current_zoom = duration.progress();
            const float scale = (current_zoom - 1) / current_zoom;

            const float tw = w / current_zoom, th = h / current_zoom;
            const float x1 = x * scale;
            const float y1 = y * scale;

            GL_CALL(glBindFramebuffer(GL_READ_FRAMEBUFFER, fb));
            GL_CALL(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target));

            GL_CALL(glBlitFramebuffer(x1, y1, x1 + tw, y1 + th, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_LINEAR));
            GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, 0));

            if (!duration.running() && current_zoom - 1 <= 0.01)
            {
                output->render->rem_post(&hook);
                hook_set = false;
            }
        }
};

extern "C"
{
    wayfire_plugin_t *newInstance()
    {
        return new wayfire_zoom_screen();
    }
}

