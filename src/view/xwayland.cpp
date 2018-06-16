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
    view->destroy();
}

static void handle_xwayland_set_parent(wl_listener*, void *data)
{
    auto surface = static_cast<wlr_xwayland_surface*> (data);
    auto view = wf_view_from_void(surface->data);
    auto parent = surface->parent ?
        wf_view_from_void(surface->parent->data)->self() : nullptr;

    assert(view);
    view->set_toplevel_parent(parent);
}

class wayfire_xwayland_view : public wayfire_view_t
{
    wlr_xwayland_surface *xw;

    /* TODO: very bad names, also in other shells */
    wl_listener destroy_ev, map_ev, unmap_ev, configure,
                request_move, request_resize,
                request_maximize, request_fullscreen,
                set_parent_ev;

    signal_callback_t output_geometry_changed;

    public:
    wayfire_xwayland_view(wlr_xwayland_surface *xww)
        : wayfire_view_t(), xw(xww)
    {
        log_info("new xwayland surface %s class: %s instance: %s",
                 nonull(xw->title), nonull(xw->class_t), nonull(xw->instance));

        destroy_ev.notify         = handle_xwayland_destroy;
        map_ev.notify             = handle_xwayland_map;
        unmap_ev.notify           = handle_xwayland_unmap;
        configure.notify          = handle_xwayland_request_configure;
        set_parent_ev.notify      = handle_xwayland_set_parent;
        request_move.notify       = handle_xwayland_request_move;
        request_resize.notify     = handle_xwayland_request_resize;
        request_maximize.notify   = handle_xwayland_request_maximize;
        request_fullscreen.notify = handle_xwayland_request_fullscreen;

        wl_signal_add(&xw->events.destroy,            &destroy_ev);
        wl_signal_add(&xw->events.unmap,              &unmap_ev);
        wl_signal_add(&xw->events.map,                &map_ev);
        wl_signal_add(&xw->events.set_parent,         &set_parent_ev);
        wl_signal_add(&xw->events.request_move,       &request_move);
        wl_signal_add(&xw->events.request_resize,     &request_resize);
        wl_signal_add(&xw->events.request_maximize,   &request_maximize);
        wl_signal_add(&xw->events.request_fullscreen, &request_fullscreen);
        wl_signal_add(&xw->events.request_configure,  &configure);

        xw->data = this;

        output_geometry_changed = [this] (signal_data*)
        {
            if (is_mapped())
                move(geometry.x, geometry.y, false);
        };

        if (output)
            output->connect_signal("output-resized", &output_geometry_changed);
    }

    ~wayfire_xwayland_view()
    {}

    virtual void destroy()
    {
        if (output)
            output->disconnect_signal("output-resized", &output_geometry_changed);

        wl_list_remove(&destroy_ev.link);
        wl_list_remove(&unmap_ev.link);
        wl_list_remove(&map_ev.link);
        wl_list_remove(&set_parent_ev.link);
        wl_list_remove(&request_move.link);
        wl_list_remove(&request_resize.link);
        wl_list_remove(&request_maximize.link);
        wl_list_remove(&request_fullscreen.link);
        wl_list_remove(&configure.link);

        wayfire_view_t::destroy();
    }

    void map(wlr_surface *surface)
    {
        if (xw->maximized_horz && xw->maximized_vert)
            maximize_request(true);

        if (xw->fullscreen)
            fullscreen_request(output, true);

        if (xw->parent)
        {
            auto parent = wf_view_from_void(xw->parent->data)->self();
            set_toplevel_parent(parent);
        }

        auto real_output = output->get_full_geometry();
        if (!maximized && !fullscreen && !parent)
            move(xw->x - real_output.x, xw->y - real_output.y, false);

        wayfire_view_t::map(surface);
    }

    bool is_subsurface() { return false; }

    void activate(bool active)
    {
        wayfire_view_t::activate(active);
        wlr_xwayland_surface_activate(xw, active);
    }

    void send_configure(int width, int height)
    {
        auto output_geometry = get_output_geometry();

        int configure_x = output_geometry.x;
        int configure_y = output_geometry.y;

        if (output)
        {
            auto real_output = output->get_full_geometry();
            configure_x += real_output.x;
            configure_y += real_output.y;
        }

        wlr_xwayland_surface_configure(xw, configure_x, configure_y,
                                       width, height);
    }

    void send_configure()
    {
        send_configure(geometry.width, geometry.height);
    }

    void set_output(wayfire_output *wo)
    {
        if (output)
            output->disconnect_signal("output-resized", &output_geometry_changed);

        wayfire_surface_t::set_output(wo);

        if (wo)
            wo->connect_signal("output-resized", &output_geometry_changed);

        send_configure();
    }

    void move(int x, int y, bool s)
    {
        wayfire_view_t::move(x, y, s);
        send_configure();
    }

    void resize(int w, int h, bool s)
    {
        send_configure(w, h);
    }

