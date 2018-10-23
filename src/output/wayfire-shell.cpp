#include <algorithm>
#include "nonstd/make_unique.hpp"
#include "output.hpp"
#include "core.hpp"
#include "debug.hpp"
#include "workspace-manager.hpp"
#include "render-manager.hpp"
#include "wayfire-shell.hpp"
#include "wayfire-shell-protocol.h"

struct wayfire_shell_output
{
    int inhibits = 0;
    std::vector<wl_resource*> resources;
};

struct wayfire_shell_client
{
    std::map<wayfire_output*, wayfire_shell_output> output_resources;
};

static struct wayfire_shell
{
    std::map<wl_client*, wayfire_shell_client> clients;
    std::map<wayfire_output*, signal_callback_t> output_autohide_callback;
} shell;


static void zwf_wm_surface_configure(struct wl_client *client,
                                     struct wl_resource *resource,
                                     int32_t x, int32_t y)
{
    auto view = (wayfire_view_t*) wl_resource_get_user_data(resource);
    view->move(x, y);
}

struct wf_shell_reserved_custom_data : public wf_custom_data_t
{
    workspace_manager::anchored_area area;

    wf_shell_reserved_custom_data() {
        area.reserved_size = -1;
        area.real_size = 0;
    }
};

static workspace_manager::anchored_area *
get_anchored_area_for_view(wayfire_view view)
{
    return &view->get_data_safe<wf_shell_reserved_custom_data>()->area;
}

static void zwf_wm_surface_set_exclusive_zone(struct wl_client *client,
                                              struct wl_resource *resource,
                                              uint32_t anchor_edge, uint32_t size)
{
    auto view = ((wayfire_view_t*) wl_resource_get_user_data(resource))->self();
    auto area = get_anchored_area_for_view(view);

    bool is_first_update = (area->reserved_size == -1);

    area->reserved_size = size;
    area->edge = (workspace_manager::anchored_edge)anchor_edge;

    if (is_first_update)
        view->get_output()->workspace->add_reserved_area(area);
    else
        view->get_output()->workspace->reflow_reserved_areas();
}

static void zwf_wm_surface_request_focus(struct wl_client *client,
                                         struct wl_resource *resource)
{
    auto view = (wayfire_view_t*) wl_resource_get_user_data(resource);
    view->get_output()->focus_view(view->self());
}

static void zwf_wm_surface_return_focus(struct wl_client *client,
                                        struct wl_resource *resource)
{
    auto view = (wayfire_view_t*) wl_resource_get_user_data(resource);
    auto wo = view->get_output();

    if (wo == core->get_active_output())
        wo->focus_view(wo->get_top_view());
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
    view->activate(true);
}

static void zwf_output_inhibit_output(struct wl_client *client,
                                      struct wl_resource *resource)
{
    auto wo = (wayfire_output*)wl_resource_get_user_data(resource);
    wo->render->add_inhibit(true);

    auto& cl = shell.clients[client];
    auto& out = cl.output_resources[wo];
    ++out.inhibits;
}

static void zwf_output_inhibit_output_done(struct wl_client *client,
                                           struct wl_resource *resource)
{
    auto wo = (wayfire_output*)wl_resource_get_user_data(resource);
    wo->render->add_inhibit(false);

    auto& cl = shell.clients[client];
    auto& out = cl.output_resources[wo];
    --out.inhibits;
}

const struct zwf_output_v1_interface zwf_output_v1_implementation =
{
    zwf_output_get_wm_surface,
    zwf_output_inhibit_output,
    zwf_output_inhibit_output_done,
};

static void destroy_zwf_output(wl_resource *resource)
{
    auto client = wl_resource_get_client(resource);
    auto wo = (wayfire_output*) wl_resource_get_user_data(resource);
    if (shell.clients.count(client) == 0)
        return;

    auto& shell_client = shell.clients[client];
    if (shell_client.output_resources.count(wo) == 0)
        return;

    auto& client_output = shell_client.output_resources[wo];
    auto it = std::find(client_output.resources.begin(), client_output.resources.end(),
                        resource);

    while(client_output.inhibits--)
        wo->render->add_inhibit(false);

    client_output.resources.erase(it);
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

    auto& shell_client = shell.clients[client];
    auto& client_output = shell_client.output_resources[wo];

    client_output.resources.push_back(wfo);
}

const struct zwf_shell_manager_v1_interface zwf_shell_manager_v1_implementation =
{
    zwf_shell_manager_get_wf_output
};

static void destroy_zwf_shell_manager(wl_resource *resource)
{
    auto client = wl_resource_get_client(resource);

    for (auto& out : shell.clients[client].output_resources)
    {
        while(out.second.inhibits--)
            out.first->render->add_inhibit(false);
    }

    shell.clients.erase(client);
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

void zwf_output_send_autohide(wayfire_shell *shell, wayfire_output *output, int value)
{
    for (auto& client : shell->clients)
    {
        if (client.second.output_resources.count(output))
        {
            for (auto resource : client.second.output_resources[output].resources)
                zwf_output_v1_send_output_hide_panels(resource, value);
        }
    }
}

/* TODO: make core, output and views each have signal abilities,
 * then these should come as events from core */
void wayfire_shell_handle_output_created(wayfire_output *output)
{
    shell.output_autohide_callback[output] = [=] (signal_data *flag)
    {
        zwf_output_send_autohide(&shell, output, bool(flag));
    };

    /* FIXME: traditionally std::map won't move memory around ... */
    output->connect_signal("autohide-panels", &shell.output_autohide_callback[output]);
}

void wayfire_shell_handle_output_destroyed(wayfire_output *output)
{
}

void wayfire_shell_unmap_view(wayfire_view view)
{
    if (view->has_data<wf_shell_reserved_custom_data>())
    {
        auto area = get_anchored_area_for_view(view);
        view->get_output()->workspace->remove_reserved_area(area);
    }
}
