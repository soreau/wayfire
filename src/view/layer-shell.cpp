#include <algorithm>
#include <cstring>

#include "priv-view.hpp"
#include "core.hpp"
#include "debug.hpp"
#include "output.hpp"
#include "workspace-manager.hpp"

extern "C"
{
#define namespace namespace_t
#include <wlr/types/wlr_layer_shell.h>
#undef namespace
}

static const uint32_t both_vert = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
static const uint32_t both_horiz = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;

class wayfire_layer_shell_view : public wayfire_view_t
{
    bool first_map = true;
    wl_listener map_ev, unmap_ev, destroy;
    public:
        wlr_layer_surface *lsurface;
        wlr_layer_surface_state prev_state;

        std::unique_ptr<workspace_manager::anchored_area> anchored_area;

        wayfire_layer_shell_view(wlr_layer_surface *lsurf);

        void map(wlr_surface *surface);
        void unmap();
        void commit();
        void close();

        void configure(wf_geometry geometry);
};

workspace_manager::anchored_edge anchor_to_edge(uint32_t edges)
{
    if (edges == ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP)
        return workspace_manager::WORKSPACE_ANCHORED_EDGE_TOP;
    if (edges == ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM)
        return workspace_manager::WORKSPACE_ANCHORED_EDGE_BOTTOM;
    if (edges == ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT)
        return workspace_manager::WORKSPACE_ANCHORED_EDGE_LEFT;
    if (edges == ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT)
        return workspace_manager::WORKSPACE_ANCHORED_EDGE_RIGHT;

    assert(false);
}

struct wf_layer_shell_manager
{
    using layer_t = std::vector<wayfire_layer_shell_view*>;
    layer_t layers[4];

    void handle_map(wayfire_layer_shell_view *view)
    {
        layers[view->lsurface->layer].push_back(view);

        arrange_layers(view->get_output());
    }

    void handle_unmap(wayfire_layer_shell_view *view)
    {
        auto& cont = layers[view->lsurface->layer];
        auto it = std::find(cont.begin(), cont.end(), view);
        cont.erase(it);

        arrange_layers(view->get_output());
    }

    layer_t filter_views(wayfire_output *output, int layer)
    {
        layer_t result;
        for (auto view : layers[layer])
        {
            if (view->get_output() == output)
                result.push_back(view);
        }

        return result;
    }

    layer_t filter_views(wayfire_output *output)
    {
        layer_t result;
        for (int i = 0; i < 4; i++)
        {
            auto layer_result = filter_views(output, i);
            result.insert(result.end(), layer_result.begin(), layer_result.end());
        }

        return result;
    }

    void set_exclusive_zone(wayfire_layer_shell_view *v)
    {
        int edges = v->lsurface->current.anchor;
        if (__builtin_popcount(edges) > 1)
        {
            log_error ("Unsupported: layer-shell exclusive zone for surfaces anchored to multiple edges");
            return;
        }

        v->anchored_area = std::unique_ptr<workspace_manager::anchored_area>
            (new workspace_manager::anchored_area);

        v->anchored_area->reflowed = [v] (wf_geometry geometry)
        { v->configure(geometry); };

        v->anchored_area->edge = anchor_to_edge(edges);
        v->anchored_area->size = v->lsurface->current.exclusive_zone;

        v->get_output()->workspace->add_reserved_area(v->anchored_area.get());
    }

    void pin_view(wayfire_layer_shell_view *v, wf_geometry usable_workarea)
    {
        auto state = &v->lsurface->current;
        auto bounds = v->lsurface->current.exclusive_zone < 0 ?
            v->get_output()->get_relative_geometry() : usable_workarea;

        wf_geometry box;
        box.x = box.y = 0;
        box.width = state->desired_width;
        box.height = state->desired_height;

		if ((state->anchor & both_horiz) && box.width == 0)
        {
			box.x = bounds.x;
			box.width = bounds.width;
		}
        else if ((state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT))
        {
			box.x = bounds.x;
		}
        else if ((state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT))
        {
			box.x = bounds.x + (bounds.width - box.width);
		} else
        {
			box.x = bounds.x + ((bounds.width / 2) - (box.width / 2));
		}

		if ((state->anchor & both_vert) && box.height == 0)
        {
			box.y = bounds.y;
			box.height = bounds.height;
		}
        else if ((state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP))
        {
			box.y = bounds.y;
		}
        else if ((state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM))
        {
			box.y = bounds.y + (bounds.height - box.height);
		} else
        {
			box.y = bounds.y + ((bounds.height / 2) - (box.height / 2));
		}

        v->configure(box);
    }

    void arrange_layer(wayfire_output *output, int layer)
    {
        auto views = filter_views(output, layer);

        /* set all views that have exclusive zones first */
        for (auto v : views)
        {
            if (v->lsurface->client_pending.exclusive_zone > 0)
                set_exclusive_zone(v);
        }

        auto usable_workarea = output->workspace->get_workarea();
        for (auto v : views)
        {
            /* anchored area is cleared in arrange_layers(), so if it's NULL,
             * then this view doesn't have exclusive zone */
            if (!v->anchored_area)
                pin_view(v, usable_workarea);
        }
    }

    void arrange_layers(wayfire_output *output)
    {
        auto views = filter_views(output);
        for (auto v : views)
        {
            if (v->anchored_area)
                output->workspace->remove_reserved_area(v->anchored_area.get());

            v->anchored_area = nullptr;
        }

        arrange_layer(output, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY);
        arrange_layer(output, ZWLR_LAYER_SHELL_V1_LAYER_TOP);
        arrange_layer(output, ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM);
        arrange_layer(output, ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND);
    }
};

