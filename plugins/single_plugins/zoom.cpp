#include <plugin.hpp>
#include <output.hpp>
#include <opengl.hpp>
#include <debug.hpp>
#include <render-manager.hpp>
#include <animation.hpp>

class wayfire_invert_screen : public wayfire_plugin_t
{

    post_hook_t hook;

    axis_callback axis;

    wf_option speed, modifier;

    GLuint program, posID, uvID;

    float zoom = 1.0;
    float target = 1.0;
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

            duration = wf_duration(new_static_option("300"), wf_animation::linear);
            auto section = config->get_section("zoom");
            modifier = section->get_option("modifier", "<super>");
            speed    = section->get_option("speed", "0.005");

            output->add_axis(modifier, &axis);
        }

        void update_zoom_target(float delta)
        {
            target += target * delta * speed->as_cached_double() * -1;
            target = std::max(target, 1.0f);
            target = std::min(target, 50.0f);

            if (!hook_set && target != zoom)
            {
                hook_set = true;
                output->render->add_post(&hook);
            }
            duration.start(zoom, target);
        }

        void render(uint32_t fb, uint32_t tex, uint32_t target)
        {
            auto w = output->handle->width;
            auto h = output->handle->height;
            zoom = duration.progress();

            GetTuple(x, y, output->get_cursor_position());

            /* get rotation & scale */
            wlr_box box;
            box.x = x;
            box.y = y;
            box.width = box.height = 1;
            box = output_transform_box(output, box);

            x = box.x;
            y = h - box.y;

            const float scale = (zoom - 1) / zoom;

            const float tw = w / zoom, th = h / zoom;
            const float x1 = x * scale;
            const float y1 = y * scale;

            GL_CALL(glBindFramebuffer(GL_READ_FRAMEBUFFER, fb));
            GL_CALL(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target));

            GL_CALL(glBlitFramebuffer(x1, y1, x1 + tw, y1 + th, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_LINEAR));
            GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, 0));
            if (!duration.running() && zoom - 1 <= 0.01)
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
        return new wayfire_invert_screen();
    }
}

