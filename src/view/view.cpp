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
#include "xdg-shell.hpp"
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

wayfire_view_t::wayfire_view_t()
    : wayfire_surface_t (NULL)
{
    set_output(core->get_active_output());
    pixman_region32_init(&offscreen_buffer.cached_damage);
}

std::string wayfire_view_t::to_string() const
{
    return "view-" + wf_object_base::to_string();
}

void wayfire_view_t::set_output(wayfire_output *wo)
{
    wayfire_surface_t::set_output(wo);
    if (decoration)
        decoration->set_output(wo);
}

wayfire_view wayfire_view_t::self()
{
    return core->find_view((wayfire_surface_t*) this);
}

bool wayfire_view_t::is_visible()
{
    return (is_mapped() || offscreen_buffer.valid()) && !is_hidden;
}

void wayfire_view_t::adjust_anchored_edge(int32_t new_width, int32_t new_height)
{
    if (edges)
    {
        auto wm = get_wm_geometry();
        if (edges & WLR_EDGE_LEFT)
            wm.x += geometry.width - new_width;
        if (edges & WLR_EDGE_TOP)
            wm.y += geometry.height - new_height;

        move(wm.x, wm.y, false);
    }
}

bool wayfire_view_t::update_size()
{
    assert(surface);

    int old_w = geometry.width, old_h = geometry.height;
    int width = surface->current.width, height = surface->current.height;
    if (geometry.width != width || geometry.height != height)
    {
        adjust_anchored_edge(width, height);
        wayfire_view_t::resize(width, height, true);
    }

    return geometry.width != old_w || geometry.height != old_h;
}

void wayfire_view_t::set_moving(bool moving)
{
    in_continuous_move += moving ? 1 : -1;
}

void wayfire_view_t::set_resizing(bool resizing, uint32_t edges)
{
    /* edges are reset on the next commit */
    if (resizing)
        this->edges = edges;

    in_continuous_resize += resizing ? 1 : -1;
}

void wayfire_view_t::move(int x, int y, bool send_signal)
{
    auto opos = get_output_position();
    auto wm   = get_wm_geometry();

    view_geometry_changed_signal data;
    data.view = self();
    data.old_geometry = wm;

    damage();
    geometry.x = x + opos.x - wm.x;
    geometry.y = y + opos.y - wm.y;
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

    if (transforms.size())
    {
        auto it = transforms.rbegin();
        auto box = get_untransformed_bounding_box();
        while(it != transforms.rend())
        {
            auto p = (*it)->transform->transformed_to_local_point(box, {cx, cy});
            cx = p.x; cy = p.y;
            ++it;
        }
    }

    for_each_surface(
        [&] (wayfire_surface_t *surface, int x, int y)
        {
            if (ret) return;

            sx = cx - x;
            sy = cy - y;

            if (surface->accepts_input(sx, sy))
                ret = surface;
        });

    return ret;
}

void wayfire_view_t::set_geometry(wf_geometry g)
{
    move(g.x, g.y, false);
    resize(g.width, g.height);
}

wlr_box wayfire_view_t::transform_region(const wlr_box& region,
    nonstd::observer_ptr<wf_view_transformer_t> upto)
{
    auto box = region;
    auto view = get_untransformed_bounding_box();

    for (auto& tr : transforms)
    {
        if (tr->transform.get() == upto.get())
            break;

        box = tr->transform->get_bounding_box(view, box);
        view = tr->transform->get_bounding_box(view, view);
    }

    return box;
}

wlr_box wayfire_view_t::transform_region(const wlr_box& region,
    std::string transformer)
{
    return transform_region(region, get_transformer(transformer));
}

wlr_box wayfire_view_t::transform_region(const wlr_box& region)
{
    return transform_region(region,
        nonstd::observer_ptr<wf_view_transformer_t>(nullptr));
}

