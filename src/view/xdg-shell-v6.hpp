#include "priv-view.hpp"
extern "C"
{
#include <wlr/types/wlr_xdg_shell_v6.h>
}

static void handle_v6_new_popup(wl_listener*, void*);
class wayfire_xdg6_popup : public wayfire_surface_t
{
    protected:
        wl_listener destroy,
                    new_popup, destroy_popup,
                    m_popup_map, m_popup_unmap;

        wlr_xdg_popup_v6 *popup;
        wlr_xdg_surface_v6 *xdg_surface;

    public:
        wayfire_xdg6_popup(wlr_xdg_popup_v6 *popup);
        ~wayfire_xdg6_popup();

        virtual void get_child_position(int &x, int &y);
        virtual bool is_subsurface() { return true; }
};

class wayfire_xdg6_view : public wayfire_view_t
{
    protected:
        wl_listener destroy, map_ev, unmap, new_popup,
                request_move, request_resize,
                request_maximize, request_fullscreen;

    public:
        wlr_xdg_surface_v6 *v6_surface;

    wayfire_xdg6_view(wlr_xdg_surface_v6 *s);
    virtual void map(wlr_surface *surface);
    virtual wf_point get_output_position();
    virtual void get_child_position(int &x, int &y);
    virtual bool update_size();
    virtual void activate(bool act);
    virtual void set_maximized(bool max);
    virtual void set_fullscreen(bool full);
    virtual void move(int w, int h, bool send);
    virtual void resize(int w, int h, bool send);

    std::string get_app_id();
    std::string get_title();
    virtual void close();
    ~wayfire_xdg6_view();
};

class wayfire_xdg6_decoration_view : public wayfire_xdg6_view
{
    wayfire_view contained = NULL;
    std::unique_ptr<wf_decorator_frame_t> frame;

    wf_point v6_surface_offset;

    public:

    wayfire_xdg6_decoration_view(wlr_xdg_surface_v6 *decor);
    void init(wayfire_view view, std::unique_ptr<wf_decorator_frame_t>&& fr);
    void map(wlr_surface *surface);
    void activate(bool state);
    void commit();
    void move(int x, int y, bool ss);
    void resize(int w, int h, bool ss);
    void child_configured(wf_geometry g);
    void unmap();

    wlr_surface *get_keyboard_focus_surface()
    { return contained->get_keyboard_focus_surface(); }

    void set_maximized(bool state);
    void set_fullscreen(bool state);
    ~wayfire_xdg6_decoration_view()
    { }
};
