#include <plugin.hpp>
#include <signal-definitions.hpp>
#include <core.hpp>
#include <view-transform.hpp>
#include <workspace-manager.hpp>
#include <nonstd/make_unique.hpp>
#include <render-manager.hpp>

extern "C"
{
#include "wobbly.h"
}

#include "wobbly-signal.hpp"

namespace wobbly_graphics
{
    namespace
    {
        const char* vertex_source = R"(
#version 100
attribute mediump vec2 position;
attribute mediump vec2 uvPosition;
varying highp vec2 uvpos;
uniform mat4 MVP;

void main() {
    gl_Position = MVP * vec4(position.xy, 0.0, 1.0);
    uvpos = uvPosition;
}
)";

        const char *frag_source = R"(
#version 100
varying highp vec2 uvpos;
uniform sampler2D smp;

void main()
{
    gl_FragColor = texture2D(smp, uvpos);
}
)";
    }

    GLuint program, uvID, posID, mvpID;

    int times_loaded = 0;

    void load_program()
    {
        if (times_loaded++ > 0)
            return;

        /* viewport dimensions don't matter really, we just care to get the proper GL context */
        wlr_renderer_begin(core->renderer, 10, 10);

        auto vs = OpenGL::compile_shader(vertex_source, GL_VERTEX_SHADER);
        auto fs = OpenGL::compile_shader(frag_source, GL_FRAGMENT_SHADER);

        program = GL_CALL(glCreateProgram());
        GL_CALL(glAttachShader(program, vs));
        GL_CALL(glAttachShader(program, fs));
        GL_CALL(glLinkProgram(program));

        uvID  = GL_CALL(glGetAttribLocation(program, "uvPosition"));
        posID = GL_CALL(glGetAttribLocation(program, "position"));
        mvpID = GL_CALL(glGetUniformLocation(program, "MVP"));

        GL_CALL(glDeleteShader(vs));
        GL_CALL(glDeleteShader(fs));

        wlr_renderer_end(core->renderer);
    }

    void destroy_program()
    {
        if (--times_loaded == 0)
        {
            wlr_renderer_begin(core->renderer, 10, 10);
            GL_CALL(glDeleteProgram(program));
            wlr_renderer_end(core->renderer);
        }
    }

    void render_triangles(GLuint tex, glm::mat4 mat, float *pos, float *uv, int cnt)
    {
        GL_CALL(glUseProgram(program));
        GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
        GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));

        GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
        GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
        GL_CALL(glBindTexture(GL_TEXTURE_2D, tex));
        GL_CALL(glActiveTexture(GL_TEXTURE0));

        GL_CALL(glVertexAttribPointer(posID, 2, GL_FLOAT, GL_FALSE, 0, pos));
        GL_CALL(glEnableVertexAttribArray(posID));

        GL_CALL(glVertexAttribPointer(uvID, 2, GL_FLOAT, GL_FALSE, 0, uv));
        GL_CALL(glEnableVertexAttribArray(uvID));

        GL_CALL(glUniformMatrix4fv(mvpID, 1, GL_FALSE, &mat[0][0]));
        GL_CALL(glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA));

        GL_CALL(glDrawArrays (GL_TRIANGLES, 0, 3 * cnt));

        GL_CALL(glDisableVertexAttribArray(uvID));
        GL_CALL(glDisableVertexAttribArray(posID));
    }
};

namespace wobbly_settings
{
    wf_option friction, spring_k, mass, resolution;

    void init(wayfire_config *config)
    {
        auto section = config->get_section("wobbly");
        friction = section->get_option("friction", "3");
        spring_k = section->get_option("spring_k", "8");
        mass = section->get_option("mass", "50");
        resolution = section->get_option("grid_resolution", "6");
    };
};

extern "C"
{
    int wobbly_settings_get_friction()
    {
        return wobbly_settings::friction->as_cached_int();
    }
    int wobbly_settings_get_spring_k()
    {
        return wobbly_settings::spring_k->as_cached_int();
    }
    int wobbly_settings_get_mass()
    {
        return wobbly_settings::mass->as_cached_int();
    }
}

static uint32_t get_time()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000ll + ts.tv_nsec / 1000000ll;
}

class wf_wobbly : public wf_view_transformer_t
{
    wayfire_view view;
    effect_hook_t pre_hook;
    signal_callback_t view_removed, view_geometry_changed;
    wayfire_grab_interface iface;

    std::unique_ptr<wobbly_surface> model;

    bool has_active_grab = false;
    int grab_x = 0, grab_y = 0;

    wlr_box last_boundingbox;
    wf_geometry snapped_geometry;
    uint32_t last_frame;

