/*
 * display_main.c - display core application (identical on ESP32-S3 and SR110).
 *
 *   ESP32-S3 : runs on APP_CPU (core 1)
 *   SR110    : runs on Cortex-M55
 *
 * Flow:
 *   1. Init SSD1306 through CFB, show a waiting screen.
 *   2. Register the link rx callback (ISR -> k_msgq).
 *   3. Loop: take the newest message, track link health, redraw on change.
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "radar_display.h"
#include "radar_link.h"
#include "radar_msg.h"

#define UI_TICK_MS        200
#define LINK_TIMEOUT_MS   3000 /* sensor core sends at least every 500 ms */

K_MSGQ_DEFINE(radar_msgq, sizeof(struct radar_msg), 8, 4);

/* Drain the queue and keep only the newest message. */
static bool take_newest(struct radar_msg *out, k_timeout_t timeout)
{
	if (k_msgq_get(&radar_msgq, out, timeout) != 0) {
		return false;
	}
	while (k_msgq_get(&radar_msgq, out, K_NO_WAIT) == 0) {
	}
	return true;
}

int main(void)
{
	struct radar_msg latest = {0};
	bool have_msg = false;
	bool prev_link_ok = false;
	int64_t last_rx_ms = 0;

	printk("\n=== LD2402 AMP: display core (%s) ===\n", CONFIG_BOARD_TARGET);

	if (radar_display_init() != 0) {
		return 0;
	}
	radar_display_show_text("LINK: WAIT", "LD2402 AMP");

	if (radar_link_rx_init(&radar_msgq) != 0) {
		radar_display_show_text("LINK: FAIL", "IPC INIT ERR");
		return 0;
	}

	while (true) {
		struct radar_msg msg;
		bool got_msg = take_newest(&msg, K_MSEC(UI_TICK_MS));
		bool link_ok;

		if (got_msg) {
			latest = msg;
			have_msg = true;
			last_rx_ms = k_uptime_get();
		}

		link_ok = have_msg && (k_uptime_get() - last_rx_ms) < LINK_TIMEOUT_MS;

		if (link_ok != prev_link_ok) {
			printk("[DISPLAY] link %s (last seq=%u)\n", link_ok ? "UP" : "DOWN",
			       latest.seq);
		}

		if (got_msg || link_ok != prev_link_ok) {
			radar_display_render(&latest, link_ok);
		}
		prev_link_ok = link_ok;
	}

	return 0;
}
