#include <sstream>
#include <algorithm>
#include <set>
#include <memory>
#include <filesystem>
#include <dlfcn.h>

#include "plugin-loader.hpp"
#include "wayfire/output-layout.hpp"
#include "wayfire/output.hpp"
#include "../core/wm.hpp"
#include "wayfire/core.hpp"
#include <wayfire/util/log.hpp>


plugin_manager::plugin_manager(wf::output_t *o)
{
    this->output = o;
    this->plugins_opt.load_option("core/plugins");

    reload_dynamic_plugins();
    load_static_plugins();

    this->plugins_opt.set_callback([=] ()
    {
        /* reload when config reload has finished */
        idle_reaload_plugins.run_once([&] () {reload_dynamic_plugins(); });
    });
}

void plugin_manager::deinit_plugins(bool unloadable)
{
    for (auto& p : loaded_plugins)
    {
        if (!p.second) // already destroyed on the previous iteration
        {
            continue;
        }

        if (p.second->is_unloadable() == unloadable)
        {
            destroy_plugin(p.second);
        }
    }
}

plugin_manager::~plugin_manager()
{
    /* First remove unloadable plugins, then others */
    deinit_plugins(true);
    deinit_plugins(false);

    loaded_plugins.clear();
}

void plugin_manager::init_plugin(wayfire_plugin& p)
{
    p->grab_interface = std::make_unique<wf::plugin_grab_interface_t>(output);
    p->output = output;
    p->init();
}

void plugin_manager::destroy_plugin(wayfire_plugin& p)
{
    p->fini();

    p->grab_interface->ungrab();
    output->deactivate_plugin(p->grab_interface);

    auto handle = p->handle;
    p.reset();

    /* dlopen()/dlclose() do reference counting, so we should close the plugin
     * as many times as we opened it.
     *
     * We also need to close the handle after deallocating the plugin, otherwise
     * we unload its destructor before calling it. */
    if (handle)
    {
        dlclose(handle);
    }
}

std::pair<void*, void*> wf::get_new_instance_handle(const std::string& plugin_name)
{
    if (!plugin_name.size())
    {
        LOGE("!plugin_name.size()");
        return {nullptr, nullptr};
    }

    std::string path;
    std::vector<std::string> plugin_prefixes;
    if (char *plugin_path = getenv("WAYFIRE_PLUGIN_PATH"))
    {
        std::stringstream ss(plugin_path);
        std::string entry;
        while (std::getline(ss, entry, ':'))
        {
            plugin_prefixes.push_back(entry);
        }
    }

    plugin_prefixes.push_back(PLUGIN_PATH);

    if (plugin_name.at(0) == '/')
    {
        path = plugin_name;
    }
    else
    {
        std::string plugin_prefix;
        auto plugin_path = std::string("lib" + plugin_name + ".so");
        path = plugin_path;
    }

    if (path.empty())
    {
        LOGE("path.empty()");
        return {nullptr, nullptr};
    }

    // RTLD_GLOBAL is required for RTTI/dynamic_cast across plugins
    void *handle = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (handle == NULL)
    {
        LOGE("error loading plugin: ", dlerror());
        return {nullptr, nullptr};
    }

    /* Check plugin version */
    auto version_func_ptr = dlsym(handle, "getWayfireVersion");
    if (version_func_ptr == NULL)
    {
        LOGE(path, ": missing getWayfireVersion()", path.c_str());
        dlclose(handle);
        return {nullptr, nullptr};
    }

    auto version_func =
        union_cast<void*, wayfire_plugin_version_func>(version_func_ptr);
    int32_t plugin_abi_version = version_func();

    if (version_func() != WAYFIRE_API_ABI_VERSION)
    {
        LOGE(path, ": API/ABI version mismatch: Wayfire is ",
            WAYFIRE_API_ABI_VERSION, ",  plugin built with ", plugin_abi_version);
        dlclose(handle);
        return {nullptr, nullptr};
    }

    auto new_instance_func_ptr = dlsym(handle, "newInstance");
    if (new_instance_func_ptr == NULL)
    {
        LOGE(path, ": missing newInstance(). ", dlerror());
        dlclose(handle);
        return {nullptr, nullptr};
    }

    LOGD("Loaded plugin ", path.c_str());

    return {handle, new_instance_func_ptr};
}

wayfire_plugin plugin_manager::load_plugin_from_file(std::string path)
{
    auto [handle, new_instance_func_ptr] = wf::get_new_instance_handle(path);

    if (!new_instance_func_ptr)
    {
        return nullptr;
    }

    auto new_instance_func =
        wf::union_cast<void*, wayfire_plugin_load_func>(new_instance_func_ptr);

    auto ptr = wayfire_plugin(new_instance_func());
    ptr->handle = handle;

    return ptr;
}

void plugin_manager::reload_dynamic_plugins()
{
    std::string plugin_list = plugins_opt;
    if (plugin_list == "none")
    {
        LOGE("No plugins specified in the config file, or config file is "
             "missing. In this state the compositor is nearly unusable, please "
             "ensure your configuration file is set up properly.");
    }

    std::stringstream stream(plugin_list);

    /* load new plugins */
    std::string plugin;
    while (stream >> plugin)
    {
        if (loaded_plugins.count(plugin))
        {
            continue;
        }

        auto ptr = load_plugin_from_file(plugin);
        if (ptr)
        {
            /* erase plugins that have been removed from the config */
            auto it = loaded_plugins.begin();
            while (it != loaded_plugins.end())
            {
                /* skip built-in(static) plugins */
                if (it->first.size() && (it->first[0] == '_'))
                {
                    ++it;
                    continue;
                }
            
                if (ptr->is_unloadable())
                {
                    LOGD("unload plugin ", it->first.c_str());
                    destroy_plugin(it->second);
                    it = loaded_plugins.erase(it);
                } else
                {
                    ++it;
                }
            }
            init_plugin(ptr);
            loaded_plugins[plugin] = std::move(ptr);
        }
    }
}

template<class T>
static wayfire_plugin create_plugin()
{
    return std::unique_ptr<wf::plugin_interface_t>(new T);
}

void plugin_manager::load_static_plugins()
{
    loaded_plugins["_exit"]  = create_plugin<wayfire_exit>();
    loaded_plugins["_focus"] = create_plugin<wayfire_focus>();
    loaded_plugins["_close"] = create_plugin<wayfire_close>();

    init_plugin(loaded_plugins["_exit"]);
    init_plugin(loaded_plugins["_focus"]);
    init_plugin(loaded_plugins["_close"]);
}