    public:
    wf_wobbly(wayfire_view view, wayfire_grab_interface iface)
    {
        this->iface = iface;
        this->view = view;
        model = nonstd::make_unique<wobbly_surface> ();
        auto g = view->get_bounding_box();
        last_boundingbox = g;

        model->x = g.x;
        model->y = g.y;
        model->width = g.width;
        model->height = g.height;

        model->grabbed = 0;
        model->synced = 1;

        model->x_cells = wobbly_settings::resolution->as_cached_int();
        model->y_cells = wobbly_settings::resolution->as_cached_int();

        model->v = NULL;
        model->uv = NULL;

        last_frame = get_time();
        wobbly_init(model.get());

        pre_hook = [=] () {
            update_model();
        };
        view->get_output()->render->add_effect(&pre_hook, WF_OUTPUT_EFFECT_PRE);

        view_removed = [=] (signal_data *data)
        {
            if (get_signaled_view(data) == view)
                destroy_self();
        };

        view_geometry_changed = [=] (signal_data *data)
        {
            if (get_signaled_view(data) == view)
            {
                auto sig = static_cast<view_geometry_changed_signal*> (data);
                update_view_geometry(sig->old_geometry);
            }
        };

        view->get_output()->connect_signal("detach-view", &view_removed);
        view->get_output()->connect_signal("unmap-view", &view_removed);
        view->get_output()->connect_signal("view-geometry-changed", &view_geometry_changed);
        view->get_output()->activate_plugin(iface);
    }

    uint32_t get_z_order() { return WF_TRANSFORMER_HIGHLEVEL; }

    virtual wlr_box get_bounding_box(wf_geometry, wf_geometry)
    {
        auto box = wobbly_boundingbox(model.get());

        wlr_box result;
        result.x = box.tlx;
        result.y = box.tly;
        result.width = std::ceil(box.brx - box.tlx);
        result.height = std::ceil(box.bry - box.tly);

        return result;
    }

    virtual wf_point local_to_transformed_point(wf_geometry view, wf_point point) { return {0, 0}; }
    virtual wf_point transformed_to_local_point(wf_geometry view, wf_point point)
    {
        return point;
    }

    void update_model()
    {
        view->damage();
        if (snapped_geometry.width <= 0)
            resize(last_boundingbox.width, last_boundingbox.height);

        auto now = get_time();
        wobbly_prepare_paint(model.get(), now - last_frame);
        last_frame = now;

        wobbly_add_geometry(model.get());
        wobbly_done_paint(model.get());

        view->damage();

        if (snapped_geometry.width <= 0 && !has_active_grab)
        {
            auto wm = view->get_wm_geometry();
            view->move(model->x + wm.x - last_boundingbox.x, model->y + wm.y - last_boundingbox.y, false);
        }

        if (!has_active_grab && model->synced)
            destroy_self();
    }

    virtual void render_with_damage(uint32_t src_tex, wlr_box src_box,
                            wlr_box scissor_box, const wf_framebuffer& target_fb)
    {
        last_boundingbox = src_box;
        target_fb.bind();
        target_fb.scissor(scissor_box);

        auto ortho = glm::ortho(1.0f * target_fb.geometry.x, 1.0f * target_fb.geometry.x + 1.0f * target_fb.geometry.width,
                                1.0f * target_fb.geometry.y + 1.0f * target_fb.geometry.height, 1.0f * target_fb.geometry.y);

        float x = src_box.x, y = src_box.y, w = src_box.width, h = src_box.height;

        std::vector<float> vert, uv;
        std::vector<int> idx;

        int per_row = model->x_cells + 1;

        for (int j = 0; j < model->y_cells; j++)
        {
            for (int i = 0; i < model->x_cells; i++)
            {
                idx.push_back(i * per_row + j);
                idx.push_back((i + 1) * per_row + j + 1);
                idx.push_back(i * per_row + j + 1);

                idx.push_back(i * per_row + j);
                idx.push_back((i + 1) * per_row + j);
                idx.push_back((i + 1) * per_row + j + 1);
            }
        }

        if (!model->v || !model->uv)
        {
            for (auto id : idx)
            {
                float tile_w = w / model->x_cells;
                float tile_h = h / model->y_cells;

                int i = id / per_row;
                int j = id % per_row;

                vert.push_back(i * tile_w + x);
                vert.push_back(j * tile_h + y);

                uv.push_back(1.0f * i / model->x_cells);
                uv.push_back(1.0f - 1.0f * j / model->y_cells);
            }
        } else
        {
            for (auto i : idx)
            {
                vert.push_back(model->v[2 * i]);
                vert.push_back(model->v[2 * i + 1]);

                uv.push_back(model->uv[2 * i]);
                uv.push_back(model->uv[2 * i + 1]);
            }
        }

        wobbly_graphics::render_triangles(src_tex, target_fb.transform * ortho,
                                          vert.data(), uv.data(),
                                          model->x_cells * model->y_cells * 2);
    }

