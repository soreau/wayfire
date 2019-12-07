/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2019 Scott Moreau
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <map>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include "core.hpp"
#include "view.hpp"
#include "plugin.hpp"
#include "output.hpp"
#include "signal-definitions.hpp"
#include "workspace-manager.hpp"
#include "output-layout.hpp"

#include "debug.hpp"


struct process
{
    nonstd::observer_ptr<wf::view_interface_t> view;
    struct wl_client *client;
    pid_t pid;
};

static std::map<wf::output_t *, struct process> procs;

class wayfire_background_view : public wf::plugin_interface_t
{
    const std::string transformer_name = "background-view";
    wf::signal_callback_t view_mapped;
    wf_option_callback option_changed;
    wf_option cmd_opt, file_opt;
    struct wl_event_source *signal = nullptr;

    public:
    void init(wayfire_config *config)
    {
        grab_interface->name = transformer_name;
        grab_interface->capabilities = 0;

        auto section = config->get_section(transformer_name.c_str());
        cmd_opt = section->get_option("command", "mpv --no-keepaspect-window --loop=inf");
        file_opt = section->get_option("file", "");

        option_changed = [=] ()
        {
            if (procs[output].view)
            {
                procs[output].view->close();
                kill(procs[output].pid, SIGINT);
                procs[output].view = nullptr;
            }

            procs[output].client = client_launch((cmd_opt->as_string() + add_arg_if_not_empty(file_opt->as_string())).c_str());
        };

        cmd_opt->add_updated_handler(&option_changed);
        file_opt->add_updated_handler(&option_changed);

        view_mapped = [=] (wf::signal_data_t *data)
        {
            auto view = get_signaled_view(data);

            for (auto& o : wf::get_core().output_layout->get_outputs())
            {
                if (procs[o].client == view->get_client())
                {
                    // Move to the respective output
                    wf::get_core().move_view_to_output(view, o);

                    // Since we set --no-keepaspect-window, we can set the size to fullscreen size
                    // If we set it fullscreen, the screensaver would be inhibited
                    view->set_geometry(o->get_relative_geometry());

                    // Set it as the background
                    o->workspace->add_view(view, wf::LAYER_BACKGROUND);

                    // Make it show on all workspaces
                    view->role = wf::VIEW_ROLE_SHELL_VIEW;

                    procs[o].view = view;

                    break;
                }
            }
        };

        output->connect_signal("map-view", &view_mapped);

        if (!signal)
            signal = wl_event_loop_add_signal(wf::get_core().ev_loop, SIGCHLD, sigchld_handler, &procs);

        procs[output].client = client_launch((cmd_opt->as_string() + add_arg_if_not_empty(file_opt->as_string())).c_str());
        procs[output].view = nullptr;
    }

    std::string add_arg_if_not_empty(std::string in)
    {
        if (!in.empty())
            return " \"" + in + "\"";
        else
            return in;
    }

    /* The following functions taken from weston */
    int os_fd_set_cloexec(int fd)
    {
        long flags;

        if (fd == -1)
            return -1;

        flags = fcntl(fd, F_GETFD);
        if (flags == -1)
            return -1;

        if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == -1)
            return -1;

        return 0;
    }

    int set_cloexec_or_close(int fd)
    {
        if (os_fd_set_cloexec(fd) != 0) {
            close(fd);
            return -1;
        }
        return fd;
    }

    int os_socketpair_cloexec(int domain, int type, int protocol, int *sv)
    {
        int ret;

#ifdef SOCK_CLOEXEC
        ret = socketpair(domain, type | SOCK_CLOEXEC, protocol, sv);
        if (ret == 0 || errno != EINVAL)
            return ret;
#endif

        ret = socketpair(domain, type, protocol, sv);
        if (ret < 0)
            return ret;

        sv[0] = set_cloexec_or_close(sv[0]);
        sv[1] = set_cloexec_or_close(sv[1]);

        if (sv[0] != -1 && sv[1] != -1)
            return 0;

        close(sv[0]);
        close(sv[1]);
        return -1;
    }

    static int sigchld_handler(int signal_number, void *data)
    {
        std::map<wf::output_t *, struct process> *procs = (std::map<wf::output_t *, struct process> *) data;
        pid_t pid;
        int status;

        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            for (auto& p : *procs)
            {
                if (p.second.pid == pid)
                {
                    p.second.view = nullptr;
                    break;
                }
            }
        }

        if (pid < 0 && errno != ECHILD)
            log_info("waitpid error %s", strerror(errno));

        return 1;
    }

    void child_client_exec(int sockfd, const char *command)
    {
        int clientfd;
        char s[32];
        sigset_t allsigs;

        /* do not give our signal mask to the new process */
        sigfillset(&allsigs);
        sigprocmask(SIG_UNBLOCK, &allsigs, NULL);

        /* Launch clients as the user. Do not launch clients with wrong euid. */
        if (seteuid(getuid()) == -1) {
            log_info("failed seteuid");
            return;
        }

        /* SOCK_CLOEXEC closes both ends, so we dup the fd to get a
         * non-CLOEXEC fd to pass through exec. */
        clientfd = dup(sockfd);
        if (clientfd == -1) {
            log_info("dup failed: %s", strerror(errno));
            return;
        }

        snprintf(s, sizeof s, "%d", clientfd);
        setenv("WAYLAND_SOCKET", s, 1);

        if (execlp("bash", "bash", "-c", command, NULL) < 0)
            log_info("executing '%s' failed: %s",
                   command, strerror(errno));
    }

    struct wl_client *client_launch(const char *command)
    {
        int sv[2];
        pid_t pid;
        struct wl_client *client;

        log_info("launching '%s'", command);

        if (os_socketpair_cloexec(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
            log_info("%s: socketpair failed while launching '%s': %s",
                __func__, command, strerror(errno));
            return NULL;
        }

        pid = fork();
        if (pid == -1) {
            close(sv[0]);
            close(sv[1]);
            log_info("%s: fork failed while launching '%s': %s", __func__, command,
                strerror(errno));
            return NULL;
        }

        if (pid == 0) {
            child_client_exec(sv[1], command);
            _exit(-1);
        }

        close(sv[1]);

        client = wl_client_create(wf::get_core().display, sv[0]);
        if (!client) {
            close(sv[0]);
            log_info("%s: wl_client_create failed while launching '%s'.",
                __func__, command);
            return NULL;
        }

        procs[output].pid = pid;

        return client;
    }
    /* End from weston */

    void fini()
    {
        if (procs[output].view)
            procs[output].view->close();

        if (signal)
            wl_event_source_remove(signal);

        output->disconnect_signal("map-view", &view_mapped);

        cmd_opt->rem_updated_handler(&option_changed);
        file_opt->rem_updated_handler(&option_changed);
    }
};

DECLARE_WAYFIRE_PLUGIN(wayfire_background_view);