/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2024 Scott Moreau <oreaus@gmail.com>
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

/*
 * Creates duplicate passthrough joystick nodes in /dev/input/ for each real joystick.
 * Rumble is supported through the associated event node. Use the dup'd nodes normally.
 * The events are sent to the compositor and the compositor can request the joystick
 * consumers receive events or not. This allows using the joysticks to control the
 * compositor while avoiding sending events to the other consumers temporarily.
 */

#define _GNU_SOURCE

#include <libudev.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <locale.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <linux/uinput.h>
#include <linux/joystick.h>

#include "gp-glue.h"

#define MAX_EVENTS 10
#define MAX_JOYSTICKS 10
#define MAX_FF_EFFECTS 16
#define BITS_TO_LONGS(x) \
        (((x) + 8 * sizeof (unsigned long) - 1) / (8 * sizeof (unsigned long)))

static int epollfd;
static struct udev *udev;
static struct epoll_event ev;

struct joystick {
	int fd;
	int slot;
	int event_fd;
	int uinput_fd;
	char *id_path, *event_id_path;
	char *node_name, *event_node_name, *uevent_path;
	mode_t orig_mode, event_orig_mode, uevent_orig_mode;
	unsigned char axes;
	unsigned char buttons;
	int *axis;
	char *button;
	uint16_t btnmap[KEY_MAX - BTN_MISC + 1];
	uint8_t axmap[ABS_MAX + 1];
	struct ff_effect rumble_effect;
};

static int compositor_fds[2];
static int num_josyticks = 0;
static struct joystick joysticks[MAX_JOYSTICKS];

static void emit(int fd, int type, int code, int val)
{
	struct input_event ie;

	ie.type = type;
	ie.code = code;
	ie.value = val;
	/* timestamp values below are ignored */
	ie.time.tv_sec = 0;
	ie.time.tv_usec = 0;

	write(fd, &ie, sizeof(ie));
}

