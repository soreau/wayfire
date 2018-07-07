#include <opengl.hpp>
#include <signal-definitions.hpp>
#include <view.hpp>
#include <view-transform.hpp>
#include <render-manager.hpp>
#include <workspace-manager.hpp>
#include <animation.hpp>

#include <queue>
#include <linux/input-event-codes.h>
#include <algorithm>

enum paint_attribs
{
    UPDATE_SCALE = 1,
    UPDATE_OFFSET = 2,
    UPDATE_ROTATION = 4
};

struct view_paint_attribs
{
    wayfire_view view;
    wf_transition scale_x, scale_y, off_x, off_y, off_z;
    wf_transition rot;

    uint32_t updates;
};

float clamp(float min, float x, float max)
{
    if (x < min) return min;
    if (x > max) return max;
    return x;
}

/* get an appropriate scaling so that a view with dimensions [w, h] takes
 * about c% of the screen with dimensions [sw, sh] and make sure that this scaling
 * won't resize the view too much */
float get_scale_factor(float w, float h, float sw, float sh, float c)
{
    float d = w * w + h * h;
    float sd = sw * sw + sh * sh;

    return clamp(0.66, std::sqrt(sd / d), 1.5) * c;
}

/* This plugin rovides abilities to switch between views.
 * Namely, it runs in several stages:
 * 1. "Fold" - views are moved to the center of the screen(they might overlap)
 *    and all except the focused one are made smaller
 * 2. "Unfold" - views are moved to the left/right and rotated
 * 3. "Rotate" - views are rotated from left to right and vice versa
 * 4. "Reverse unfold"
 * 5. "Reverse fold"
 * */

class wayfire_view_switcher : public wayfire_plugin_t
{
    key_callback init_binding;
    wf_option next_view, prev_view, terminate;
    wf_option activate_key;

    signal_callback_t destroyed;

    wf_duration initial_animation, regular_animation;

#define MAX_ACTIONS 4
    std::queue<int> next_actions;

    struct
    {
        bool active = false;

        bool mod_released = false;
        bool in_fold = false;
        bool in_unfold = false;
        bool in_rotate = false;

        bool reversed_folds = false;

        /* if the user presses alt-tab(assuming this is our binding)
         * and then presses tab several times, holding alt, we assume
         * he/she wants to switch between windows, so we track if this is the case */
        bool in_continuous_switch = false;
    } state;

    size_t current_view_index;

    struct
    {
        float offset = 0.6f;
        float angle = M_PI / 6.;
        float back = 0.3f;
    } attribs;

    effect_hook_t hook;

    std::vector<wayfire_view> views; // all views on current viewport
    std::vector<view_paint_attribs> active_views; // views that are rendered

    wf_option view_scale_config;

    public:

    void init(wayfire_config *config)
    {
        grab_interface->name = "switcher";
        grab_interface->abilities_mask = WF_ABILITY_CONTROL_WM;

        auto section = config->get_section("switcher");

        regular_animation = section->get_option("duration", "250");
        initial_animation = section->get_option("initial_animation", "150");

        view_scale_config = section->get_option("view_thumbnail_size", "0.4");

        activate_key = section->get_option("activate", "<alt> KEY_TAB");

        init_binding = [=] (uint32_t)
        {
            if (!state.active)
            {
                activate();
            } else if (state.mod_released)
            {
                push_exit();
            }
        };

        output->add_key(activate_key, &init_binding);

        using namespace std::placeholders;
        grab_interface->callbacks.keyboard.key = std::bind(std::mem_fn(&wayfire_view_switcher::handle_key),
                this, _1, _2);

        grab_interface->callbacks.keyboard.mod = std::bind(std::mem_fn(&wayfire_view_switcher::handle_mod),
                this, _1, _2);

        next_view = section->get_option("next", "KEY_RIGHT");
        prev_view = section->get_option("prev", "KEY_LEFT");
        terminate = section->get_option("exit", "KEY_ENTER");

        hook = [=] () { update_animation(); };
        destroyed = [=] (signal_data *data)
        {
            cleanup_view(get_signaled_view(data));
        };
    }

    void setup_graphics()
    {
        if (views.size() == 2) {
            attribs.offset = 0.4f;
            attribs.angle = M_PI / 5.;
            attribs.back = 0.;
        } else {
            attribs.offset = 0.6f;
            attribs.angle = M_PI / 6.;
            attribs.back = 0.3f;
        }
    }

