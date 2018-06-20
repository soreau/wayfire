#include <algorithm>
extern "C"
{
#define static
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_matrix.h>
#include <wlr/util/region.h>
#undef static
}

#include "priv-view.hpp"
#include "opengl.hpp"
#include "core.hpp"
#include "output.hpp"
#include "debug.hpp"
#include "signal-definitions.hpp"

void handle_surface_committed(wl_listener*, void *data)
{
    auto wlr_surf = (wlr_surface*) data;
    auto surface = wf_surface_from_void(wlr_surf->data);
    assert(surface);

    surface->commit();
}

void handle_subsurface_created(wl_listener*, void *data)
{
    auto sub = static_cast<wlr_subsurface*> (data);
    if (sub->surface->data)
        return;

    auto parent = wf_surface_from_void(sub->parent->data);
    if (!parent)
    {
        log_error("subsurface created with invalid parent!");
        return;
    }

    auto it = std::find_if(parent->surface_children.begin(),
                           parent->surface_children.end(),
                           [=] (wayfire_surface_t *surface)
                            { return surface->surface == sub->surface; });

    if (it == parent->surface_children.end())
    {
        auto surf = new wayfire_surface_t(parent);
        surf->map(sub->surface);
    } else
    {
        log_debug("adding same subsurface twice");
    }
}

void handle_subsurface_destroyed(wl_listener*, void *data)
{
    auto wlr_surf = (wlr_surface*) data;
    auto surface = wf_surface_from_void(wlr_surf->data);

    log_error ("subsurface destroyed");

    surface->unmap();
    surface->dec_keep_count();
}

wayfire_surface_t::wayfire_surface_t(wayfire_surface_t* parent)
{
    inc_keep_count();
    this->parent_surface = parent;

    if (parent)
    {
        set_output(parent->output);
        parent->surface_children.push_back(this);
    }

    new_sub.notify   = handle_subsurface_created;
    committed.notify = handle_surface_committed;
    destroy.notify   = nullptr;
}

wayfire_surface_t::~wayfire_surface_t()
{
    if (parent_surface)
    {
        auto it = parent_surface->surface_children.begin();
        while(it != parent_surface->surface_children.end())
        {
            if (*it == this)
                it = parent_surface->surface_children.erase(it);
            else
                ++it;
        }
    }

    for (auto c : surface_children)
    {
        /* if we are a decoration window, then we shouldn't
         * destroy children(contained view) like that */
        auto cast = dynamic_cast<wayfire_view_t*> (c);
        if (!cast)
            c->destruct();
    }
}

wayfire_surface_t *wayfire_surface_t::get_main_surface()
{
    return this->parent_surface ? this->parent_surface->get_main_surface() : this;
}

bool wayfire_surface_t::is_subsurface()
{
    return wlr_surface_is_subsurface(surface);
}

void wayfire_surface_t::get_child_position(int &x, int &y)
{
    x = surface->current->subsurface_position.x;
    y = surface->current->subsurface_position.y;
}

void wayfire_surface_t::get_child_offset(int &x, int &y)
{
    x = 0;
    y = 0;
};

wf_point wayfire_surface_t::get_output_position()
{
    auto pos = parent_surface->get_output_position();

    int dx, dy;
    get_child_position(dx, dy);
    pos.x += dx; pos.y += dy;

    return pos;
}

wf_geometry wayfire_surface_t::get_output_geometry()
{
    if (!is_mapped())
        return {0, 0, 0, 0};

    auto pos = get_output_position();
    return {
        pos.x, pos.y,
        surface->current ? surface->current->width : 0,
        surface->current ? surface->current->height : 0
    };
}

void wayfire_surface_t::map(wlr_surface *surface)
{
    assert(!this->surface && surface);
    this->surface = surface;

    /* force surface_send_enter() */
    set_output(output);

    wl_signal_add(&surface->events.new_subsurface, &new_sub);
    wl_signal_add(&surface->events.commit,         &committed);

    /* map by default if this is a subsurface, only toplevels/popups have map/unmap events */
    if (wlr_surface_is_subsurface(surface))
    {
        destroy.notify = handle_subsurface_destroyed;
        wl_signal_add(&surface->events.destroy, &destroy);
    }

    surface->data = this;
    damage();

    wlr_subsurface *sub;
    wl_list_for_each(sub, &surface->subsurfaces, parent_link)
        handle_subsurface_created(NULL, sub);
}

void wayfire_surface_t::unmap()
{
    assert(this->surface);
    damage();

    this->surface = nullptr;

    if (this->output)
    {
        _surface_unmapped_signal data;
        data.surface = this;
        output->emit_signal("_surface_unmapped", &data);
    }

    wl_list_remove(&new_sub.link);
    wl_list_remove(&committed.link);
    if (destroy.notify)
        wl_list_remove(&destroy.link);
}
void wayfire_surface_t::inc_keep_count()
{
    ++keep_count;
}

void wayfire_surface_t::dec_keep_count()
{
    --keep_count;
    if (!keep_count)
        destruct();
}

