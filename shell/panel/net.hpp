#ifndef NET_HPP
#define NET_HPP

#include "widgets.hpp"
#include <thread>
#include <mutex>

struct connection_info
{
    std::string name;
    int strength; /* in percentage, for Wi-Fi/Broadband */
    std::string icon;

    std::map<int, bool> updated;
    std::mutex mutex;
};

struct network_provider_backend;

struct network_widget : public widget
{
    private:
    static std::thread updater_thread;
    static connection_info *connection;
    static network_provider_backend *backend;

    int id;

    public:
    ~network_widget();
    void create();
    int  get_width() { return width; };
    bool update(bool reset);
    bool resized();
    void repaint();
};

#endif /* end of include guard: NET_HPP */
