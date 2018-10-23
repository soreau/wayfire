#ifndef OBJECT_HPP
#define OBJECT_HPP

#include <unordered_map>
#include <vector>
#include <typeinfo>

#include <nonstd/observer_ptr.h>
#include <nonstd/make_unique.hpp>
#include "plugin.hpp"

/* A base class for "objects".
 * Provides signals & attaching custom data */
class wf_custom_data_t {
    public:
    virtual ~wf_custom_data_t() {};
};

class wf_object_base
{
    public:
    /* Get a human-readable description of the object */
    std::string to_string() const
    {
        return std::to_string(get_id());
    }

    /* Get the ID of the object. Each object has a unique ID */
    uint32_t get_id() const
    {
        return object_id;
    }

    /* Register a callback to be called whenever the given signal is emitted */
    void connect_signal(std::string name, signal_callback_t* callback)
    {
        signals[name].push_back(callback);
    }

    /* Unregister a registered callback */
    void disconnect_signal(std::string name, signal_callback_t* callback)
    {
        auto& container = signals[name];
        auto it = std::remove(container.begin(), container.end(), callback);
        container.erase(it, container.end());
    }

    /* Emit the given signal. No type checking for data is required */
    void emit_signal(std::string name, signal_data *data)
    {
        std::vector<signal_callback_t> callbacks;
        for (auto call : signals[name])
            callbacks.push_back(*call);

        for (auto call : callbacks)
            call(data);
    }

    /* Retrieve custom data stored with the given name. If no such
     * data exists, then it is created with the default constructor
     *
     * REQUIRES a default constructor
     * If your type doesn't have one, use store_data + get_data
     * */
    template<class T> nonstd::observer_ptr<T> get_data_safe(
        std::string name = typeid(T).name())
    {
        if (data.count(name) == 0)
            store_data<T>(nonstd::make_unique<T>(), name);
        return get_data<T>(name);
    }

    /* Retrieve custom data stored with the given name. If no such
     * data exists, NULL is returned */
    template<class T> nonstd::observer_ptr<T> get_data(
        std::string name = typeid(T).name())
    {
        return nonstd::make_observer(dynamic_cast<T*> (data[name].get()));
    }

    /* Assigns the given data to the given name */
    template<class T> void store_data(std::unique_ptr<T> stored_data,
        std::string name = typeid(T).name())
    {
        data[name] = std::move(stored_data);
    }

    /* Returns true if there is saved data under the given name */
    template<class T> bool has_data()
    {
        return has_data(typeid(T).name());
    }

    /* Returns if there is saved data with the given name */
    bool has_data(std::string name)
    {
        return data.count(name);
    }

    /* Remove the saved data under the given name */
    void erase_data(std::string name)
    {
        data.erase(name);
    }

    /* Remove the saved data for the type T */
    template<class T> void erase_data()
    {
        erase_data(typeid(T).name());
    }

    /* Erase the saved data from the store and return the pointer */
    template<class T> std::unique_ptr<T> release_data(std::string name = typeid(T).name())
    {
        if (data.count(name) == 0)
            return {nullptr};

        auto stored = std::move(data[name]);
        data.erase(name);

        return std::unique_ptr<T> (dynamic_cast<T*>(stored.release()));
    }

    protected:
    wf_object_base()
    {
        static uint32_t global_id = 0;
        object_id = global_id++;
    }

    uint32_t object_id;

    std::unordered_map<std::string, std::vector<signal_callback_t*>> signals;
    std::unordered_map<std::string, std::unique_ptr<wf_custom_data_t>> data;
};


#endif /* end of include guard: OBJECT_HPP */
