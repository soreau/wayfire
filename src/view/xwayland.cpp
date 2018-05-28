#include "priv-view.hpp"
#include "debug.hpp"
#include "core.hpp"
#include "output.hpp"
#include "workspace-manager.hpp"

extern "C"
{
#define class class_t
#define static
#include <wlr/xwayland.h>
#undef static
#undef class
}

static void handle_xwayland_request_move(wl_listener*, void *data)
{
    auto ev = static_cast<wlr_xwayland_move_event*> (data);
    auto view = wf_view_from_void(ev->surface->data);
    view->move_request();
}

static void handle_xwayland_request_resize(wl_listener*, void *data)
{
    auto ev = static_cast<wlr_xwayland_resize_event*> (data);
    auto view = wf_view_from_void(ev->surface->data);
    view->resize_request();
}

static void handle_xwayland_request_configure(wl_listener*, void *data)
{
    auto ev = static_cast<wlr_xwayland_surface_configure_event*> (data);
    auto view = wf_view_from_void(ev->surface->data);
     view->set_geometry({ev->x, ev->y, ev->width, ev->height});
}

static void handle_xwayland_request_maximize(wl_listener*, void *data)
{
    auto surf = static_cast<wlr_xwayland_surface*> (data);
    auto view = wf_view_from_void(surf->data);
    view->maximize_request(surf->maximized_horz && surf->maximized_vert);
}

static void handle_xwayland_request_fullscreen(wl_listener*, void *data)
{
    auto surf = static_cast<wlr_xwayland_surface*> (data);
    auto view = wf_view_from_void(surf->data);
    view->fullscreen_request(view->get_output(), surf->fullscreen);
}

static void handle_xwayland_map(wl_listener* listener, void *data)
{
    auto xsurf = static_cast<wlr_xwayland_surface*> (data);
    auto view = wf_view_from_void(xsurf->data);

    log_info("xwayland map %p %p -> %p", xsurf, xsurf->surface, view);
    view->map(xsurf->surface);
}

static void handle_xwayland_unmap(wl_listener*, void *data)
{
    auto xsurf = static_cast<wlr_xwayland_surface*> (data);
    auto view = wf_view_from_void(xsurf->data);

    log_info("xwayland unmap %p", xsurf);
    view->unmap();
}

static void handle_xwayland_destroy(wl_listener*, void *data)
{
    auto xsurf = static_cast<wlr_xwayland_surface*> (data);
    auto view = wf_view_from_void(xsurf->data);

    log_info("xwayland destroy %p", xsurf);
    view->destroyed = 1;
    view->dec_keep_count();
}

class wayfire_xwayland_view : public wayfire_view_t
{
    wlr_xwayland_surface *xw;

    /* TODO: very bad names, also in other shells */
    wl_listener destroy, map_ev, unmap, configure,
                request_move, request_resize,
                request_maximize, request_fullscreen;

    public:
    wayfire_xwayland_view(wlr_xwayland_surface *xww)
        : wayfire_view_t(), xw(xww)
    {
        log_info("new xwayland surface %s class: %s instance: %s",
                 nonull(xw->title), nonull(xw->class_t), nonull(xw->instance));

        destroy.notify            = handle_xwayland_destroy;
        map_ev.notify             = handle_xwayland_map;
        unmap.notify              = handle_xwayland_unmap;
        configure.notify          = handle_xwayland_request_configure;
        request_move.notify       = handle_xwayland_request_move;
        request_resize.notify     = handle_xwayland_request_resize;
        request_maximize.notify   = handle_xwayland_request_maximize;
        request_fullscreen.notify = handle_xwayland_request_fullscreen;

        wl_signal_add(&xw->events.destroy,            &destroy);
        wl_signal_add(&xw->events.unmap,              &unmap);
        wl_signal_add(&xw->events.map,                &map_ev);
        wl_signal_add(&xw->events.request_move,       &request_move);
        wl_signal_add(&xw->events.request_resize,     &request_resize);
        wl_signal_add(&xw->events.request_maximize,   &request_maximize);
        wl_signal_add(&xw->events.request_fullscreen, &request_fullscreen);
        wl_signal_add(&xw->events.request_configure,  &configure);

        xw->data = this;
    }

