#include <algorithm>
#include <map>
#include <wayfire/debug.hpp>
extern "C"
{
#include <wlr/types/wlr_surface.h>
#define static
#include <wlr/types/wlr_compositor.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/gles2.h>
#include <wlr/types/wlr_matrix.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/util/region.h>
#undef static
}

#include "surface-impl.hpp"
#include "subsurface.hpp"
#include "wayfire/opengl.hpp"
#include "../core/core-impl.hpp"
#include "wayfire/output.hpp"
#include <wayfire/util/log.hpp>
#include "wayfire/render-manager.hpp"
#include "wayfire/signal-definitions.hpp"

/****************************
 * surface_interface_t functions
 ****************************/
wf::surface_interface_t::surface_interface_t(surface_interface_t *parent)
{
    this->priv = std::make_unique<impl>();
    take_ref();
    this->priv->parent_surface = parent;

    if (parent)
    {
        set_output(parent->get_output());
        parent->priv->surface_children.insert(
            parent->priv->surface_children.begin(), this);
    }
}

wf::surface_interface_t::~surface_interface_t()
{
    if (priv->parent_surface)
    {
        auto& container = priv->parent_surface->priv->surface_children;
        auto it = std::remove(container.begin(), container.end(), this);
        container.erase(it, container.end());
    }

    for (auto c : priv->surface_children)
        c->priv->parent_surface = nullptr;
}

void wf::surface_interface_t::take_ref()
{
    ++priv->ref_cnt;
}

void wf::surface_interface_t::unref()
{
    --priv->ref_cnt;
    if (priv->ref_cnt <= 0)
        destruct();
}

wf::surface_interface_t *wf::surface_interface_t::get_main_surface()
{
    if (priv->parent_surface)
        return priv->parent_surface->get_main_surface();

    return this;
}

std::vector<wf::surface_iterator_t> wf::surface_interface_t::enumerate_surfaces(
    wf::point_t surface_origin)
{
    std::vector<wf::surface_iterator_t> result;
    for (auto& child : priv->surface_children)
    {
        if (child->is_mapped())
        {
            auto child_surfaces = child->enumerate_surfaces(
                child->get_offset() + surface_origin);

            result.insert(result.end(),
                child_surfaces.begin(), child_surfaces.end());
        }
    }

    if (is_mapped())
        result.push_back({this, surface_origin});

    return result;
}

wf::output_t *wf::surface_interface_t::get_output()
{
    return priv->output;
}

void wf::surface_interface_t::set_output(wf::output_t* output)
{
    priv->output = output;
    for (auto& c : priv->surface_children)
        c->set_output(output);
}

/* Static method */
int wf::surface_interface_t::impl::active_shrink_constraint = 0;

void wf::surface_interface_t::set_opaque_shrink_constraint(
    std::string constraint_name, int value)
{
    static std::map<std::string, int> shrink_constraints;

    shrink_constraints[constraint_name] = value;

    impl::active_shrink_constraint = 0;
    for (auto& constr : shrink_constraints)
    {
        impl::active_shrink_constraint =
            std::max(impl::active_shrink_constraint, constr.second);
    }
}

int wf::surface_interface_t::get_active_shrink_constraint()
{
    return impl::active_shrink_constraint;
}

void wf::surface_interface_t::destruct()
{
    delete this;
}

/****************************
 * surface_interface_t functions for surfaces which are
 * backed by a wlr_surface
 ****************************/
void wf::surface_interface_t::send_frame_done(const timespec& time)
{
    if (priv->wsurface)
        wlr_surface_send_frame_done(priv->wsurface, &time);
}

bool wf::surface_interface_t::accepts_input(int32_t sx, int32_t sy)
{
    if (!priv->wsurface)
        return false;

    return wlr_surface_point_accepts_input(priv->wsurface, sx, sy);
}

void wf::surface_interface_t::subtract_opaque(wf::region_t& region, int x, int y)
{
    if (!priv->wsurface)
        return;

    wf::region_t opaque{&priv->wsurface->opaque_region};
    opaque += wf::point_t{x, y};
    opaque *= get_output()->handle->scale;

    /* region scaling uses std::ceil/std::floor, so the resulting region
     * encompasses the opaque region. However, in the case of opaque region, we
     * don't want any pixels that aren't actually opaque. So in case of
     * different scales, we just shrink by 1 to compensate for the ceil/floor
     * discrepancy */
    int ceil_factor = 0;
    if (get_output()->handle->scale != (float)priv->wsurface->current.scale)
        ceil_factor = 1;

    opaque.expand_edges(-get_active_shrink_constraint() - ceil_factor);
    region ^= opaque;
}

wl_client* wf::surface_interface_t::get_client()
{
    if (priv->wsurface)
        return wl_resource_get_client(priv->wsurface->resource);

    return nullptr;
}

wlr_surface *wf::surface_interface_t::get_wlr_surface()
{
    return priv->wsurface;
}

wf::wlr_surface_base_t::wlr_surface_base_t(surface_interface_t *self)
{
    _as_si = self;
    handle_new_subsurface = [&] (void* data)
    {
        auto sub = static_cast<wlr_subsurface*> (data);
        if (sub->data)
        {
            LOGE("Creating the same subsurface twice!");
            return;
        }

        // parent isn't mapped yet
        if (!sub->parent->data)
            return;

        // will be deleted by destruct()
        auto subsurface = new subsurface_implementation_t(sub, _as_si);
        if (sub->mapped)
            subsurface->map(sub->surface);
    };

    on_new_subsurface.set_callback(handle_new_subsurface);
    on_commit.set_callback([&] (void*) { commit(); });
}

