#include "priv-view.hpp"
#include "xdg-shell-v6.hpp"
#include "debug.hpp"
#include "core.hpp"
#include "decorator.hpp"

static void handle_new_popup(wl_listener*, void*);
static void handle_v6_map(wl_listener*, void *data);
static void handle_v6_unmap(wl_listener*, void *data);
static void handle_v6_destroy(wl_listener*, void *data);

/* TODO: Figure out a way to animate this */
wayfire_xdg6_popup::wayfire_xdg6_popup(wlr_xdg_popup_v6 *popup)
    :wayfire_surface_t(wf_surface_from_void(popup->parent->surface->data))
{
    assert(parent_surface);
    this->popup = popup;

    destroy.notify       = handle_v6_destroy;
    new_popup.notify     = handle_new_popup;
    m_popup_map.notify   = handle_v6_map;
    m_popup_unmap.notify = handle_v6_unmap;

    wl_signal_add(&popup->base->events.new_popup, &new_popup);
    wl_signal_add(&popup->base->events.map,       &m_popup_map);
    wl_signal_add(&popup->base->events.unmap,     &m_popup_unmap);
    wl_signal_add(&popup->base->events.destroy,   &destroy);

    popup->base->data = this;
}

wayfire_xdg6_popup::~wayfire_xdg6_popup()
{
    wl_list_remove(&new_popup.link);
    wl_list_remove(&m_popup_map.link);
    wl_list_remove(&m_popup_unmap.link);
    wl_list_remove(&destroy.link);
}

void wayfire_xdg6_popup::get_child_position(int &x, int &y)
{
    struct wlr_xdg_surface_v6 *parent = popup->parent;
    x = parent->geometry.x + popup->geometry.x - popup->base->geometry.x;
    y = parent->geometry.y + popup->geometry.y - popup->base->geometry.y;
}


void handle_new_popup(wl_listener*, void *data)
{
    auto popup = static_cast<wlr_xdg_popup_v6*> (data);
    auto parent = wf_surface_from_void(popup->parent->surface->data);
    if (!parent)
    {
        log_error("attempting to create a popup with unknown parent");
        return;
    }

    new wayfire_xdg6_popup(popup);
}

static void handle_v6_map(wl_listener*, void *data)
{
    auto surface = static_cast<wlr_xdg_surface_v6*> (data);
    auto wf_surface = wf_surface_from_void(surface->data);

    assert(wf_surface);
    wf_surface->map(surface->surface);
}

static void handle_v6_unmap(wl_listener*, void *data)
{
    auto surface = static_cast<wlr_xdg_surface_v6*> (data);
    auto wf_surface = wf_surface_from_void(surface->data);

    assert(wf_surface);
    wf_surface->unmap();
}

static void handle_v6_destroy(wl_listener*, void *data)
{
    auto surface = static_cast<wlr_xdg_surface_v6*> (data);
    auto wf_surface = wf_surface_from_void(surface->data);

    assert(wf_surface);
    wf_surface->destroyed = 1;
    wf_surface->dec_keep_count();
}

static void handle_v6_request_move(wl_listener*, void *data)
{
    auto ev = static_cast<wlr_xdg_toplevel_v6_move_event*> (data);
    auto view = wf_view_from_void(ev->surface->data);
    view->move_request();
}

static void handle_v6_request_resize(wl_listener*, void *data)
{
    auto ev = static_cast<wlr_xdg_toplevel_v6_resize_event*> (data);
    auto view = wf_view_from_void(ev->surface->data);
    view->resize_request();
}

static void handle_v6_request_maximized(wl_listener*, void *data)
{
    auto surf = static_cast<wlr_xdg_surface_v6*> (data);
    auto view = wf_view_from_void(surf->data);
    view->maximize_request(surf->toplevel->client_pending.maximized);
}

static void handle_v6_request_fullscreen(wl_listener*, void *data)
{
    auto ev = static_cast<wlr_xdg_toplevel_v6_set_fullscreen_event*> (data);
    auto view = wf_view_from_void(ev->surface->data);
    auto wo = core->get_output(ev->output);
    view->fullscreen_request(wo, ev->fullscreen);
}