    ~wayfire_xwayland_view()
    {
        wl_list_remove(&destroy.link);
        wl_list_remove(&unmap.link);
        wl_list_remove(&map_ev.link);
        wl_list_remove(&request_move.link);
        wl_list_remove(&request_resize.link);
        wl_list_remove(&request_maximize.link);
        wl_list_remove(&request_fullscreen.link);
        wl_list_remove(&configure.link);
    }

    void map(wlr_surface *surface)
    {
        geometry.x = xw->x;
        geometry.y = xw->y;
        wayfire_view_t::map(surface);

        if (xw->maximized_horz && xw->maximized_vert)
            maximize_request(true);

        if (xw->fullscreen)
            fullscreen_request(output, true);

        if (xw->override_redirect)
            output->workspace->add_view_to_layer(self(), WF_LAYER_XWAYLAND);
    }

    bool is_subsurface() { return false; }

    virtual void commit()
    {
        wayfire_view_t::commit();
        if (xw->x != geometry.x || xw->y != geometry.y)
            wayfire_view_t::move(xw->x, xw->y, false);
    }

    void activate(bool active)
    {
        wayfire_view_t::activate(active);
        wlr_xwayland_surface_activate(xw, active);
    }

    void send_configure()
    {
        auto output_geometry = get_output_geometry();
        wlr_xwayland_surface_configure(xw, output_geometry.x, output_geometry.y,
                                       geometry.width, geometry.height);
    }

    void move(int x, int y, bool s)
    {
        wayfire_view_t::move(x, y, s);
        send_configure();
    }

    void resize(int w, int h, bool s)
    {
        wayfire_view_t::resize(w, h, s);
        send_configure();
    }

    /* TODO: bad with decoration */
    void set_geometry(wf_geometry g)
    {
        damage();
        geometry = g;

        /* send the geometry-changed signal */
        resize(g.width, g.height, true);
        send_configure();
    }

    void close()
    {
        wlr_xwayland_surface_close(xw);
    }

    void set_maximized(bool maxim)
    {
        wayfire_view_t::set_maximized(maxim);
        wlr_xwayland_surface_set_maximized(xw, maxim);
    }

    std::string get_title() { return nonull(xw->title); }
    std::string get_app_id() {return nonull(xw->class_t); }

    void set_fullscreen(bool full)
    {
        wayfire_view_t::set_fullscreen(full);
        wlr_xwayland_surface_set_fullscreen(xw, full);
    }
};

class wayfire_unmanaged_xwayland_view : public wayfire_view_t
{
    wlr_xwayland_surface *xw;
    wl_listener destroy, unmap_listener, map_ev, configure;

    public:
    wayfire_unmanaged_xwayland_view(wlr_xwayland_surface *xww)
        : wayfire_view_t(), xw(xww)
    {
        log_info("new unmanaged xwayland surface %s class: %s instance: %s",
                 nonull(xw->title), nonull(xw->class_t), nonull(xw->instance));

        map_ev.notify         = handle_xwayland_map;
        destroy.notify        = handle_xwayland_destroy;
        unmap_listener.notify = handle_xwayland_unmap;
        configure.notify      = handle_xwayland_request_configure;

        wl_signal_add(&xw->events.destroy,            &destroy);
        wl_signal_add(&xw->events.unmap,              &unmap_listener);
        wl_signal_add(&xw->events.request_configure,  &configure);
        wl_signal_add(&xw->events.map,                &map_ev);

        xw->data = this;
    }

    bool is_subsurface() { return false; }

