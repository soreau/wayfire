#include <algorithm>
#include "debug.hpp"
#include "wf-output-config.hpp"
#include "core.hpp"
#include <assert.h>
#include <map>

static void set_layout(struct wl_client *client, struct wl_resource *resource,
                       int32_t x, int32_t y)
{
    auto wo = (wayfire_output*) wl_resource_get_user_data(resource);
    wlr_output_layout_move(core->output_layout, wo->handle, x, y);
}

static void set_mode(struct wl_client *client, struct wl_resource *resource,
                     int32_t width, int32_t height, int32_t refresh)
{
    auto wo = (wayfire_output*) wl_resource_get_user_data(resource);
    wo->set_mode(width, height, refresh);
}

static void set_transform(struct wl_client *client, struct wl_resource *resource,
                          uint32_t transform)
{
    auto wo = (wayfire_output*) wl_resource_get_user_data(resource);
    wo->set_transform((wl_output_transform)transform);
}

static void set_scale(struct wl_client *client, struct wl_resource *resource,
                      wl_fixed_t scale)
{
    auto wo = (wayfire_output*) wl_resource_get_user_data(resource);
    wo->set_scale(wl_fixed_to_double(scale));
}

static const struct zwf_output_v1_interface zwf_output_v1_impl =
{
    set_layout,
    set_mode,
    set_transform,
    set_scale,
};

void get_wf_output(struct wl_client *client, struct wl_resource *resource,
                   uint32_t id, struct wl_resource *output)
{
    auto wlr_out = (wlr_output*) wl_resource_get_user_data(output);
    auto wo = core->get_output(wlr_out);

    auto wf_output = wl_resource_create(client, &zwf_output_v1_interface, 1, id);
    wl_resource_set_implementation(wf_output, &zwf_output_v1_impl, wo, NULL);
}

static const struct zwf_output_manager_v1_interface zwf_output_manager_v1_impl =
{
    get_wf_output
};

void bind_wf_output_manager(wl_client *client, void *data, uint32_t version,
                            uint32_t id)
{
    log_info("bind wf output manager");
    auto resource = wl_resource_create(client, &zwf_output_manager_v1_interface,
                                       1, id);
    wl_resource_set_implementation(resource, &zwf_output_manager_v1_impl, data, NULL);
}

void wf_output_manager_create()
{
    wl_global_create(core->display, &zwf_output_manager_v1_interface,
                     1, NULL, bind_wf_output_manager);
    log_info("create us");
}