wayfire_xdg6_view::wayfire_xdg6_view(wlr_xdg_surface_v6 *s)
    : wayfire_view_t(), v6_surface(s)
{
    log_info ("new xdg_shell_v6 surface: %s app-id: %s",
              nonull(v6_surface->toplevel->title),
              nonull(v6_surface->toplevel->app_id));

    destroy.notify            = handle_v6_destroy;
    new_popup.notify          = handle_new_popup;
    map_ev.notify             = handle_v6_map;
    unmap.notify              = handle_v6_unmap;
    request_move.notify       = handle_v6_request_move;
    request_resize.notify     = handle_v6_request_resize;
    request_maximize.notify   = handle_v6_request_maximized;
    request_fullscreen.notify = handle_v6_request_fullscreen;

    wlr_xdg_surface_v6_ping(s);

    wl_signal_add(&v6_surface->events.destroy, &destroy);
    wl_signal_add(&s->events.new_popup,        &new_popup);
    wl_signal_add(&v6_surface->events.map,     &map_ev);
    wl_signal_add(&v6_surface->events.unmap,   &unmap);
    wl_signal_add(&v6_surface->toplevel->events.request_move,       &request_move);
    wl_signal_add(&v6_surface->toplevel->events.request_resize,     &request_resize);
    wl_signal_add(&v6_surface->toplevel->events.request_maximize,   &request_maximize);
    wl_signal_add(&v6_surface->toplevel->events.request_fullscreen, &request_fullscreen);

    v6_surface->data = this;
}

void wayfire_xdg6_view::map(wlr_surface *surface)
{
    if (v6_surface->toplevel->client_pending.maximized)
        maximize_request(true);

    if (v6_surface->toplevel->client_pending.fullscreen)
        fullscreen_request(output, true);

    wayfire_view_t::map(surface);
}

wf_point wayfire_xdg6_view::get_output_position()
{
    if (decoration)
        return decoration->get_output_position()
            + wf_point{decor_x, decor_y}
    + wf_point{-v6_surface->geometry.x, -v6_surface->geometry.y};

    wf_point position {
        geometry.x - v6_surface->geometry.x,
            geometry.y - v6_surface->geometry.y,
    };

    return position;
}

void wayfire_xdg6_view::get_child_position(int &x, int &y)
{
    assert(decoration);

    x = decor_x - v6_surface->geometry.x;
    y = decor_y - v6_surface->geometry.y;
}

bool wayfire_xdg6_view::update_size()
{
    auto old_w = geometry.width, old_h = geometry.height;
    if (v6_surface->geometry.width > 0 && v6_surface->geometry.height > 0)
    {
        geometry.width = v6_surface->geometry.width;
        geometry.height = v6_surface->geometry.height;
    } else
    {
        wayfire_view_t::update_size();
    }

    return old_w != geometry.width || old_h != geometry.height;
}

void wayfire_xdg6_view::activate(bool act)
{
    wayfire_view_t::activate(act);
    wlr_xdg_toplevel_v6_set_activated(v6_surface, act);
}

void wayfire_xdg6_view::set_maximized(bool max)
{
    wayfire_view_t::set_maximized(max);
    wlr_xdg_toplevel_v6_set_maximized(v6_surface, max);
}

void wayfire_xdg6_view::set_fullscreen(bool full)
{
    wayfire_view_t::set_fullscreen(full);
    wlr_xdg_toplevel_v6_set_fullscreen(v6_surface, full);
}

void wayfire_xdg6_view::move(int w, int h, bool send)
{
    wayfire_view_t::move(w, h, send);
}

void wayfire_xdg6_view::resize(int w, int h, bool send)
{
    wayfire_view_t::resize(w, h, send);
    wlr_xdg_toplevel_v6_set_size(v6_surface, w, h);
}

std::string wayfire_xdg6_view::get_app_id()
{
    return nonull(v6_surface->toplevel->app_id);
}

std::string wayfire_xdg6_view::get_title()
{
    return nonull(v6_surface->toplevel->title);
}

void wayfire_xdg6_view::close()
{
    wlr_xdg_surface_v6_send_close(v6_surface);
}

wayfire_xdg6_view::~wayfire_xdg6_view()
{

    wl_list_remove(&destroy.link);
    wl_list_remove(&new_popup.link);
    wl_list_remove(&map_ev.link);
    wl_list_remove(&unmap.link);
    wl_list_remove(&request_move.link);
    wl_list_remove(&request_resize.link);
    wl_list_remove(&request_maximize.link);
    wl_list_remove(&request_fullscreen.link);
}

