#include "render-manager.hpp"
#include "output.hpp"
#include "core.hpp"
#include "workspace-manager.hpp"
#include "input-manager.hpp"
#include "opengl.hpp"
#include <algorithm>

extern "C"
{
    /* wlr uses some c99 extensions, we "disable" the static keyword to workaround */
#define static
#include <wlr/render/wlr_renderer.h>
#undef static
#include <wlr/types/wlr_output_damage.h>
#include <wlr/util/region.h>
}

struct wf_output_damage
{
    pixman_region32_t frame_damage;
    wlr_output *output;
    wlr_output_damage *damage_manager;

    wf_output_damage(wlr_output *output)
    {
        this->output = output;
        pixman_region32_init(&frame_damage);
        damage_manager = wlr_output_damage_create(output);
    }

    void add()
    {
        int w, h;
        wlr_output_transformed_resolution(output, &w, &h);
        add({0, 0, w, h});
    }

    void add(const wlr_box& box)
    {
        pixman_region32_union_rect(&frame_damage, &frame_damage,
                                   box.x, box.y, box.width, box.height);

        auto sbox = box;
        wlr_output_damage_add_box(damage_manager, &sbox);
        schedule_repaint();
    }

    void add(pixman_region32_t *region)
    {
        pixman_region32_union(&frame_damage, &frame_damage, region);
        wlr_output_damage_add(damage_manager, region);
        schedule_repaint();
    }

    bool make_current(pixman_region32_t *out_damage, bool& need_swap)
    {
        auto r = wlr_output_damage_make_current(damage_manager, &need_swap, out_damage);
        if (r)
        {
            pixman_region32_t regular;

            int w, h;
            wlr_output_transformed_resolution(output, &w, &h);

            pixman_region32_init_rect(&regular, 0, 0, w, h);
            pixman_region32_subtract(&frame_damage, &frame_damage, &regular);

            pixman_region32_union(out_damage, out_damage, &frame_damage);

            pixman_region32_fini(&regular);
        }
        return r;
    }

    void swap_buffers(timespec *when, pixman_region32_t *swap_damage)
    {
        wlr_output_damage_swap_buffers(damage_manager, when, swap_damage);
        pixman_region32_clear(&frame_damage);
    }

    void schedule_repaint()
    {
        wlr_output_schedule_frame(output);
    }
};

void frame_cb (wl_listener*, void *data)
{
    auto damage = static_cast<wlr_output*>(data);
    assert(damage);

    auto output = core->get_output(damage);
    assert(output);
    output->render->paint();
}

render_manager::render_manager(wayfire_output *o)
{
    output = o;

    /* TODO: do we really need a unique_ptr? */
    output_damage = std::unique_ptr<wf_output_damage>(new wf_output_damage(output->handle));
    output_damage->add();

    frame_listener.notify = frame_cb;
    wl_signal_add(&output->handle->events.frame, &frame_listener);

    pixman_region32_init(&frame_damage);

    schedule_redraw();
}

void render_manager::init_default_streams()
{
    GetTuple(vw, vh, output->workspace->get_workspace_grid_size());
    output_streams.resize(vw);

    for (int i = 0; i < vw; i++) {
        for (int j = 0; j < vh; j++) {
            output_streams[i].push_back(wf_workspace_stream{});
            output_streams[i][j].tex = output_streams[i][j].fbuff = 0;
            output_streams[i][j].ws = std::make_tuple(i, j);
        }
    }
}

void render_manager::load_context()
{
    ctx = OpenGL::create_gles_context(output, core->shadersrc.c_str());
    OpenGL::bind_context(ctx);

    dirty_context = false;
    output->emit_signal("reload-gl", nullptr);

    init_default_streams();
}

void render_manager::release_context()
{
    OpenGL::release_context(ctx);
    dirty_context = true;
}

render_manager::~render_manager()
{
    wl_list_remove(&frame_listener.link);

    if (idle_redraw_source)
        wl_event_source_remove(idle_redraw_source);
    if (idle_damage_source)
        wl_event_source_remove(idle_damage_source);

    pixman_region32_fini(&frame_damage);
    release_context();
}

void render_manager::damage(const wlr_box& box)
{
    output_damage->add(box);
}

void render_manager::damage(pixman_region32_t *region)
{
    if (region)
        output_damage->add(region);
    else
        output_damage->add();
}

void redraw_idle_cb(void *data)
{
    wayfire_output *output = (wayfire_output*) data;
    assert(output);

    wlr_output_schedule_frame(output->handle);
    output->render->idle_redraw_source = NULL;
}

void render_manager::auto_redraw(bool redraw)
{
    constant_redraw += (redraw ? 1 : -1);
    if (constant_redraw > 1) /* no change, exit */
        return;
    if (constant_redraw < 0)
    {
        constant_redraw = 0;
        return;
    }

    schedule_redraw();
}

