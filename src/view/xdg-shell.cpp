#include "debug.hpp"
#include "core.hpp"
#include "output.hpp"
#include "decorator.hpp"
#include "xdg-shell.hpp"

static void handle_xdg_map(wl_listener*, void *data);
static void handle_xdg_unmap(wl_listener*, void *data);
static void handle_xdg_popup_destroy(wl_listener*, void *data);
static void handle_xdg_destroy(wl_listener*, void *data);

/* TODO: Figure out a way to animate this */
wayfire_xdg_popup::wayfire_xdg_popup(wlr_xdg_popup *popup)
    :wayfire_surface_t(wf_surface_from_void(popup->parent->data))
{
    assert(parent_surface);
    this->popup = popup;

    destroy.notify       = handle_xdg_popup_destroy;
    new_popup.notify     = handle_xdg_new_popup;
    m_popup_map.notify   = handle_xdg_map;
    m_popup_unmap.notify = handle_xdg_unmap;

    wl_signal_add(&popup->base->events.new_popup, &new_popup);
    wl_signal_add(&popup->base->events.map,       &m_popup_map);
    wl_signal_add(&popup->base->events.unmap,     &m_popup_unmap);
    wl_signal_add(&popup->base->events.destroy,   &destroy);

    popup->base->data = this;

    unconstrain();
}

void wayfire_xdg_popup::unconstrain()
{
    auto view = dynamic_cast<wayfire_view_t*> (get_main_surface());
    if (!output || !view)
        return;

    auto box = output->get_relative_geometry();
    auto wm = view->get_output_geometry();
    box.x -= wm.x;
    box.y -= wm.y;

    wlr_xdg_popup_unconstrain_from_box(popup, &box);
}

wayfire_xdg_popup::~wayfire_xdg_popup()
{
    wl_list_remove(&new_popup.link);
    wl_list_remove(&m_popup_map.link);
    wl_list_remove(&m_popup_unmap.link);
    wl_list_remove(&destroy.link);
}

void wayfire_xdg_popup::get_child_position(int &x, int &y)
{
    auto parent = wf_surface_from_void(popup->parent->data);
    assert(parent);

    parent->get_child_offset(x, y);
    x += popup->geometry.x - popup->base->geometry.x;
    y += popup->geometry.y - popup->base->geometry.y;
}

void wayfire_xdg_popup::get_child_offset(int &x, int &y)
{
    x = popup->base->geometry.x;
    y = popup->base->geometry.y;
}

void handle_xdg_new_popup(wl_listener*, void *data)
{
    auto popup = static_cast<wlr_xdg_popup*> (data);
    auto parent = wf_surface_from_void(popup->parent->data);
    if (!parent)
    {
        log_error("attempting to create a popup with unknown parent");
        return;
    }

    new wayfire_xdg_popup(popup);
}

static void handle_xdg_map(wl_listener*, void *data)
{
    auto surface = static_cast<wlr_xdg_surface*> (data);
    auto wf_surface = wf_surface_from_void(surface->data);

    assert(wf_surface);
    wf_surface->map(surface->surface);
}

static void handle_xdg_unmap(wl_listener*, void *data)
{
    auto surface = static_cast<wlr_xdg_surface*> (data);
    auto wf_surface = wf_surface_from_void(surface->data);

    assert(wf_surface);
    wf_surface->unmap();
}

static void handle_xdg_destroy(wl_listener*, void *data)
{
    auto surface = static_cast<wlr_xdg_surface*> (data);
    auto view = wf_view_from_void(surface->data);

    assert(view);

    view->destroy();
}

static void handle_xdg_popup_destroy(wl_listener*, void *data)
{
    auto surface = static_cast<wlr_xdg_surface*> (data);
    auto wf_surface = wf_surface_from_void(surface->data);

    wf_surface->destroyed = 1;
    wf_surface->dec_keep_count();
}

static void handle_xdg_request_move(wl_listener*, void *data)
{
    auto ev = static_cast<wlr_xdg_toplevel_move_event*> (data);
    auto view = wf_view_from_void(ev->surface->data);
    view->move_request();
}

static void handle_xdg_request_resize(wl_listener*, void *data)
{
    auto ev = static_cast<wlr_xdg_toplevel_resize_event*> (data);
    auto view = wf_view_from_void(ev->surface->data);

    view->resize_request(ev->edges);
}

