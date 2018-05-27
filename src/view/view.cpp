#include "debug.hpp"
#include "core.hpp"
#include "opengl.hpp"
#include "output.hpp"
#include "view.hpp"
#include "view-transform.hpp"
#include "decorator.hpp"
#include "workspace-manager.hpp"
#include "render-manager.hpp"
#include "priv-view.hpp"
#include "xdg-shell-v6.hpp"

#include <algorithm>
#include <glm/glm.hpp>
#include "signal-definitions.hpp"

extern "C"
{
#define static
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_matrix.h>
#include <wlr/util/region.h>
#undef static
}

/* TODO: clean up the code, currently it is a horrible mess
 * Target: split view.cpp into several files, PIMPL the wayfire_view_t and wayfire_surface_t structures */

/* TODO: consistently use wf_geometry/wlr_box, don't simply mix them
 * There must be a distinction(i.e what is a box, what is geometry) */

uint32_t _last_view_id = 0;
wayfire_view_t::wayfire_view_t()
    : wayfire_surface_t (NULL), id(_last_view_id++)
{
    set_output(core->get_active_output());
    pixman_region32_init(&offscreen_buffer.cached_damage);
}

wayfire_view wayfire_view_t::self()
{
    return core->find_view((wayfire_surface_t*) this);
}

bool wayfire_view_t::is_visible()
{
    return (is_mapped() || offscreen_buffer.valid()) && !is_hidden;
}

bool wayfire_view_t::update_size()
{
    assert(surface);

    int old_w = geometry.width, old_h = geometry.height;

    geometry.width = surface->current ? surface->current->width  : 0;
    geometry.height = surface->current? surface->current->height : 0;

    return geometry.width != old_w || geometry.height != old_h;
}

void wayfire_view_t::set_moving(bool moving)
{
    in_continuous_move += moving ? 1 : -1;
    if (decoration)
        decoration->set_moving(moving);
}

void wayfire_view_t::set_resizing(bool resizing)
{
    in_continuous_resize += resizing ? 1 : -1;
    if (decoration)
        decoration->set_resizing(resizing);
}

void wayfire_view_t::move(int x, int y, bool send_signal)
{
    view_geometry_changed_signal data;
    data.view = self();
    data.old_geometry = get_wm_geometry();

    damage();
    geometry.x = x;
    geometry.y = y;
    damage();

    if (send_signal)
        output->emit_signal("view-geometry-changed", &data);
}

void wayfire_view_t::resize(int w, int h, bool send_signal)
{
    view_geometry_changed_signal data;
    data.view = self();
    data.old_geometry = get_wm_geometry();

    damage();
    geometry.width = w;
    geometry.height = h;
    damage();

    if (send_signal)
        output->emit_signal("view-geometry-changed", &data);
}

wayfire_surface_t *wayfire_view_t::map_input_coordinates(int cx, int cy, int& sx, int& sy)
{
    if (!is_mapped())
        return nullptr;

    wayfire_surface_t *ret = NULL;

    auto wm = get_wm_geometry();
    int center_x = wm.x + wm.width / 2;
    int center_y = wm.y + wm.height / 2;

    for_each_surface(
        [&] (wayfire_surface_t *surface, int x, int y)
        {
            if (ret) return;

            int lx = cx - center_x,
                ly = center_y - cy;

            if (transform)
            {
                auto transformed = transform->transformed_to_local_point({lx, ly});
                lx = transformed.x;
                ly = transformed.y;
            }

            lx = lx + center_x;
            ly = center_y - ly;

            sx = lx - x;
            sy = ly - y;

            if (wlr_surface_point_accepts_input(surface->surface, sx, sy))
                ret = surface;
        });

    return ret;
}

void wayfire_view_t::set_geometry(wf_geometry g)
{
    move(g.x, g.y, false);
    resize(g.width, g.height);
}

wf_geometry wayfire_view_t::get_bounding_box()
{
    if (!transform)
        return get_output_geometry();

    wlr_box wm = get_wm_geometry();
    wlr_box box = get_output_geometry();

    box.x = (box.x - wm.x) - wm.width / 2;
    box.y = wm.height / 2 - (box.y - wm.y);

    box = transform->get_bounding_box(box);

    box.x = box.x + wm.x + wm.width / 2;
    box.y = (wm.height / 2 - box.y) + wm.y;

    return box;
}

void wayfire_view_t::set_maximized(bool maxim)
{
    maximized = maxim;
}

void wayfire_view_t::set_fullscreen(bool full)
{
    fullscreen = full;
}

void wayfire_view_t::activate(bool active)
{ }

void wayfire_view_t::set_parent(wayfire_view parent)
{
    if (this->parent)
    {
        auto it = std::remove(this->parent->children.begin(), this->parent->children.end(), self());
        this->parent->children.erase(it);
    }

    this->parent = parent;
    if (parent)
    {
        auto it = std::find(parent->children.begin(), parent->children.end(), self());
        if (it == parent->children.end())
            parent->children.push_back(self());
    }
}

