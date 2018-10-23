#ifndef VIEW_HPP
#define VIEW_HPP
#include <vector>
#include <map>
#include <functional>
#include <pixman.h>
#include <nonstd/observer_ptr.h>

#include "plugin.hpp"
#include "opengl.hpp"
#include "object.hpp"

extern "C"
{
#include <wlr/types/wlr_box.h>
#include <wlr/types/wlr_surface.h>
#include <wlr/util/edges.h>
}

class wayfire_output;
struct wf_point
{
    int x, y;
};
using wf_geometry = wlr_box;

bool operator == (const wf_geometry& a, const wf_geometry& b);
bool operator != (const wf_geometry& a, const wf_geometry& b);

wf_point operator + (const wf_point& a, const wf_point& b);
wf_point operator + (const wf_point& a, const wf_geometry& b);
wf_geometry operator + (const wf_geometry &a, const wf_point& b);
wf_point operator - (const wf_point& a);

/* scale box */
wf_geometry get_output_box_from_box(const wlr_box& box, float scale);

/* rotate box */
wlr_box get_scissor_box(wayfire_output *output, const wlr_box& box);
wlr_box get_scissor_box(uint32_t fb_width, uint32_t fb_height, uint32_t transform,
                        const wlr_box& box);

/* scale + rotate */
wlr_box output_transform_box(wayfire_output *output, const wlr_box& box);

bool point_inside(wf_point point, wf_geometry rect);
bool rect_intersect(wf_geometry screen, wf_geometry win);

/* General TODO: mark member functions const where appropriate */
class wayfire_view_t;
using wayfire_view = nonstd::observer_ptr<wayfire_view_t>;

/* do not copy the surface, it is not reference counted and you will get crashes */
class wayfire_surface_t;
using wf_surface_iterator_callback = std::function<void(wayfire_surface_t*, int, int)>;

class wf_decorator_frame_t;
class wf_view_transformer_t;

/* abstraction for desktop-apis, no real need for plugins
 * This is a base class to all "drawables" - desktop views, subsurfaces, popups */
class wayfire_surface_t
{
    /* TODO: maybe some functions don't need to be virtual? */
    protected:

        wl_listener committed, destroy, new_sub;
        virtual void for_each_surface_recursive(wf_surface_iterator_callback callback,
                                                int x, int y, bool reverse = false);
        wayfire_output *output = nullptr;

        /* position relative to parent */
        virtual void get_child_position(int &x, int &y);

        wf_geometry geometry = {0, 0, 0, 0};

        virtual bool is_subsurface();
        virtual void damage(const wlr_box& box);
        virtual void damage(pixman_region32_t *region);

        void apply_surface_damage(int x, int y);

        struct wlr_fb_attribs
        {
            int width, height;
            wl_output_transform transform = WL_OUTPUT_TRANSFORM_NORMAL;
        };

        virtual void _wlr_render_box(const wlr_fb_attribs& fb, int x, int y, const wlr_box& scissor);
        virtual void _render_pixman(const wlr_fb_attribs& fb, int x, int y, pixman_region32_t *damage);

    public:
        /* NOT API */
        wayfire_surface_t *parent_surface;
        /* NOT API */
        std::vector<wayfire_surface_t*> surface_children;

        /* NOT API */
        bool has_client_decoration = true;

        /* offset to be applied for children, NOT API */
        virtual void get_child_offset(int &x, int &y);

        wayfire_surface_t(wayfire_surface_t *parent = nullptr);
        virtual ~wayfire_surface_t();

        /* if surface != nullptr, then the surface is mapped */
        wlr_surface *surface = nullptr;

        /* NOT API */
        virtual void map(wlr_surface *surface);
        /* NOT API */
        virtual void unmap();
        /* NOT API */
        virtual void destruct() { delete this; }

        virtual bool accepts_input(int32_t sx, int32_t sy);
        virtual void send_frame_done(const timespec& now);

        virtual wl_client *get_client();

        virtual bool is_mapped();

        virtual wlr_buffer *get_buffer();

        int keep_count = 0;
        bool destroyed = false;

        void inc_keep_count();
        void dec_keep_count();

        virtual wayfire_surface_t *get_main_surface();

        virtual void damage();

        float alpha = 1.0;

        /* returns top-left corner in output coordinates */
        virtual wf_point get_output_position();

