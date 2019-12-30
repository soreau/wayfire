#define GLM_FORCE_RADIANS
#include <glm/gtc/matrix_transform.hpp>

#include <linux/input-event-codes.h>

#include <wayfire/compositor-surface.hpp>
#include <wayfire/output.hpp>
#include <wayfire/opengl.hpp>
#include <wayfire/core.hpp>
#include <wayfire/debug.hpp>
#include <wayfire/decorator.hpp>
#include <wayfire/view-transform.hpp>
#include <wayfire/signal-definitions.hpp>
#include "deco-subsurface.hpp"

#include <cairo.h>

extern "C"
{
#define static
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_matrix.h>
#include <wlr/types/wlr_xcursor_manager.h>
#undef static
}

const int titlebar_thickness = 30;
const int resize_edge_threshold = 5;
const int normal_thickness = resize_edge_threshold;

GLuint get_text_texture(int width, int height,
    std::string text, std::string cairo_font)
{
    const auto format = CAIRO_FORMAT_ARGB32;
    auto surface = cairo_image_surface_create(format, width, height);
    auto cr = cairo_create(surface);

    const float font_scale = 0.8;
    const float font_size = height * font_scale;

    // render text
    cairo_select_font_face(cr, cairo_font.c_str(), CAIRO_FONT_SLANT_NORMAL,
        CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_source_rgba(cr, 1, 1, 1, 1);

    cairo_set_font_size(cr, font_size);

    cairo_text_extents_t ext;
    cairo_text_extents(cr, text.c_str(), &ext);

    cairo_move_to(cr, normal_thickness, font_size);
    cairo_show_text(cr, text.c_str());

    cairo_destroy(cr);

    auto src = cairo_image_surface_get_data(surface);

    GLuint tex;
    GL_CALL(glGenTextures(1, &tex));
    GL_CALL(glBindTexture(GL_TEXTURE_2D, tex));

    GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
    GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
    GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
    GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
    GL_CALL(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
            GL_RGBA, GL_UNSIGNED_BYTE, src));

    cairo_surface_destroy(surface);

    return tex;
}