void wayfire_view_t::get_child_position(int &x, int &y)
{
    assert(decoration);

    x = decor_x;
    y = decor_y;
}

wayfire_surface_t* wayfire_view_t::get_main_surface()
{
    if (decoration)
        return decoration->get_main_surface();
    return this;
}

wf_point wayfire_view_t::get_output_position()
{
    if (decoration)
        return decoration->get_output_position() + wf_point{decor_x, decor_y};
    return wf_point{geometry.x, geometry.y};
}

wf_geometry wayfire_view_t::get_output_geometry()
{
    if (is_mapped() || !offscreen_buffer.valid())
        return wayfire_surface_t::get_output_geometry();

    return {
        offscreen_buffer.output_x,
        offscreen_buffer.output_y,
        (int32_t)(offscreen_buffer.fb_width / output->handle->scale),
        (int32_t)(offscreen_buffer.fb_height / output->handle->scale)
    };
}

void wayfire_view_t::damage(const wlr_box& box)
{
    if (decoration)
        return decoration->damage(box);

    if (!output)
        return;

    auto wm_geometry = get_wm_geometry();
    if (transform)
    {
        auto real_box = box;
        real_box.x -= wm_geometry.x;
        real_box.y -= wm_geometry.y;

        pixman_region32_union_rect(&offscreen_buffer.cached_damage,
                                   &offscreen_buffer.cached_damage,
                                   real_box.x, real_box.y,
                                   real_box.width, real_box.height);

        /* TODO: damage only the bounding box of region */
        output->render->damage(get_output_box_from_box(get_bounding_box(),
                                                       output->handle->scale));
    } else
    {
        output->render->damage(get_output_box_from_box(box,
                                                       output->handle->scale));
    }
}

static wf_geometry get_output_centric_geometry(int32_t screen_w, int32_t screen_h, wf_geometry view)
{
    view.x = view.x - screen_w / 2;
    view.y = screen_h /2 - view.y;

    return view;
}

void wayfire_view_t::offscreen_buffer_t::init(int w, int h)
{
    OpenGL::prepare_framebuffer_size(w, h, fbo, tex);
    fb_width = w;
    fb_height = h;

    GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, fbo));
    GL_CALL(glViewport(0, 0, fb_width, fb_height));

    wlr_renderer_scissor(core->renderer, NULL);

    GL_CALL(glClearColor(0, 0, 0, 0));
    GL_CALL(glClear(GL_COLOR_BUFFER_BIT));
}

bool wayfire_view_t::offscreen_buffer_t::valid()
{
    return fbo != (uint32_t)-1;
}

void wayfire_view_t::offscreen_buffer_t::fini()
{
    if (!valid())
        return;

    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);

    fb_width = fb_height = 0;
    fbo = tex = -1;
}

void wayfire_view_t::take_snapshot()
{
    if (!is_mapped() || !wlr_surface_has_buffer(surface))
        return;

    wlr_renderer_begin(core->renderer, output->handle->width, output->handle->height);
    auto output_geometry = get_output_geometry();

    offscreen_buffer.output_x = output_geometry.x;
    offscreen_buffer.output_y = output_geometry.y;

    int scale = surface->current->scale;
    if (output_geometry.width * scale != offscreen_buffer.fb_width
        || output_geometry.height * scale != offscreen_buffer.fb_height)
    {
        offscreen_buffer.fini();
    }

    if (!offscreen_buffer.valid())
    {
        offscreen_buffer.init(output_geometry.width * scale, output_geometry.height * scale);
        pixman_region32_init(&offscreen_buffer.cached_damage);
    }

    GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, offscreen_buffer.fbo));
    GL_CALL(glViewport(0, 0, offscreen_buffer.fb_width, offscreen_buffer.fb_height));
    wlr_renderer_scissor(core->renderer, NULL);

    GL_CALL(glClearColor(0, 0, 0, 0));
    GL_CALL(glClear(GL_COLOR_BUFFER_BIT));

    for_each_surface([=] (wayfire_surface_t *surface, int x, int y) {
        GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, offscreen_buffer.fbo));
        GL_CALL(glViewport(0, 0, offscreen_buffer.fb_width, offscreen_buffer.fb_height));
        surface->render_fbo((x - output_geometry.x) * scale, (y - output_geometry.y) * scale,
                            offscreen_buffer.fb_width, offscreen_buffer.fb_height,
                            NULL);
    }, true);

    wlr_renderer_end(core->renderer);
}