    void start_grab(int x, int y)
    {
        grab_x = x;
        grab_y = y;
        has_active_grab = 1;
        wobbly_grab_notify(model.get(), x, y);
        unsnap();
    }

    void move(int x, int y)
    {
        wobbly_move_notify(model.get(), x - grab_x, y - grab_y);
        grab_x = x;
        grab_y = y;
    }

    void resize(int w, int h)
    {
        model->width = w;
        model->height = h;
        wobbly_resize_notify(model.get());
    }

    void end_grab(bool unanchor)
    {
        if (has_active_grab && unanchor)
            wobbly_ungrab_notify(model.get());
        has_active_grab = false;
    }

    void snap(wf_geometry geometry)
    {
        wobbly_force_geometry(model.get(), geometry.x, geometry.y, geometry.width, geometry.height);
        snapped_geometry = geometry;
    }

    void unsnap()
    {
        wobbly_unenforce_geometry(model.get());
        snapped_geometry.width = -1;
    }

    void translate(int dx, int dy)
    {
        wobbly_translate(model.get(), dx, dy);
    }

    void destroy_self()
    {
        view->pop_transformer("wobbly");
    }

    void update_view_geometry(wf_geometry old_geometry)
    {
        if (has_active_grab)
            return;

        auto wm = view->get_wm_geometry();

        int dx = wm.x - old_geometry.x;
        int dy = wm.y - old_geometry.y;
        translate(dx, dy);
        last_boundingbox.x += dx;
        last_boundingbox.y += dy;
    }

    virtual ~wf_wobbly()
    {
        wobbly_fini(model.get());
        view->get_output()->deactivate_plugin(iface);
        view->get_output()->render->rem_effect(&pre_hook, WF_OUTPUT_EFFECT_PRE);
        view->get_output()->disconnect_signal("view-geometry-changed", &view_geometry_changed);
        view->get_output()->disconnect_signal("detach-view", &view_removed);
        view->get_output()->disconnect_signal("unmap-view", &view_removed);
    }
};

class wayfire_wobbly : public wayfire_plugin_t
{
    signal_callback_t wobbly_changed;
    public:
        void init(wayfire_config *config)
        {
            wobbly_settings::init(config);
            grab_interface->abilities_mask = 0;
            grab_interface->name = "wobbly";

            wobbly_changed = [=] (signal_data *data)
            {
                adjust_wobbly(static_cast<wobbly_signal*> (data));
            };

            output->connect_signal("wobbly-event", &wobbly_changed);

            wobbly_graphics::load_program();
        }

        void adjust_wobbly(wobbly_signal *data)
        {
            if (data->view->get_output() != output)
                return;

            if ((data->events & (WOBBLY_EVENT_GRAB | WOBBLY_EVENT_SNAP))
                && data->view->get_transformer("wobbly") == nullptr)
                data->view->add_transformer(nonstd::make_unique<wf_wobbly> (data->view, grab_interface), "wobbly");

            auto wobbly = dynamic_cast<wf_wobbly*> (data->view->get_transformer("wobbly").get());
            if (!wobbly)
                return;

            log_info("wobbly event %d", data->events);
            if (data->events & WOBBLY_EVENT_GRAB)
                wobbly->start_grab(data->grab_x, data->grab_y);

            if (data->events & WOBBLY_EVENT_MOVE)
                wobbly->move(data->geometry.x, data->geometry.y);

            if (data->events & WOBBLY_EVENT_END)
                wobbly->end_grab(data->unanchor);

            if (data->events & WOBBLY_EVENT_SNAP)
                wobbly->snap(data->geometry);

            if (data->events & WOBBLY_EVENT_UNSNAP)
                wobbly->unsnap();

            if (data->events & WOBBLY_EVENT_TRANSLATE)
                wobbly->translate(data->geometry.x, data->geometry.y);
        }

        void fini()
        {
            output->workspace->for_each_view([] (wayfire_view view)
            {
                auto wobbly = dynamic_cast<wf_wobbly*> (view->get_transformer("wobbly").get());
                if (wobbly)
                    wobbly->destroy_self();
            }, WF_ALL_LAYERS);

            wobbly_graphics::destroy_program();
            output->disconnect_signal("wobbly-event", &wobbly_changed);
        }
};

extern "C"
{
    wayfire_plugin_t *newInstance()
    {
        return new wayfire_wobbly;
    }
};
