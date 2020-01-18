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
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_matrix.h>
#undef static
}

#include <wayfire/util/log.hpp>


class mag_view_t : public wf::color_rect_view_t
{
    /* Default colors */
    const wf::color_t base_color = {0.5, 0.5, 1, 0.5};
    const wf::color_t base_border = {0.25, 0.25, 0.5, 0.8};
    const int base_border_w = 3;

    bool should_close = false;

  public:

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
        set_border_color(base_border);
        set_border(base_border_w);

        this->role = wf::VIEW_ROLE_SHELL_VIEW;
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
    bool active;

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
        active = false;
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
        ensure_preview();
        mag_view->set_target_geometry({100, 100}, 1.0, false);
        return true;
    }

    bool deactivate()
    {
        mag_view->set_target_geometry({100, 100}, 1.0, true);
        return true;
    }

    void fini() override
    {
    }
};

DECLARE_WAYFIRE_PLUGIN(wayfire_magnifier);