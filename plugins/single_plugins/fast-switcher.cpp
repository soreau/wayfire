#include <signal-definitions.hpp>
#include <view.hpp>
#include <workspace-manager.hpp>
#include <linux/input-event-codes.h>

/*
 * This plugin rovides abilities to switch between views.
 * It works similarly to the alt-esc binding in Windows or GNOME
 */

class wayfire_fast_switcher : public wayfire_plugin_t
{
    key_callback init_binding;
    wf_option activate_key;

    signal_callback_t destroyed;

    size_t current_view_index;
    std::vector<wayfire_view> views; // all views on current viewport

    bool active;

    public:
    void init(wayfire_config *config)
    {
        grab_interface->name = "fast-switcher";
        grab_interface->abilities_mask = WF_ABILITY_CONTROL_WM;

        auto section = config->get_section("fast-switcher");

        activate_key = section->get_option("activate", "<alt> KEY_TAB");
        init_binding = [=] (uint32_t key)
        {
            fast_switch();
        };

        output->add_key(activate_key, &init_binding);

        using namespace std::placeholders;
        grab_interface->callbacks.keyboard.key = std::bind(std::mem_fn(&wayfire_fast_switcher::handle_key),
                this, _1, _2);

        grab_interface->callbacks.keyboard.mod = std::bind(std::mem_fn(&wayfire_fast_switcher::handle_mod),
                this, _1, _2);

        destroyed = [=] (signal_data *data)
        {
            cleanup_view(get_signaled_view(data));
        };
    }

    void handle_mod(uint32_t mod, uint32_t st)
    {
        bool mod_released = (mod == activate_key->as_cached_key().mod && st == WLR_KEY_RELEASED);

        if (mod_released)
        {
            switch_terminate();

            for (auto view : views)
            {
                view->alpha = 1.0;
                view->damage();
            }
        }
    }

    void handle_key(uint32_t key, uint32_t kstate)
    {
        if (kstate != WLR_KEY_PRESSED)
            return;

        switch_next();
    }

    void update_views()
    {
        current_view_index = 0;
        views = output->workspace->get_views_on_workspace(output->workspace->get_current_workspace(), WF_LAYER_WORKSPACE);
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

        if (i <= current_view_index)
            current_view_index = (current_view_index + views.size() - 1) % views.size();
    }

    void fast_switch()
    {
        if (active)
            return;

        if (!output->activate_plugin(grab_interface))
            return;

        update_views();

        if (views.size() < 1)
        {
            output->deactivate_plugin(grab_interface);
            return;
        }

        current_view_index = 0;
        active = true;

        for (auto view : views)
        {
            view->alpha = 0.7;
            view->damage();
        }

        grab_interface->grab();
        switch_next();

        output->connect_signal("unmap-view", &destroyed);
        output->connect_signal("detach-view", &destroyed);
    }

    void switch_terminate()
    {
        grab_interface->ungrab();
        output->deactivate_plugin(grab_interface);
        active = false;

        output->disconnect_signal("unmap-view", &destroyed);
        output->disconnect_signal("detach-view", &destroyed);
    }

    void switch_next()
    {
#define index current_view_index
        if (views[index]) {
            views[index]->alpha = 0.7;
            views[index]->damage();
        }

        index = (index + 1) % views.size();

        if (views[index]) {
            views[index]->alpha = 1.0;
            views[index]->damage();
        }

        output->bring_to_front(views[index]);
#undef index
        view_chosen(current_view_index);
    }

    void fini()
    {
        if (active)
            switch_terminate();

        output->rem_key(&init_binding);
    }
};

extern "C"
{
    wayfire_plugin_t* newInstance()
    {
        return new wayfire_fast_switcher();
    }
}