    void activate()
    {
        if (output->is_plugin_active(grab_interface->name))
            return;
        if (!output->activate_plugin(grab_interface))
            return;

        update_views();
        update_transforms();

        if (!views.size())
        {
            output->deactivate_plugin(grab_interface);
            return;
        }

        state.active = true;
        state.mod_released = false;
        state.in_continuous_switch = false;
        state.reversed_folds = false;
        next_actions = std::queue<int>();

        grab_interface->grab();

        output->render->auto_redraw(true);
        output->render->damage(NULL);
        output->render->add_effect(&hook, WF_OUTPUT_EFFECT_PRE);

        output->connect_signal("unmap-view", &destroyed);
        output->connect_signal("detach-view", &destroyed);

        setup_graphics();
        start_fold();

        auto bgl = output->workspace->get_views_on_workspace(output->workspace->get_current_workspace(),
                                                            WF_LAYER_BACKGROUND);
        if (bgl.size())
        {
            auto bg = bgl[0];

            bg->add_transformer(std::unique_ptr<wf_3D_view> (new wf_3D_view(bg)), "switcher");
            auto tr = dynamic_cast<wf_3D_view*> (bg->get_transformer("switcher").get());
            assert(tr);

            tr->color = {0.6, 0.6, 0.6, 1.0};
            tr->scaling = glm::scale(glm::mat4(1.0f), glm::vec3(1, 1, 1));
        }
    }

    void push_exit()
    {
        if (state.in_rotate || state.in_fold || state.in_unfold)
            next_actions.push(0);
        else
        {
            state.reversed_folds = true;
            if (views.size() >= 2)
                start_unfold();
            else
                start_fold();
        }
    }

    void push_next_view(int delta)
    {
        log_info("push next view %d", state.in_rotate || state.in_fold || state.in_unfold);
        if ((state.in_rotate || state.in_fold || state.in_unfold) &&
                next_actions.size() < MAX_ACTIONS)
            next_actions.push(delta);
        else
            start_rotate(delta);
    }

    void stop_continuous_switch()
    {
        state.in_continuous_switch = false;
        push_exit();
    }

    void handle_mod(uint32_t mod, uint32_t st)
    {
        bool mod_released = (mod == activate_key->as_cached_key().mod && st == WLR_KEY_RELEASED);

        if (mod_released)
        {
            if (state.in_continuous_switch)
                stop_continuous_switch();
            else
                state.mod_released = true;
        }
    }

    void handle_key(uint32_t key, uint32_t kstate)
    {
        if (kstate != WLR_KEY_PRESSED)
            return;

        if (!state.mod_released && (key == activate_key->as_cached_key().keyval))
        {
            state.in_continuous_switch = true;
        }

        if (key == activate_key->as_cached_key().keyval && state.in_continuous_switch)
        {
            push_next_view(1);
            return;
        }

        if (state.active &&
            (key == terminate->as_cached_key().keyval || key == activate_key->as_cached_key().keyval))
        {
            push_exit();
        }

        if ((key == prev_view->as_cached_key().keyval || key == next_view->as_cached_key().keyval))
        {
            int dx = (key == prev_view->as_cached_key().keyval ? -1 : 1);
            push_next_view(dx);
        }
    }

    void update_views()
    {
        current_view_index = 0;
        views = output->workspace->get_views_on_workspace(output->workspace->get_current_workspace(), WF_LAYER_WORKSPACE);
    }

    void update_transforms()
    {
        for (auto v : views)
        {
            auto tr = v->get_transformer("switcher");
            if (!tr)
                v->add_transformer(std::unique_ptr<wf_3D_view> (new wf_3D_view(v)), "switcher");
        }
    }

    void view_chosen(int i)
    {
        for (int i = views.size() - 1; i >= 0; i--)
            output->bring_to_front(views[i]);

        output->focus_view(views[i]);
    }

    void cleanup_view(wayfire_view view)
    {
        size_t i = 0;
        for (; i < views.size() && views[i] != view; i++);
        if (i == views.size())
            return;

        views.erase(views.begin() + i);

        if (views.empty())
            deactivate();

        if (i <= current_view_index)
            current_view_index = (current_view_index + views.size() - 1) % views.size();

        auto it = active_views.begin();
        while(it != active_views.end())
        {
            if (it->view == view)
                it = active_views.erase(it);
            else
                ++it;
        }

        if (views.size() == 2)
            push_next_view(1);
    }

    void update_animation()
    {
        if (state.in_fold)
            update_fold();
        else if (state.in_unfold)
            update_unfold();
        else if (state.in_rotate)
            update_rotate();
    }

