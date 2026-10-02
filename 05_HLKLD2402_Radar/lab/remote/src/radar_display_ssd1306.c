/*
 * radar_display_ssd1306.c - ESP32-S3 implementation of radar_display.h.
 *
 * Uses the raw-I2C SSD1306 driver (ssd1306_display.c) that was hardware-
 * verified in the multi-sensor AMP labs, NOT Zephyr's solomon,ssd1306 +
 * CFB: on ESP32-S3 the in-tree display driver splits the control byte and
 * the pixel payload into separate I2C messages, which hits the known ESP32
 * I2C driver issue and produces noise. The raw driver always sends one
 * contiguous i2c_write().
 *
 * Screen layout (5x7 font, 8 px text rows = SSD1306 pages):
 *   page 0 (y= 0.. 7)  "LINK: OK" / "LINK: FAIL"
 *   page 2 (y=16..23)  "MOVE  123cm" ... + direction arrow at x=112..127
 *   page 3 (y=24..31)  "APPROACH -96cm/s"
 *   y=32               target-gate marker
 *   y=34..63           16-gate motion energy bar graph
 */
#include "radar_display.h"

#include <errno.h>
#include <stdio.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "ssd1306_display.h"

#define PAGE_LINK       0
#define PAGE_STATE      2
#define PAGE_TREND      3

#define MARKER_Y        32
#define BAR_TOP_Y       34
#define BAR_BOTTOM_Y    63
#define BAR_MAX_H       (BAR_BOTTOM_Y - BAR_TOP_Y + 1) /* 30 px */
#define BAR_PITCH       8                              /* 16 gates x 8 px = 128 px */
#define BAR_WIDTH       6

/* map 10..70 dB to full bar height (background ~10-25 dB) */
#define DB_FLOOR        10
#define DB_CEIL         70

#define GATE_SIZE_CM    70

/* direction arrow, vertically centered in page 2 */
#define ARROW_X0        112
#define ARROW_X1        127
#define ARROW_Y         19
#define ARROW_HEAD      3

static const struct device *const i2c_dev = DEVICE_DT_GET(DT_NODELABEL(i2c1));

int radar_display_init(void)
{
	int ret;

	if (!device_is_ready(i2c_dev)) {
		printk("[DISP] i2c1 not ready\n");
		return -ENODEV;
	}

	ret = ssd1306_display_init(i2c_dev);
	if (ret != 0) {
		printk("[DISP] SSD1306 init failed (%d)\n", ret);
	}
	return ret;
}

