#include "output.hpp"
#include "core.hpp"
#include "debug.hpp"
#include "workspace-manager.hpp"
#include "wayfire-shell.hpp"
#include "wayfire-shell-protocol.h"

struct wayfire_shell_output
{
    std::vector<wl_resource*> resources;
};

struct wayfire_shell_client
{
    wl_client *client;
    wl_resource *shell_resource;

    std::map<wayfire_output*, wayfire_shell_output> output_resources;
};

static struct wayfire_shell
{
    std::map<wl_client*, wayfire_shell_client> clients;
} shell;


static void zwf_wm_surface_configure(struct wl_client *client,
                                     struct wl_resource *resource,
                                     int32_t x, int32_t y)
{
    auto view = (wayfire_view_t*) wl_resource_get_user_data(resource);
    view->move(x, y);

    log_info("configure surface");
}

static void zwf_wm_surface_set_exclusive_zone(struct wl_client *client,
                                              struct wl_resource *resource,
                                              uint32_t anchor_edge, uint32_t size)
{
    log_info("set exclusive zone");
}

static void zwf_wm_surface_request_focus(struct wl_client *client,
                                         struct wl_resource *resource)
{
    log_info("request focus");
    /*
    if (wo == view->get_output())
        wo->focus_view(view);
        */

}

static void zwf_wm_surface_return_focus(struct wl_client *client,
                                         struct wl_resource *resource)
{
    /*
    if (wo == core->get_active_output())
        wo->focus_view(wo->get_top_view());
        */

    log_info("return focus");
}

const struct zwf_wm_surface_v1_interface zwf_wm_surface_v1_implementation = {
    zwf_wm_surface_configure,
    zwf_wm_surface_set_exclusive_zone,
    zwf_wm_surface_request_focus,
    zwf_wm_surface_return_focus
};

static void zwf_output_get_wm_surface(struct wl_client *client,
                                      struct wl_resource *resource,
                                      struct wl_resource *surface,
                                      uint32_t role, uint32_t id)
{
    log_info("requested get_wm_surface");
    auto wo = (wayfire_output*)wl_resource_get_user_data(resource);
    auto view = wl_surface_to_wayfire_view(surface);

    if (!view)
    {
        log_error ("wayfire_shell: get_wm_surface() for invalid surface!");
        return;
    }

    auto wfo = wl_resource_create(client, &zwf_wm_surface_v1_interface, 1, id);
    wl_resource_set_implementation(wfo, &zwf_wm_surface_v1_implementation, view.get(), NULL);

    view->role = WF_VIEW_ROLE_SHELL_VIEW;
    view->get_output()->detach_view(view);
    view->set_output(wo);

    uint32_t layer = 0;
    switch(role)
    {
        case ZWF_OUTPUT_V1_WM_ROLE_BACKGROUND:
            layer = WF_LAYER_BACKGROUND;
            break;
        case ZWF_OUTPUT_V1_WM_ROLE_BOTTOM:
            layer = WF_LAYER_BOTTOM;
            break;
        case ZWF_OUTPUT_V1_WM_ROLE_PANEL:
            layer = WF_LAYER_TOP;
            break;
        case ZWF_OUTPUT_V1_WM_ROLE_OVERLAY:
            layer = WF_LAYER_LOCK;
            break;

        default:
            log_error ("Invalid role for shell view");
    }

    wo->workspace->add_view_to_layer(view, layer);
}

static void zwf_output_inhibit_output(struct wl_client *client,
                                      struct wl_resource *resource)
{
    /* TODO: implement */
}

static void zwf_output_inhibit_output_done(struct wl_client *client,
                                           struct wl_resource *resource)
{
    /* TODO: implement */
}

const struct zwf_output_v1_interface zwf_output_v1_implementation =
{
    zwf_output_get_wm_surface,
    zwf_output_inhibit_output,
    zwf_output_inhibit_output_done,
};

static void destroy_zwf_output(wl_resource *resource)
{
    /* TODO: destroy */
}

void zwf_shell_manager_get_wf_output(struct wl_client *client,
                                     struct wl_resource *resource,
                                     struct wl_resource *output,
                                     uint32_t id)
{
    log_info("request get wf output");
    auto wlr_out = (wlr_output*) wl_resource_get_user_data(output);
    auto wo = core->get_output(wlr_out);

    log_info("found output %s", wo->handle->name);

    auto wfo = wl_resource_create(client, &zwf_output_v1_interface, 1, id);
    wl_resource_set_implementation(wfo, &zwf_output_v1_implementation, wo, destroy_zwf_output);
}

const struct zwf_shell_manager_v1_interface zwf_shell_manager_v1_implementation =
{
    zwf_shell_manager_get_wf_output
};

static void destroy_zwf_shell_manager(wl_resource *resource)
{
    //auto client = wl_resource_get_client(resource);
    /* TODO: destroy from list */
}

void bind_zwf_shell_manager(wl_client *client, void *data, uint32_t version, uint32_t id)
{
    log_info("bind zwf shell manager");
    auto resource = wl_resource_create(client, &zwf_shell_manager_v1_interface, 1, id);
    wl_resource_set_implementation(resource, &zwf_shell_manager_v1_implementation, NULL, destroy_zwf_shell_manager);
}

wayfire_shell* wayfire_shell_create(wl_display *display)
{
    if (wl_global_create(display, &zwf_shell_manager_v1_interface,
                         1, NULL, bind_zwf_shell_manager) == NULL)
    {
        log_error("Failed to create wayfire_shell interface");
    }

    return &shell;
}


struct wf_shell_reserved_custom_data : public wf_custom_view_data
{
    workspace_manager::anchored_area area;
    static const std::string cname;
};

const std::string wf_shell_reserved_custom_data::cname = "wf-shell-reserved-area";

bool view_has_anchored_area(wayfire_view view)
{
    return view->custom_data.count(wf_shell_reserved_custom_data::cname);
}

static workspace_manager::anchored_area *get_anchored_area_for_view(wayfire_view view)
{
    wf_shell_reserved_custom_data *cdata = NULL;
    const auto cname = wf_shell_reserved_custom_data::cname;

    if (view->custom_data.count(cname))
    {
        cdata = dynamic_cast<wf_shell_reserved_custom_data*> (view->custom_data[cname]);
    } else
    {
        cdata = new wf_shell_reserved_custom_data;
        cdata->area.reserved_size = -1;
        cdata->area.real_size = 0;
        view->custom_data[cname] = cdata;
    }

    return &cdata->area;
}

static void shell_reserve(struct wl_client *client, struct wl_resource *resource,
        struct wl_resource *surface, uint32_t side, uint32_t size)
{
    auto view = wl_surface_to_wayfire_view(surface);

    if (!view || !view->get_output()) {
        log_error("shell_reserve called with invalid output/surface");
        return;
    }

    auto area = get_anchored_area_for_view(view);

    bool is_first_update = (area->reserved_size == -1);

    area->reserved_size = size;
    area->edge = (workspace_manager::anchored_edge)side;

    if (is_first_update)
        view->get_output()->workspace->add_reserved_area(area);
    else
        view->get_output()->workspace->reflow_reserved_areas();
}

void wayfire_shell_unmap_view(wayfire_view view)
{
    if (view_has_anchored_area(view))
    {
        auto area = get_anchored_area_for_view(view);
        view->get_output()->workspace->remove_reserved_area(area);
    }
}
