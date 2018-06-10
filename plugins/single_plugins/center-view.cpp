#include <output.hpp>
#include <core.hpp>
#include <view.hpp>
#include <workspace-manager.hpp>
#include <signal-definitions.hpp>

class wayfire_center_view : public wayfire_plugin_t
{
	signal_callback_t created_cb;

	public:
	void init(wayfire_config *config)
	{
		created_cb = [=] (signal_data *data)
		{
			auto view = get_signaled_view(data);
			if (view->role == WF_VIEW_ROLE_TOPLEVEL && !view->parent)
			{
				wf_geometry workarea = output->workspace->get_workarea();
				wf_geometry window = view->get_wm_geometry();
				window.x = workarea.x + (workarea.width / 2) - (window.width / 2);
				window.y = workarea.y + (workarea.height / 2) - (window.height / 2);
				view->move(window.x, window.y);
			}
		};
		output->connect_signal("map-view", &created_cb);
	}
};

extern "C"
{
	wayfire_plugin_t *newInstance() 
	{
		return new wayfire_center_view();
	}
}

