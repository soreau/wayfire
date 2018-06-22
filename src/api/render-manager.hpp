#ifndef RENDER_MANAGER_HPP
#define RENDER_MANAGER_HPP

#include "plugin.hpp"
#include <vector>
#include <pixman.h>

namespace OpenGL { struct context_t; }

/* Workspace streams are used if you need to continuously render a workspace
 * to a texture, for example if you call texture_from_viewport at every frame */
struct wf_workspace_stream
{
    std::tuple<int, int> ws;
    uint fbuff, tex;
    bool running = false;

    float scale_x, scale_y;
};

enum wf_output_effect_type
{
    WF_OUTPUT_EFFECT_PRE = 0,
    WF_OUTPUT_EFFECT_OVERLAY = 1,
    WF_OUTPUT_EFFECT_POST = 2,
    WF_OUTPUT_EFFECT_TOTAL = 3
};

/* effect hooks are called after main rendering */
using effect_hook_t = std::function<void()>;

/* post hooks are used for postprocessing. the first two params
 * are the source framebuffer and the source texture, and the third
 * is the target fbo which you should write to */
using post_hook_t = std::function<void(uint32_t, uint32_t, uint32_t)>;

/* render hooks are used when a plugin requests to draw the whole desktop on their own
 * example plugin is cube. The parameter they take is the target framebuffer */
using render_hook_t = std::function<void(uint32_t)>;


struct wf_output_damage;
class render_manager
{

    friend void redraw_idle_cb(void *data);
    friend void damage_idle_cb(void *data);
    friend void frame_cb (wl_listener*, void *data);

    private:
        wayfire_output *output;
        wl_event_source *idle_redraw_source = NULL, *idle_damage_source = NULL;

        wl_listener frame_listener;

        signal_callback_t output_resized;

        bool dirty_context = true;
        void load_context();
        void release_context();

        bool draw_overlay_panel = true;

        pixman_region32_t frame_damage;
        std::unique_ptr<wf_output_damage> output_damage;

        std::vector<std::vector<wf_workspace_stream>> output_streams;
        wf_workspace_stream *current_ws_stream = nullptr;

        void get_ws_damage(std::tuple<int, int> ws, pixman_region32_t *out_damage);

        using effect_container_t = std::vector<effect_hook_t*>;
        effect_container_t effects[WF_OUTPUT_EFFECT_TOTAL];

        struct wf_post_effect;
        /* TODO: use unique_ptr */
        using post_container_t = std::vector<wf_post_effect*>;
        post_container_t post_effects;

        uint32_t default_fb = 0, default_tex = 0;

        int constant_redraw = 0;
        int output_inhibit = 0;
        render_hook_t renderer;

        void paint();
        void post_paint();

        void run_effects(effect_container_t&);

        void _rem_post(wf_post_effect *hook);
        void cleanup_post_hooks();

        void init_default_streams();

    public:
        OpenGL::context_t *ctx;

        render_manager(wayfire_output *o);
        ~render_manager();

        void set_renderer(render_hook_t rh = nullptr);
        void reset_renderer();

        /* schedule repaint immediately after finishing the last one
         * to undo, call auto_redraw(false) as much times as auto_redraw(true) was called */
        void auto_redraw(bool redraw);
        void schedule_redraw();
        void set_hide_overlay_panels(bool set);

        void add_inhibit(bool add);

        void add_effect(effect_hook_t*, wf_output_effect_type type);
        void rem_effect(const effect_hook_t*, wf_output_effect_type type);

        void add_post(post_hook_t*);
        void rem_post(post_hook_t*);

        void damage(const wlr_box& box);
        void damage(pixman_region32_t *region);

        void workspace_stream_start(wf_workspace_stream *stream);
        void workspace_stream_update(wf_workspace_stream *stream,
                float scale_x = 1, float scale_y = 1);
        void workspace_stream_stop(wf_workspace_stream *stream);
};

#endif
