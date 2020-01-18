#include <wayfire/plugin.hpp>
#include <wayfire/signal-definitions.hpp>
#include <wayfire/core.hpp>
#include <wayfire/view-transform.hpp>
#include <wayfire/workspace-manager.hpp>
#include <wayfire/render-manager.hpp>

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
@builtin_ext@

varying highp vec2 uvpos;
@builtin@

void main()
{
    gl_FragColor = get_pixel(uvpos);
}
)";
}

OpenGL::program_t program;
int times_loaded = 0;

void load_program()
{
    if (times_loaded++ > 0)
        return;

    OpenGL::render_begin();
    program.compile(vertex_source, frag_source);
    OpenGL::render_end();
}

void destroy_program()
{
    if (--times_loaded == 0)
    {
        OpenGL::render_begin();
        program.free_resources();
        OpenGL::render_end();
    }
}

/**
 * Enumerate the needed triangles for rendering the model
 */
void prepare_geometry(wobbly_surface *model, wf::geometry_t src_box,
    std::vector<float>& vert, std::vector<float>& uv)
{
    float x = src_box.x, y = src_box.y, w = src_box.width, h = src_box.height;
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
}

/* Requires bound opengl context */
void render_triangles(wf::texture_t tex, glm::mat4 mat, float *pos, float *uv, int cnt)
{
    program.use(tex.type);
    program.set_active_texture(tex);

    program.attrib_pointer("position", 2, 0, pos);
    program.attrib_pointer("uvPosition", 2, 0, uv);
    program.uniformMatrix4f("MVP", mat);

    GL_CALL(glEnable(GL_BLEND));
    GL_CALL(glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA));

    GL_CALL(glDrawArrays (GL_TRIANGLES, 0, 3 * cnt));
    GL_CALL(glDisable(GL_BLEND));

    program.deactivate();
}
};

namespace wobbly_settings
{
    wf::option_wrapper_t<double> friction{"wobbly/friction"};
    wf::option_wrapper_t<double> spring_k{"wobbly/spring_k"};
    wf::option_wrapper_t<int> resolution{"wobbly/grid_resolution"};
};

extern "C"
{
    double wobbly_settings_get_friction()
    {
        return wf::clamp((double) wobbly_settings::friction,
            MINIMAL_FRICTION, MAXIMAL_FRICTION);
    }

    double wobbly_settings_get_spring_k()
    {
        return wf::clamp((double) wobbly_settings::spring_k,
            MINIMAL_SPRING_K, MAXIMAL_SPRING_K);
    }
}

namespace wf
{
using wobbly_model_t = std::unique_ptr<wobbly_surface>;
static const std::string wobbly_transformer_name = "wobbly";

/**
 * Different states in which wobbly can be
 */
enum ewobbly_state_t
{
    WOBBLY_STATE_FLOATING      = 0,
    WOBBLY_STATE_FREE          = 1,
    WOBBLY_STATE_GRABBED       = 2,
    WOBBLY_STATE_TILED         = 3,
    WOBBLY_STATE_TILED_GRABBED = 4,
};

/**
 * Interface representing the wobbly state.
 */
class iwobbly_state_t
{
  public:
    virtual ~iwobbly_state_t() {}

    /** Called when a grab starts */
    virtual void handle_grab_start(wf::point_t grab, bool takeover) {};

    /** Called when the wobbly grab is moved. */
    virtual void handle_grab_move(wf::point_t grab) {};

    /** Query the last grab point */
    virtual wf::point_t get_grab_position() const { return {0, 0}; };

    /**
     * Called when the wobbly grab is ended.
     * @param release_grab Whether to remove the grabbed object in the model.
     */
    virtual void handle_grab_end(bool release_grab) {};

    /** Called when the next frame is being prepared */
    virtual void handle_frame()
    {
        this->bounding_box = view->get_bounding_box("wobbly");
        this->wm_geometry = view->get_wm_geometry();
    }

    /** Called when the view wm geometry changes */
    virtual void handle_wm_geometry(const wf::geometry_t& old_wm_geometry) { }

    /** @return true if the wobbly animation is done. */
    virtual bool is_wobbly_done() const
    {
        return model->synced;
    }

    /** @return the current state of wobbly */
    virtual ewobbly_state_t get_wobbly_state() const = 0;