wayfire_xdg6_decoration_view::wayfire_xdg6_decoration_view(wlr_xdg_surface_v6 *decor) :
    wayfire_xdg6_view(decor)
{ }

void wayfire_xdg6_decoration_view::init(wayfire_view view, std::unique_ptr<wf_decorator_frame_t>&& fr)
{
    frame = std::move(fr);
    contained = view;
    geometry = view->get_wm_geometry();

    set_geometry(geometry);
    surface_children.push_back(view.get());

    v6_surface_offset = {v6_surface->geometry.x, v6_surface->geometry.y};
}

void wayfire_xdg6_decoration_view::map(wlr_surface *surface)
{
    wayfire_xdg6_view::map(surface);

    if (contained->maximized)
        maximize_request(true);

    if (contained->fullscreen)
        fullscreen_request(output, true);
}

void wayfire_xdg6_decoration_view::activate(bool state)
{
    wayfire_xdg6_view::activate(state);
    contained->activate(state);
}

void wayfire_xdg6_decoration_view::commit()
{
    wayfire_xdg6_view::commit();

    wf_point new_offset = {v6_surface->geometry.x, v6_surface->geometry.y};
    if (new_offset.x != v6_surface_offset.x || new_offset.y != v6_surface_offset.y)
    {
        move(geometry.x, geometry.y, false);
        v6_surface_offset = new_offset;
    }
}

void wayfire_xdg6_decoration_view::move(int x, int y, bool ss)
{
    auto new_g = frame->get_child_geometry(geometry);
    new_g.x += v6_surface->geometry.x;
    new_g.y += v6_surface->geometry.y;

    log_info ("contained is moved to %d+%d, decor to %d+%d", new_g.x, new_g.y, x, y);

    contained->decor_x = new_g.x - geometry.x;
    contained->decor_y = new_g.y - geometry.y;

    contained->move(new_g.x, new_g.y, false);
    wayfire_xdg6_view::move(x, y, ss);
}

void wayfire_xdg6_decoration_view::resize(int w, int h, bool ss)
{
    auto new_geometry = geometry;
    new_geometry.width = w;
    new_geometry.height = h;

    auto new_g = frame->get_child_geometry(new_geometry);
    log_info ("contained is resized to %dx%d, decor to %dx%d", new_g.width, new_g.height,
              w, h);

    contained->resize(new_g.width, new_g.height, false);
}

void wayfire_xdg6_decoration_view::child_configured(wf_geometry g)
{
    auto new_g = frame->get_geometry_interior(g);
    /*
       log_info("contained configured %dx%d, we become: %dx%d",
       g.width, g.height, new_g.width, new_g.height);
       */
    if (new_g.width != geometry.width || new_g.height != geometry.height)
        wayfire_xdg6_view::resize(new_g.width, new_g.height, false);
}

void wayfire_xdg6_decoration_view::unmap()
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

void wayfire_xdg6_decoration_view::set_maximized(bool state)
{
    wayfire_xdg6_view::set_maximized(state);
    contained->set_maximized(state);
}

void wayfire_xdg6_decoration_view::set_fullscreen(bool state)
{
    wayfire_xdg6_view::set_fullscreen(state);
    contained->set_fullscreen(state);
}

static void notify_v6_created(wl_listener*, void *data)
{
    auto surf = static_cast<wlr_xdg_surface_v6*> (data);

    if (surf->role == WLR_XDG_SURFACE_V6_ROLE_TOPLEVEL)
    {
        if (surf->toplevel->title &&
            wf_decorator &&
            wf_decorator->is_decoration_window(surf->toplevel->title))
        {
            auto view = std::make_shared<wayfire_xdg6_decoration_view> (surf);
            core->add_view(view);

            wf_decorator->decoration_ready(view);
        } else
        {
            core->add_view(std::make_shared<wayfire_xdg6_view> (surf));
        }
    }
}

static wlr_xdg_shell_v6 *v6_handle;
static wl_listener v6_created;

void init_xdg_shell_v6()
{
    v6_created.notify = notify_v6_created;
    v6_handle = wlr_xdg_shell_v6_create(core->display);
    wl_signal_add(&v6_handle->events.new_surface, &v6_created);
}