    void start_fold()
    {
        GetTuple(sw, sh, output->get_screen_size());
        active_views.clear();

        initial_animation.start();
        state.in_fold = true;

        update_views();
        for (size_t i = current_view_index, cnt = 0; cnt < views.size(); ++cnt, i = (i + 1) % views.size())
        {
            const auto& v = views[i];
            /* center of screen minus center of view */
            auto wm_geometry = v->get_wm_geometry();
            float cx = (sw / 2.0 - wm_geometry.width / 2.0f) - wm_geometry.x;
            float cy = wm_geometry.y - (sh / 2.0 - wm_geometry.height / 2.0f);

            log_info("factor is %lf", view_scale_config->as_cached_double());
            float scale_factor = get_scale_factor(wm_geometry.width, wm_geometry.height,
                                                  sw, sh, view_scale_config->as_cached_double());

            view_paint_attribs elem;
            elem.view = v;
            elem.off_z = {0, 0};

            if (state.reversed_folds)
            {
                elem.off_x = {cx, 0};
                elem.off_y = {cy, 0};
                elem.scale_x = {scale_factor, 1};
                elem.scale_y = {scale_factor, 1};
            } else
            {
                elem.off_x = {0, cx};
                elem.off_y = {0, cy};
                elem.scale_x = {1, scale_factor};
                elem.scale_y = {1, scale_factor};
            }

            elem.updates = UPDATE_OFFSET | UPDATE_SCALE;
            active_views.push_back(elem);
        }
    }

    void update_view_transforms()
    {
        auto &duration = initial_animation.running() ?
            initial_animation : regular_animation;

        for (auto v : active_views)
        {
            auto tr = dynamic_cast<wf_3D_view*> (v.view->get_transformer("switcher").get());
            assert(tr);

            v.view->damage();
            if (v.updates & UPDATE_OFFSET)
            {
                tr->translation = glm::translate(glm::mat4(1.0), glm::vec3(
                            duration.progress(v.off_x),
                            duration.progress(v.off_y),
                            duration.progress(v.off_z)));
            }
            if (v.updates & UPDATE_SCALE)
            {
                tr->scaling = glm::scale(glm::mat4(1.0), glm::vec3(
                        duration.progress(v.scale_x), duration.progress(v.scale_y), 1));
            }
            if (v.updates & UPDATE_ROTATION)
            {
                tr->rotation = glm::rotate(glm::mat4(1.0),
                                           (float)duration.progress(v.rot),
                                           glm::vec3(0, 1, 0));
            }

            v.view->damage();
        }
    }

    void dequeue_next_action()
    {
        if (!next_actions.empty())
        {
            int next = next_actions.front(); next_actions.pop();

            /* we aren't in any fold, unfold or rotation,
             * so the following will call the necessary functions
             * and not push to the queue */
            assert(!state.in_fold && !state.in_unfold && !state.in_rotate);

            if (next == 0)
                push_exit();
            else
                push_next_view(next);
        }
    }

    void update_fold()
    {
        update_view_transforms();

        if(!initial_animation.running())
        {
            state.in_fold = false;
            if (!state.reversed_folds)
            {
                if (active_views.size() == 1)
                    return;
                start_unfold();
            } else
            {
                deactivate();
            }
        }
    }

    void push_unfolded_transformed_view(wayfire_view v,
                               wf_transition off_x, wf_transition off_z,
                               wf_transition rot)
    {
        GetTuple(sw, sh, output->get_screen_size());
        auto wm_geometry = v->get_wm_geometry();

        float cx = (sw / 2.0 - wm_geometry.width / 2.0f) - wm_geometry.x;
        float cy = wm_geometry.y - (sh / 2.0 - wm_geometry.height / 2.0f);

        view_paint_attribs elem;
        elem.view = v;
        elem.off_x = {cx + off_x.start * sw / 2.0f, cx + off_x.end * sw / 2.0f};
        elem.off_y = {cy, cy};
        elem.off_z = off_z;
        elem.rot = rot;

        elem.updates = UPDATE_ROTATION | UPDATE_OFFSET;

        active_views.push_back(elem);
    }

