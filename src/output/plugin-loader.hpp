#include <vector>
#include "plugin.hpp"
#include "config.h"

class wayfire_output;
class wayfire_config;

using wayfire_plugin = std::unique_ptr<wayfire_plugin_t>;
struct plugin_manager
{
    std::vector<wayfire_plugin> plugins;
    std::string to_load, prefix;

    plugin_manager(wayfire_output *o, wayfire_config *config,
                   std::string list_of_plugins = "default",
                   std::string prefix = INSTALL_PREFIX "/lib/");

    ~plugin_manager();

    wayfire_plugin load_plugin_from_file(std::string path);
    void load_dynamic_plugins();
    void init_default_plugins();
};