void render_manager::schedule_redraw()
{
    if (idle_redraw_source == NULL)
        idle_redraw_source = wl_event_loop_add_idle(core->ev_loop, redraw_idle_cb, output);
}

/* return damage from this frame for the given workspace, coordinates relative to the workspace */
void render_manager::get_ws_damage(std::tuple<int, int> ws, pixman_region32_t *out_damage)
{
    GetTuple(vx, vy, ws);
    GetTuple(cx, cy, output->workspace->get_current_workspace());

    int sw, sh;
    wlr_output_transformed_resolution(output->handle, &sw, &sh);

    if (!pixman_region32_selfcheck(out_damage))
        pixman_region32_init(out_damage);

    pixman_region32_intersect_rect(out_damage,
                                   &frame_damage,
                                   (vx - cx) * sw,
                                   (vy - cy) * sh,
                                   sw, sh);

    pixman_region32_translate(out_damage, (cx - vx) * sw, (cy - vy) * sh);
}

void damage_idle_cb(void *data)
{
    auto rm = (render_manager*) data;
    assert(rm);

    rm->damage(NULL);
    rm->idle_damage_source = NULL;
}


void render_manager::reset_renderer()
{
    renderer = nullptr;

    if (!idle_damage_source)
        idle_damage_source = wl_event_loop_add_idle(core->ev_loop, damage_idle_cb, this);
}

void render_manager::set_renderer(render_hook_t rh)
{
    renderer = rh;
}

void render_manager::set_hide_overlay_panels(bool set)
{
    draw_overlay_panel = !set;
}

void render_manager::render_panels()
{

    auto vp = output->workspace->get_current_workspace();
    auto views = output->workspace->get_views_on_workspace(vp, WF_ABOVE_LAYERS);
    auto it = views.rbegin();
    while (it != views.rend())
    {
        auto view = *it;
        if (!view->is_hidden) /* use is_visible() when implemented */
        {
            auto og = view->get_output_position();
            view->render(og.x, og.y, NULL);
        }

        ++it;
    }
}

static inline int64_t timespec_to_msec(const struct timespec *a) {
     return (int64_t)a->tv_sec * 1000 + a->tv_nsec / 1000000;
}

void render_manager::paint()
{
    timespec repaint_started;
    clock_gettime(CLOCK_MONOTONIC, &repaint_started);

    /* TODO: perhaps we don't need to copy frame damage */
    pixman_region32_clear(&frame_damage);

    run_effects(effects[WF_OUTPUT_EFFECT_PRE]);

    bool needs_swap;
    if (!output_damage->make_current(&frame_damage, needs_swap) || !needs_swap)
        if (!constant_redraw && false)
            return;

    pixman_region32_t swap_damage;
    pixman_region32_init(&swap_damage);

    auto rr = wlr_backend_get_renderer(core->backend);
    wlr_renderer_begin(rr, output->handle->width, output->handle->height);

    int w, h;
    wlr_output_transformed_resolution(output->handle, &w, &h);

    if (dirty_context)
        load_context();

    if (renderer)
    {
        renderer();
        /* TODO: let custom renderers specify what they want to repaint... */
    } else
    {
        pixman_region32_intersect_rect(&frame_damage, &frame_damage, 0, 0, w, h);

        if (pixman_region32_not_empty(&frame_damage))
        {
            pixman_region32_copy(&swap_damage, &frame_damage);
            GetTuple(vx, vy, output->workspace->get_current_workspace());
            auto target_stream = &output_streams[vx][vy];
            if (current_ws_stream != target_stream)
            {
                if (current_ws_stream)
                    workspace_stream_stop(current_ws_stream);

                current_ws_stream = target_stream;
                workspace_stream_start(current_ws_stream);
            } else
            {
                workspace_stream_update(current_ws_stream);
            }
        } else
        {
        }
    }

    run_effects(effects[WF_OUTPUT_EFFECT_OVERLAY]);

    wlr_renderer_scissor(rr, NULL);
    wlr_renderer_end(rr);

    if (renderer)
        output_damage->swap_buffers(&repaint_started, &swap_damage);
    else
        output_damage->swap_buffers(&repaint_started, &swap_damage);

    pixman_region32_fini(&swap_damage);
    post_paint();

    /*
       OpenGL::bind_context(ctx);
    if (renderer)
    {
        OpenGL::bind_context(ctx);
        renderer();
    }  else {
    } */
}

