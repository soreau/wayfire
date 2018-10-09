#include <core.hpp>
#include <view.hpp>
#include <plugin.hpp>
#include <output.hpp>
#include "view-transform.hpp"
#include "workspace-manager.hpp"
#include "render-manager.hpp"
#include <nonstd/make_unique.hpp>

class wf_kawase_blur : public wf_view_transformer_t
{
    public:
        // the first two functions are for input transform, blur doesn't need it
        virtual wf_point local_to_transformed_point(wf_geometry view,
            wf_point point)
        {
            return point;
        }

        virtual wf_point transformed_to_local_point(wf_geometry view,
            wf_point point)
        {
            return point;
        }

        // again, nothing to transform when blurring
        virtual wlr_box get_bounding_box(wf_geometry view, wlr_box region)
        {
            return region;
        }

        /* src_tex        the internal FBO texture,
         *
         * src_box        box of the view that has to be repainted, contains other transforms
         *
         * scissor_box    the subbox of the FB which the transform renderer must update,
         *                drawing outside of it will cause artifacts
         *
         * target_fb      the framebuffer the transform should render to.
         *                it can be part of the screen, so it's geometry is
         *                given in output-local coordinates */

        /* The above is copied from the docs
         * Now, the purpose of this function is to render src_tex
         *
         * The view has geometry src_box
         * It is in output-local coordinates
         * The target_fb.geometry is also in output-local coords
         *
         * You need to repaint ONLY inside scissor_box, just use it like view-3d.cpp#205
         *
         * target_fb contains some very useful values. First of all, you'd need
         * target_fb.tex (that is the texture of the screen as it has been rendered up to now)
         * You can copy this texture, or feed it to your shader, etc.
         * When you want to DRAW on it, use target_fb.bind(), that should be your last step
         *
         * as we both have 1920x1080 monitors without any transform or scaling (or I'm wrong?)
         * I suggest hardcoding it just to see if it works, and then we'll figure out what else
         * is there to be done. */

        virtual void render_with_damage(uint32_t src_tex,
                                        wlr_box src_box,
                                        wlr_box scissor_box,
                                        const wf_framebuffer& target_fb)
        {
        }

        virtual ~wf_kawase_blur() {}
};

class wayfire_kawase : public wayfire_plugin_t
{
    button_callback btn;
    effect_hook_t damage;
    post_hook_t passthrough;

    public:
    void init(wayfire_config *config)
    {
        grab_interface->name = "kawase";
        grab_interface->abilities_mask = WF_ABILITY_CONTROL_WM;

        btn = [=] (uint32_t, int, int)
        {

            auto focus = core->get_cursor_focus();

            if (!focus)
                return;

            auto view = core->find_view(focus->get_main_surface());
            view->add_transformer(nonstd::make_unique<wf_kawase_blur> ());
        };

        output->add_button(new_static_option("<super> <alt> BTN_LEFT"), &btn);

        damage = [=] () {
            output->render->damage(NULL);
        };

        output->render->add_effect(&damage, WF_OUTPUT_EFFECT_PRE);

        passthrough = [=] (uint32_t fb, uint32_t tex, uint32_t target)
        {
            auto w = output->handle->width;
            auto h = output->handle->height;

            GL_CALL(glBindFramebuffer(GL_READ_FRAMEBUFFER, fb));
            GL_CALL(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target));

            GL_CALL(glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_LINEAR));
            GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, 0));
        };
    }
};

extern "C"
{
    wayfire_plugin_t *newInstance()
    {
        return new wayfire_kawase();
    }
}
