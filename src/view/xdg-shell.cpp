#include "debug.hpp"
#include "core.hpp"
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
    view->resize_request();
}

static void handle_xdg_request_maximized(wl_listener*, void *data)
{
    auto surf = static_cast<wlr_xdg_surface*> (data);
    auto view = wf_view_from_void(surf->data);
    view->maximize_request(surf->toplevel->client_pending.maximized);
}

static void handle_xdg_request_fullscreen(wl_listener*, void *data)
{
    auto ev = static_cast<wlr_xdg_toplevel_set_fullscreen_event*> (data);
    auto view = wf_view_from_void(ev->surface->data);
    auto wo = core->get_output(ev->output);
    view->fullscreen_request(wo, ev->fullscreen);
}

void handle_xdg_set_parent(wl_listener* listener, void *data)
{
    auto surface = static_cast<wlr_xdg_surface*> (data);
    auto view = wf_view_from_void(surface->data);
    auto parent = surface->toplevel->parent ?
        wf_view_from_void(surface->toplevel->parent->data)->self() : nullptr;

    assert(view);
    view->set_toplevel_parent(parent);
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
    set_parent_ev.notify      = handle_xdg_set_parent;
    request_move.notify       = handle_xdg_request_move;
    request_resize.notify     = handle_xdg_request_resize;
    request_maximize.notify   = handle_xdg_request_maximized;
    request_fullscreen.notify = handle_xdg_request_fullscreen;

    wlr_xdg_surface_ping(s);

    wl_signal_add(&xdg_surface->events.destroy, &destroy_ev);
    wl_signal_add(&s->events.new_popup,         &new_popup);
    wl_signal_add(&xdg_surface->events.map,     &map_ev);
    wl_signal_add(&xdg_surface->events.unmap,   &unmap_ev);
    wl_signal_add(&xdg_surface->toplevel->events.set_parent,         &set_parent_ev);
    wl_signal_add(&xdg_surface->toplevel->events.request_move,       &request_move);
    wl_signal_add(&xdg_surface->toplevel->events.request_resize,     &request_resize);
    wl_signal_add(&xdg_surface->toplevel->events.request_maximize,   &request_maximize);
    wl_signal_add(&xdg_surface->toplevel->events.request_fullscreen, &request_fullscreen);

    xdg_surface->data = this;
}

void wayfire_xdg_view::map(wlr_surface *surface)
{
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

}

wf_point wayfire_xdg_view::get_output_position()
{
    if (decoration)
        return decoration->get_output_position()
            + wf_point{decor_x, decor_y}
    + wf_point{-xdg_surface->geometry.x, -xdg_surface->geometry.y};

    wf_point position {
        geometry.x - xdg_surface->geometry.x,
            geometry.y - xdg_surface->geometry.y,
    };

    return position;
}

void wayfire_xdg_view::get_child_position(int &x, int &y)
{
    assert(decoration);

    x = decor_x - xdg_surface->geometry.x;
    y = decor_y - xdg_surface->geometry.y;
}

void wayfire_xdg_view::get_child_offset(int &x, int &y)
{
    x = xdg_surface->geometry.x;
    y = xdg_surface->geometry.y;
}

bool wayfire_xdg_view::update_size()
{
    auto old_w = geometry.width, old_h = geometry.height;

    int width = xdg_surface->geometry.width, height = xdg_surface->geometry.height;
    if (width > 0 && height > 0)
    {
        if (geometry.width != width || geometry.height != height)
        {
            adjust_anchored_edge(width, height);
            wayfire_view_t::resize(width, height, true);
        }
    } else
    {
        wayfire_view_t::update_size();
    }

    return old_w != geometry.width || old_h != geometry.height;
}

void wayfire_xdg_view::activate(bool act)
{
    wayfire_view_t::activate(act);
    wlr_xdg_toplevel_set_activated(xdg_surface, act);
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
    wlr_xdg_surface_send_close(xdg_surface);
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
    wl_list_remove(&request_fullscreen.link);
    wl_list_remove(&set_parent_ev.link);

    wayfire_view_t::destroy();
}

wayfire_xdg_view::~wayfire_xdg_view() {}