    void start_unfold()
    {
        state.in_unfold = true;
        regular_animation.start();

        active_views.clear();

        if (views.size() == 2)
        {
            push_unfolded_transformed_view(views[current_view_index],
                                           {0, attribs.offset},
                                           {0, -attribs.back},
                                           {0, -attribs.angle});

            push_unfolded_transformed_view(views[1 - current_view_index],
                                           {0, -attribs.offset},
                                           {0, -attribs.back},
                                           {0, attribs.angle});
        } else
        {
            int prev = (current_view_index + views.size() - 1) % views.size();
            int next = (current_view_index + 1) % views.size();

            view_paint_attribs elem;

            push_unfolded_transformed_view(views[current_view_index],
                                           {0, 0},
                                           {0, 0},
                                           {0, 0});

            push_unfolded_transformed_view(views[prev],
                                           {0, -attribs.offset},
                                           {0, -attribs.back},
                                           {0, +attribs.angle});

            push_unfolded_transformed_view(views[next],
                                           {0, +attribs.offset},
                                           {0, -attribs.back},
                                           {0, -attribs.angle});
        }

        for (auto& elem : active_views)
        {
            if (state.reversed_folds)
            {
                std::swap(elem.off_x.start, elem.off_x.end);
                std::swap(elem.off_z.start, elem.off_z.end);
                std::swap(elem.rot.start, elem.rot.end);
            }
        }
    }

    void update_unfold()
    {
        update_view_transforms();

        if (!regular_animation.running())
        {
            state.in_unfold = false;
            if (!state.reversed_folds)
            {
                dequeue_next_action();
            } else
            {
                start_fold();
            }
        }
    }

    void start_rotate (int dir)
    {
        int sz = views.size();
        if (sz <= 1)
            return;

        state.in_rotate = true;
        regular_animation.start();

        current_view_index    = (current_view_index + dir + sz) % sz;
        output->bring_to_front(views[current_view_index]);

        int next = (current_view_index + 1) % sz;
        int prev = (current_view_index - 1 + sz) % sz;

        active_views.clear();

        /* only two views */

        if (next == prev) {
            push_unfolded_transformed_view(views[current_view_index],
                                           {-attribs.offset, attribs.offset},
                                           {attribs.back, attribs.back},
                                           {attribs.angle, -attribs.angle});

            push_unfolded_transformed_view(views[next],
                                           {-attribs.offset, -attribs.offset},
                                           {attribs.back, attribs.back},
                                           {attribs.angle, attribs.angle});
        } else {
            push_unfolded_transformed_view(views[current_view_index],
                                           {attribs.offset * dir, 0},
                                           {-attribs.back, 0},
                                           {-attribs.angle * dir, 0});

            if (dir == 1) {
                push_unfolded_transformed_view(views[prev],
                                               {0, -attribs.offset},
                                               {0, -attribs.back},
                                               {0, attribs.angle});

                push_unfolded_transformed_view(views[next],
                                               {attribs.offset, attribs.offset},
                                               {-attribs.back, -attribs.back},
                                               {-attribs.angle, -attribs.angle});

            } else {
                push_unfolded_transformed_view(views[next],
                                               {0, attribs.offset},
                                               {0, -attribs.back},
                                               {0, -attribs.angle});

                push_unfolded_transformed_view(views[prev],
                                               {-attribs.offset, -attribs.offset},
                                               {-attribs.back, -attribs.back},
                                               {attribs.angle, attribs.angle});
            }
        }

        for (auto& elem : active_views)
            elem.updates = UPDATE_ROTATION | UPDATE_OFFSET;
    }

    void update_rotate()
    {
        update_view_transforms();

        if (!regular_animation.running())
        {
            state.in_rotate = false;
            dequeue_next_action();
        }
    }

    void deactivate()
    {
        output->render->auto_redraw(false);
        output->render->reset_renderer();
        output->render->rem_effect(&hook, WF_OUTPUT_EFFECT_PRE);
        grab_interface->ungrab();
        output->deactivate_plugin(grab_interface);

        auto bgl = output->workspace->get_views_on_workspace(output->workspace->get_current_workspace(),
                                                            WF_LAYER_BACKGROUND);
        if (bgl.size())
        {
            auto bg = bgl[0];
            bg->pop_transformer("switcher");
        }

        log_info("reset tranforms");
        for(auto v : views)
            v->pop_transformer("switcher");

        state.active = false;
        view_chosen(current_view_index);

        output->disconnect_signal("unmap-view", &destroyed);
        output->disconnect_signal("detach-view", &destroyed);
    }

    void fini()
    {
        if (state.active)
            deactivate();

        output->rem_key(&init_binding);
    }
};

extern "C"
{
    wayfire_plugin_t* newInstance()
    {
        return new wayfire_view_switcher();
    }
}
