/*
 * Copyright 2024 Intel Corp.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * VA LINUX SYSTEMS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef EF1E_TP_PROTOCOL_H
#define EF1E_TP_PROTOCOL_H

#define TP_PROTOCOL_COMMAND_ID_TP_REPORT	0x60
#define TP_PROTOCOL_COMMAND_ID_QUERY		0xfe
/* Command id + data length + checksum */
#define TP_PROTOCOL_TRANSPORT_OVERHEAD		3
#define TP_PROTOCOL_DATA_LENGTH_TP_REPORT	62

#define TP_PROTOCOL_TP_STATUS_PRESS		0xc0
#define TP_PROTOCOL_TP_STATUS_RELEASE		0xa0
#define TP_PROTOCOL_TP_STATUS_MOVE		0x90

#define TP_MAX_POINTS				10
#define TP_PRESSURE_MAX				100

static inline const char *tp_status_str(u8 status)
{
	switch (status) {
	case TP_PROTOCOL_TP_STATUS_MOVE:
		return "move";
	case TP_PROTOCOL_TP_STATUS_PRESS:
		return "press";
	case TP_PROTOCOL_TP_STATUS_RELEASE:
		return "release";
	}
	return "unknown";
}

struct __packed tp_point_data {
	u8 id;
	u8 status;
	u8 x_h;
	u8 x_l;
	u8 y_h;
	u8 y_l;
};


struct __packed tp_report_data {
	u8 command_id;
	u8 data_length_h;
	u8 data_length_l;
	u8 number_points;
	struct tp_point_data point_data[TP_MAX_POINTS];
	u8 checksum;
};

#endif /* EF1E_TP_PROTOCOL_H */