void radar_display_show_text(const char *line1, const char *line2)
{
	ssd1306_clear();
	if (line1 != NULL) {
		ssd1306_draw_text(PAGE_LINK, 0, line1);
	}
	if (line2 != NULL) {
		ssd1306_draw_text(PAGE_STATE, 0, line2);
	}
	ssd1306_flush();
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

static const char *direction_label(uint8_t direction)
{
	switch (direction) {
	case RADAR_DIR_APPROACH:
		return "APPROACH";
	case RADAR_DIR_LEAVE:
		return "LEAVE";
	case RADAR_DIR_STEADY:
		return "STEADY";
	default:
		return NULL;
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

static void draw_energy_bars(const uint8_t db[RADAR_GATE_COUNT])
{
	for (int gate = 0; gate < RADAR_GATE_COUNT; gate++) {
		int h = db_to_bar_height(db[gate]);
		int x0 = gate * BAR_PITCH;

		/* 1 px baseline so empty gates are still visible */
		ssd1306_draw_hline(x0, x0 + BAR_WIDTH - 1, BAR_BOTTOM_Y);

		for (int x = x0; x < x0 + BAR_WIDTH && h > 0; x++) {
			ssd1306_draw_vline(x, BAR_BOTTOM_Y - h + 1, BAR_BOTTOM_Y);
		}
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
	ssd1306_draw_hline(x0, x0 + BAR_WIDTH - 1, MARKER_Y);
}

/* Gate 0 (= sensor) is on the left: approach points left, leave points right. */
static void draw_direction(uint8_t direction)
{
	switch (direction) {
	case RADAR_DIR_APPROACH:
		ssd1306_draw_hline(ARROW_X0, ARROW_X1, ARROW_Y);
		for (int i = 1; i <= ARROW_HEAD; i++) {
			ssd1306_draw_vline(ARROW_X0 + i, ARROW_Y - i, ARROW_Y + i);
		}
		break;
	case RADAR_DIR_LEAVE:
		ssd1306_draw_hline(ARROW_X0, ARROW_X1, ARROW_Y);
		for (int i = 1; i <= ARROW_HEAD; i++) {
			ssd1306_draw_vline(ARROW_X1 - i, ARROW_Y - i, ARROW_Y + i);
		}
		break;
	case RADAR_DIR_STEADY:
		ssd1306_draw_hline(ARROW_X0 + 4, ARROW_X1 - 4, ARROW_Y);
		break;
	default:
		break;
	}
}

/* Outlined progress bar with a filled part, y = 44..55 */
static void draw_progress_bar(uint8_t percent)
{
	int fill = (percent * 126) / 100;

	ssd1306_draw_hline(0, 127, 44);
	ssd1306_draw_hline(0, 127, 55);
	ssd1306_draw_vline(0, 44, 55);
	ssd1306_draw_vline(127, 44, 55);
	for (int x = 1; x <= fill; x++) {
		ssd1306_draw_vline(x, 46, 53);
	}
}

/* Calibration screens (auto threshold generation on the sensor core). */
static void render_calibration(const struct radar_msg *msg)
{
	char line[24];

	switch (msg->calib_state) {
	case RADAR_CALIB_COUNTDOWN:
		ssd1306_draw_text(PAGE_STATE, 0, "AUTO THRESHOLD");
		ssd1306_draw_text(PAGE_TREND, 0, "LEAVE THE AREA");
		snprintf(line, sizeof(line), "START IN %us", (unsigned int)msg->calib_pct);
		ssd1306_draw_text(5, 0, line);
		break;
	case RADAR_CALIB_RUNNING:
		ssd1306_draw_text(PAGE_STATE, 0, "AUTO THRESHOLD");
		snprintf(line, sizeof(line), "CALIBRATING %u%%", (unsigned int)msg->calib_pct);
		ssd1306_draw_text(PAGE_TREND, 0, line);
		draw_progress_bar(msg->calib_pct);
		break;
	case RADAR_CALIB_DONE:
		ssd1306_draw_text(PAGE_STATE, 0, "CALIB DONE");
		ssd1306_draw_text(PAGE_TREND, 0, "SAVED TO RADAR");
		break;
	default:
		ssd1306_draw_text(PAGE_STATE, 0, "CALIB FAIL");
		ssd1306_draw_text(PAGE_TREND, 0, "SEE CONSOLE");
		break;
	}
}

void radar_display_render(const struct radar_msg *msg, bool link_ok)
{
	char line[24];
	bool sensor_ok = (msg->flags & RADAR_FLAG_SENSOR_OK) != 0;
	bool has_target = (msg->presence != RADAR_PRESENCE_NONE);
	const char *dir_text = direction_label(msg->direction);

	ssd1306_clear();

	/* page 0: inter-core link status - always on top */
	ssd1306_draw_text(PAGE_LINK, 0, link_ok ? "LINK: OK" : "LINK: FAIL");

	if (!link_ok) {
		ssd1306_flush();
		return;
	}

	if (msg->calib_state != RADAR_CALIB_IDLE) {
		render_calibration(msg);
		ssd1306_flush();
		return;
	}

	/* page 2: radar state */
	if (!sensor_ok) {
		snprintf(line, sizeof(line), "NO SENSOR");
	} else if (has_target) {
		snprintf(line, sizeof(line), "%-5s %4ucm", presence_label(msg->presence),
			 (unsigned int)msg->distance_cm);
	} else {
		snprintf(line, sizeof(line), "%s", presence_label(msg->presence));
	}
	ssd1306_draw_text(PAGE_STATE, 0, line);

	/* page 3 + arrow: approach / leave (needs ~0.8 s of samples) */
	if (sensor_ok && has_target && dir_text != NULL) {
		snprintf(line, sizeof(line), "%-8s %+dcm/s", dir_text, (int)msg->speed_cms);
		ssd1306_draw_text(PAGE_TREND, 0, line);
		draw_direction(msg->direction);
	}

	/* bottom: per-gate motion energy (engineering mode only) */
	if (sensor_ok && (msg->flags & RADAR_FLAG_ENERGY) != 0) {
		if (has_target) {
			draw_target_marker(msg->distance_cm);
		}
		draw_energy_bars(msg->motion_db);
	} else if (sensor_ok) {
		ssd1306_draw_text(5, 0, "ASCII MODE");
	}

	ssd1306_flush();
}