    /**
     * This isn't really meant to be used standalone, only subclasses should
     * be instantiated.
     */
    iwobbly_state_t(const wobbly_model_t& m, wayfire_view v)
        : view(v), model(m)
    {
        wm_geometry = v->get_wm_geometry();
        bounding_box = {0, 0, 1, 1};
    }

    /**
     * Translate the model by the given offset
     */
    virtual void translate_model(int dx, int dy)
    {
        wobbly_translate(model.get(), dx, dy);
        wobbly_add_geometry(model.get());

        wm_geometry.x += dx;
        wm_geometry.y += dy;
        bounding_box.x += dx;
        bounding_box.y += dy;
    }

  protected:
    wayfire_view view;
    const wobbly_model_t& model;
    wf::geometry_t wm_geometry;
    wf::geometry_t bounding_box;
};

/**
 * Determines the behavior of the wobbly model when the view is grabbed,
 * for ex. when moving or resizing.
 */
class wobbly_state_grabbed_t : public iwobbly_state_t
{
  public:
    using iwobbly_state_t::iwobbly_state_t;
    virtual void handle_grab_start(wf::point_t grab, bool takeover) override
    {
        this->last_grab = {(int)grab.x, (int)grab.y};
        if (!takeover)
            wobbly_grab_notify(model.get(), last_grab.x, last_grab.y);
    }

    virtual wf::point_t get_grab_position() const override
    {
        return this->last_grab;
    }

    /** @return the current state of wobbly */
    virtual ewobbly_state_t get_wobbly_state() const override
    {
        return WOBBLY_STATE_GRABBED;
    }

    virtual void handle_grab_end(bool release_grab) override
    {
        if (release_grab)
            wobbly_ungrab_notify(model.get());
    }

    virtual void translate_model(int dx, int dy) override
    {
        iwobbly_state_t::translate_model(dx, dy);
        this->last_grab.x += dx;
        this->last_grab.y += dy;
    }

    void handle_frame() override
    {
        auto old_bbox = bounding_box;
        iwobbly_state_t::handle_frame();

        if (old_bbox != bounding_box)
        {
            /* Directly accept new size, but keep position,
             * because it is managed by the grab. */
            wobbly_resize(model.get(), bounding_box.width, bounding_box.height);
        }
    }

  protected:
    wf::point_t last_grab;
    void handle_grab_move(wf::point_t grab) override
    {
        wobbly_move_notify(model.get(), grab.x, grab.y);
        this->last_grab = {(int)grab.x, (int)grab.y};
    }

    bool is_wobbly_done() const override
    {
        return false;
    }
};

static void wobbly_tiled_state_handle_frame(const wobbly_model_t& model,
    const wf::geometry_t& old_bbox, const wf::geometry_t& new_bbox)
{
    if (new_bbox != old_bbox)
    {
        /* Bounding box (excluding the wobbly transformer) changed, this
         * means the view got resized/moved by something outside of wobbly.
         * Adjust the geometry. */
        wobbly_force_geometry(model.get(), new_bbox.x, new_bbox.y,
            new_bbox.width, new_bbox.height);
    }
}

/**
 * Determines the behavior of the wobbly model when the view is tiled or
 * fullscreen, i.e the view keeps its geometry where it was put.
 */
class wobbly_state_tiled_t : public iwobbly_state_t
{
  public:
    using iwobbly_state_t::iwobbly_state_t;

    void handle_frame() override
    {
        auto old_bbox = bounding_box;
        iwobbly_state_t::handle_frame();
        wobbly_tiled_state_handle_frame(model, old_bbox, bounding_box);
    }

    virtual ~wobbly_state_tiled_t()
    {
        wobbly_unenforce_geometry(model.get());
    }

    ewobbly_state_t get_wobbly_state() const override
    {
        return WOBBLY_STATE_TILED;
    }
};

/**
 * Determines the behavior of the wobbly model when the view is tiled or
 * fullscreen, i.e the view keeps its geometry where it was put, and it has
 * an active grab at the same time.
 *
 * This is basically a combination of tiled and grabbed.
 */
class wobbly_state_tiled_grabbed_t : public wobbly_state_grabbed_t
{
  public:
    using wobbly_state_grabbed_t::wobbly_state_grabbed_t;

    void handle_frame() override
    {
        auto old_bbox = bounding_box;
        iwobbly_state_t::handle_frame();
        wobbly_tiled_state_handle_frame(model, old_bbox, bounding_box);
    }