void render_manager::post_paint()
{
    /*
    if (!renderer || draw_overlay_panel)
        render_panels();
        */

    run_effects(effects[WF_OUTPUT_EFFECT_POST]);

    if (constant_redraw)
        schedule_redraw();

    auto send_frame_done =
        [=] (wayfire_view v)
        {
            if (!v->is_mapped())
                return;

            v->for_each_surface([] (wayfire_surface_t *surface, int, int)
                                {
                                    struct timespec now;
                                    clock_gettime(CLOCK_MONOTONIC, &now);
                                    wlr_surface_send_frame_done(surface->surface, &now);
                                });
        };

    /* TODO: do this only if the view isn't fully occluded by another */
    if (renderer)
    {
        output->workspace->for_each_view(send_frame_done, WF_ALL_LAYERS);
    } else
    {
        auto views = output->workspace->get_views_on_workspace(
            output->workspace->get_current_workspace(), WF_ALL_LAYERS);

        for (auto v : views)
            send_frame_done(v);
    }
}

void render_manager::run_effects(effect_container_t& container)
{
    std::vector<effect_hook_t*> active_effects;
    for (auto effect : container)
        active_effects.push_back(effect);

    for (auto effect : active_effects)
        (*effect)();
}

void render_manager::add_effect(effect_hook_t* hook, wf_output_effect_type type)
{
    effects[type].push_back(hook);
}

void render_manager::rem_effect(const effect_hook_t *hook, wf_output_effect_type type)
{
    auto& container = effects[type];
    auto it = std::remove_if(container.begin(), container.end(),
                             [hook] (const effect_hook_t *h)
                             {
                                 if (h == hook)
                                     return true;
                                 return false;
                             });

    container.erase(it, container.end());
}

void render_manager::workspace_stream_start(wf_workspace_stream *stream)
{
    stream->running = true;
    stream->scale_x = stream->scale_y = 1;

    OpenGL::bind_context(output->render->ctx);

    if (stream->fbuff == (uint)-1 || stream->tex == (uint)-1)
        OpenGL::prepare_framebuffer(stream->fbuff, stream->tex);

    GL_CALL(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, stream->fbuff));
    GL_CALL(glClearColor(0, 0, 0, 1));
    GL_CALL(glClear(GL_COLOR_BUFFER_BIT));

    GetTuple(vx, vy, stream->ws);
    GetTuple(cx, cy, output->workspace->get_current_workspace());

    int sw, sh;
    wlr_output_transformed_resolution(output->handle, &sw, &sh);

    /* damage the whole workspace region, so that we get a full repaint */
    pixman_region32_union_rect(&frame_damage,
                               &frame_damage,
                               (vx - cx) * sw,
                               (vy - cy) * sh,
                               sw, sh);

    workspace_stream_update(stream, 1, 1);

}

