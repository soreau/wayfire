#include "wayfire/output.hpp"
#include "plugin-loader.hpp"

#include <unordered_set>
#include <wayfire/nonstd/safe-list.hpp>

namespace wf
{
class output_impl_t : public output_t
{
  private:
    std::unordered_multiset<wf::plugin_grab_interface_t*> active_plugins;
    std::unique_ptr<plugin_manager> plugin;

    signal_callback_t view_disappeared_cb;
    bool inhibited = false;

    /**
     * Set the given view as the active view.
     * If the output has focus, try to focus the view as well.
     */
    void update_active_view(wayfire_view view);

  public:
    output_impl_t(wlr_output *output);
    virtual ~output_impl_t();
    wayfire_view active_view;

    /**
     * Implementations of the public APIs
     */
    bool can_activate_plugin(const plugin_grab_interface_uptr& owner,
        bool ignore_inhibit) override;
    bool activate_plugin(const plugin_grab_interface_uptr& owner,
        bool ignore_inhibit) override;
    bool deactivate_plugin(const plugin_grab_interface_uptr& owner) override;
    bool is_plugin_active(std::string owner_name) const override;
    wayfire_view get_active_view() const override;
    void focus_view(wayfire_view v, bool raise) override;
    void refocus(wayfire_view skip_view, uint32_t layers) override;

    /**
     * Set the output as inhibited, so that no plugins can be activated
     * except those that ignore inhibitions.
     */
    void inhibit_plugins();

    /**
     * Uninhibit the output.
     */
    void uninhibit_plugins();

    /** @return true if the output is inhibited */
    bool is_inhibited() const;

    /**
     * @return The currently active input grab interface, or nullptr if none
     */
    plugin_grab_interface_t* get_input_grab_interface();
};
}