        /* NOT API */
        virtual void update_output_position();

        /* return surface box in output coordinates */
        virtual wf_geometry get_output_geometry();

        /* NOT API */
        virtual void commit();

        virtual wayfire_output *get_output() { return output; };
        virtual void set_output(wayfire_output*);

        /* NOT API */
        virtual void  render_pixman(const wlr_fb_attribs& fb, int x, int y, pixman_region32_t* damage);

        /* render the surface to the given fb */
        virtual void render_fb(pixman_region32_t* damage, wf_framebuffer fb);

        /* iterate all (sub) surfaces, popups, etc. in top-most order
         * for example, first popups, then subsurfaces, then main surface
         * When reverse=true, the order in which surfaces are visited is reversed */
        virtual void for_each_surface(wf_surface_iterator_callback callback, bool reverse = false);
};

enum wf_view_role
{
    WF_VIEW_ROLE_TOPLEVEL, // regular, "WM" views
    WF_VIEW_ROLE_UNMANAGED, // xwayland override redirect or unmanaged views
    WF_VIEW_ROLE_SHELL_VIEW // background, lockscreen, panel, notifications, etc
};

/* Represents a desktop window (not as X11 window, but like a xdg_toplevel surface) */
class wayfire_view_t : public wayfire_surface_t, public wf_object_base
{
    friend void surface_destroyed_cb(wl_listener*, void *data);

    protected:

        /* those two point to the same object. Two fields are used to avoid
         * constant casting to and from types */
        wayfire_surface_t *decoration = NULL;
        wf_decorator_frame_t *frame = NULL;

        void force_update_xwayland_position();
        int in_continuous_move = 0, in_continuous_resize = 0;

        bool wait_decoration = false;
        virtual bool update_size();
        void adjust_anchored_edge(int32_t new_width, int32_t new_height);

        uint32_t id;
        virtual void damage(const wlr_box& box);

        struct offscreen_buffer_t
        {
            uint32_t fbo = -1, tex = -1;
            /* used to store output_geometry when the view has been destroyed */
            int32_t output_x = 0, output_y = 0;
            int32_t fb_width = 0, fb_height = 0;
            float fb_scale = 1;
            pixman_region32_t cached_damage;

            void init(int w, int h);
            void fini();
            bool valid();

        } offscreen_buffer;

        struct transform_t
        {
            std::string plugin_name = "";
            bool to_remove = false;
            std::unique_ptr<wf_view_transformer_t> transform;
            wf_framebuffer fb;
        };

        bool in_paint = false;
        std::vector<std::unique_ptr<transform_t>> transforms;
        void _pop_transformer(nonstd::observer_ptr<transform_t>);
        void cleanup_transforms();

        virtual wf_geometry get_untransformed_bounding_box();
        void reposition_relative_to_parent();

        uint32_t edges = 0;

    public:

        /* these represent toplevel relations, children here are transient windows,
         * such as the close file dialogue */
        wayfire_view parent = nullptr;
        std::vector<wayfire_view> children;
        virtual void set_toplevel_parent(wayfire_view parent);
        virtual void set_output(wayfire_output*);

        wf_view_role role = WF_VIEW_ROLE_TOPLEVEL;

        wayfire_view_t();
        virtual ~wayfire_view_t();
        std::string to_string() const;

        wayfire_view self();

        virtual void move(int x, int y, bool send_signal = true);

        /* both resize and set_geometry just request the client to resize,
         * there is no guarantee that they will actually honour the size.
         * However, maximized surfaces typically do resize to the dimensions
         * they are asked */
        virtual void resize(int w, int h, bool send_signal = true);
        virtual void activate(bool active);
        virtual void close();

        virtual void set_parent(wayfire_view parent);
        virtual wayfire_surface_t* get_main_surface();

        /* return geometry as should be used for all WM purposes */
        virtual wf_geometry get_wm_geometry();

        virtual wf_point get_output_position();

        /* return the output-local transformed coordinates of the view
         * and all its subsurfaces */
        virtual wlr_box get_bounding_box();

        /* return the output-local transformed coordinates of the view,
         * up to the given transformer. */
        virtual wlr_box get_bounding_box(std::string transformer);
        virtual wlr_box get_bounding_box(
            nonstd::observer_ptr<wf_view_transformer_t> transformer);

