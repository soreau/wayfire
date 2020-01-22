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
    wf::option_wrapper_t<int> default_height{"mag/default_height"};
    bool should_close = false;

  public:
    wf::framebuffer_t mag_tex;

    mag_view_t(wf::output_t *output, float aspect)
        : wf::color_rect_view_t()
    {
        set_output(output);

        set_geometry({100, 100, (int) (default_height * aspect), default_height});

        this->role = wf::VIEW_ROLE_TOPLEVEL;
        get_output()->workspace->add_view(self(), wf::LAYER_TOP);
    }

    bool accepts_input(int32_t sx, int32_t sy) override
    {
        auto vg = get_wm_geometry();

        /* Allow move and resize */
        if (0 < sx && sx < vg.width && 0 < sy && sy < vg.height)
            return true;

        return false;
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
            OpenGL::render_transformed_texture(mag_tex.tex, src_geometry, {},
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
    wf::option_wrapper_t<int> zoom_level{"mag/zoom_level"};
    nonstd::observer_ptr<mag_view_t> mag_view;
    bool active, hook_set;
    int width, height;

    wf::activator_callback toggle_cb = [=] (wf::activator_source_t, uint32_t)
    {
        active = !active;
        if (active) {
            return activate();
        } else {
            deactivate();
            return true;
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

        auto og = output->get_relative_geometry();
        auto view = std::make_unique<mag_view_t>(output, (float) og.width / og.height);

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
            hook_set = true;
        }

        ensure_preview();

        return true;
    }

    wf::texture_t get_texture_from_surface(wlr_texture *texture)
    {
        assert(wlr_texture_is_gles2(texture));

        wlr_gles2_texture_attribs attribs;
        wlr_gles2_texture_get_attribs(texture, &attribs);

        wf::texture_t tex;
        /* Wayfire Y-inverts by default */
        tex.invert_y = !attribs.inverted_y;
        tex.target = attribs.target;
        tex.tex_id = attribs.tex;

        if (tex.target == GL_TEXTURE_2D) {
            tex.type = attribs.has_alpha ?
                wf::TEXTURE_TYPE_RGBA : wf::TEXTURE_TYPE_RGBX;
        } else {
            tex.type = wf::TEXTURE_TYPE_EXTERNAL;
        }

        return tex;
    }

    wf::effect_hook_t post_hook = [=]()
    {
        wlr_dmabuf_attributes dmabuf_attribs;

        if (!wlr_output_export_dmabuf(output->handle, &dmabuf_attribs))
        {
            LOGE("Failed reading output contents");
            deactivate();
            active = false;
            return;
        }
        
        auto cursor_position = output->get_cursor_position();

        auto og = output->get_relative_geometry();
        gl_geometry src_geometry = {0, 0, (float) og.width, (float) og.height};

        width = og.width;
        height = og.height;
        float x = cursor_position.x / width;
        float y = cursor_position.y / height;
        float min = 0.5;
        float max = 0.01;
        float range = min - max;
        auto level = (1.0 - (zoom_level / 100.0)) * range + max;

        gl_geometry zoom_box;

        y = 1.0 - y;

        zoom_box.x1 = x - level;
        zoom_box.y1 = y - level;
        zoom_box.x2 = x + level;
        zoom_box.y2 = y + level;

        if (zoom_box.x1 < 0.0)
        {
            zoom_box.x2 -= zoom_box.x1;
            zoom_box.x1 = 0.0;
        }
        if (zoom_box.y1 < 0.0)
        {
            zoom_box.y2 -= zoom_box.y1;
            zoom_box.y1 = 0.0;
        }
        if (zoom_box.x2 > 1.0)
        {
            zoom_box.x1 += 1.0 - zoom_box.x2;
            zoom_box.x2 = 1.0;
        }
        if (zoom_box.y2 > 1.0)
        {
            zoom_box.y1 += 1.0 - zoom_box.y2;
            zoom_box.y2 = 1.0;
        }

        auto wlr_texture = wlr_texture_from_dmabuf(
            wf::get_core().renderer, &dmabuf_attribs);

        auto texture = get_texture_from_surface(wlr_texture);

        OpenGL::render_begin();
        mag_view->mag_tex.allocate(width, height);
        mag_view->mag_tex.geometry = og;
        mag_view->mag_tex.bind();

        OpenGL::render_transformed_texture(texture, src_geometry, zoom_box,
            mag_view->mag_tex.get_orthographic_projection(), glm::vec4(1.0),
            TEXTURE_USE_TEX_GEOMETRY | TEXTURE_TRANSFORM_INVERT_Y);
        OpenGL::render_end();

        wlr_texture_destroy(wlr_texture);
        wlr_dmabuf_attributes_finish(&dmabuf_attribs);

        mag_view->damage();
    };

    void deactivate()
    {
        if (hook_set)
        {
            output->render->rem_effect(&post_hook);
            hook_set = false;
        }

        output->render->damage_whole();

        if (!mag_view)
            return;

        mag_view->close();
        mag_view = nullptr;
    }

    void fini() override
    {
    }
};

DECLARE_WAYFIRE_PLUGIN(wayfire_magnifier);