static void handle_xdg_request_maximized(wl_listener*, void *data)
{
    auto surf = static_cast<wlr_xdg_surface*> (data);
    auto view = wf_view_from_void(surf->data);
    view->maximize_request(surf->toplevel->client_pending.maximized);
}

static void handle_xdg_request_minimized(wl_listener*, void *data)
{
    auto surf = static_cast<wlr_xdg_surface*> (data);
    auto view = wf_view_from_void(surf->data);
    view->minimize_request(true);
}

static void handle_xdg_request_fullscreen(wl_listener*, void *data)
{
    auto ev = static_cast<wlr_xdg_toplevel_set_fullscreen_event*> (data);
    auto view = wf_view_from_void(ev->surface->data);
    auto wo = core->get_output(ev->output);
    view->fullscreen_request(wo, ev->fullscreen);
}

static void handle_xdg_set_parent(wl_listener* listener, void *data)
{
    auto surface = static_cast<wlr_xdg_surface*> (data);
    auto view = wf_view_from_void(surface->data);
    auto parent = surface->toplevel->parent ?
        wf_view_from_void(surface->toplevel->parent->data)->self() : nullptr;

    assert(view);
    view->set_toplevel_parent(parent);
}

static void handle_xdg_set_title(wl_listener *listener, void *data)
{
    auto surface = static_cast<wlr_xdg_surface*> (data);
    auto view = wf_view_from_void(surface->data);
    view->handle_title_changed();
}

static void handle_xdg_set_app_id(wl_listener *listener, void *data)
{
    auto surface = static_cast<wlr_xdg_surface*> (data);
    auto view = wf_view_from_void(surface->data);
    view->handle_app_id_changed();
}

wayfire_xdg_view::wayfire_xdg_view(wlr_xdg_surface *s)
    : wayfire_view_t(), xdg_surface(s)
{
    log_info ("new xdg_shell_stable surface: %s app-id: %s",
              nonull(xdg_surface->toplevel->title),
              nonull(xdg_surface->toplevel->app_id));

    destroy_ev.notify         = handle_xdg_destroy;
    new_popup.notify          = handle_xdg_new_popup;
    map_ev.notify             = handle_xdg_map;
    unmap_ev.notify           = handle_xdg_unmap;
    set_title.notify          = handle_xdg_set_title;
    set_app_id.notify         = handle_xdg_set_app_id;
    set_parent_ev.notify      = handle_xdg_set_parent;
    request_move.notify       = handle_xdg_request_move;
    request_resize.notify     = handle_xdg_request_resize;
    request_maximize.notify   = handle_xdg_request_maximized;
    request_minimize.notify   = handle_xdg_request_minimized;
    request_fullscreen.notify = handle_xdg_request_fullscreen;

    wlr_xdg_surface_ping(s);

    wl_signal_add(&xdg_surface->events.destroy, &destroy_ev);
    wl_signal_add(&s->events.new_popup,         &new_popup);
    wl_signal_add(&xdg_surface->events.map,     &map_ev);
    wl_signal_add(&xdg_surface->events.unmap,   &unmap_ev);
    wl_signal_add(&xdg_surface->toplevel->events.set_title,          &set_title);
    wl_signal_add(&xdg_surface->toplevel->events.set_app_id,         &set_app_id);
    wl_signal_add(&xdg_surface->toplevel->events.set_parent,         &set_parent_ev);
    wl_signal_add(&xdg_surface->toplevel->events.request_move,       &request_move);
    wl_signal_add(&xdg_surface->toplevel->events.request_resize,     &request_resize);
    wl_signal_add(&xdg_surface->toplevel->events.request_maximize,   &request_maximize);
    wl_signal_add(&xdg_surface->toplevel->events.request_minimize,   &request_minimize);
    wl_signal_add(&xdg_surface->toplevel->events.request_fullscreen, &request_fullscreen);

    xdg_surface->data = this;
}

static wf_geometry get_xdg_geometry(wlr_xdg_surface *surface)
{
    wlr_box xdg_geometry;
    wlr_xdg_surface_get_geometry(surface, &xdg_geometry);
    return xdg_geometry;
}

void wayfire_xdg_view::on_xdg_geometry_updated()
{
    auto wm = get_wm_geometry();
    auto xdg_geometry = get_xdg_geometry(xdg_surface);
    xdg_surface_offset = {xdg_geometry.x, xdg_geometry.y};
    move(wm.x, wm.y, false);
}

