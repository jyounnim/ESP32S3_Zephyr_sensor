/*
 * ssd1306_display.c
 *
 * See ssd1306_display.h for the rationale behind driving this panel
 * with raw I2C calls instead of the in-tree display subsystem.
 */

#include "ssd1306_display.h"
#include "font5x7.h"

#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <errno.h>

LOG_MODULE_REGISTER(ssd1306, LOG_LEVEL_INF);

/* Candidate 7-bit addresses; 0x3C is by far the most common on 4-pin
 * I2C SSD1306 breakout boards, 0x3D shows up on a few variants.
 */
static const uint8_t candidate_addrs[] = { 0x3C, 0x3D };

static const struct device *dev_i2c;
static uint8_t panel_addr;

/* GDDRAM mirror. Contiguous in memory (page-major), so it can be
 * pushed to the panel with a single memcpy + single I2C write.
 */
static uint8_t framebuffer[SSD1306_PAGES][SSD1306_WIDTH];

/* Standard SSD1306 128x64 init sequence (see SSD1306 datasheet,
 * section "Command Table"). Sent as ONE command stream: the control
 * byte 0x00 with the Co bit cleared tells the panel every following
 * byte is a command, so this entire array can go out as a single
 * i2c_write() alongside that one control byte.
 */
static const uint8_t init_cmds[] = {
	0xAE,       /* display off */
	0xD5, 0x80, /* clock divide ratio / oscillator frequency */
	0xA8, 0x3F, /* multiplex ratio = 64 - 1 */
	0xD3, 0x00, /* display offset = 0 */
	0x40,       /* display start line = 0 */
	0x8D, 0x14, /* charge pump enable (required for most 3.3V-only modules) */
	0x20, 0x00, /* memory addressing mode = horizontal */
	0xA1,       /* segment remap (column 127 -> SEG0) */
	0xC8,       /* COM output scan direction, remapped */
	0xDA, 0x12, /* COM pins hardware configuration */
	0x81, 0xCF, /* contrast control */
	0xD9, 0xF1, /* pre-charge period */
	0xDB, 0x40, /* VCOMH deselect level */
	0xA4,       /* resume RAM content display (not "all pixels on") */
	0xA6,       /* normal display (not inverted) */
	/* Fix the addressing window to the full panel once. In horizontal
	 * addressing mode the GDDRAM pointer auto-wraps from the last
	 * column of one page to the first column of the next, and wraps
	 * from page 7 back to page 0 after a full 1024-byte write - so
	 * this only needs to be set once, not before every flush.
	 */
	0x21, 0x00, 0x7F, /* column address range: 0..127 */
	0x22, 0x00, 0x07, /* page address range: 0..7 */
	0xAF,             /* display on */
};

static int send_cmd_stream(uint8_t addr, const uint8_t *cmds, size_t len)
{
	/* control byte (0x00 = command stream) + payload, single buffer */
	uint8_t buf[64];

	if (len + 1 > sizeof(buf)) {
		return -ENOMEM;
	}

	buf[0] = 0x00;
	memcpy(&buf[1], cmds, len);

	return i2c_write(dev_i2c, buf, len + 1, addr);
}

static int probe_panel(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(candidate_addrs); i++) {
		/* A real command (display-off) doubles as the probe: a
		 * NACK from an unpopulated address returns a non-zero
		 * errno here rather than corrupting anything, since only
		 * the addressed device (if any) reacts to it.
		 */
		int ret = send_cmd_stream(candidate_addrs[i], init_cmds, 1);

		if (ret == 0) {
			panel_addr = candidate_addrs[i];
			LOG_INF("SSD1306 found at 0x%02X", panel_addr);
			return 0;
		}
	}

	LOG_ERR("SSD1306 not found at 0x3C or 0x3D");
	return -ENODEV;
}

int ssd1306_display_init(const struct device *i2c_dev)
{
	int ret;

	dev_i2c = i2c_dev;

	ret = probe_panel();
	if (ret) {
		return ret;
	}

	/* Boot stabilization delay, same convention as the OLED I2C labs
	 * earlier in this series.
	 */
	k_msleep(100);

	ret = send_cmd_stream(panel_addr, init_cmds, sizeof(init_cmds));
	if (ret) {
		LOG_ERR("SSD1306 init command stream failed: %d", ret);
		return ret;
	}

	ssd1306_clear();
	return ssd1306_flush();
}

void ssd1306_clear(void)
{
	memset(framebuffer, 0, sizeof(framebuffer));
}

void ssd1306_draw_text(uint8_t page, uint8_t col, const char *text)
{
	if (page >= SSD1306_PAGES) {
		return;
	}

	for (const char *p = text; *p != '\0'; p++) {
		if (col + FONT5X7_WIDTH > SSD1306_WIDTH) {
			break; /* clip, don't wrap */
		}

		const uint8_t *glyph = font5x7_lookup(*p);

		if (glyph != NULL) {
			memcpy(&framebuffer[page][col], glyph, FONT5X7_WIDTH);
		} else {
			memset(&framebuffer[page][col], 0, FONT5X7_WIDTH);
		}

		col += FONT5X7_WIDTH;

		/* 1px inter-character gap */
		if (col < SSD1306_WIDTH) {
			framebuffer[page][col] = 0x00;
			col += 1;
		}
	}
}

void ssd1306_set_pixel(int x, int y, bool on)
{
	uint8_t mask;

	if (x < 0 || x >= SSD1306_WIDTH || y < 0 || y >= SSD1306_HEIGHT) {
		return;
	}

	/* page-major GDDRAM: bit0 of each byte is the top row of the page */
	mask = (uint8_t)(1u << (y % 8));
	if (on) {
		framebuffer[y / 8][x] |= mask;
	} else {
		framebuffer[y / 8][x] &= (uint8_t)~mask;
	}
}

void ssd1306_draw_hline(int x0, int x1, int y)
{
	for (int x = x0; x <= x1; x++) {
		ssd1306_set_pixel(x, y, true);
	}
}

void ssd1306_draw_vline(int x, int y0, int y1)
{
	for (int y = y0; y <= y1; y++) {
		ssd1306_set_pixel(x, y, true);
	}
}

int ssd1306_flush(void)
{
	/* control byte (0x40 = data stream) + full 1024-byte framebuffer,
	 * single contiguous buffer, single i2c_write() - see the header
	 * comment for why this must never be split into two messages.
	 */
	static uint8_t tx_buf[1 + sizeof(framebuffer)];

	tx_buf[0] = 0x40;
	memcpy(&tx_buf[1], framebuffer, sizeof(framebuffer));

	return i2c_write(dev_i2c, tx_buf, sizeof(tx_buf), panel_addr);
}
