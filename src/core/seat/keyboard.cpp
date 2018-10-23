#include <string.h>
#include <linux/input-event-codes.h>

extern "C"
{
#include <wlr/backend/session.h>
#include <wlr/backend/multi.h>
}

#include "keyboard.hpp"
#include "core.hpp"
#include "input-manager.hpp"


void handle_keyboard_destroy_cb(wl_listener *listener, void*)
{
    wf_keyboard::listeners *lss = wl_container_of(listener, lss, destroy);
    core->input->handle_input_destroyed(lss->keyboard->device);
}

static void handle_keyboard_key_cb(wl_listener* listener, void *data)
{
    auto ev = static_cast<wlr_event_keyboard_key*> (data);
    wf_keyboard::listeners *lss = wl_container_of(listener, lss, key);

    auto seat = core->get_current_seat();
    wlr_seat_set_keyboard(seat, lss->keyboard->device);

    if (!core->input->handle_keyboard_key(ev->keycode, ev->state))
    {
        wlr_seat_keyboard_notify_key(core->input->seat, ev->time_msec,
                                     ev->keycode, ev->state);
    }

    wlr_idle_notify_activity(core->protocols.idle, seat);
}

static void handle_keyboard_mod_cb(wl_listener* listener, void* data)
{
    auto kbd = static_cast<wlr_keyboard*> (data);
    wf_keyboard::listeners *lss = wl_container_of(listener, lss, modifier);

    auto seat = core->get_current_seat();
    wlr_seat_set_keyboard(seat, lss->keyboard->device);
    wlr_seat_keyboard_send_modifiers(core->input->seat, &kbd->modifiers);

    wlr_idle_notify_activity(core->protocols.idle, seat);
}

wf_keyboard::wf_keyboard(wlr_input_device *dev, wayfire_config *config)
    : handle(dev->keyboard), device(dev)
{
    auto section = config->get_section("input");

    model   = section->get_option("xkb_model", "");
    variant = section->get_option("xkb_variant", "");
    layout  = section->get_option("xkb_layout", "");
    options = section->get_option("xkb_option", "");
    rules   = section->get_option("xkb_rule", "");

    repeat_rate  = section->get_option("kb_repeat_rate", "40");
    repeat_delay = section->get_option("kb_repeat_delay", "400");

    keyboard_options_updated = [=] () {
        reload_input_options();
    };

    model->add_updated_handler(&keyboard_options_updated);
    rules->add_updated_handler(&keyboard_options_updated);
    layout->add_updated_handler(&keyboard_options_updated);
    variant->add_updated_handler(&keyboard_options_updated);
    options->add_updated_handler(&keyboard_options_updated);
    repeat_rate->add_updated_handler(&keyboard_options_updated);
    repeat_delay->add_updated_handler(&keyboard_options_updated);

    lss.keyboard = this;
    lss.key.notify      = handle_keyboard_key_cb;
    lss.modifier.notify = handle_keyboard_mod_cb;
    lss.destroy.notify  = handle_keyboard_destroy_cb;

    wl_signal_add(&dev->keyboard->events.key,       &lss.key);
    wl_signal_add(&dev->keyboard->events.modifiers, &lss.modifier);
    wl_signal_add(&dev->events.destroy,             &lss.destroy);

    reload_input_options();
    wlr_seat_set_keyboard(core->get_current_seat(), dev);
}

void wf_keyboard::reload_input_options()
{
    auto ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

    auto rules = this->rules->as_string();
    auto model = this->model->as_string();
    auto layout = this->layout->as_string();
    auto variant = this->variant->as_string();
    auto options = this->options->as_string();

    xkb_rule_names names;
    names.rules   = rules.c_str();
    names.model   = model.c_str();
    names.layout  = layout.c_str();
    names.variant = variant.c_str();
    names.options = options.c_str();

    auto keymap = xkb_map_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);

    wlr_keyboard_set_keymap(handle, keymap);

    xkb_keymap_unref(keymap);
    xkb_context_unref(ctx);

    wlr_keyboard_set_repeat_info(handle, repeat_rate->as_int(),
                                 repeat_delay->as_int());
}