void wayfire_xdg_view::commit()
{
    wayfire_view_t::commit();

    auto xdg_geometry = get_xdg_geometry(xdg_surface);
    if (xdg_geometry.x != xdg_surface_offset.x ||
        xdg_geometry.y != xdg_surface_offset.y)
    {
        on_xdg_geometry_updated();
    }
}

void wayfire_xdg_view::map(wlr_surface *surface)
{
    auto xdg_geometry = get_xdg_geometry(xdg_surface);
    xdg_surface_offset = {xdg_geometry.x, xdg_geometry.y};

    if (xdg_surface->toplevel->client_pending.maximized)
        maximize_request(true);

    if (xdg_surface->toplevel->client_pending.fullscreen)
        fullscreen_request(output, true);

    if (xdg_surface->toplevel->parent)
    {
        auto parent = wf_view_from_void(xdg_surface->toplevel->parent->data)->self();
        set_toplevel_parent(parent);
    }

    wayfire_view_t::map(surface);
    create_toplevel();
}

void wayfire_xdg_view::get_child_offset(int &x, int &y)
{
    x = xdg_surface_offset.x;
    y = xdg_surface_offset.y;
}

wf_geometry wayfire_xdg_view::get_wm_geometry()
{
    if (!xdg_surface)
        return get_untransformed_bounding_box();

    auto opos = get_output_position();
    auto xdg_geometry = get_xdg_geometry(xdg_surface);
    wf_geometry wm = {
        opos.x + xdg_surface_offset.x,
        opos.y + xdg_surface_offset.y,
        xdg_geometry.width,
        xdg_geometry.height
    };

    if (frame)
        wm = frame->expand_wm_geometry(wm);

    return wm;
}

void wayfire_xdg_view::activate(bool act)
{
    wlr_xdg_toplevel_set_activated(xdg_surface, act);
    wayfire_view_t::activate(act);
}

void wayfire_xdg_view::set_maximized(bool max)
{
    wayfire_view_t::set_maximized(max);
    wlr_xdg_toplevel_set_maximized(xdg_surface, max);
}

void wayfire_xdg_view::set_fullscreen(bool full)
{
    wayfire_view_t::set_fullscreen(full);
    wlr_xdg_toplevel_set_fullscreen(xdg_surface, full);
}

void wayfire_xdg_view::move(int w, int h, bool send)
{
    wayfire_view_t::move(w, h, send);
}

void wayfire_xdg_view::resize(int w, int h, bool send)
{
    damage();

    if (frame)
        frame->calculate_resize_size(w, h);

    wlr_xdg_toplevel_set_size(xdg_surface, w, h);
}

std::string wayfire_xdg_view::get_app_id()
{
    return nonull(xdg_surface->toplevel->app_id);
}

std::string wayfire_xdg_view::get_title()
{
    return nonull(xdg_surface->toplevel->title);
}

void wayfire_xdg_view::close()
{
    wlr_xdg_toplevel_send_close(xdg_surface);
    wayfire_view_t::close();
}

void wayfire_xdg_view::destroy()
{
    wl_list_remove(&destroy_ev.link);
    wl_list_remove(&new_popup.link);
    wl_list_remove(&map_ev.link);
    wl_list_remove(&unmap_ev.link);
    wl_list_remove(&request_move.link);
    wl_list_remove(&request_resize.link);
    wl_list_remove(&request_maximize.link);
    wl_list_remove(&request_minimize.link);
    wl_list_remove(&request_fullscreen.link);
    wl_list_remove(&set_parent_ev.link);
    wl_list_remove(&set_title.link);
    wl_list_remove(&set_app_id.link);

    xdg_surface = nullptr;
    wayfire_view_t::destroy();
}

wayfire_xdg_view::~wayfire_xdg_view() {}

static void notify_created(wl_listener*, void *data)
{
    auto surf = static_cast<wlr_xdg_surface*> (data);
    if (surf->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL)
        core->add_view(nonstd::make_unique<wayfire_xdg_view> (surf));
}

static wlr_xdg_shell *xdg_handle;
static wl_listener xdg_created;

void init_xdg_shell()
{
    xdg_created.notify = notify_created;
    xdg_handle = wlr_xdg_shell_create(core->display);
    log_info("create xdg shell is %p", xdg_handle);
    if (xdg_handle)
        wl_signal_add(&xdg_handle->events.new_surface, &xdg_created);
}
