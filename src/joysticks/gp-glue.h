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

#pragma once

#define GP_CONSUMER_STATE	0x0

#define GP_HOTPLUG_EVENT	0x0
#define GP_BUTTON_EVENT		0x2
#define GP_AXIS_EVENT		0x3

struct gp_consumer_state {
	int state;
};

struct gp_hotplug {
	int connected;
};

struct gp_button {
	int button;
	int value;
};

struct gp_axis {
	int axis;
	int value;
};

struct gp_event {
	int js;
	int code;
	union {
		struct gp_hotplug hotplug;
		struct gp_button button;
		struct gp_axis axis;
	};
};

struct gp_request {
	int js;
	int code;
	union {
		struct gp_consumer_state consumer_state;
	};
};
