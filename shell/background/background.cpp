#include <memory>
#include <getopt.h>

#include "config.hpp"
#include "window.hpp"

#ifdef BUILD_WITH_PIXBUF
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdk/gdkcairo.h>
#endif


bool g_type_init_ran = false;

static cairo_surface_t *create_dummy_surface(int w, int h)
{
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
    cairo_t *cr = cairo_create(surf);
    cairo_rectangle(cr, 0, 0, w, h);
    cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
    cairo_fill(cr);
    cairo_destroy(cr);

    return surf;
}

static cairo_surface_t *create_cairo_surface_from_file(std::string name, int w, int h)
{

#ifdef BUILD_WITH_PIXBUF

#if !GLIB_CHECK_VERSION(2,35,0)
    if (!g_type_init_ran)
    {
        g_type_init();
        g_type_init_ran = true;
    }
#endif

    auto pbuf = gdk_pixbuf_new_from_file(name.c_str(), NULL);
    if (!pbuf)
    {
        std::cerr << "Failed to create a pbuf. Possibly wrong background path?" << std::endl;
        return create_dummy_surface(w, h);
    }

    int w_ = gdk_pixbuf_get_width(pbuf);
    int h_ = gdk_pixbuf_get_height(pbuf);

    auto surface = cairo_image_surface_create(
        gdk_pixbuf_get_has_alpha(pbuf) ? CAIRO_FORMAT_ARGB32 : CAIRO_FORMAT_RGB24,
        w_, h_);

    auto cr = cairo_create(surface);
    gdk_cairo_set_source_pixbuf(cr, pbuf, 0, 0);
    cairo_paint(cr);

    cairo_destroy(cr);
    g_object_unref(pbuf);

    return surface;
#else
    auto surface = cairo_try_load_png(name.c_str());
    return surface ?: create_dummy_surface(w, h);
#endif
}

class wayfire_background
{
    std::string image;

    cairo_surface_t *img_surface = NULL;
    cairo_t *cr;

    wayfire_output *output;
    wayfire_window *window = nullptr;

    public:
    wayfire_background(wayfire_output *output, std::string image)
    {
        this->image = image;
        this->output = output;
        output->resized_callback = [=] (wayfire_output *output, int w, int h)
        { resize(w, h); };

        output->destroyed_callback = [=] (wayfire_output* output)
        { delete this; };

        zwf_output_v1_inhibit_output(output->zwf);
    }

    void resize(int w, int h)
    {
        if (!img_surface)
            img_surface = create_cairo_surface_from_file(image, w, h);

        if (window)
        {
            /* the first inhibit was called in the constructor */
            zwf_output_v1_inhibit_output(output->zwf);
            delete window;
        }

        window = output->create_window(w, h, [=] () { init_background(w, h); });
    }

    void init_background(int width, int height)
    {
        std::cout << output->zwf << " " << window->surface << " " << ZWF_OUTPUT_V1_WM_ROLE_BACKGROUND << std::endl;
        window->zwf = zwf_output_v1_get_wm_surface(output->zwf, window->surface,
                                                   ZWF_OUTPUT_V1_WM_ROLE_BACKGROUND);
        zwf_wm_surface_v1_configure(window->zwf, 0, 0);

        using namespace std::placeholders;
        window->pointer_enter = std::bind(std::mem_fn(&wayfire_background::on_enter),
                                          this, _1, _2, _3, _4);

        cr = cairo_create(window->cairo_surface);

        double img_w = cairo_image_surface_get_width(img_surface);
        double img_h = cairo_image_surface_get_height(img_surface);

        cairo_rectangle(cr, 0, 0, width, height);
        cairo_scale(cr, width / img_w, height / img_h);
        cairo_set_source_surface(cr, img_surface, 0, 0);
        cairo_fill(cr);

        window->damage_commit();
        zwf_output_v1_inhibit_output_done(output->zwf);
    }

    void on_enter(wl_pointer *ptr, uint32_t serial, int x, int y)
    {
        output->display->show_default_cursor(serial);
    }

    ~wayfire_background()
    {
        std::cout << "destroy background" << std::endl;
        cairo_destroy(cr);
        cairo_surface_destroy(img_surface);

        if (window)
            delete window;
    }
};

wayfire_config *config;
std::map<uint32_t, std::unique_ptr<wayfire_background>> outputs;

std::string bg_path;

/* TODO: share option parsing between panel and background */
int main(int argc, char *argv[])
{
    std::string home_dir = secure_getenv("HOME");
    std::string config_file = home_dir + "/.config/wayfire.ini";

    struct option opts[] = {
        { "config",   required_argument, NULL, 'c' },
        { 0,          0,                 NULL,  0  }
    };

    int c, i;
    while((c = getopt_long(argc, argv, "c:l:", opts, &i)) != -1)
    {
        switch(c)
        {
            case 'c':
                config_file = optarg;
                break;
            default:
                std::cerr << "failed to parse option " << optarg << std::endl;
        }
    }

    config = new wayfire_config(config_file);
    auto section = config->get_section("shell");

    bg_path = section->get_option("background", "none")->as_string();

    auto display = new wayfire_display([=] (wayfire_output *output)
    {
        new wayfire_background(output, bg_path);
    });

    while(true)
    {
        if (wl_display_dispatch(display->display) < 0)
            break;
    }

    delete display;
    return 0;
}
