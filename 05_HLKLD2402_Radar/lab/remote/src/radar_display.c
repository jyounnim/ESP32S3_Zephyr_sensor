/*
 * radar_display.c - SSD1306 renderer for radar_msg (see radar_display.h).
 */
#include "radar_display.h"

#include <errno.h>
#include <stdio.h>
#include <zephyr/device.h>
#include <zephyr/display/cfb.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define FONT_HEIGHT_WANTED  16

#define ROW_LINK_Y      0
#define ROW_STATE_Y     16
#define MARKER_Y        32
#define BAR_TOP_Y       34
#define BAR_BOTTOM_Y    63
#define BAR_MAX_H       (BAR_BOTTOM_Y - BAR_TOP_Y + 1) /* 30 px */
#define BAR_PITCH       8                              /* 16 gates x 8 px = 128 px */
#define BAR_WIDTH       6

/*
 * Bar scaling: typical background is 10..25 dB and thresholds sit around
 * 40..60 dB (manual / field experience), so map 10..70 dB to full height.
 */
#define DB_FLOOR        10
#define DB_CEIL         70

#define GATE_SIZE_CM    70

static const struct device *const display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

static void select_font(void)
{
	int count = cfb_get_numof_fonts(display);

	for (int idx = 0; idx < count; idx++) {
		uint8_t w;
		uint8_t h;

		if (cfb_get_font_size(display, idx, &w, &h) == 0 && h == FONT_HEIGHT_WANTED) {
			cfb_framebuffer_set_font(display, idx);
			return;
		}
	}
	cfb_framebuffer_set_font(display, 0); /* fallback: first font */
}

int radar_display_init(void)
{
	if (!device_is_ready(display)) {
		printk("[DISP] display device not ready\n");
		return -ENODEV;
	}

	if (display_blanking_off(display) != 0) {
		printk("[DISP] display_blanking_off failed\n");
	}

	if (cfb_framebuffer_init(display) != 0) {
		/* most common cause: CONFIG_HEAP_MEM_POOL_SIZE too small */
		printk("[DISP] cfb_framebuffer_init failed\n");
		return -EIO;
	}

	select_font();
	cfb_set_kerning(display, 0);
	cfb_framebuffer_clear(display, true);
	return 0;
}

void radar_display_show_text(const char *line1, const char *line2)
{
	cfb_framebuffer_clear(display, false);
	if (line1 != NULL) {
		cfb_print(display, line1, 0, ROW_LINK_Y);
	}
	if (line2 != NULL) {
		cfb_print(display, line2, 0, ROW_STATE_Y);
	}
	cfb_framebuffer_finalize(display);
}

static const char *presence_label(uint8_t presence)
{
	switch (presence) {
	case RADAR_PRESENCE_MOVING:
		return "MOVE";
	case RADAR_PRESENCE_STILL:
		return "STILL";
	default:
		return "NONE";
	}
}

static int db_to_bar_height(uint8_t db)
{
	if (db <= DB_FLOOR) {
		return 0;
	}
	if (db >= DB_CEIL) {
		return BAR_MAX_H;
	}
	return ((db - DB_FLOOR) * BAR_MAX_H) / (DB_CEIL - DB_FLOOR);
}

static void draw_vline(int x, int y_top, int y_bottom)
{
	struct cfb_position start;
	struct cfb_position end;

	start.x = x;
	start.y = y_top;
	end.x = x;
	end.y = y_bottom;
	cfb_draw_line(display, &start, &end);
}

static void draw_hline(int x_left, int x_right, int y)
{
	struct cfb_position start;
	struct cfb_position end;

	start.x = x_left;
	start.y = y;
	end.x = x_right;
	end.y = y;
	cfb_draw_line(display, &start, &end);
}

static void draw_energy_bars(const uint8_t db[RADAR_GATE_COUNT])
{
	for (int gate = 0; gate < RADAR_GATE_COUNT; gate++) {
		int h = db_to_bar_height(db[gate]);
		int x0 = gate * BAR_PITCH;

		/* 1 px baseline so empty gates are still visible */
		draw_hline(x0, x0 + BAR_WIDTH - 1, BAR_BOTTOM_Y);

		for (int x = x0; x < x0 + BAR_WIDTH && h > 0; x++) {
			draw_vline(x, BAR_BOTTOM_Y - h + 1, BAR_BOTTOM_Y);
		}
	}
}

/*
 * Direction arrow at the right end of row 2 (x 112..127, y 16..31).
 * Gate 0 (= sensor) is on the LEFT of the bar graph, so:
 *   APPROACH -> arrow points left  (toward the sensor)
 *   LEAVE    -> arrow points right (away from the sensor)
 *   STEADY   -> short horizontal bar
 */
#define ARROW_X0   112
#define ARROW_X1   127
#define ARROW_Y    23

static void draw_direction(uint8_t direction)
{
	switch (direction) {
	case RADAR_DIR_APPROACH:
		draw_hline(ARROW_X0, ARROW_X1, ARROW_Y);
		for (int i = 1; i <= 4; i++) {
			draw_vline(ARROW_X0 + i, ARROW_Y - i, ARROW_Y + i);
		}
		break;
	case RADAR_DIR_LEAVE:
		draw_hline(ARROW_X0, ARROW_X1, ARROW_Y);
		for (int i = 1; i <= 4; i++) {
			draw_vline(ARROW_X1 - i, ARROW_Y - i, ARROW_Y + i);
		}
		break;
	case RADAR_DIR_STEADY:
		draw_hline(ARROW_X0 + 4, ARROW_X1 - 4, ARROW_Y);
		break;
	default:
		break; /* unknown: draw nothing */
	}
}

static void draw_target_marker(uint16_t distance_cm)
{
	int gate = distance_cm / GATE_SIZE_CM;
	int x0;

	if (gate >= RADAR_GATE_COUNT) {
		gate = RADAR_GATE_COUNT - 1;
	}
	x0 = gate * BAR_PITCH;
	draw_hline(x0, x0 + BAR_WIDTH - 1, MARKER_Y);
}

void radar_display_render(const struct radar_msg *msg, bool link_ok)
{
	char line[16];
	bool sensor_ok = (msg->flags & RADAR_FLAG_SENSOR_OK) != 0;
	bool has_target = (msg->presence != RADAR_PRESENCE_NONE);

	cfb_framebuffer_clear(display, false);

	/* row 1: inter-core link status - always on top */
	cfb_print(display, link_ok ? "LINK: OK" : "LINK: FAIL", 0, ROW_LINK_Y);

	if (!link_ok) {
		cfb_framebuffer_finalize(display);
		return;
	}

	/* row 2: radar state */
	if (!sensor_ok) {
		snprintf(line, sizeof(line), "NO SENSOR");
	} else if (has_target) {
		snprintf(line, sizeof(line), "%-5s%4ucm", presence_label(msg->presence),
			 (unsigned int)msg->distance_cm);
	} else {
		snprintf(line, sizeof(line), "%s", presence_label(msg->presence));
	}
	cfb_print(display, line, 0, ROW_STATE_Y);
	if (sensor_ok && has_target) {
		draw_direction(msg->direction);
	}

	/* bottom: per-gate motion energy (engineering mode only) */
	if (sensor_ok && (msg->flags & RADAR_FLAG_ENERGY) != 0) {
		if (has_target) {
			draw_target_marker(msg->distance_cm);
		}
		draw_energy_bars(msg->motion_db);
	} else if (sensor_ok) {
		cfb_print(display, "ASCII MODE", 0, 40);
	}

	cfb_framebuffer_finalize(display);
}