class simple_decoration_surface : public wf::surface_interface_t,
    public wf::compositor_surface_t, public wf::decorator_frame_t_t
{
    bool _mapped = true;
    int thickness = normal_thickness;
    int titlebar = titlebar_thickness;

    wayfire_view view;
    wf::option_wrapper_t<std::string> font_option{"decoration/font"};
    wf::signal_callback_t title_set;

    int width = 100, height = 100;

    bool active = true; // when views are mapped, they are usually activated
    wf::color_t border_color{0.15, 0.15, 0.15, 0.8};
    wf::color_t border_color_inactive = {0.25, 0.25, 0.25, 0.95};

    GLuint tex = -1;


  public:
    simple_decoration_surface(wayfire_view view)
        : surface_interface_t(view.get())
    {
        this->view = view;

        title_set = [=] (wf::signal_data_t *data)
        {
            if (get_signaled_view(data) == view)
                notify_view_resized(view->get_wm_geometry());
        };
        view->connect_signal("title-changed", &title_set);

        // make sure to hide frame if the view is fullscreen
        update_decoration_size();
    }


    virtual ~simple_decoration_surface()
    {
        _mapped = false;
        wf::emit_map_state_change(this);
        view->disconnect_signal("title-changed", &title_set);
    }


    /* wf::surface_interface_t implementation */
    virtual bool is_mapped() const final
    {
        return _mapped;
    }

    wf::point_t get_offset() final
    {
        return { -thickness, -titlebar };
    }

    virtual wf::dimensions_t get_size() const final
    {
        return {width, height};
    }

    void render_box(const wf::framebuffer_t& fb, int x, int y,
        const wlr_box& scissor)
    {
        wlr_box geometry {x + fb.geometry.x, y + fb.geometry.y, width, height};
        OpenGL::render_begin(fb);
        fb.scissor(scissor);
        OpenGL::render_rectangle(geometry,
            active ? border_color : border_color_inactive,
            fb.get_orthographic_projection());


        if (tex == (uint)-1)
        {
            tex = get_text_texture(width * fb.scale, titlebar * fb.scale,
                view->get_title(), font_option);
        }

        gl_geometry gg;
        gg.x1 = x + fb.geometry.x;
        gg.y1 = y + fb.geometry.y;
        gg.x2 = gg.x1 + width;
        gg.y2 = gg.y1 + titlebar;

        OpenGL::render_transformed_texture(tex, gg, {},
            fb.get_orthographic_projection(), {1, 1, 1, 1},
            TEXTURE_TRANSFORM_INVERT_Y);

        GL_CALL(glUseProgram(0));
        OpenGL::render_end();
    }

    /* The region which is only the frame. Origin is 0,0 */
    wf::region_t frame_region;
    void update_frame_region()
    {
        frame_region.clear();
        frame_region |= {0, 0, width, titlebar}; // top
        frame_region |= {0, 0, thickness, height}; // left
        frame_region |= {(width - thickness), 0, thickness, height}; // right
        frame_region |= {0, (height - thickness), width, thickness}; // bottom
    }

    virtual void simple_render(const wf::framebuffer_t& fb, int x, int y,
        const wf::region_t& damage) override
    {
        wf::region_t frame = this->frame_region + wf::point_t{x, y};
        frame *= fb.scale;
        frame &= damage;

        for (const auto& box : frame)
        {
            auto sbox = fb.framebuffer_box_from_damage_box(
                wlr_box_from_pixman_box(box));
            render_box(fb, x, y, sbox);
        }
    }

    bool accepts_input(int32_t sx, int32_t sy) override
    {
        return pixman_region32_contains_point(frame_region.to_pixman(),
            sx, sy, NULL);
    }

    /* wf::compositor_surface_t implementation */
    virtual void on_pointer_enter(int x, int y) override
    {
        cursor_x = x;
        cursor_y = y;

        update_cursor();
    }

    virtual void on_pointer_leave() override
    { }

    int cursor_x, cursor_y;
    virtual void on_pointer_motion(int x, int y) override
    {
        cursor_x = x;
        cursor_y = y;

        update_cursor();
    }

    void send_move_request()
    {
        move_request_signal move_request;
        move_request.view = view;
        get_output()->emit_signal("move-request", &move_request);
    }

    void send_resize_request(int x, int y)
    {
        resize_request_signal resize_request;
        resize_request.view = view;
        resize_request.edges = get_edges(x, y);
        get_output()->emit_signal("resize-request", &resize_request);
    }

    uint32_t get_edges(int x, int y)
    {
        uint32_t edges = 0;
        if (x <= thickness)
            edges |= WLR_EDGE_LEFT;
        if (x >= width - thickness)
            edges |= WLR_EDGE_RIGHT;
        if (y <= thickness)
            edges |= WLR_EDGE_TOP;
        if (y >= height - thickness)
            edges |= WLR_EDGE_BOTTOM;

        return edges;
    }

    std::string get_cursor(uint32_t edges)
    {
        if (edges)
            return wlr_xcursor_get_resize_name((wlr_edges) edges);
        return "default";
    }

    void update_cursor()
    {
        wf::get_core().set_cursor(get_cursor(get_edges(cursor_x, cursor_y)));
    }

    virtual void on_pointer_button(uint32_t button, uint32_t state) override
    {
        if (button != BTN_LEFT || state != WLR_BUTTON_PRESSED)
            return;

        if (get_edges(cursor_x, cursor_y))
            return send_resize_request(cursor_x, cursor_y);
        send_move_request();
    }

    virtual void on_touch_down(int x, int y) override
    {
        if (get_edges(x, y))
            return send_resize_request(x, y);

        send_move_request();
    }

    /* frame implementation */
    virtual wf::geometry_t expand_wm_geometry(
        wf::geometry_t contained_wm_geometry) override
    {
        contained_wm_geometry.x -= thickness;
        contained_wm_geometry.y -= titlebar;
        contained_wm_geometry.width += 2 * thickness;
        contained_wm_geometry.height += thickness + titlebar;

        return contained_wm_geometry;
    }

    virtual void calculate_resize_size(
        int& target_width, int& target_height) override
    {
        target_width -= 2 * thickness;
        target_height -= thickness + titlebar;

        target_width = std::max(target_width, 1);
        target_height = std::max(target_height, 1);
    }

    virtual void notify_view_activated(bool active) override
    {
        if (this->active != active)
            view->damage();

        this->active = active;
    }

    virtual void notify_view_resized(wf::geometry_t view_geometry) override
    {
        view->damage();

        if (tex != (uint32_t)-1)
        {
            GL_CALL(glDeleteTextures(1, &tex));
        }

        tex = -1;

        width = view_geometry.width;
        height = view_geometry.height;
        update_frame_region();

        view->damage();
    };

    virtual void notify_view_tiled() override
    { }

    void update_decoration_size()
    {
        if (view->fullscreen)
        {
            thickness = 0;
            titlebar = 0;
        } else
        {
            thickness = normal_thickness;
            titlebar = titlebar_thickness;
        }
    }

    virtual void notify_view_fullscreen() override
    {
        update_decoration_size();
        update_frame_region();
    };
};

void init_view(wayfire_view view)
{
    auto surf = new simple_decoration_surface(view);
    view->set_decoration(surf);
    view->damage();
}