    virtual ~wobbly_state_tiled_grabbed_t()
    {
        wobbly_unenforce_geometry(model.get());
    }

    ewobbly_state_t get_wobbly_state() const override
    {
        return WOBBLY_STATE_TILED_GRABBED;
    }
};

/**
 * Determines the behavior of the wobbly model when the view is wobblying freely
 * without being grabbed or tiled. In this state, the model dictates the
 * position of the view.
 */
class wobbly_state_floating_t : public iwobbly_state_t
{
  public:
    using iwobbly_state_t::iwobbly_state_t;

  protected:
    void handle_frame() override
    {
        /* Synchronize view position with the model */
        auto new_bbox = view->get_bounding_box(wobbly_transformer_name);
        auto wm = view->get_wm_geometry();

        int target_x = model->x + wm.x - new_bbox.x;
        int target_y = model->y + wm.y - new_bbox.y;
        if (target_x != wm.x || target_y != wm.y)
            view->move(model->x + wm.x - new_bbox.x, model->y + wm.y - new_bbox.y);

        if (new_bbox.width != this->bounding_box.width ||
            new_bbox.height != this->bounding_box.height)
        {
            wobbly_resize(model.get(), new_bbox.width, new_bbox.height);
        }

        this->bounding_box = new_bbox;
        this->wm_geometry = wm;
    }

    void handle_wm_geometry(const wf::geometry_t& old_wm) override
    {
        auto bbox = view->get_bounding_box("wobbly");
        wobbly_set_top_anchor(model.get(),
            bbox.x, bbox.y, bbox.width, bbox.height);
    }

    ewobbly_state_t get_wobbly_state() const override
    {
        return WOBBLY_STATE_FLOATING;
    }
};

/**
 * Determines the behavior of wobbly when the view is not grabbed or tiled,
 * but the model should keep the true origin of the view.
 */
class wobbly_state_free_t : public iwobbly_state_t
{
  public:
    using iwobbly_state_t::iwobbly_state_t;

  protected:
    void handle_frame() override
    {
        auto old_bbox = bounding_box;
        iwobbly_state_t::handle_frame();

        if (old_bbox != bounding_box)
        {
            wobbly_set_top_anchor(model.get(), bounding_box.x, bounding_box.y,
                bounding_box.width, bounding_box.height);
            wobbly_resize(model.get(), bounding_box.width, bounding_box.height);
        }
    }

    ewobbly_state_t get_wobbly_state() const override
    {
        return WOBBLY_STATE_FREE;
    }
};
}

class wf_wobbly : public wf::view_transformer_t
{
    wayfire_view view;
    wf::effect_hook_t pre_hook;

    wf::signal_callback_t view_removed = [=] (wf::signal_data_t *) {
        destroy_self();
    };

    wf::signal_callback_t view_state_changed = [=] (wf::signal_data_t *) {
        update_wobbly_state(false, {0, 0}, false);
    };

    wf::signal_callback_t view_geometry_changed = [=] (wf::signal_data_t *data)
    {
        auto sig = static_cast<view_geometry_changed_signal*> (data);
        state->handle_wm_geometry(sig->old_geometry);
    };

    wf::signal_callback_t view_output_changed = [=] (wf::signal_data_t *data)
    {
        auto sig = static_cast<_output_signal*> (data);

        if (!view->get_output())
            return destroy_self();

        /* Wobbly is active only when there's already been an output */
        assert(sig->output);

        /* Translate wobbly when its output changes */
        auto old_geometry = sig->output->get_layout_geometry();
        auto new_geometry = view->get_output()->get_layout_geometry();
        state->translate_model(old_geometry.x - new_geometry.x,
            old_geometry.y - new_geometry.y);

        sig->output->render->rem_effect(&pre_hook);
        view->get_output()->render->add_effect(&pre_hook,
            wf::OUTPUT_EFFECT_PRE);
    };

    const wf::plugin_grab_interface_uptr& iface;

    std::unique_ptr<wobbly_surface> model;
    std::unique_ptr<wf::iwobbly_state_t> state;
    uint32_t last_frame;