void wayfire_surface_t::damage(pixman_region32_t *region)
{
    int n_rect;
    auto rects = pixman_region32_rectangles(region, &n_rect);

    for (int i = 0; i < n_rect; i++)
        damage({rects[i].x1, rects[i].y1, rects[i].x2 - rects[i].x1, rects[i].y2 - rects[i].y1});
}

void wayfire_surface_t::damage(const wlr_box& box)
{
    assert(parent_surface);
    parent_surface->damage(box);
}

void wayfire_surface_t::damage()
{
    /* TODO: bounding box damage */
    damage(geometry);
}

void wayfire_surface_t::update_output_position()
{
    wf_geometry rect = get_output_geometry();

    /* TODO: recursively damage children? */
    if (is_subsurface() && rect != geometry)
    {
        damage(geometry);
        damage(rect);

        geometry = rect;
    }
}

void wayfire_surface_t::commit()
{
    update_output_position();
    auto rect = get_output_position();

    pixman_region32_t dmg;
    pixman_region32_init(&dmg);
    pixman_region32_copy(&dmg, &surface->current->surface_damage);
    pixman_region32_translate(&dmg, rect.x, rect.y);

    /* TODO: transform damage */
    damage(&dmg);
    pixman_region32_fini(&dmg);
}

void wayfire_surface_t::set_output(wayfire_output *out)
{
    if (output && surface)
        wlr_surface_send_leave(surface, output->handle);

    if (out && surface)
        wlr_surface_send_enter(surface, out->handle);

    output = out;
    for (auto c : surface_children)
        c->set_output(out);
}

void wayfire_surface_t::for_each_surface_recursive(wf_surface_iterator_callback call,
                                                   int x, int y, bool reverse)
{
    if (reverse)
    {
        if (is_mapped())
            call(this, x, y);

        int dx, dy;

        for (auto c : surface_children)
        {
            if (!c->is_mapped())
                continue;

            c->get_child_position(dx, dy);
            c->for_each_surface_recursive(call, x + dx, y + dy, reverse);
        }
    } else
    {
        auto it = surface_children.rbegin();
        int dx, dy;

        while(it != surface_children.rend())
        {
            auto& c = *it;

            if (c->is_mapped())
            {
                c->get_child_position(dx, dy);
                c->for_each_surface_recursive(call, x + dx, y + dy, reverse);
            }

            ++it;
        }

        if (is_mapped())
            call(this, x, y);
    }
}

void wayfire_surface_t::for_each_surface(wf_surface_iterator_callback call, bool reverse)
{
    auto pos = get_output_position();
    for_each_surface_recursive(call, pos.x, pos.y, reverse);
}

void wayfire_surface_t::render_fbo(int x, int y, int fb_w, int fb_h,
                                   pixman_region32_t *damage)
{
    if (!is_mapped() || !wlr_surface_has_buffer(surface))
        return;

    /* TODO: optimize, use offscreen_buffer.cached_damage */
    wlr_box fb_geometry;

    fb_geometry.x = x; fb_geometry.y = y;
    fb_geometry.width = surface->current->width;
    fb_geometry.height = surface->current->height;

    float id[9];
    wlr_matrix_projection(id, fb_w, fb_h, WL_OUTPUT_TRANSFORM_NORMAL);

    float matrix[9];
    wlr_matrix_project_box(matrix, &fb_geometry, WL_OUTPUT_TRANSFORM_NORMAL, 0, id);

    wlr_matrix_scale(matrix, 1.0 / fb_geometry.width, 1.0 / fb_geometry.height);

    wlr_renderer_scissor(core->renderer, NULL);
    wlr_render_texture(core->renderer, surface->texture, matrix, 0, 0, 1.0f);
}

void wayfire_surface_t::render(int x, int y, wlr_box *damage)
{
    if (!wlr_surface_has_buffer(surface))
        return;

    wlr_box geometry {x, y, surface->current->width, surface->current->height};
    geometry = get_output_box_from_box(geometry, output->handle->scale);

    if (!damage) damage = &geometry;

    auto rr = core->renderer;
    float matrix[9];
    wlr_matrix_project_box(matrix, &geometry,
                           surface->current->transform,
                           0, output->handle->transform_matrix);

    auto box = get_scissor_box(output, *damage);
    wlr_renderer_scissor(rr, &box);

    wlr_render_texture_with_matrix(rr, surface->texture, matrix, alpha);
}

void wayfire_surface_t::render_pixman(int x, int y, pixman_region32_t *damage)
{
    int n_rect;
    auto rects = pixman_region32_rectangles(damage, &n_rect);

    for (int i = 0; i < n_rect; i++)
    {
        wlr_box d = wlr_box_from_pixman_box(rects[i]);
        render(x, y, &d);
    }
}

void wayfire_surface_t::render_fb(int x, int y, pixman_region32_t *damage, int fb)
{
    if (!is_mapped() || !wlr_surface_has_buffer(surface))
        return;

    GL_CALL(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb));
    render_pixman(x, y, damage);
}