wf_keyboard::~wf_keyboard()
{
    model->rem_updated_handler(&keyboard_options_updated);
    rules->rem_updated_handler(&keyboard_options_updated);
    layout->rem_updated_handler(&keyboard_options_updated);
    variant->rem_updated_handler(&keyboard_options_updated);
    options->rem_updated_handler(&keyboard_options_updated);
    repeat_rate->rem_updated_handler(&keyboard_options_updated);
    repeat_delay->rem_updated_handler(&keyboard_options_updated);
}

/* input manager things */
static bool check_vt_switch(wlr_session *session, uint32_t key, uint32_t mods)
{
    if (!session)
        return false;
    if (mods ^ (WLR_MODIFIER_ALT | WLR_MODIFIER_CTRL))
        return false;

    if (key < KEY_F1 || key > KEY_F10)
        return false;

    int target_vt = key - KEY_F1 + 1;
    wlr_session_change_vt(session, target_vt);
    return true;
}

std::vector<std::function<void()>> input_manager::match_keys(uint32_t mod_state, uint32_t key)
{
    std::vector<std::function<void()>> callbacks;

    for (auto& binding : bindings[WF_BINDING_KEY])
    {
        if (binding->value->as_cached_key().matches({mod_state, key}) &&
            binding->output == core->get_active_output())
        {
            /* make sure we don't capture a reference to binding,
             * because it might get destroyed later */
            auto callback = binding->call.key;
            callbacks.push_back([key, callback] () {
                (*callback) (key);
            });
        }
    }

    for (auto& binding : bindings[WF_BINDING_ACTIVATOR])
    {
        if (binding->value->matches_key({mod_state, key}) &&
            binding->output == core->get_active_output())
        {
            callbacks.push_back(*binding->call.activator);
        }
    }

    return callbacks;
}

static uint32_t mod_from_key(uint32_t key)
{
    if (key == KEY_LEFTALT || key == KEY_RIGHTALT)
        return WLR_MODIFIER_ALT;
    if (key == KEY_LEFTCTRL || key == KEY_RIGHTCTRL)
        return WLR_MODIFIER_CTRL;
    if (key == KEY_LEFTSHIFT || key == KEY_RIGHTSHIFT)
        return WLR_MODIFIER_SHIFT;
    if (key == KEY_LEFTMETA || key == KEY_RIGHTMETA)
        return WLR_MODIFIER_LOGO;

    return 0;
}

bool input_manager::handle_keyboard_key(uint32_t key, uint32_t state)
{
    if (active_grab && active_grab->callbacks.keyboard.key)
        active_grab->callbacks.keyboard.key(key, state);

    auto mod = mod_from_key(key);
    if (mod)
        handle_keyboard_mod(mod, state);

    std::vector<std::function<void()>> callbacks;
    auto kbd = wlr_seat_get_keyboard(seat);

    if (state == WLR_KEY_PRESSED)
    {
        if (check_vt_switch(wlr_backend_get_session(core->backend), key, get_modifiers()))
            return true;

        /* as long as we have pressed only modifiers, we should check for modifier bindings on release */
        if (mod)
        {
            bool modifiers_only = !count_other_inputs;
            for (size_t i = 0; i < kbd->num_keycodes; i++)
                if (!mod_from_key(kbd->keycodes[i]))
                    modifiers_only = false;

            if (modifiers_only)
                in_mod_binding = true;
            else
                in_mod_binding = false;
        } else
        {
            in_mod_binding = false;
        }

        callbacks = match_keys(get_modifiers(), key);
    } else
    {
        if (in_mod_binding)
            callbacks = match_keys(get_modifiers() | mod, 0);

        in_mod_binding = false;
    }

    for (auto call : callbacks)
        call();

    return active_grab || !callbacks.empty();
}

void input_manager::handle_keyboard_mod(uint32_t modifier, uint32_t state)
{
    if (active_grab && active_grab->callbacks.keyboard.mod)
        active_grab->callbacks.keyboard.mod(modifier, state);
}