static wf_layer_shell_manager layer_shell_manager;

static void handle_layer_surface_map(wl_listener*, void *data);
static void handle_layer_surface_unmap(wl_listener*, void *data);
static void handle_layer_surface_destroy(wl_listener*, void *data);

wayfire_layer_shell_view::wayfire_layer_shell_view(wlr_layer_surface *lsurf)
    : wayfire_view_t(), lsurface(lsurf)
{
    log_debug("Create a layer surface: namespace %s layer %d anchor %d,"
              "size %dx%d, margin top:%d, down:%d, left:%d, right:%d",
              lsurf->namespace_t, lsurf->layer, lsurf->client_pending.anchor,
              lsurf->client_pending.desired_width, lsurf->client_pending.desired_height,
              lsurf->client_pending.margin.top,
              lsurf->client_pending.margin.bottom,
              lsurf->client_pending.margin.left,
              lsurf->client_pending.margin.right);

    if (lsurf->output)
    {
        auto wo = core->get_output(lsurf->output);
        set_output(wo);
    }

    if (!output)
    {
        log_error ("Couldn't find output for the layer surface");
        close();
        return;
    }

    lsurf->output = output->handle;

    is_special = 1;
    lsurface->data = this;

    map_ev.notify   = handle_layer_surface_map;
    unmap_ev.notify = handle_layer_surface_unmap;
    destroy.notify  = handle_layer_surface_destroy;

    wl_signal_add(&lsurface->events.map,     &map_ev);
    wl_signal_add(&lsurface->events.unmap,   &unmap_ev);
    wl_signal_add(&lsurface->events.destroy, &destroy);

    /* easy reflowing */
    auto old_current = lsurface->current;
    lsurface->current = lsurface->client_pending;
    layer_shell_manager.handle_map(this);
    lsurface->current = old_current;
}

static uint32_t zwlr_layer_to_wf_layer(zwlr_layer_shell_v1_layer layer)
{
    switch (layer)
    {
        case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:
            return WF_LAYER_LOCK;
        case ZWLR_LAYER_SHELL_V1_LAYER_TOP:
            return WF_LAYER_TOP;
        case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
            return WF_LAYER_BOTTOM;
        case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
            return WF_LAYER_BACKGROUND;
    }
}

void wayfire_layer_shell_view::map(wlr_surface *surface)
{
    wayfire_view_t::map(surface);

    /* we already called handle_map in constructor to get proper size */
    /* TODO: maybe we can circumvent this? */
    if (!first_map)
        layer_shell_manager.handle_map(this);

    first_map = false;

    output->workspace->add_view_to_layer(self(),
                                         zwlr_layer_to_wf_layer(lsurface->layer));
}

void wayfire_layer_shell_view::unmap()
{
    wayfire_view_t::unmap();
    layer_shell_manager.handle_unmap(this);
}

void wayfire_layer_shell_view::commit()
{
    wayfire_view_t::commit();
    auto state = &lsurface->current;

    if (std::memcmp(state, &prev_state, sizeof(*state)))
        layer_shell_manager.arrange_layers(output);
}

void wayfire_layer_shell_view::close()
{
    wayfire_view_t::close();
    wlr_layer_surface_close(lsurface);
}

void wayfire_layer_shell_view::configure(wf_geometry box)
{
    auto state = &lsurface->current;
    if ((state->anchor & both_horiz) == both_horiz)
    {
        box.x += state->margin.left;
        box.width -= state->margin.left + state->margin.right;
    }
    else if ((state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT))
    {
        box.x += state->margin.left;
    }
    else if ((state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT))
    {
        box.x -= state->margin.right;
    }

    if ((state->anchor & both_vert) == both_vert)
    {
        box.y += state->margin.top;
        box.height -= state->margin.top + state->margin.bottom;
    }
    else if ((state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP))
    {
        box.y += state->margin.top;
    }
    else if ((state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM))
    {
        box.y -= state->margin.bottom;
    }

    if (box.width < 0 || box.height < 0) {
        log_error ("layer-surface has calculated width and height < 0");
        close();
    }

    wayfire_view_t::move(box.x, box.y, false);
    wayfire_view_t::resize(box.width, box.height, false);

    wlr_layer_surface_configure(lsurface, box.width, box.height);
}

static void handle_layer_surface_map(wl_listener*, void *data)
{
    auto lsurface = static_cast<wlr_layer_surface*> (data);
    auto view = wf_view_from_void(lsurface->data);

    view->map(lsurface->surface);
}

static void handle_layer_surface_unmap(wl_listener*, void *data)
{
    auto lsurface = static_cast<wlr_layer_surface*> (data);
    auto view = wf_view_from_void(lsurface->data);

    view->unmap();
}

static void handle_layer_surface_destroy(wl_listener*, void *data)
{
    auto lsurface = static_cast<wlr_layer_surface*> (data);
    auto view = wf_view_from_void(lsurface->data);

    view->destroyed = 1;
    view->dec_keep_count();
}

static void handle_layer_surface_created(wl_listener *listener, void *data)
{
    auto lsurf = static_cast<wlr_layer_surface*> (data);
    core->add_view(std::make_shared<wayfire_layer_shell_view>(lsurf));
}

static wlr_layer_shell *layer_shell_handle;
wl_listener layer_surface_created;

void init_layer_shell()
{
    layer_shell_handle = wlr_layer_shell_create(core->display);
    layer_surface_created.notify = handle_layer_surface_created;

    wl_signal_add(&layer_shell_handle->events.new_surface, &layer_surface_created);
}