bool wayfire_view_t::intersects_region(const wlr_box& region)
{
    /* fallback to the whole transformed boundingbox, if it exists */
    if (!is_mapped())
        return rect_intersect(region, get_bounding_box());

    bool result = false;
    for_each_surface([=, &result] (wayfire_surface_t* surface, int, int)
    {
        auto box = transform_region(surface->get_output_geometry());
        result = result || rect_intersect(region, box);
    });

    return result;
}

wf_geometry wayfire_view_t::get_untransformed_bounding_box()
{
    if (is_mapped() || !offscreen_buffer.valid())
    {
        auto bbox = get_output_geometry();
        int x1 = bbox.x, x2 = bbox.x + bbox.width;
        int y1 = bbox.y, y2 = bbox.y + bbox.height;

        for_each_surface([&x1, &x2, &y1, &y2] (wayfire_surface_t* surface, int, int)
                         {
                             auto sbox = surface->get_output_geometry();
                             int sx1 = sbox.x, sx2 = sbox.x + sbox.width;
                             int sy1 = sbox.y, sy2 = sbox.y + sbox.height;

                             x1 = std::min(x1, sx1);
                             y1 = std::min(y1, sy1);
                             x2 = std::max(x2, sx2);
                             y2 = std::max(y2, sy2);
                         });

        bbox.x = x1;
        bbox.y = y1;
        bbox.width = x2 - x1;
        bbox.height = y2 - y1;

        return bbox;
    }

    return {
        offscreen_buffer.output_x,
        offscreen_buffer.output_y,
        (int32_t)(offscreen_buffer.fb_width / output->handle->scale + 0.99),
        (int32_t)(offscreen_buffer.fb_height / output->handle->scale + 0.99)
    };
}

wf_geometry wayfire_view_t::get_bounding_box()
{
    return transform_region(get_untransformed_bounding_box());
}

void wayfire_view_t::set_maximized(bool maxim)
{
    maximized = maxim;

    if (frame)
        frame->notify_view_maximized();
}

void wayfire_view_t::set_fullscreen(bool full)
{
    fullscreen = full;

    if (frame)
        frame->notify_view_fullscreened();
}

void wayfire_view_t::activate(bool active)
{
    if (frame)
        frame->notify_view_activated(active);

    /* we don't send activated or deactivated for shell views,
     * they should always be active */
    if (role == WF_VIEW_ROLE_SHELL_VIEW)
    {
        if (!active)
            activate(true);
    }
}

void wayfire_view_t::close()
{
}

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

wayfire_surface_t* wayfire_view_t::get_main_surface()
{
    return this;
}

void wayfire_view_t::for_each_surface(wf_surface_iterator_callback callback, bool reverse)
{
    if (reverse && decoration)
    {
        auto pos = decoration->get_output_position();
        callback(decoration, pos.x, pos.y);
    }

    wayfire_surface_t::for_each_surface(callback, reverse);

    if (!reverse && decoration)
    {
        auto pos = decoration->get_output_position();
        callback(decoration, pos.x, pos.y);
    }
}

bool wayfire_view_t::should_be_decorated()
{
    return role == WF_VIEW_ROLE_TOPLEVEL && !has_client_decoration;
}

void wayfire_view_t::set_decoration(wayfire_surface_t *deco)
{
    damage();

    if (decoration)
        decoration->dec_keep_count();

    auto wm = get_wm_geometry();

    decoration = deco;
    frame = dynamic_cast<wf_decorator_frame_t*> (deco);

    if (!deco)
        return;

    assert(frame);
    decoration->parent_surface = this;
    decoration->set_output(output);
    decoration->inc_keep_count();

    frame->notify_view_resized(frame->expand_wm_geometry(wm));
    move(wm.x, wm.y);

    damage();

    if (maximized)
        set_geometry(output->workspace->get_workarea());
    if (fullscreen)
        set_geometry(output->get_relative_geometry());
}

wf_point wayfire_view_t::get_output_position()
{
    return wf_point{geometry.x, geometry.y};
}

wf_geometry wayfire_view_t::get_wm_geometry()
{
    if (frame)
        return frame->expand_wm_geometry(geometry);
    else
        return geometry;
}