    void commit()
    {
        log_info("commit at %dx%d", xw->x, xw->y);
        if (geometry.x != xw->x || geometry.y != xw->y)
            wayfire_view_t::move(xw->x, xw->y, false);

        wayfire_surface_t::commit();

        auto old_geometry = geometry;
        if (update_size())
        {
            damage(old_geometry);
            damage();
        }

        log_info("geometry is %d@%d %dx%d", geometry.x, geometry.y, geometry.width, geometry.height);
        auto og = get_output_geometry();
        log_info("ogeometry is %d@%d %dx%d", og.x, og.y, og.width, og.height);
    }

    void map(wlr_surface *surface)
    {
        log_info("map unmanaged %p", surface);
        wayfire_surface_t::map(surface);
        wayfire_view_t::move(xw->x, xw->y, false);
        damage();

        output->workspace->add_view_to_layer(self(), WF_LAYER_XWAYLAND);

        if (!wlr_xwayland_surface_is_unmanaged(xw))
        {
            emit_view_map(self());
            output->focus_view(self());
        }
    }

    void unmap()
    {
        if (!wlr_xwayland_surface_is_unmanaged(xw))
            emit_view_unmap(self());

        wayfire_surface_t::unmap();
    }

    void activate(bool active)
    {
        wayfire_view_t::activate(active);
        wlr_xwayland_surface_activate(xw, active);
    }


    void send_configure()
    {
        wlr_xwayland_surface_configure(xw, geometry.x, geometry.y,
                                       geometry.width, geometry.height);
        damage();
    }

    void move(int x, int y, bool s)
    {
        damage();
        geometry.x = x;
        geometry.y = y;
        send_configure();
    }

    void resize(int w, int h, bool s)
    {
        damage();
        geometry.width = w;
        geometry.height = h;
        send_configure();
    }

    /* TODO: bad with decoration */
    void set_geometry(wf_geometry g)
    {
        damage();
        geometry = g;
        send_configure();
    }

    void close()
    {
        wlr_xwayland_surface_close(xw);
    }

    void dec_keep_count()
    {
        wayfire_surface_t::dec_keep_count();
        log_info("dec keep count");
    }

    virtual void render_fb(int x, int y, pixman_region32_t* damage, int target_fb)
    {
        log_info("render fb unmanaged");
        wayfire_view_t::render_fb(x, y, damage, target_fb);
    }

    wlr_surface *get_keyboard_focus_surface()
    {
        if (wlr_xwayland_surface_is_unmanaged(xw))
            return nullptr;
        return surface;
    }

    ~wayfire_unmanaged_xwayland_view()
    {
        wl_list_remove(&destroy.link);
        wl_list_remove(&unmap_listener.link);
        wl_list_remove(&configure.link);
        wl_list_remove(&map_ev.link);
    }

    std::string get_title()  { return nonull(xw->title);   }
    std::string get_app_id() { return nonull(xw->class_t); }
};


void notify_xwayland_created(wl_listener *, void *data)
{
    auto xsurf = (wlr_xwayland_surface*) data;

    wayfire_view view = nullptr;
    if (wlr_xwayland_surface_is_unmanaged(xsurf) || xsurf->override_redirect)
    {
        view = std::make_shared<wayfire_unmanaged_xwayland_view> (xsurf);
    } else
    {
        view = std::make_shared<wayfire_xwayland_view> (xsurf);
    }

    core->add_view(view);
    log_info("xwayland create %p -> %p", xsurf, view.get());
}

static wlr_xwayland *xwayland_handle;
static wl_listener xwayland_created;

void init_xwayland()
{
    xwayland_created.notify = notify_xwayland_created;
    xwayland_handle = wlr_xwayland_create(core->display, core->compositor, false);

    if (xwayland_handle)
        wl_signal_add(&xwayland_handle->events.new_surface, &xwayland_created);
}

void xwayland_set_seat(wlr_seat *seat)
{
    if (xwayland_handle)
        wlr_xwayland_set_seat(xwayland_handle, core->get_current_seat());
}

std::string xwayland_get_display()
{
    return std::to_string(xwayland_handle ? xwayland_handle->display : -1);
}