void render_manager::workspace_stream_update(wf_workspace_stream *stream,
                                             float scale_x, float scale_y)
{
    OpenGL::bind_context(output->render->ctx);
    auto g = output->get_relative_geometry();

    GetTuple(x, y, stream->ws);
    GetTuple(cx, cy, output->workspace->get_current_workspace());

    int dx = g.x + (x - cx) * g.width,
        dy = g.y + (y - cy) * g.height;

    pixman_region32_t ws_damage;
    pixman_region32_init(&ws_damage);
    get_ws_damage(stream->ws, &ws_damage);

    float current_resolution = stream->scale_x * stream->scale_y;
    float target_resolution = scale_x * scale_y;

    float scaling = std::max(current_resolution / target_resolution,
                             target_resolution / current_resolution);

    /* TODO: investigate if this still works */
    if (scaling > 2 && false)
    {
        int sw, sh;
        wlr_output_transformed_resolution(output->handle, &sw, &sh);

        pixman_region32_union_rect(&ws_damage, &ws_damage, dx, dy,
                sw, sh);
    }

    /* we don't have to update anything */
    if (!pixman_region32_not_empty(&ws_damage))
    {
        pixman_region32_fini(&ws_damage);
        return;
    }

    if (scale_x != stream->scale_x || scale_y != stream->scale_y)
    {
        /* FIXME: enable scaled rendering */
//        stream->scale_x = scale_x;
//        stream->scale_y = scale_y;

        int sw, sh;
        wlr_output_transformed_resolution(output->handle, &sw, &sh);
        pixman_region32_union_rect(&ws_damage, &ws_damage, dx, dy,
                                   sw, sh);
    }

    auto views = output->workspace->get_views_on_workspace(stream->ws, WF_ALL_LAYERS);

    struct damaged_surface_t
    {
        wayfire_surface_t *surface;

        int x, y;
        pixman_region32_t damage;

        ~damaged_surface_t()
        { pixman_region32_fini(&damage); }
    };

    using damaged_surface = std::unique_ptr<damaged_surface_t>;

    std::vector<damaged_surface> to_render;

    const auto schedule_render_snapshotted_view =
        [&] (wayfire_view view, int view_dx, int view_dy)
        {
            auto ds = damaged_surface(new damaged_surface_t);

            auto bbox = view->get_bounding_box();
            auto obox = view->get_output_geometry();

            bbox = bbox + wf_point{view_dx, view_dy};
            bbox = get_output_box_from_box(bbox, output->handle->scale);

            pixman_region32_init_rect(&ds->damage,
                                      bbox.x, bbox.y, bbox.width, bbox.height);

            pixman_region32_intersect(&ds->damage, &ds->damage, &ws_damage);
            if (pixman_region32_not_empty(&ds->damage))
            {
                ds->x = obox.x + view_dx;
                ds->y = obox.y + view_dy;
                ds->surface = view.get();

                to_render.push_back(std::move(ds));
            }
        };

    const auto schedule_render_surface =
        [&] (wayfire_surface_t *surface, int x, int y, int view_dx, int view_dy)
        {
            if (!surface->is_mapped())
                return;

            if (!wlr_surface_has_buffer(surface->surface)
                || !pixman_region32_not_empty(&ws_damage))
                return;

            /* make sure all coordinates are in workspace-local coords */
            x += view_dx;
            y += view_dy;

            auto ds = damaged_surface(new damaged_surface_t);

            auto obox = surface->get_output_geometry();
            obox.x = x;
            obox.y = y;

            obox = get_output_box_from_box(obox, output->handle->scale);

            pixman_region32_init_rect(&ds->damage,
                                      obox.x, obox.y,
                                      obox.width, obox.height);

            pixman_region32_intersect(&ds->damage, &ds->damage, &ws_damage);
            if (pixman_region32_not_empty(&ds->damage))
            {
                ds->x = x;
                ds->y = y;
                ds->surface = surface;

                if (ds->surface->alpha >= 0.999f)
                {
                    pixman_region32_t opaque;
                    pixman_region32_init(&opaque);
                    pixman_region32_copy(&opaque, &surface->surface->current->opaque);
                    pixman_region32_translate(&opaque, x, y);
                    wlr_region_scale(&opaque, &opaque, output->handle->scale);
                    //pixman_region32_subtract(&ws_damage, &ws_damage, &opaque);
                    pixman_region32_fini(&opaque);
                }

                to_render.push_back(std::move(ds));
            }
        };

    /* we "move" all icons to the current output */
    if (!renderer)
    {
        for (auto& icon : core->input->drag_icons)
        {
            icon->set_output(output);
            icon->for_each_surface([&] (wayfire_surface_t *surface, int x, int y)
                                   {
                                       schedule_render_surface(surface, x, y, 0, 0);
                                   });
        }
    }

    auto it = views.begin();
    while (it != views.end() && pixman_region32_not_empty(&ws_damage))
    {
        auto view = *it;
        int view_dx = 0, view_dy = 0;

        if (!view->is_visible())
            goto next;

        if (!view->is_special)
        {
            view_dx = -dx;
            view_dy = -dy;
        }

        /* We use the snapshot of a view if either condition is happening:
         * 1. The view has a transform
         * 2. The view is visible, but not mapped
         *    => it is snapshotted and kept alive by some plugin */

        /* Snapshotted views include all of their subsurfaces, so we handle them separately */
        if (view->get_transformer() || !view->is_mapped())
        {
            schedule_render_snapshotted_view(view, view_dx, view_dy);
            goto next;
        }

        /* Iterate over all subsurfaces/menus of a "regular" view */
        view->for_each_surface([&] (wayfire_surface_t *surface, int x, int y)
        { schedule_render_surface(surface, x, y, view_dx, view_dy); });

        next: ++it;
    };

    /*
     TODO; implement scale != 1
    glm::mat4 scale = glm::scale(glm::mat4(), glm::vec3(scale_x, scale_y, 1));
    glm::mat4 translate = glm::translate(glm::mat4(), glm::vec3(scale_x - 1, scale_y - 1, 0));
    std::swap(wayfire_view_transform::global_scale, scale);
    std::swap(wayfire_view_transform::global_translate, translate);
    */


    auto rev_it = to_render.rbegin();
    while(rev_it != to_render.rend())
    {
        auto ds = std::move(*rev_it);
        ds->surface->render_fb(ds->x, ds->y, &ds->damage, stream->fbuff);

        ++rev_it;
    }

   // std::swap(wayfire_view_transform::global_scale, scale);
   // std::swap(wayfire_view_transform::global_translate, translate);

    GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, 0));
    pixman_region32_fini(&ws_damage);

    if (!renderer)
    {
        for (auto& icon : core->input->drag_icons)
            icon->set_output(nullptr);
    }
}

void render_manager::workspace_stream_stop(wf_workspace_stream *stream)
{
    stream->running = false;
}

/* End render_manager */