wayfire_xdg_decoration_view::wayfire_xdg_decoration_view(wlr_xdg_surface *decor) :
    wayfire_xdg_view(decor)
{ }

void wayfire_xdg_decoration_view::init(wayfire_view view, std::unique_ptr<wf_decorator_frame_t>&& fr)
{
    frame = std::move(fr);
    contained = view;
    geometry = view->get_wm_geometry();

    set_geometry(geometry);
    surface_children.push_back(view.get());

    xdg_surface_offset = {xdg_surface->geometry.x, xdg_surface->geometry.y};
}

void wayfire_xdg_decoration_view::map(wlr_surface *surface)
{
    wayfire_xdg_view::map(surface);

    if (contained->maximized)
        maximize_request(true);

    if (contained->fullscreen)
        fullscreen_request(output, true);
}

void wayfire_xdg_decoration_view::activate(bool state)
{
    wayfire_xdg_view::activate(state);
    contained->activate(state);
}

void wayfire_xdg_decoration_view::commit()
{
    wayfire_xdg_view::commit();

    wf_point new_offset = {xdg_surface->geometry.x, xdg_surface->geometry.y};
    if (new_offset.x != xdg_surface_offset.x || new_offset.y != xdg_surface_offset.y)
    {
        move(geometry.x, geometry.y, false);
        xdg_surface_offset = new_offset;
    }
}

void wayfire_xdg_decoration_view::move(int x, int y, bool ss)
{
    auto new_g = frame->get_child_geometry(geometry);
    new_g.x += xdg_surface->geometry.x;
    new_g.y += xdg_surface->geometry.y;

    log_info ("contained is moved to %d+%d, decor to %d+%d", new_g.x, new_g.y, x, y);

    contained->decor_x = new_g.x - geometry.x;
    contained->decor_y = new_g.y - geometry.y;

    contained->move(new_g.x, new_g.y, false);
    wayfire_xdg_view::move(x, y, ss);
}

void wayfire_xdg_decoration_view::resize(int w, int h, bool ss)
{
    auto new_geometry = geometry;
    new_geometry.width = w;
    new_geometry.height = h;

    auto new_g = frame->get_child_geometry(new_geometry);
    log_info ("contained is resized to %dx%d, decor to %dx%d", new_g.width, new_g.height,
              w, h);

    contained->resize(new_g.width, new_g.height, false);
}

void wayfire_xdg_decoration_view::child_configured(wf_geometry g)
{
    auto new_g = frame->get_geometry_interior(g);
    /*
       log_info("contained configured %dx%d, we become: %dx%d",
       g.width, g.height, new_g.width, new_g.height);
       */
    if (new_g.width != geometry.width || new_g.height != geometry.height)
        wayfire_xdg_view::resize(new_g.width, new_g.height, false);
}

void wayfire_xdg_decoration_view::unmap()
{
    /* if the contained view was closed earlier, then the decoration view
     * has already been forcibly unmapped */
    if (!surface) return;

    wayfire_view_t::unmap();

    if (contained->is_mapped())
    {
        contained->set_decoration(nullptr, nullptr);
        contained->close();
    }
}

void wayfire_xdg_decoration_view::set_maximized(bool state)
{
    wayfire_xdg_view::set_maximized(state);
    contained->set_maximized(state);
}

void wayfire_xdg_decoration_view::set_fullscreen(bool state)
{
    wayfire_xdg_view::set_fullscreen(state);
    contained->set_fullscreen(state);
}

static void notify_created(wl_listener*, void *data)
{
    auto surf = static_cast<wlr_xdg_surface*> (data);

    if (surf->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL)
    {
        if (surf->toplevel->title &&
            wf_decorator &&
            wf_decorator->is_decoration_window(surf->toplevel->title))
        {
            auto view = std::unique_ptr<wayfire_xdg_decoration_view> (new wayfire_xdg_decoration_view(surf));
            core->add_view(std::move(view));

            wf_decorator->decoration_ready(view->self());
        } else
        {
            std::unique_ptr<wayfire_xdg_view> ptr(new wayfire_xdg_view(surf));
            core->add_view(std::move(ptr));
        }
    }
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