void wayfire_view_t::damage(const wlr_box& box)
{
    if (!output)
        return;

    auto wm_geometry = get_wm_geometry();

    wlr_box damage_box;

    if (transforms.size())
    {
        auto real_box = box;
        real_box.x -= wm_geometry.x;
        real_box.y -= wm_geometry.y;

        pixman_region32_union_rect(&offscreen_buffer.cached_damage,
                                   &offscreen_buffer.cached_damage,
                                   real_box.x, real_box.y,
                                   real_box.width, real_box.height);

        /* TODO: damage only the bounding box of region */
        damage_box = get_output_box_from_box(transform_region(box), output->handle->scale);
    } else
    {
        damage_box = get_output_box_from_box(box, output->handle->scale);
    }

    /* shell views are visible in all workspaces. That's why we must apply
     * their damage to all workspaces as well */

    if (role == WF_VIEW_ROLE_SHELL_VIEW)
    {
        GetTuple(vw, vh, output->workspace->get_workspace_grid_size());
        GetTuple(vx, vy, output->workspace->get_current_workspace());
        GetTuple(sw, sh, output->get_screen_size());

        for (int i = 0; i < vw; i++)
        {
            for (int j = 0; j < vh; j++)
            {
                int dx = (i - vx) * sw;
                int dy = (j - vy) * sh;

                auto local_box = damage_box + wf_point{dx, dy};
                output->render->damage(local_box);
            }
        }
    } else
    {
        output->render->damage(damage_box);
    }
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
    if (!get_buffer())
        return;

    auto buffer_geometry = get_untransformed_bounding_box();

    offscreen_buffer.output_x = buffer_geometry.x;
    offscreen_buffer.output_y = buffer_geometry.y;

    float scale = output->handle->scale;
    if (int(buffer_geometry.width  * scale) != offscreen_buffer.fb_width ||
        int(buffer_geometry.height * scale) != offscreen_buffer.fb_height ||
        offscreen_buffer.fb_scale != scale)
    {
        offscreen_buffer.fini();
    }

    offscreen_buffer.fb_scale = scale;
    if (!offscreen_buffer.valid())
    {
        offscreen_buffer.init(buffer_geometry.width * scale, buffer_geometry.height * scale);
        //pixman_region32_init(&offscreen_buffer.cached_damage);
    }

    GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, offscreen_buffer.fbo));
    wlr_renderer_begin(core->renderer, offscreen_buffer.fb_width, offscreen_buffer.fb_height);
    wlr_renderer_scissor(core->renderer, NULL);
    float clear_color[] = {0, 0, 0, 0};
    wlr_renderer_clear(core->renderer, clear_color);
    wlr_renderer_end(core->renderer);

    wlr_fb_attribs fb;
    fb.width = offscreen_buffer.fb_width;
    fb.height = offscreen_buffer.fb_height;

    for_each_surface([=] (wayfire_surface_t *surface, int x, int y)
    {
        surface->render_pixman(fb, x - buffer_geometry.x, y - buffer_geometry.y, NULL);
    }, true);

    GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, 0));
}