    /* TODO: bad with decoration */
    void set_geometry(wf_geometry g)
    {
        damage();

        wayfire_view_t::move(g.x, g.y, false);
        resize(g.width, g.height, false);
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
    wl_listener destroy_ev, unmap_listener, map_ev, configure;

    public:
    wayfire_unmanaged_xwayland_view(wlr_xwayland_surface *xww)
        : wayfire_view_t(), xw(xww)
    {
        log_info("new unmanaged xwayland surface %s class: %s instance: %s",
                 nonull(xw->title), nonull(xw->class_t), nonull(xw->instance));

        map_ev.notify         = handle_xwayland_map;
        destroy_ev.notify     = handle_xwayland_destroy;
        unmap_listener.notify = handle_xwayland_unmap;
        configure.notify      = handle_xwayland_request_configure;

        wl_signal_add(&xw->events.destroy,            &destroy_ev);
        wl_signal_add(&xw->events.unmap,              &unmap_listener);
        wl_signal_add(&xw->events.request_configure,  &configure);
        wl_signal_add(&xw->events.map,                &map_ev);

        xw->data = this;
        role = WF_VIEW_ROLE_UNMANAGED;
    }

    bool is_subsurface() { return false; }

    int global_x, global_y;

    void commit()
    {
        if (global_x != xw->x || global_y != xw->y)
        {
            global_x = xw->x;
            global_y = xw->y;

            if (output)
            {
                auto real_output = output->get_full_geometry();
                wayfire_view_t::move(xw->x - real_output.x, xw->y - real_output.y, false);
            }
        }

        wayfire_surface_t::commit();

        auto old_geometry = geometry;
        if (update_size())
        {
            damage(old_geometry);
            damage();
        }
    }

    void map(wlr_surface *surface)
    {
        /* move to the output where our center is
         * FIXME: this is a bad idea, because a dropdown menu might get sent to
         * an incorrect output. However, no matter how we calculate the real
         * output, we just can't be 100% compatible because in X all windows are
         * positioned in a global coordinate space */
        auto wo = core->get_output_at(xw->x + surface->current->width / 2, xw->y + surface->current->height / 2);

        if (!wo)
        {
            /* if surface center is outside of anything, try to check the output where the pointer is */
            GetTuple(cx, cy, core->get_cursor_position());
            wo = core->get_output_at(cx, cy);
        }

        if (!wo)
            wo = core->get_active_output();
        assert(wo);

        if (wo != output)
        {
            if (output)
                output->workspace->add_view_to_layer(self(), 0);
            set_output(wo);
        }

        auto real_output = output->get_full_geometry();

        wayfire_view_t::move(xw->x - real_output.x, xw->y - real_output.y, false);
        global_x = xw->x;
        global_y = xw->y;

        damage();

        wayfire_surface_t::map(surface);
        output->workspace->add_view_to_layer(self(), WF_LAYER_XWAYLAND);

        if (!wlr_xwayland_surface_is_unmanaged(xw))
        {
            auto wa = output->workspace->get_workarea();
            move(xw->x + wa.x - real_output.x, xw->y + wa.y - real_output.y, false);

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
        if (output)
        {
            auto real_output = output->get_full_geometry();
            global_x = geometry.x + real_output.x;
            global_y = geometry.y + real_output.y;
        } else
        {
            global_x = geometry.x;
            global_y = geometry.y;
        }

        wlr_xwayland_surface_configure(xw, global_x, global_y, geometry.width, geometry.height);
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

    void set_output(wayfire_output *wo)
    {
        wayfire_surface_t::set_output(wo);

        if (wo)
        {
            auto real_geometry = wo->get_full_geometry();
            wayfire_view_t::move(global_x - real_geometry.x, global_y - real_geometry.y);
        }
    }

    void close()
    {
        wlr_xwayland_surface_close(xw);
    }

    void dec_keep_count()
    {
        wayfire_surface_t::dec_keep_count();
    }

    virtual void render_fb(int x, int y, pixman_region32_t* damage, int target_fb)
    {
        wayfire_view_t::render_fb(x, y, damage, target_fb);
    }

    wlr_surface *get_keyboard_focus_surface()
    {
        if (wlr_xwayland_surface_is_unmanaged(xw))
            return nullptr;
        return surface;
    }

    ~wayfire_unmanaged_xwayland_view()
    { }

    virtual void destroy()
    {
        wl_list_remove(&destroy_ev.link);
        wl_list_remove(&unmap_listener.link);
        wl_list_remove(&configure.link);
        wl_list_remove(&map_ev.link);

        wayfire_view_t::destroy();
    }

    std::string get_title()  { return nonull(xw->title);   }
    std::string get_app_id() { return nonull(xw->class_t); }
};


void notify_xwayland_created(wl_listener *, void *data)
{
    auto xsurf = (wlr_xwayland_surface*) data;

    if (wlr_xwayland_surface_is_unmanaged(xsurf) || xsurf->override_redirect)
    {
        auto view = std::unique_ptr<wayfire_unmanaged_xwayland_view>
            (new wayfire_unmanaged_xwayland_view(xsurf));
        core->add_view(std::move(view));
    } else
    {
        auto view = std::unique_ptr<wayfire_xwayland_view>
            (new wayfire_xwayland_view(xsurf));
        core->add_view(std::move(view));
    }
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
