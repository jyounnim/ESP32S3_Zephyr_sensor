/*
 * radar_display.h - SSD1306 (128x64) renderer for radar_msg.
 *
 * Same API on both platforms, different backend:
 *   ESP32-S3 : radar_display_ssd1306.c (raw-I2C driver, 5x7 font)
 *   SR110    : radar_display.c (Zephyr solomon,ssd1306 + CFB, 10x16 font)
 */
#ifndef RADAR_DISPLAY_H_
#define RADAR_DISPLAY_H_

#include <stdbool.h>
#include "radar_msg.h"

/* Uses the devicetree chosen node "zephyr,display". */
int radar_display_init(void);

void radar_display_show_text(const char *line1, const char *line2);

void radar_display_render(const struct radar_msg *msg, bool link_ok);

#endif /* RADAR_DISPLAY_H_ */