wf::wlr_surface_base_t::~wlr_surface_base_t() {}



wf::point_t wf::wlr_surface_base_t::get_window_offset()
{
    return {0, 0};
}

bool wf::wlr_surface_base_t::_is_mapped() const
{
    return surface;
}

wf::dimensions_t wf::wlr_surface_base_t::_get_size() const
{
    if (!_is_mapped())
        return {0, 0};

    return {
        surface->current.width,
        surface->current.height,
    };
}

void wf::emit_map_state_change(wf::surface_interface_t *surface)
{
    std::string state = surface->is_mapped() ? "_surface_mapped" : "_surface_unmapped";

    _surface_map_state_changed_signal data;
    data.surface = surface;
    wf::get_core().emit_signal(state, &data);
}

void wf::wlr_surface_base_t::map(wlr_surface *surface)
{
    assert(!this->surface && surface);
    this->surface = surface;

    _as_si->priv->wsurface = surface;

    /* force surface_send_enter(), and also check whether parent surface
     * output hasn't changed while we were unmapped */
    wf::output_t *output = _as_si->priv->parent_surface ?
        _as_si->priv->parent_surface->get_output() : _as_si->get_output();
    _as_si->set_output(output);

    on_new_subsurface.connect(&surface->events.new_subsurface);
    on_commit.connect(&surface->events.commit);

    surface->data = _as_si;

    /* Handle subsurfaces which were created before this surface was mapped */
    wlr_subsurface *sub;
    wl_list_for_each(sub, &surface->subsurfaces, parent_link)
        handle_new_subsurface(sub);

    emit_map_state_change(_as_si);
}

void wf::wlr_surface_base_t::unmap()
{
    assert(this->surface);
    apply_surface_damage();
    damage_surface_box({.x = 0, .y = 0,
        .width = _get_size().width, .height = _get_size().height});

    this->surface->data = NULL;
    this->surface = nullptr;
    this->_as_si->priv->wsurface = nullptr;
    emit_map_state_change(_as_si);

    on_new_subsurface.disconnect();
    on_destroy.disconnect();
    on_commit.disconnect();
}

wlr_buffer* wf::wlr_surface_base_t::get_buffer()
{
    if (surface && wlr_surface_has_buffer(surface))
        return surface->buffer;

    return nullptr;
}

void wf::wlr_surface_base_t::damage_surface_region(
    const wf::region_t& dmg)
{
    for (const auto& rect : dmg)
        damage_surface_box(wlr_box_from_pixman_box(rect));
}

void wf::wlr_surface_base_t::damage_surface_box(const wlr_box& box)
{
    auto parent =
        dynamic_cast<wlr_surface_base_t*> (_as_si->priv->parent_surface);

    /* wlr_view_t overrides damage_surface_box and applies it to the output */
    if (parent && parent->_is_mapped())
    {
        wlr_box parent_box = box;
        parent_box.x += _as_si->get_offset().x;
        parent_box.y += _as_si->get_offset().y;
        parent->damage_surface_box(parent_box);
    }
}

void wf::wlr_surface_base_t::apply_surface_damage()
{
    if (!_as_si->get_output() || !_is_mapped())
        return;

    wf::region_t dmg;
    wlr_surface_get_effective_damage(surface, dmg.to_pixman());

    if (surface->current.scale != 1 ||
        surface->current.scale != _as_si->get_output()->handle->scale)
        dmg.expand_edges(1);

    damage_surface_region(dmg);
}

void wf::wlr_surface_base_t::commit()
{
    apply_surface_damage();
    if (_as_si->get_output())
    {
        /* we schedule redraw, because the surface might expect
         * a frame callback */
        _as_si->get_output()->render->schedule_redraw();
    }
}

void wf::wlr_surface_base_t::update_output(wf::output_t *old_output,
    wf::output_t *new_output)
{
    /* We should send send_leave only if the output is different from the last. */
    if (old_output && old_output != new_output && surface)
        wlr_surface_send_leave(surface, old_output->handle);

    if (new_output && surface)
        wlr_surface_send_enter(surface, new_output->handle);
}

wf::texture_t wf::get_texture_from_surface(wlr_surface *surface)
{
    assert(wlr_texture_is_gles2(surface->buffer->texture));

    wlr_gles2_texture_attribs attribs;
    wlr_gles2_texture_get_attribs(surface->buffer->texture, &attribs);

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

void wf::wlr_surface_base_t::_simple_render(const wf::framebuffer_t& fb,
    int x, int y, const wf::region_t& damage)
{
    if (!get_buffer())
        return;

    float rx = x + fb.geometry.x;
    float ry = y + fb.geometry.y;
    gl_geometry geometry {
        rx, ry,
        rx + surface->current.width, ry + surface->current.height,
    };
    auto texture = get_texture_from_surface(surface);
    auto matrix = fb.get_orthographic_projection();

    OpenGL::render_begin(fb);
    for (const auto& rect : damage)
    {
        auto box = wlr_box_from_pixman_box(rect);
        fb.scissor(fb.framebuffer_box_from_damage_box(box));
        OpenGL::render_transformed_texture(texture, geometry, {}, matrix);
    }
    OpenGL::render_end();
}

wf::wlr_child_surface_base_t::wlr_child_surface_base_t(
    surface_interface_t *parent, surface_interface_t *self) :
      wf::surface_interface_t(parent),
      wlr_surface_base_t(self)
{
}

wf::wlr_child_surface_base_t::~wlr_child_surface_base_t() { }
