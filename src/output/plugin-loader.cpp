#include <sstream>
#include <memory>
#include <dlfcn.h>

#include "plugin-loader.hpp"
#include "wm.hpp"
#include "core.hpp"
#include "debug.hpp"

namespace
{
    template<class A, class B> B union_cast(A object)
    {
        union {
            A x;
            B y;
        } helper;
        helper.x = object;
        return helper.y;
    }
}

plugin_manager::plugin_manager(wayfire_output *o, wayfire_config *config,
                               std::string list_of_plugins,
                               std::string prefix)
{
    this->prefix = prefix;
    this->to_load = (list_of_plugins == "default") ?
        "viewport_impl move resize animate "
        "switcher vswitch cube expo command grid" :
        list_of_plugins;

    load_dynamic_plugins();
    init_default_plugins();

    for (auto& p : plugins)
    {
        p->grab_interface = new wayfire_grab_interface_t(o);
        p->output = o;

        p->init(config);
    }
}

plugin_manager::~plugin_manager()
{
    for (auto& p : plugins)
    {
        p->fini();
        delete p->grab_interface;

        /* we load the same plugins for each output, so we must dlclose() the handle
         * only when we remove the last output */
        if (core->get_num_outputs() < 1)
        {
            if (p->dynamic)
                dlclose(p->handle);
        }

        p.reset();
    }
}

wayfire_plugin plugin_manager::load_plugin_from_file(std::string path)
{
    void *handle = dlopen(path.c_str(), RTLD_NOW);
    if(handle == NULL)
    {
        log_error("error loading plugin: %s", dlerror());
        return nullptr;
    }


    auto initptr = dlsym(handle, "newInstance");
    if(initptr == NULL)
    {
        log_error("%s: missing newInstance(). %s", path.c_str(), dlerror());
        return nullptr;
    }

    log_debug("loading plugin %s", path.c_str());
    get_plugin_instance_t init = union_cast<void*, get_plugin_instance_t> (initptr);

    auto ptr = wayfire_plugin(init());

    ptr->handle = handle;
    ptr->dynamic = true;

    return wayfire_plugin(init());
}

void plugin_manager::load_dynamic_plugins()
{
    std::stringstream stream(to_load);
    auto path = prefix + "/wayfire/";

    std::string plugin;
    while(stream >> plugin)
    {
        if(plugin != "")
        {
            auto ptr = load_plugin_from_file(path + "/lib" + plugin + ".so");
            if(ptr)
                plugins.push_back(std::move(ptr));
        }
    }
}

template<class T> static wayfire_plugin create_plugin()
{
    return std::unique_ptr<wayfire_plugin_t>(new T);
}

void plugin_manager::init_default_plugins()
{
    plugins.push_back(create_plugin<wayfire_focus>());
    plugins.push_back(create_plugin<wayfire_close>());
    plugins.push_back(create_plugin<wayfire_exit>());
    plugins.push_back(create_plugin<wayfire_fullscreen>());
    plugins.push_back(create_plugin<wayfire_handle_focus_parent>());
}