void wayfire_view_t::render_fb(pixman_region32_t* damage, wf_framebuffer fb)
{
    in_paint = true;
    if (transforms.size())
    {
        pixman_region32_t fb_region;
        if (damage == NULL)
        {
            pixman_region32_init_rect(&fb_region, 0, 0,
                fb.viewport_width, fb.viewport_height);
            damage = &fb_region;
        }

        take_snapshot();

        wf_geometry obox = get_untransformed_bounding_box();
        obox.width = offscreen_buffer.fb_width / offscreen_buffer.fb_scale;
        obox.height = offscreen_buffer.fb_height / offscreen_buffer.fb_scale;

        auto it = transforms.begin();

        GLuint last_tex = offscreen_buffer.tex;
        while(std::next(it) != transforms.end())
        {
            auto& tr = *it;
            auto& fb = tr->fb;

            auto tbox = tr->transform->get_bounding_box(obox, obox);
            if (fb.geometry.width != tbox.width || fb.geometry.height != tbox.height)
            {
                fb.release();
                fb.init(tbox.width, tbox.height);
            }

            fb.geometry = tbox;
            fb.viewport_width = tbox.width;
            fb.viewport_height = tbox.height;
            fb.clear();

            tr->transform->render_with_damage(last_tex, obox, {0, 0, tbox.width, tbox.height}, fb);
            last_tex = fb.tex;

            obox = tbox;
            ++it;
        }

        int n_rect;
        auto rects = pixman_region32_rectangles(damage, &n_rect);
        for (int i = 0; i < n_rect; i++)
        {
            auto box = wlr_box_from_pixman_box(rects[i]);
            auto sbox = get_scissor_box(output, box);
            (*it)->transform->render_with_damage(last_tex, obox, sbox, fb);

#ifdef WAYFIRE_GRAPHICS_DEBUG
            float proj[9];
            wlr_matrix_projection(proj, fb.viewport_width, fb.viewport_height,
                                  WL_OUTPUT_TRANSFORM_NORMAL);

            float col[4] = {0, 0.0, 0.2, 0.5};
            wlr_render_rect(core->renderer, &sbox, col, proj);
#endif
        }

        cleanup_transforms();

        if (damage == &fb_region)
            pixman_region32_fini(&fb_region);
    } else
    {
        wayfire_surface_t::render_fb(damage, fb);
    }
    in_paint = false;
}

void wayfire_view_t::add_transformer(std::unique_ptr<wf_view_transformer_t> transformer, std::string name)
{
    damage();
    auto tr = nonstd::make_unique<transform_t> ();
    tr->transform = std::move(transformer);
    tr->plugin_name = name;
    transforms.push_back(std::move(tr));
    damage();
}

void wayfire_view_t::add_transformer(std::unique_ptr<wf_view_transformer_t> transformer)
{

    add_transformer(std::move(transformer), "");
}

nonstd::observer_ptr<wf_view_transformer_t> wayfire_view_t::get_transformer(std::string name)
{
    for (auto& tr : transforms)
    {
        if (tr->plugin_name == name)
            return nonstd::make_observer(tr->transform.get());
    }

    return nullptr;
}

void wayfire_view_t::_pop_transformer(nonstd::observer_ptr<transform_t> transformer)
{
    damage();

    auto it = transforms.begin();
    while(it != transforms.end())
    {
        if ((*it).get() == transformer.get())
        {
            (*it)->fb.release();
            it = transforms.erase(it);
        } else
        {
            ++it;
        }
    }

    damage();
}

void wayfire_view_t::pop_transformer(nonstd::observer_ptr<wf_view_transformer_t> transformer)
{
    for(auto& tr : transforms)
    {
        if (tr->transform.get() == transformer.get())
            tr->to_remove = 1;
    }

    if (!in_paint)
        cleanup_transforms();
}

void wayfire_view_t::pop_transformer(std::string name)
{
    for(auto& tr : transforms)
    {
        if (tr->plugin_name == name)
            tr->to_remove = 1;
    }

    if (!in_paint)
        cleanup_transforms();
}

void wayfire_view_t::cleanup_transforms()
{
    std::vector<nonstd::observer_ptr<transform_t>> to_remove;
    for (auto& tr: transforms)
    {
        if (tr->to_remove)
            to_remove.push_back(nonstd::make_observer(tr.get()));
    }

    for (auto r : to_remove)
        _pop_transformer(r);
}

bool wayfire_view_t::has_transformer()
{
    return transforms.size();
}

wlr_box wayfire_view_t::get_bounding_box(std::string transformer)
{
    return get_bounding_box(get_transformer(transformer));
}

wlr_box wayfire_view_t::get_bounding_box(
    nonstd::observer_ptr<wf_view_transformer_t> transformer)
{
    return transform_region(get_untransformed_bounding_box(), transformer);
}


void emit_view_map(wayfire_view view)
{
    /* TODO: consider not emitting a create-view for special surfaces */
    map_view_signal data;
    data.view = view;
    view->get_output()->emit_signal("map-view", &data);
    emit_map_state_change(view.get());
}

