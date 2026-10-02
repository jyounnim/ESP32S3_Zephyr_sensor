/*
 * ssd1306_display.h
 *
 * Minimal SSD1306 128x64 I2C text display driver, written raw against
 * the I2C API instead of Zephyr's in-tree display subsystem.
 *
 * Why raw I2C and not the official `solomon,ssd1306fb` driver + CFB:
 * this series' roadmap already root-caused a confirmed ESP32 Zephyr
 * I2C driver bug where multi-segment write transactions are not
 * reliably concatenated (see Lab 02 / parked SSD1306 I2C lab notes).
 * The official display driver issues the control byte and the pixel
 * payload as separate i2c messages, which reproduces that exact bug
 * on this SoC. This driver instead always merges the control byte and
 * the payload into ONE contiguous buffer and issues a single
 * `i2c_write()`, which is the workaround this series has already
 * validated on real hardware.
 */

#ifndef SSD1306_DISPLAY_H_
#define SSD1306_DISPLAY_H_

#include <zephyr/device.h>
#include <stdint.h>
#include <stdbool.h>

#define SSD1306_WIDTH  128
#define SSD1306_HEIGHT 64
#define SSD1306_PAGES  (SSD1306_HEIGHT / 8) /* 8 pages, 8 rows each */

/* Probes 0x3C then 0x3D (write-based probing, per this series' I2C0
 * convention), runs the panel init sequence, and clears the screen.
 * Returns 0 on success, -ENODEV if neither address responds, or a
 * negative I2C errno otherwise.
 */
int ssd1306_display_init(const struct device *i2c_dev);

/* Clears the in-RAM framebuffer only. Call ssd1306_flush() to push it
 * to the panel.
 */
void ssd1306_clear(void);

/* Draws a string using the 5x7 font at character-cell granularity.
 * `page` is 0..7 (each page is one 8px text row on a 64px-tall panel).
 * `col` is the starting pixel column, 0..127. Characters not present
 * in the font table render as a blank cell. Text is clipped, not
 * wrapped, if it would run past column 127.
 */
void ssd1306_draw_text(uint8_t page, uint8_t col, const char *text);

/* Pixel primitives on the in-RAM framebuffer (added for the LD2402 lab).
 * Coordinates outside the panel are clipped.
 */
void ssd1306_set_pixel(int x, int y, bool on);
void ssd1306_draw_hline(int x0, int x1, int y);
void ssd1306_draw_vline(int x, int y0, int y1);

/* Pushes the in-RAM framebuffer to the panel as a single I2C write. */
int ssd1306_flush(void);

#endif /* SSD1306_DISPLAY_H_ */
