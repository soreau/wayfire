/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2020 Scott Moreau
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "wayfire/core.hpp"
#include "wayfire/view.hpp"
#include "wayfire/plugin.hpp"
#include "wayfire/output.hpp"
#include "wayfire/signal-definitions.hpp"
#include "wayfire/workspace-manager.hpp"
#include "wayfire/output-layout.hpp"
#include "wayfire/compositor-surface.hpp"
#include "wayfire/compositor-view.hpp"
#include "wayfire/render-manager.hpp"
#include "wayfire/opengl.hpp"

extern "C"
{
#define static
#include <wlr/config.h>
#include <wlr/render/gles2.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_matrix.h>
#undef static
}

#include <wayfire/util/log.hpp>


class mag_view_t : public wf::color_rect_view_t
{
    /* Default colors */
    const wf::color_t base_color = {0.5, 0.5, 1, 0.5};

    bool should_close = false;

  public:
    wf::framebuffer_t mag_tex;

    /**
     * Create a new indication preview on the indicated output.
     *
     * @param start_geometry The geometry the preview should have, relative to
     *                       the output
     */
    mag_view_t(wf::output_t *output)
        : wf::color_rect_view_t()
    {
        set_output(output);

        set_geometry({100, 100, 640, 480});

        set_color(base_color);
        set_border(0);

        this->role = wf::VIEW_ROLE_COMPOSITOR_VIEW;
        get_output()->workspace->add_view(self(), wf::LAYER_TOP);
    }

    /**
     * Animate the preview to the given target geometry and alpha.
     *
     * @param close Whether the view should be closed when the target is
     *              reached.
     */
    void set_target_geometry(wf::geometry_t target, float alpha, bool close = false)
    {
        this->should_close = close;
    }

    /**
     * A wrapper around set_target_geometry(wf::geometry_t, double, bool)
     */
    void set_target_geometry(wf::point_t point, double alpha,
        bool should_close = false)
    {
        return set_target_geometry({point.x, point.y, 1, 1},
            alpha, should_close);
    }

    bool accepts_input(int32_t sx, int32_t sy) override
    {
        LOGI(__func__, ": ", sx, ", ", sy);

        /* Allow move and resize */
        return true;
    }

    void move(int x, int y) override
    {
        LOGI(__func__, ": ", x, ", ", y);

        wf::color_rect_view_t::move(x, y);

        /* Handle move */
    }

    void resize(int w, int h) override
    {
        LOGI(__func__, ": ", w, "x", h);

        wf::color_rect_view_t::resize(w, h);

        /* Handle size change */
    }

    void simple_render(const wf::framebuffer_t& fb, int x, int y,
        const wf::region_t& damage) override
    {
        OpenGL::render_begin(fb);
        auto vg = get_wm_geometry();
        gl_geometry src_geometry = {(float) vg.x, (float) vg.y, (float) vg.x + vg.width, (float) vg.y + vg.height};
        for (const auto& box : damage)
        {
            auto sbox = fb.framebuffer_box_from_damage_box(wlr_box_from_pixman_box(box));
            wlr_renderer_scissor(wf::get_core().renderer, &sbox);
            OpenGL::render_transformed_texture(mag_tex.fb, src_geometry, {},
                                               fb.get_orthographic_projection(),
                                               glm::vec4(1.0), 0);
        }
        OpenGL::render_end();
    }

    virtual ~mag_view_t()
    {
    }
};


class wayfire_magnifier : public wf::plugin_interface_t
{
    const std::string transformer_name = "mag";
    wf::config::option_base_t::updated_callback_t option_changed;
    wf::option_wrapper_t<wf::activatorbinding_t> toggle_binding{"mag/toggle"};
    nonstd::observer_ptr<mag_view_t> mag_view;
    bool active, hook_set;
    int width, height;

    wf::activator_callback toggle_cb = [=] (wf::activator_source_t, uint32_t)
    {
        active = !active;
        if (active) {
            return activate();
        } else {
            return deactivate();
        }
    };

    public:
    void init() override
    {
        grab_interface->name = transformer_name;
        grab_interface->capabilities = 0;

        option_changed = [=] ()
        {
        };
        output->add_activator(toggle_binding, &toggle_cb);
        hook_set = active = false;
    }

    void ensure_preview()
    {
        if (mag_view)
            return;

        auto view = std::make_unique<mag_view_t>(output);

        mag_view = {view};

        wf::get_core().add_view(std::move(view));
    }

    bool activate()
    {
        if (!output->activate_plugin(grab_interface))
            return false;

        if (!hook_set)
        {
            output->render->add_effect(&post_hook, wf::OUTPUT_EFFECT_POST);
            output->render->set_redraw_always();
            hook_set = true;
        }

        width = output->get_layout_geometry().width;
        height = output->get_layout_geometry().height;

        ensure_preview();

        return true;
    }

    wf::effect_hook_t post_hook = [=]()
    {
        wlr_dmabuf_attributes dmabuf_attribs;
        struct wlr_gles2_texture_attribs texture_attribs;

        if (!wlr_output_export_dmabuf(output->handle, &dmabuf_attribs))
        {
            LOGE("Failed reading output contents");
            return;
        }
        
        //auto cursor_position = output->get_cursor_position();

        auto texture = wlr_texture_from_dmabuf(
            wf::get_core().renderer, &dmabuf_attribs);

        wlr_gles2_texture_get_attribs(texture, &texture_attribs);

        /* Use texture */
        OpenGL::render_begin();
        mag_view->mag_tex.allocate(width, height);
        mag_view->mag_tex.bind();

        GL_CALL(glBindFramebuffer(GL_READ_FRAMEBUFFER, texture_attribs.tex));
        GL_CALL(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, mag_view->mag_tex.fb));
        GL_CALL(glBlitFramebuffer(
                0, 0,
                width, height,
                0, 0, width, height,
                GL_COLOR_BUFFER_BIT, GL_LINEAR));
        OpenGL::render_end();

        wlr_texture_destroy(texture);
        wlr_dmabuf_attributes_finish(&dmabuf_attribs);
    };

    bool deactivate()
    {
        if (hook_set)
        {
            output->render->rem_effect(&post_hook);
            hook_set = false;
        }

        if (!mag_view)
            return true;

        mag_view->close();
        mag_view = nullptr;

        return true;
    }

    void fini() override
    {
    }
};

DECLARE_WAYFIRE_PLUGIN(wayfire_magnifier);