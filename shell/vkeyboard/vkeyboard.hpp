#ifndef VKEYBOARD_HPP
#define VKEYBOARD_HPP

#include "window.hpp"
#include <libevdev/libevdev.h>
#include <vector>

struct widget;
class wayfire_config;

class vkeyboard
{
    wl_callback *repaint_callback = nullptr;
    cairo_t *cr;

    uint32_t output;
    uint32_t width, height;

    struct key
    {
        uint32_t code;
        std::string text;
        double target_w;

        key() {}
        key(uint32_t c, std::string t, double tw) : code(c), text(t), target_w(tw) {}

        /* real position, used only in current layout */
        double x = 0, y = 0, w = 0, h = 0;
    };

    using layout = std::vector<std::vector<key> >;

    layout *current_layout = nullptr;
    layout default_layout, shift_layout, numeric_layout;

    void init_layouts();
    void set_layout(layout&);

    void add_callback();

    int cx, cy;
    void input_motion(int x, int y);
    void input_released();

    void handle_action_start(uint32_t code);
    void handle_action_end(uint32_t code, bool finger_up = true);

    wayfire_config *config;

    public:
        wayfire_window *window = nullptr;
        vkeyboard(wayfire_config *config);

        void resize(uint32_t width, uint32_t height);
        void show();
        void render_frame();
};

#endif /* end of include guard: VKEYBOARD_HPP */