    void init_model()
    {
        model = std::make_unique<wobbly_surface> ();
        auto g = view->get_bounding_box();

        model->x = g.x;
        model->y = g.y;
        model->width = g.width;
        model->height = g.height;

        model->grabbed = 0;
        model->synced = 1;

        model->x_cells = wobbly_settings::resolution;
        model->y_cells = wobbly_settings::resolution;

        model->v = NULL;
        model->uv = NULL;
        wobbly_init(model.get());
    }

  public:
    wf_wobbly(wayfire_view view, const wf::plugin_grab_interface_uptr& _iface)
        : iface(_iface)
    {
        this->view = view;
        init_model();
        last_frame = wf::get_current_time();

        pre_hook = [=] () { update_model(); };
        view->get_output()->render->add_effect(&pre_hook, wf::OUTPUT_EFFECT_PRE);

        view->connect_signal("unmap", &view_removed);
        view->connect_signal("tiled", &view_state_changed);
        view->connect_signal("fullscreen", &view_state_changed);
        view->connect_signal("set-output", &view_output_changed);
        view->connect_signal("geometry-changed", &view_geometry_changed);

        /* Set to free state initially but then look for the correct state */
        this->state = std::make_unique<wf::wobbly_state_free_t> (model, view);
        update_wobbly_state(false, {0, 0}, false);
    }


    uint32_t get_z_order() override { return wf::TRANSFORMER_HIGHLEVEL; }
    wlr_box get_bounding_box(wf::geometry_t, wf::geometry_t) override
    {
        auto box = wobbly_boundingbox(model.get());

        wlr_box result;
        result.x = box.tlx;
        result.y = box.tly;
        result.width = std::ceil(box.brx - box.tlx);
        result.height = std::ceil(box.bry - box.tly);

        return result;
    }

    wf::pointf_t transform_point(
        wf::geometry_t view, wf::pointf_t point) override
    {
        return point;
    }

    wf::pointf_t untransform_point(
        wf::geometry_t view, wf::pointf_t point) override
    {
        return point;
    }

    void update_model()
    {
        view->damage();

        /* It is possible that the wobbly state needs to adjust view geometry.
         * We do not want it to get feedback from itself */
        view->disconnect_signal("geometry-changed",
            &this->view_geometry_changed);
        state->handle_frame();
        view->connect_signal("geometry-changed", &this->view_geometry_changed);

        /* Update all the wobbly model */
        auto now = wf::get_current_time();
        wobbly_prepare_paint(model.get(), now - last_frame);

        /* Update wobbly geometry */
        last_frame = now;
        wobbly_add_geometry(model.get());
        wobbly_done_paint(model.get());
        view->damage();

        if (state->is_wobbly_done())
            destroy_self();
    }

    void render_box(wf::texture_t src_tex, wlr_box src_box,
        wlr_box scissor_box, const wf::framebuffer_t& target_fb) override
    {
        OpenGL::render_begin(target_fb);
        target_fb.scissor(scissor_box);

        std::vector<float> vert, uv;
        wobbly_graphics::prepare_geometry(model.get(), src_box, vert, uv);
        wobbly_graphics::render_triangles(src_tex,
            target_fb.get_orthographic_projection(),
            vert.data(), uv.data(),
            model->x_cells * model->y_cells * 2);

        OpenGL::render_end();
    }

