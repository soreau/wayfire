#include "panel.hpp"
#include "config.hpp"
#include <vector>
#include <map>
#include <getopt.h>
#include <signal.h>

int main(int argc, char *argv[])
{
    std::string home_dir = secure_getenv("HOME");
    std::string config_file = home_dir + "/.config/wayfire.ini";

    struct option opts[] = {
        { "config",   required_argument, NULL, 'c' },
        { 0,          0,                 NULL,  0  }
    };

    int c, i;
    while((c = getopt_long(argc, argv, "c:l:", opts, &i)) != -1)
    {
        switch(c)
        {
            case 'c':
                config_file = optarg;
                break;
            default:
                std::cerr << "failed to parse option " << optarg << std::endl;
        }
    }

    auto config = new wayfire_config(config_file);
    auto display = new wayfire_display([=] (wayfire_output *output)
    {
        new wayfire_panel(config, output);
    });

    while(true) {
        if (wl_display_dispatch(display->display) < 0)
            break;
    }

    delete display;
}