void wayfire_view_t::render_fb(int x, int y, pixman_region32_t* damage, int fb)
{
    //log_info("render_pixman %p", surface);

    if (decoration && decoration->get_transformer())
        assert(false);

    if (transform && !decoration)
    {
        take_snapshot();
        auto output_geometry = get_output_geometry();
        auto obox = output_geometry;

        obox.x = x;
        obox.y = y;

        int w, h;
        wlr_output_effective_resolution(output->handle, &w, &h);
        obox = get_output_centric_geometry(w, h, obox);

        int n_rect;
        auto rects = pixman_region32_rectangles(damage, &n_rect);
        auto output_matrix = get_output_matrix_from_transform(output->get_transform());

        for (int i = 0; i < n_rect; i++)
        {
            auto box = wlr_box{rects[i].x1, rects[i].y1,
                rects[i].x2 - rects[i].x1, rects[i].y2 - rects[i].y1};

            auto sbox = get_scissor_box(output, &box);
            transform->render_with_damage(offscreen_buffer.tex, fb, obox, output_matrix, sbox);
        }

    } else
    {
        wayfire_surface_t::render_fb(x, y, damage, fb);
    }
}

void wayfire_view_t::set_transformer(std::unique_ptr<wf_view_transformer_t> transformer)
{
    /* TODO: damage all */
    transform = std::move(transformer);
}

void emit_view_map(wayfire_view view)
{
    /* TODO: consider not emitting a create-view for special surfaces */
    map_view_signal data;
    data.view = view;
    view->get_output()->emit_signal("map-view", &data);
}

void wayfire_view_t::map(wlr_surface *surface)
{
    wayfire_surface_t::map(surface);

    if (!is_special && !maximized && !fullscreen)
    {
        auto workarea = output->workspace->get_workarea();
        geometry.x += workarea.x;
        geometry.y += workarea.y;
    }

    if (update_size())
        damage();

    if (!is_special)
    {
        output->attach_view(self());
        output->focus_view(self());
    }

    emit_view_map(self());
}

void emit_view_unmap(wayfire_view view)
{
    unmap_view_signal data;
    data.view = view;
    view->get_output()->emit_signal("unmap-view", &data);
}

void wayfire_view_t::unmap()
{
    log_info("unmap %s %s %p", get_title().c_str(), get_app_id().c_str(), this);
    if (output)
        emit_view_unmap(self());

    wayfire_surface_t::unmap();
    if (decoration)
    {
        decoration->close();
        decoration->unmap();
    }
}

void wayfire_view_t::move_request()
{
    if (decoration)
        return decoration->move_request();

    move_request_signal data;
    data.view = self();
    output->emit_signal("move-request", &data);
}

void wayfire_view_t::resize_request()
{
    if (decoration)
        return decoration->resize_request();

    resize_request_signal data;
    data.view = self();
    output->emit_signal("resize-request", &data);
}

void wayfire_view_t::maximize_request(bool state)
{
    if (decoration)
        return decoration->maximize_request(state);

    if (maximized == state)
        return;

    view_maximized_signal data;
    data.view = self();
    data.state = state;

    if (surface)
    {
        output->emit_signal("view-maximized-request", &data);
    } else if (state)
    {
        set_geometry(output->workspace->get_workarea());
        set_maximized(state);
        output->emit_signal("view-maximized", &data);
    }
}

void wayfire_view_t::fullscreen_request(wayfire_output *out, bool state)
{
    if (decoration)
        return decoration->fullscreen_request(out, state);

    if (fullscreen == state)
        return;

    auto wo = (out ? out : (output ? output : core->get_active_output()));
    assert(wo);

    if (output != wo)
        core->move_view_to_output(self(), wo);

    view_fullscreen_signal data;
    data.view = self();
    data.state = state;

    if (surface) {
        wo->emit_signal("view-fullscreen-request", &data);
    } else if (state) {
        set_geometry(output->get_relative_geometry());
        output->emit_signal("view-fullscreen", &data);
    }

    set_fullscreen(state);
}

void wayfire_view_t::commit()
{
    wayfire_surface_t::commit();

    auto old = get_output_geometry();
    if (update_size())
    {
        damage(old);
        damage();
    }

    /* configure frame_interior */
    if (decoration)
    {
        auto decor = std::dynamic_pointer_cast<wayfire_xdg6_decoration_view> (decoration);
        assert(decor);
        decor->child_configured(geometry);
    }
}

void wayfire_view_t::set_decoration(wayfire_view decor,
                                    std::unique_ptr<wf_decorator_frame_t> frame)
{
    if (decor)
    {
        auto raw_ptr = dynamic_cast<wayfire_xdg6_decoration_view*> (decor.get());
        assert(raw_ptr);

        if (output)
            output->detach_view(self());

        raw_ptr->init(self(), std::move(frame));
    }

    decoration = decor;
}

void wayfire_view_t::damage()
{
    damage(get_bounding_box());
}

void wayfire_view_t::destruct()
{
    core->erase_view(self());
}

wayfire_view_t::~wayfire_view_t()
{
    pixman_region32_fini(&offscreen_buffer.cached_damage);
    for (auto& kv : custom_data)
        delete kv.second;
}

decorator_base_t *wf_decorator;
void init_desktop_apis()
{
    init_xdg_shell_v6();
    init_xwayland();
    init_layer_shell();
}