static void add_joystick(struct udev_device *dev, int hotplug)
{
	if (num_josyticks >= MAX_JOYSTICKS) {
		printf("10 joysticks maximum\n");
		return;
	}

	const char *device_node_name = udev_device_get_devnode(dev);
	if (!device_node_name)
	{
		return;
	}
	printf("Device Node Path: %s\n", device_node_name);
	int js_slot;
	struct joystick *js_dev = NULL;
	struct udev_list_entry *properties = udev_device_get_properties_list_entry(dev);
	while (properties) {
		const char *property_name = udev_list_entry_get_name(properties);
		const char *property_value = udev_list_entry_get_value(properties);
		if (strcmp(property_name, "ID_VENDOR_ID") && strcmp(property_name, "ID_MODEL_ID") &&
			strcmp(property_name, "DEVNAME") && strcmp(property_name, "ID_MODEL") &&
			strcmp(property_name, "ID_PATH")) {
			properties = udev_list_entry_get_next(properties);
			continue;
		}
		if (!strcmp(property_name, "ID_PATH")) {
			if (!strncmp(device_node_name, "/dev/input/js", strlen("/dev/input/js"))) {
				for (int i = 0; i <= MAX_JOYSTICKS; i++) {
					if (!joysticks[i].node_name && !joysticks[i].event_node_name) {
						joysticks[i].node_name = strdup(device_node_name);
						joysticks[i].id_path = strdup(property_value);
						return;
					} else if (joysticks[i].event_node_name && !strcmp(joysticks[i].event_id_path, property_value)) {
						joysticks[i].node_name = strdup(device_node_name);
						joysticks[i].id_path = strdup(property_value);
						js_dev = &joysticks[i];
						js_slot = i;
						break;
					}
				}
			} else if (!strncmp(device_node_name, "/dev/input/event", strlen("/dev/input/event"))) {
				for (int i = 0; i <= MAX_JOYSTICKS; i++) {
					if (!joysticks[i].event_node_name && !joysticks[i].node_name) {
						joysticks[i].event_node_name = strdup(device_node_name);
						joysticks[i].event_id_path = strdup(property_value);
						return;
					} else if (joysticks[i].node_name && !strcmp(joysticks[i].id_path, property_value)) {
						joysticks[i].event_node_name = strdup(device_node_name);
						joysticks[i].event_id_path = strdup(property_value);
						js_dev = &joysticks[i];
						js_slot = i;
						break;
					}
				}
			}
		} else {
			printf("%s - %s\n", property_name, property_value);
		}
		properties = udev_list_entry_get_next(properties);
	}
	if (!js_dev) {
		return;
	}
	struct uinput_setup usetup;

	struct stat st;
	mode_t add_rw_perms, remove_rw_perms;

	stat(js_dev->node_name, &st);

	js_dev->orig_mode = st.st_mode & 0xFFF;

	add_rw_perms = js_dev->orig_mode | S_IRUSR | S_IRGRP;
	remove_rw_perms = js_dev->orig_mode & ~(S_IRUSR | S_IRGRP | S_IROTH);

	chmod(js_dev->node_name, add_rw_perms);
	int js_fd = open(js_dev->node_name, O_RDONLY);
	if (js_fd == -1) {
		perror("open js");
	}
	chmod(js_dev->node_name, remove_rw_perms);

	ev.events = EPOLLIN;
	ev.data.fd = js_fd;

	if (epoll_ctl(epollfd, EPOLL_CTL_ADD, js_fd, &ev) == -1) {
		printf("epoll_ctl: Failed to add joystick: %s\n", js_dev->node_name);
		return;
	}

	stat(js_dev->event_node_name, &st);

	js_dev->event_orig_mode = st.st_mode & 0xFFF;

	add_rw_perms = js_dev->event_orig_mode | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP;
	remove_rw_perms = js_dev->event_orig_mode & ~(S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);

	chmod(js_dev->event_node_name, add_rw_perms);
	js_dev->event_fd = open(js_dev->event_node_name, O_RDWR);
	if (js_dev->event_fd == -1) {
		perror("open event");
	}
	chmod(js_dev->event_node_name, remove_rw_perms);
	printf("Opened %s: fd: %d\n", js_dev->event_node_name, js_dev->event_fd);

	js_dev->fd = js_fd;
	ioctl(js_fd, JSIOCGAXES, &js_dev->axes);
	ioctl(js_fd, JSIOCGBUTTONS, &js_dev->buttons);
	js_dev->axis = calloc(js_dev->axes, sizeof(int));
	js_dev->button = calloc(js_dev->buttons, sizeof(char));
	js_dev->uinput_fd = open("/dev/uinput", O_RDWR | O_NONBLOCK);
	ev.events = EPOLLIN;
	ev.data.fd = js_dev->uinput_fd;

	if (epoll_ctl(epollfd, EPOLL_CTL_ADD, js_dev->uinput_fd, &ev) == -1) {
		printf("epoll_ctl: Failed to add joystick: %s\n", device_node_name);
		return;
	}
	js_dev->uevent_path = strdup(udev_device_get_syspath(dev));
	if (js_dev->uevent_path) {
		strcpy(strstr(js_dev->uevent_path, "input"), "uevent");
		printf("uevent_path: %s\n", js_dev->uevent_path);

		stat(js_dev->uevent_path, &st);
		if (hotplug) {
			remove_rw_perms = (st.st_mode & 0xFFF) & ~(S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
		} else {
			js_dev->uevent_orig_mode = st.st_mode & 0xFFF;
			remove_rw_perms = js_dev->uevent_orig_mode & ~(S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
		}

		chmod(js_dev->uevent_path, remove_rw_perms);
	}
#define test_bit(array, bit) ((array[bit / (8 * sizeof(unsigned char))] & (1 << (bit % (8 * sizeof(unsigned char))))))
	unsigned char key_bits[KEY_MAX / 8 + 1];
	memset(key_bits, 0, sizeof(key_bits));
	if (js_dev->buttons > 0) {
		ioctl(js_dev->uinput_fd, UI_SET_EVBIT, EV_KEY);
		memset(js_dev->btnmap, 0, sizeof(js_dev->btnmap));
		ioctl(js_dev->fd, JSIOCGBTNMAP, js_dev->btnmap);
		ioctl(js_dev->event_fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), &key_bits);
	}
	for (int i = BTN_MISC; i < BTN_GEAR_UP + 1; i++) {
		if (test_bit(key_bits, i)) {
			printf("Adding BTN: 0x%x\n", i);
			ioctl(js_dev->uinput_fd, UI_SET_KEYBIT, i);
		}
	}
#undef test_bit
#define test_bit(array, bit) ((array[bit / (8 * sizeof(unsigned long))] >> (bit % (8 * sizeof(unsigned long)))) & 1)
	unsigned long abs_features[BITS_TO_LONGS(ABS_CNT)];
	memset(abs_features, 0, sizeof(abs_features));
	if (js_dev->axes > 0) {
		ioctl(js_dev->uinput_fd, UI_SET_EVBIT, EV_ABS);
		memset(js_dev->axmap, 0, sizeof(js_dev->axmap));
		ioctl(js_dev->fd, JSIOCGAXMAP, js_dev->axmap);
		memset(abs_features, 0, sizeof(abs_features));
		if (ioctl(js_dev->event_fd, EVIOCGBIT(EV_ABS, sizeof(abs_features)), abs_features) == -1) {
			perror("Ioctl abs features query");
			exit(1);
		}
	}
	for (int i = ABS_X; i < ABS_CNT; i++) {
		if (test_bit(abs_features, i)) {
			printf("Adding ABS: 0x%x\n", i);
			ioctl(js_dev->uinput_fd, UI_SET_ABSBIT, i);
		}
	}
	/* Force Feedback */
	unsigned long ff_features[BITS_TO_LONGS(FF_CNT)];
	memset(ff_features, 0, sizeof(ff_features));
	if (ioctl(js_dev->event_fd, EVIOCGBIT(EV_FF, sizeof(ff_features)), ff_features) == -1) {
		perror("Ioctl force feedback features query");
		exit(1);
	}
	int has_ff = 0;
	int max_ff_effects = 0;
	for (int i = FF_EFFECT_MIN; i < FF_CNT; i++) {
		if (test_bit(ff_features, i)) {
			printf("Adding Force Feedback Effect: 0x%x\n", i);
			ioctl(js_dev->uinput_fd, UI_SET_FFBIT, i);
			has_ff = 1;
		}
	}
	if (has_ff) {
		ioctl(js_dev->uinput_fd, UI_SET_EVBIT, EV_FF);
		ioctl(js_dev->event_fd, EVIOCGEFFECTS, &max_ff_effects);
	}
#undef test_bit
	memset(&usetup, 0, sizeof(usetup));
	usetup.id.bustype = BUS_USB;
	usetup.id.vendor = 0x776C;
	usetup.id.product = 0x6A73;
	usetup.id.version = 123;
	usetup.ff_effects_max = max_ff_effects;
	char wayland_display_num = '1';
	char *wayland_display_name = getenv("WAYLAND_DISPLAY");
	if (wayland_display_name) {
		wayland_display_num = wayland_display_name[strlen(wayland_display_name) - 1];
	}
	char *js_name;
	asprintf(&js_name, "%d Wayland %c Joystick %d", getuid(), wayland_display_num, js_slot);
	js_dev->slot = js_slot;
	strcpy(usetup.name, js_name);
	free(js_name);
	ioctl(js_dev->uinput_fd, UI_DEV_SETUP, &usetup);
	ioctl(js_dev->uinput_fd, UI_DEV_CREATE);
	struct gp_event gp_ev;
	gp_ev.code = GP_HOTPLUG_EVENT;
	gp_ev.js = js_slot;
	gp_ev.hotplug.connected = 1;
	write(compositor_fds[1], (const void *) &gp_ev, sizeof(gp_ev));
	printf("Successfully added wayland joystick %d: %s\n", js_slot, js_dev->event_node_name);
	num_josyticks++;
}

static void remove_joystick(const char *node_name)
{
	struct joystick *js_dev = NULL;
	for (int i = 0; i < MAX_JOYSTICKS; i++) {
		if (joysticks[i].node_name && !strcmp(node_name, joysticks[i].node_name)) {
			js_dev = &joysticks[i];
		}
	}
	if (!js_dev) {
		return;
	}
	printf("Removing %s\n", js_dev->node_name);
	printf("EPOLL_CTL_DEL %d\n", js_dev->fd);
	if (epoll_ctl(epollfd, EPOLL_CTL_DEL, js_dev->fd, NULL) == -1) {
		printf("epoll_ctl: Failed to remove joystick from epoll\n");
	}
	printf("EPOLL_CTL_DEL %d\n", js_dev->uinput_fd);
	if (epoll_ctl(epollfd, EPOLL_CTL_DEL, js_dev->uinput_fd, NULL) == -1) {
		printf("epoll_ctl: Failed to remove uinput joystick from epoll\n");
	}
	struct gp_event gp_ev;
	gp_ev.code = GP_HOTPLUG_EVENT;
	gp_ev.js = js_dev->slot;
	gp_ev.hotplug.connected = 0;
	write(compositor_fds[1], (const void *) &gp_ev, sizeof(gp_ev));
	ioctl(js_dev->uinput_fd, UI_DEV_DESTROY);
	close(js_dev->uinput_fd);
	js_dev->uinput_fd = -1;
	fchmod(js_dev->fd, js_dev->orig_mode);
	close(js_dev->fd);
	js_dev->fd = -1;
	free(js_dev->node_name);
	js_dev->node_name = NULL;
	fchmod(js_dev->event_fd, js_dev->event_orig_mode);
	close(js_dev->event_fd);
	js_dev->event_fd = -1;
	free(js_dev->event_node_name);
	js_dev->event_node_name = NULL;
	chmod(js_dev->uevent_path, js_dev->uevent_orig_mode);
	free(js_dev->uevent_path);
	js_dev->uevent_path = NULL;
	free(js_dev->axis);
	free(js_dev->button);
	num_josyticks--;
}

static void free_resources()
{
	for (int i = 0; i < MAX_JOYSTICKS; i++) {
		if (joysticks[i].node_name) {
			remove_joystick(joysticks[i].node_name);
		}
	}
	close(epollfd);
	udev_unref(udev);
}

static void signal_handler(int signum)
{
	if (signum == SIGINT) {
		free_resources();
	}
}

int main(int argc, char *argv[])
{
	struct js_event js;
	struct input_event ie;

	struct udev_enumerate *enumerate;
	struct udev_list_entry *devices, *dev_list_entry;
	struct udev_device *dev;
	struct epoll_event events[MAX_EVENTS];

	prctl(PR_SET_PDEATHSIG, SIGINT);

	signal(SIGINT, signal_handler);

	if (argc < 3) {
		printf("Program must be called with read and write descriptors as arguments\n");
		exit(EXIT_FAILURE);
	}

	epollfd = epoll_create1(0);
	if (epollfd == -1) {
		perror("epoll_create1");
		exit(EXIT_FAILURE);
	}

	udev = udev_new();
	if (!udev) {
		printf("Can't create udev\n");
		exit(EXIT_FAILURE);
	}

	compositor_fds[0] = atoi(argv[1]);
	compositor_fds[1] = atoi(argv[2]);

	ev.events = EPOLLIN;
	ev.data.fd = compositor_fds[0];

	if (epoll_ctl(epollfd, EPOLL_CTL_ADD, compositor_fds[0], &ev) == -1) {
		printf("epoll_ctl: Failed to add read pipe from compositor\n");
		exit(EXIT_FAILURE);
	}

	enumerate = udev_enumerate_new(udev);
	udev_enumerate_add_match_property(enumerate, "ID_INPUT_JOYSTICK", "1");
	udev_enumerate_scan_devices(enumerate);
	devices = udev_enumerate_get_list_entry(enumerate);
        memset(joysticks, 0, sizeof(joysticks));
	udev_list_entry_foreach(dev_list_entry, devices) {
		const char *path;

		path = udev_list_entry_get_name(dev_list_entry);
		printf("path: %s\n", path);
		dev = udev_device_new_from_syspath(udev, path);
		add_joystick(dev, 0);
		udev_device_unref(dev);
	}

	udev_enumerate_unref(enumerate);

	struct udev_monitor *mon = udev_monitor_new_from_netlink(udev, "udev");
	udev_monitor_filter_add_match_subsystem_devtype(mon, "input", NULL);
	udev_monitor_enable_receiving(mon);
	int udev_mon_fd = udev_monitor_get_fd(mon);

	ev.events = EPOLLIN;
	ev.data.fd = udev_mon_fd;

	if (epoll_ctl(epollfd, EPOLL_CTL_ADD, udev_mon_fd, &ev) == -1) {
		printf("epoll_ctl: Failed to add udev joystick monitor\n");
		exit(EXIT_FAILURE);
	}

	int consumer_state = 1;

	while (1) {
		int nfds = epoll_wait(epollfd, events, MAX_EVENTS, -1);
		if (nfds == -1) {
			perror("epoll_wait");
			exit(EXIT_FAILURE);
		}

		for (int n = 0; n < nfds; ++n) {
			if (events[n].data.fd == compositor_fds[0]) {
				struct gp_request gp_rq;
				memset(&gp_rq, 0, sizeof(gp_rq));
				read(compositor_fds[0], &gp_rq, sizeof(gp_rq));
				if (gp_rq.code == GP_CONSUMER_STATE) {
					consumer_state = gp_rq.consumer_state.state;
					if (consumer_state) {
						for (int i = 0; i < MAX_JOYSTICKS; i++) {
							if (joysticks[i].fd > 0) {
								for (int b = 0; b < joysticks[i].buttons; b++) {
									emit(joysticks[i].uinput_fd, EV_KEY, joysticks[i].btnmap[b], joysticks[i].button[b]);
									emit(joysticks[i].uinput_fd, EV_SYN, SYN_REPORT, 0);
								}
								for (int a = 0; a < joysticks[i].axes; a++) {
									emit(joysticks[i].uinput_fd, EV_ABS, ABS_X + joysticks[i].axmap[a], joysticks[i].axis[a]);
									emit(joysticks[i].uinput_fd, EV_SYN, SYN_REPORT, 0);
								}
							}
						}
					} else {
						for (int i = 0; i < MAX_JOYSTICKS; i++) {
							if (joysticks[i].fd > 0) {
								usleep(300000);
								for (int b = 0; b < joysticks[i].buttons; b++) {
									if (joysticks[i].button[b]) {
										emit(joysticks[i].uinput_fd, EV_KEY, joysticks[i].btnmap[b], 0);
										emit(joysticks[i].uinput_fd, EV_SYN, SYN_REPORT, 0);
										joysticks[i].button[b] = 0;
									}
								}
								for (int a = 0; a < joysticks[i].axes; a++) {
									if (joysticks[i].axis[a]) {
										emit(joysticks[i].uinput_fd, EV_ABS, ABS_X + joysticks[i].axmap[a], 0);
										emit(joysticks[i].uinput_fd, EV_SYN, SYN_REPORT, 0);
										joysticks[i].axis[a] = 0;
									}
								}
							}
						}
					}
				}
				continue;
			} else if (events[n].data.fd == udev_mon_fd) {
				struct udev_device *dev = udev_monitor_receive_device(mon);
				if (dev) {
					const char *node_name = udev_device_get_devnode(dev);
					const char *dev_path = udev_device_get_devpath(dev);
					const char *action = udev_device_get_action(dev);
					if (node_name && !strstr(dev_path, "virtual") && udev_device_get_property_value(dev, "ID_INPUT_JOYSTICK")) {
						printf("Joystick hotplug:\n");
						printf("   Node: %s\n", node_name);
						printf("   Subsystem: %s\n", udev_device_get_subsystem(dev));
						printf("   Devtype: %s\n", udev_device_get_devtype(dev));
						printf("   Devpath: %s\n", dev_path);
						printf("   Action: %s\n", action);
						if (!strcmp(action, "remove") && !strncmp(node_name, "/dev/input/js", strlen("/dev/input/js"))) {
							remove_joystick(node_name);
						} else if (!strcmp(action, "add") &&
							(!strncmp(node_name, "/dev/input/js", strlen("/dev/input/js")) ||
							!strncmp(node_name, "/dev/input/event", strlen("/dev/input/event")))) {
							add_joystick(dev, 1);
						}
					}
					udev_device_unref(dev);
				}
				else {
					printf("No Device from receive_device(). An error occured.\n");
				}
				continue;
			}
			struct joystick *js_dev = NULL;
			struct joystick *ev_dev = NULL;
			for (int i = 0; i < MAX_JOYSTICKS; i++) {
				if (events[n].data.fd == joysticks[i].fd) {
					js_dev = &joysticks[i];
					break;
				} else if (events[n].data.fd == joysticks[i].uinput_fd) {
					ev_dev = &joysticks[i];
					break;
				}
			}
			if (js_dev) {
				if (read(events[n].data.fd, &js, sizeof(struct js_event)) != sizeof(struct js_event)) {
					perror("\nwl-js: error reading");
					continue;
				}
			} else if (ev_dev) {
				if (read(events[n].data.fd, &ie, sizeof(struct input_event)) != sizeof(struct input_event)) {
					perror("\nwl-js: error reading");
					continue;
				}
				if (ie.type == EV_UINPUT) {
					if (ie.code == UI_FF_UPLOAD) {
						struct uinput_ff_upload upload_data;
						memset(&upload_data, 0, sizeof(upload_data));
						upload_data.request_id = ie.value;
						ioctl(ev_dev->uinput_fd, UI_BEGIN_FF_UPLOAD, &upload_data);
						ioctl(ev_dev->event_fd, EVIOCRMFF, upload_data.effect.id);
						upload_data.effect.id = -1;
						ioctl(ev_dev->event_fd, EVIOCSFF, &upload_data.effect);
						upload_data.retval = 0;
						ioctl(ev_dev->uinput_fd, UI_END_FF_UPLOAD, &upload_data);
					} else if (ie.code == UI_FF_ERASE) {
						struct uinput_ff_erase erase_data;
						memset(&erase_data, 0, sizeof(erase_data));
						erase_data.request_id = ie.value;
						ioctl(ev_dev->uinput_fd, UI_BEGIN_FF_ERASE, &erase_data);
						ioctl(ev_dev->event_fd, EVIOCRMFF, erase_data.effect_id);
						erase_data.retval = 0;
						ioctl(ev_dev->uinput_fd, UI_END_FF_ERASE, &erase_data);
					}
				} else if (ie.type == EV_FF) {
					if (ie.code == FF_GAIN) {
						printf("Setting force feedback gain to %d%% ... \n", (int)(((ie.value * 1.0f) / 0xFFFF) * 100));
					} else if (ie.value) {
						printf("Playing rumble effect code 0x%x value 0x%x on event fd %d..\n", ie.code, ie.value, ev_dev->event_fd);
					}
					write(ev_dev->event_fd, (const void*) &ie, sizeof(ie));
				}
				continue;
			} else {
				continue;
			}

			printf("\r");

			switch(js.type & ~JS_EVENT_INIT) {
			case JS_EVENT_BUTTON:
				js_dev->button[js.number] = js.value;
				if (js_dev->buttons) {
					if (consumer_state) {
						emit(js_dev->uinput_fd, EV_KEY, js_dev->btnmap[js.number], js_dev->button[js.number]);
						emit(js_dev->uinput_fd, EV_SYN, SYN_REPORT, 0);
					}
					struct gp_event gp_ev;
					gp_ev.js = js_dev->slot;
					gp_ev.code = GP_BUTTON_EVENT;
					gp_ev.button.button = js_dev->btnmap[js.number];
					gp_ev.button.value  = js_dev->button[js.number];
					write(compositor_fds[1], (const void *) &gp_ev, sizeof(gp_ev));
					//printf("Buttons: ");
					for (int i = 0; i < js_dev->buttons; i++) {
					//	printf("%2d:%s ", i, js_dev->button[i] ? "on " : "off");
						if (!i && js_dev->button[i]) {
							struct input_event play;
							memset(&play, 0, sizeof(play));
							ioctl(js_dev->event_fd, EVIOCRMFF, js_dev->rumble_effect.id);
							memset(&js_dev->rumble_effect, 0, sizeof(js_dev->rumble_effect));
							js_dev->rumble_effect.type = FF_RUMBLE;
							js_dev->rumble_effect.id = -1;
							js_dev->rumble_effect.u.rumble.strong_magnitude = 0x8000;
							js_dev->rumble_effect.u.rumble.weak_magnitude = 0;
							js_dev->rumble_effect.replay.length = 500;
							js_dev->rumble_effect.replay.delay = 0;
							ioctl(js_dev->event_fd, EVIOCSFF, &js_dev->rumble_effect);
							play.type = EV_FF;
							play.code = js_dev->rumble_effect.id;
							play.value = 1;
							write(js_dev->event_fd, (const void*) &play, sizeof(play));
						}
					}
				}
				break;
			case JS_EVENT_AXIS:
				js_dev->axis[js.number] = js.value;
				if (js_dev->axes) {
					if (consumer_state) {
						emit(js_dev->uinput_fd, EV_ABS, ABS_X + js_dev->axmap[js.number], js_dev->axis[js.number]);
						emit(js_dev->uinput_fd, EV_SYN, SYN_REPORT, 0);
					}
					struct gp_event gp_ev;
					gp_ev.js = js_dev->slot;
					gp_ev.code = GP_AXIS_EVENT;
					gp_ev.axis.axis  = ABS_X + js_dev->axmap[js.number];
					gp_ev.axis.value = js_dev->axis[js.number];
					write(compositor_fds[1], (const void *) &gp_ev, sizeof(gp_ev));
					//printf("Axes: ");
					//for (int i = 0; i < js_dev->axes; i++) {
					//	printf("%2d:%6d ", i, js_dev->axis[i]);
					//}
				}
				break;
			}


			fflush(stdout);
		}
	}

	free_resources();

	return 0;
}