void wayfire_view_t::reposition_relative_to_parent()
{
    assert(parent);
    auto workarea = output->workspace->get_workarea();

    auto wm_geometry = get_wm_geometry();
    if (parent->is_mapped())
    {
        auto parent_geometry = parent->get_wm_geometry();
        int sx = parent_geometry.x + (parent_geometry.width  - wm_geometry.width) / 2;
        int sy = parent_geometry.y + (parent_geometry.height - wm_geometry.height) / 2;

        move(sx, sy, false);
    } else
    {
        /* if we have a parent which still isn't mapped, we cannot determine
         * the view's position, so we center it on the screen */
        int sx = workarea.width / 2 - wm_geometry.width / 2;
        int sy = workarea.height/ 2 - wm_geometry.height/ 2;

        move(sx, sy, false);
    }
}

void wayfire_view_t::map(wlr_surface *surface)
{
    wayfire_surface_t::map(surface);

    if (role == WF_VIEW_ROLE_TOPLEVEL && !parent && !maximized && !fullscreen)
    {
        auto wm = get_wm_geometry();
        auto workarea = output->workspace->get_workarea();

        move(wm.x + workarea.x, wm.y + workarea.y, false);
    }

    if (update_size())
        damage();

    if (parent)
        reposition_relative_to_parent();

    if (role != WF_VIEW_ROLE_SHELL_VIEW)
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
    if (parent)
        set_toplevel_parent(nullptr);

    auto copy = children;
    for (auto c : copy)
        c->set_toplevel_parent(nullptr);

    log_info("unmap %s %s %p", get_title().c_str(), get_app_id().c_str(), this);
    if (output)
        emit_view_unmap(self());

    wayfire_surface_t::unmap();
}

void wayfire_view_t::move_request()
{
    move_request_signal data;
    data.view = self();
    output->emit_signal("move-request", &data);
}

void wayfire_view_t::resize_request(uint32_t edges)
{
    resize_request_signal data;
    data.view = self();
    data.edges = edges;
    output->emit_signal("resize-request", &data);
}

void wayfire_view_t::maximize_request(bool state)
{
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
    if (update_size())
    {
        if (frame)
            frame->notify_view_resized(get_wm_geometry());
    }

    /* clear the resize edges.
     * This is must be done here because if the user(or plugin) resizes too fast,
     * the shell client might still haven't configured the surface, * and in this
     * case the next commit(here) needs to still have access to the gravity */
    if (!in_continuous_resize)
        edges = 0;
}

void wayfire_view_t::damage()
{
    damage(get_untransformed_bounding_box());
}

void wayfire_view_t::destruct()
{
    set_decoration(nullptr);
    core->erase_view(self());
}

void wayfire_view_t::destroy()
{
    destroyed = 1;
    dec_keep_count();
}

void wayfire_view_t::set_toplevel_parent(wayfire_view parent)
{
    if (parent == this->parent)
        return;

    if (parent)
    {
        parent->children.push_back(self());
    } else
    {
        auto it = std::remove(this->parent->children.begin(), this->parent->children.end(), self());
        this->parent->children.erase(it, this->parent->children.end());
    }

    this->parent = parent;

    /* if the view isn't mapped, then it will be positioned properly in map() */
    if (is_mapped())
    {
        if (parent)
            reposition_relative_to_parent();
    }
}

wayfire_view_t::~wayfire_view_t()
{
    pixman_region32_fini(&offscreen_buffer.cached_damage);
}

void emit_title_changed(wayfire_view view)
{
    if (!view)
        return;
    title_changed_signal data;
    data.view = view;
    view->get_output()->emit_signal("view-title-changed", &data);
}

void emit_app_id_changed(wayfire_view view)
{
    if (!view)
        return;

    app_id_changed_signal data;
    data.view = view;
    view->get_output()->emit_signal("view-app-id-changed", &data);
}

void init_desktop_apis()
{
    init_xdg_shell();
    init_layer_shell();
    init_xdg_shell_v6();
    init_xwayland();
}