        /* transform the given region using the view's transform */
        virtual wlr_box transform_region(const wlr_box &box);

        /* transform the given region using the view's transform
         * up to the given transformer */
        virtual wlr_box transform_region(const wlr_box& box, std::string transformer);
        virtual wlr_box transform_region(const wlr_box& box,
            nonstd::observer_ptr<wf_view_transformer_t> transformer);

        /* check whether the given region intersects any of the surfaces
         * in the view's surface tree. */
        virtual bool intersects_region(const wlr_box& region);

        /* map from global to surface local coordinates
         * returns the (sub)surface under the cursor or NULL iff the cursor is outside of the view
         * TODO: it should be overwritable by plugins which deform the view */
        virtual wayfire_surface_t *map_input_coordinates(int cursor_x, int cursor_y, int &sx, int &sy);
        virtual wlr_surface *get_keyboard_focus_surface() { return surface; };

        virtual void set_geometry(wf_geometry g);

        /* set edges to control the gravity of resize update
         * default: top-left corner stays where it is */
        virtual void set_resizing(bool resizing, uint32_t edges = 0);
        virtual void set_moving(bool moving);

        bool maximized = false, fullscreen = false;

        virtual void set_maximized(bool maxim);
        virtual void set_fullscreen(bool fullscreen);

        bool is_visible();
        virtual void commit();
        virtual void map(wlr_surface *surface);
        virtual void unmap();

        /* cleanup of the wf_view part. Not API function */
        virtual void destruct();

        /* cleanup of the wlroots handle. Not API function */
        virtual void destroy();

        virtual void damage();

        virtual std::string get_app_id() { return ""; }
        virtual std::string get_title() { return ""; }


        /* Set if the current view should not be rendered by built-in renderer */
        bool is_hidden = false;

        virtual void move_request();
        virtual void resize_request(uint32_t edges = 0);
        virtual void maximize_request(bool state);
        virtual void fullscreen_request(wayfire_output *output, bool state);

        /* returns whether the view should be decorated */
        virtual bool should_be_decorated();

        /* Used to set a decoration.
         * The parameter object MUST be a subclass of both wayfire_surface_t
         * and of wf_decorator_frame_t
         *
         * The life-time of the decoration ({inc,dec}_keep_count) is managed by the view itself
         * Setting the decoration may change the view output and wm geometry */
        virtual void set_decoration(wayfire_surface_t *frame);

        /* iterate all (sub) surfaces, popups, decorations, etc. in top-most order
         * for example, first popups, then subsurfaces, then main surface
         * When reverse=true, the order in which surfaces are visited is reversed */
        virtual void for_each_surface(wf_surface_iterator_callback callback, bool reverse = false);

        /*
         *                              View transforms
         * A view transform can be any kind of transformation, for example 3D rotation,
         * wobbly effect or similar. When we speak of transforms, a "view" is defined
         * as a toplevel window (including decoration) and also all of its subsurfaces/popups.
         * The transformation then is applied to this group of surfaces together.
         *
         * When a view has a custom transform, then internally all these surfaces are
         * rendered to a FBO, and then the custom transformation renders the resulting
         * texture as it sees fit. In case of multiple transforms, we do multiple render passes
         * where each transform is fed the result of the previous transforms
         *
         * Damage tracking for transformed views is done on the boundingbox of the
         * damaged region after applying the transformation, but all damaged parts
         * of the internal FBO are updated.
         * */

        void add_transformer(std::unique_ptr<wf_view_transformer_t> transformer);

        /* add a transformer with the given name. Note that you can add multiple transforms with the same name!
         * However, get_transformer() and pop_transformer() return only the first transform with the given name */
        void add_transformer(std::unique_ptr<wf_view_transformer_t> transformer, std::string name);

        /* returns NULL if there is no such transform */
        nonstd::observer_ptr<wf_view_transformer_t> get_transformer(std::string name);

        void pop_transformer(nonstd::observer_ptr<wf_view_transformer_t> transformer);
        void pop_transformer(std::string name);

        bool has_transformer();

        virtual void render_fb(pixman_region32_t* damage, wf_framebuffer framebuffer);

        bool has_snapshot = false;
        virtual void take_snapshot();
};

wayfire_view wl_surface_to_wayfire_view(wl_resource *surface);
#endif
