#include "output.hpp"
#include "core.hpp"
#include "debug.hpp"
#include "workspace-manager.hpp"
#include "wayfire-shell.hpp"

wayfire_output* wl_output_to_wayfire_output(uint32_t output)
{
    if (output == (uint32_t) -1)
        return core->get_active_output();

    wayfire_output *result = nullptr;
    core->for_each_output([output, &result] (wayfire_output *wo) {
        if (wo->id == (int)output)
            result = wo;
    });

    return result;
}


static void shell_add_background(struct wl_client *client, struct wl_resource *resource,
        uint32_t output, struct wl_resource *surface, int32_t x, int32_t y)
{
    auto view = wl_surface_to_wayfire_view(surface);
    auto wo = wl_output_to_wayfire_output(output);
    if (!wo) wo = view ? view->get_output() : nullptr;

    if (!wo || !view)
    {
        log_error("shell_add_background called with invalid surface or output %p %p", wo, view.get());
        return;
    }

    log_debug("wf_shell: add_background to %s", wo->handle->name);

    view->is_special = 1;
    view->get_output()->detach_view(view);
    view->set_output(wo);
    view->move(x, y);

    wo->workspace->add_view_to_layer(view, WF_LAYER_BACKGROUND);
}

static void shell_add_panel(struct wl_client *client, struct wl_resource *resource,
        uint32_t output, struct wl_resource *surface)
{
    auto view = wl_surface_to_wayfire_view(surface);
    auto wo = wl_output_to_wayfire_output(output);
    if (!wo) wo = view ? view->get_output() : nullptr;


    if (!wo || !view) {
        log_error("shell_add_panel called with invalid surface or output");
        return;
    }

    log_debug("wf_shell: add_panel");

    view->is_special = 1;
    view->get_output()->detach_view(view);
    view->set_output(wo);

    wo->workspace->add_view_to_layer(view, WF_LAYER_TOP);
}

static void shell_configure_panel(struct wl_client *client, struct wl_resource *resource,
        uint32_t output, struct wl_resource *surface, int32_t x, int32_t y)
{
    auto view = wl_surface_to_wayfire_view(surface);
    auto wo = wl_output_to_wayfire_output(output);
    if (!wo) wo = view ? view->get_output() : nullptr;

    if (!wo || !view) {
        log_error("shell_configure_panel called with invalid surface or output");
        return;
    }

    view->move(x, y);
}

static void shell_focus_panel(struct wl_client *client, struct wl_resource *resource,
        uint32_t output, struct wl_resource *surface)
{
    auto view = wl_surface_to_wayfire_view(surface);
    auto wo = wl_output_to_wayfire_output(output);
    if (!wo) wo = view ? view->get_output() : nullptr;

    if (!wo || !view) {
        log_error("shell_focus_panel called with invalid surface or output");
        return;
    }

    if (wo == view->get_output())
        wo->focus_view(view);
}

static void shell_return_focus(struct wl_client *client, struct wl_resource *resource,
        uint32_t output)
{
    auto wo = wl_output_to_wayfire_output(output);

    if (!wo) {
        log_error("shell_return_focus called with invalid surface or output");
        return;
    }

    if (wo == core->get_active_output())
        wo->focus_view(wo->get_top_view());
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

static void shell_set_color_gamma(wl_client *client, wl_resource *res,
        uint32_t output, wl_array *r, wl_array *g, wl_array *b)
{
    /*
    auto wo = wl_output_to_wayfire_output(output);
    if (!wo || !wo->handle->set_gamma) {
        errio << "shell_set_gamma called with invalid/unsupported output" << std::endl;
        return;
    }

    size_t size = wo->handle->gamma_size * sizeof(uint16_t);
    if (r->size != size || b->size != size || g->size != size) {
        errio << "gamma size is not equal to output's gamma size " << r->size << " " << size << std::endl;
        return;
    }

    size /= sizeof(uint16_t);
#ifndef ushort
#define ushort unsigned short
    wo->handle->set_gamma(wo->handle, size, (ushort*)r->data, (ushort*)g->data, (ushort*)b->data);
#undef ushort
#endif */
}

static void shell_output_fade_in_start(wl_client *client, wl_resource *res, uint32_t output)
{
    auto wo = wl_output_to_wayfire_output(output);
    if (!wo)
    {
        log_error("output_fade_in_start called for wrong output!");
        return;
    }

    wo->emit_signal("output-fade-in-request", nullptr);
}

const struct wayfire_shell_interface shell_interface_impl = {
    .add_background = shell_add_background,
    .add_panel = shell_add_panel,
    .configure_panel = shell_configure_panel,
    .focus_panel = shell_focus_panel,
    .return_focus = shell_return_focus,
    .reserve = shell_reserve,
    .set_color_gamma = shell_set_color_gamma,
    .output_fade_in_start = shell_output_fade_in_start
};

void wayfire_shell_unmap_view(wayfire_view view)
{
    if (view_has_anchored_area(view))
    {
        auto area = get_anchored_area_for_view(view);
        view->get_output()->workspace->remove_reserved_area(area);
    }
}