    /**
     * Update the current wobbly state based on:
     * 1. View state (tiled & fullscreen)
     * 2. Current wobbly state (grabbed or not)
     * 3. Whether we are starting a grab, or ending a grab.
     *
     * @param start_grab Whether to start a new grab at @grab
     * @param grab The position of the starting grab.
     * @param end_grab Whether to end an existing grab.
     */
    void update_wobbly_state(bool start_grab, wf::point_t grab, bool end_grab)
    {
        bool tiled = view->tiled_edges || view->fullscreen;
        bool was_grabbed =
            (state->get_wobbly_state() == wf::WOBBLY_STATE_GRABBED ||
            state->get_wobbly_state() == wf::WOBBLY_STATE_TILED_GRABBED);
        bool grabbed = (start_grab || was_grabbed) && !end_grab;

        uint32_t next_state_mask = 0;
        if (tiled && grabbed) {
            next_state_mask = wf::WOBBLY_STATE_TILED_GRABBED;
        } else if (tiled) {
            next_state_mask = wf::WOBBLY_STATE_TILED;
        } else if (grabbed) {
            next_state_mask = wf::WOBBLY_STATE_GRABBED;
        } else if (was_grabbed ||
            state->get_wobbly_state() == wf::WOBBLY_STATE_FLOATING) {
            /* If previously grabbed, we can let the view float freely */
            next_state_mask = wf::WOBBLY_STATE_FLOATING;
        } else {
            /* Otherwise, we need to keep the position */
            next_state_mask = wf::WOBBLY_STATE_FREE;
        }
        if (next_state_mask == state->get_wobbly_state())
            return;

        std::unique_ptr<wf::iwobbly_state_t> next_state;
        switch (next_state_mask)
        {
            case wf::WOBBLY_STATE_FREE:
                next_state = std::make_unique<
                    wf::wobbly_state_free_t> (model, view);
                break;

            case wf::WOBBLY_STATE_FLOATING:
                next_state = std::make_unique<
                    wf::wobbly_state_floating_t> (model, view);
                break;

            case wf::WOBBLY_STATE_TILED:
                next_state = std::make_unique<
                    wf::wobbly_state_tiled_t> (model, view);
                break;

            case wf::WOBBLY_STATE_GRABBED:
                next_state = std::make_unique<
                    wf::wobbly_state_grabbed_t> (model, view);
                break;

            case wf::WOBBLY_STATE_TILED_GRABBED:
                next_state = std::make_unique<
                    wf::wobbly_state_tiled_grabbed_t> (model, view);
                break;
            default:
                /* Not reached except by a bug */
                assert(false);
        }

        if (was_grabbed)
            this->state->handle_grab_end(end_grab);

        if (grabbed)
        {
            if (was_grabbed)
                grab = this->state->get_grab_position();

            next_state->handle_grab_start(grab, was_grabbed);
        }

        /* New state has been set up */
        this->state = std::move(next_state);
    }

    void start_grab(int x, int y)
    {
        update_wobbly_state(true, {x, y}, false);
    }

    void move(wf::point_t point)
    {
        state->handle_grab_move(point);
    }

    void end_grab()
    {
        update_wobbly_state(false, {0, 0}, true);
    }

    void wobble()
    {
        wobbly_slight_wobble(model.get());
        model->synced = 0;
    }

    void destroy_self()
    {
        view->pop_transformer("wobbly");
    }

    virtual ~wf_wobbly()
    {
        state = nullptr;
        wobbly_fini(model.get());
        view->get_output()->render->rem_effect(&pre_hook);

        view->disconnect_signal("unmap", &view_removed);
        view->disconnect_signal("tiled", &view_state_changed);
        view->disconnect_signal("fullscreen", &view_state_changed);
        view->disconnect_signal("set-output", &view_output_changed);
        view->disconnect_signal("geometry-changed", &view_geometry_changed);
    }
};

class wayfire_wobbly : public wf::plugin_interface_t
{
    wf::signal_callback_t wobbly_changed;
    public:
        void init() override
        {
            grab_interface->capabilities = 0;
            grab_interface->name = "wobbly";

            wobbly_changed = [=] (wf::signal_data_t *data)
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

            if ((data->events & (WOBBLY_EVENT_GRAB | WOBBLY_EVENT_ACTIVATE))
                && data->view->get_transformer("wobbly") == nullptr)
            {
                data->view->add_transformer(
                    std::make_unique<wf_wobbly> (data->view, grab_interface),
                    "wobbly");
            }

            auto wobbly = dynamic_cast<wf_wobbly*> (
                data->view->get_transformer("wobbly").get());
            if (!wobbly)
                return;

            if (data->events & WOBBLY_EVENT_ACTIVATE)
                wobbly->wobble();

            if (data->events & WOBBLY_EVENT_GRAB)
                wobbly->start_grab(data->grab_x, data->grab_y);

            if (data->events & WOBBLY_EVENT_MOVE)
                wobbly->move({data->grab_x, data->grab_y});

            if (data->events & WOBBLY_EVENT_END)
                wobbly->end_grab();
        }

        void fini() override
        {
            for (auto& view : output->workspace->get_views_in_layer(wf::ALL_LAYERS))
            {
                auto wobbly = dynamic_cast<wf_wobbly*> (view->get_transformer("wobbly").get());
                if (wobbly)
                    wobbly->destroy_self();
            }

            wobbly_graphics::destroy_program();
            output->disconnect_signal("wobbly-event", &wobbly_changed);
        }
};

DECLARE_WAYFIRE_PLUGIN(wayfire_wobbly);
