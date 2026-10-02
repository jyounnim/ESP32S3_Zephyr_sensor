/*
 * sensor_main.c - sensor core application (identical on ESP32-S3 and SR110).
 *
 *   ESP32-S3 : runs on PRO_CPU (core 0)
 *   SR110    : runs on Cortex-M4 (Always-On core)
 *
 * Flow:
 *   1. Bind the LD2402 driver to the UART selected by alias "ld2402-uart".
 *   2. Startup config session: firmware version, dump of the current radar
 *      parameters (max distance, disappear delay, per-gate thresholds),
 *      switch to engineering mode. Retries, falls back to ASCII mode.
 *   3. Loop: read radar reports, median-filter the distance, estimate
 *      approach/leave (radar_trend), push radar_msg to the display core on
 *      change or periodically (heartbeat).
 *   4. Optional (devicetree alias "calib-button"): long press runs the
 *      radar's automatic threshold generation and saves it to radar flash,
 *      with countdown/progress shown on the display core.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "ld2402.h"
#include "radar_filter.h"
#include "radar_link.h"
#include "radar_msg.h"
#include "radar_trend.h"

#define LD2402_UART_NODE     DT_ALIAS(ld2402_uart)
#define CALIB_BUTTON_NODE    DT_ALIAS(calib_button)
#define HAS_CALIB_BUTTON     DT_NODE_EXISTS(CALIB_BUTTON_NODE)

#define STARTUP_RETRIES      5
#define STARTUP_RETRY_MS     1000
#define REPORT_WAIT_MS       500   /* loop tick while the radar is silent */
#define SENSOR_TIMEOUT_MS    2000  /* no report this long -> SENSOR_OK cleared */
#define DIST_DELTA_CM        10    /* distance change that triggers an immediate send */
#define PERIODIC_SEND_MS     500   /* bar-graph refresh + heartbeat */
#define LOG_PERIOD_MS        1000

/* auto threshold generation */
#define CALIB_LONG_PRESS_MS  2000
#define CALIB_COUNTDOWN_S    10    /* time to leave the detection area */
#define CALIB_COEFF_X10      30    /* trigger / hold / micro coefficient 3.0 (tool default) */
#define CALIB_POLL_MS        1000
#define CALIB_TIMEOUT_MS     120000
#define CALIB_RESULT_SHOW_MS 3000

static const struct device *const radar_uart = DEVICE_DT_GET(LD2402_UART_NODE);

#if HAS_CALIB_BUTTON
static const struct gpio_dt_spec calib_button = GPIO_DT_SPEC_GET(CALIB_BUTTON_NODE, gpios);
#endif

static uint32_t tx_seq;

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

static const char *presence_name(uint8_t presence)
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

static const char *direction_name(uint8_t direction)
{
	switch (direction) {
	case RADAR_DIR_APPROACH:
		return "APPROACH";
	case RADAR_DIR_LEAVE:
		return "LEAVE";
	case RADAR_DIR_STEADY:
		return "STEADY";
	default:
		return "-";
	}
}

static int send_msg(struct radar_msg *msg)
{
	msg->seq = ++tx_seq;
	return radar_link_send(msg);
}

/* ------------------------------------------------------------------------ */
/* Radar parameters                                                          */
/* ------------------------------------------------------------------------ */

static void print_gate_row(const char *label, const uint32_t thr[LD2402_GATE_COUNT])
{
	printk("[PARAM] %-9s:", label);
	for (int i = 0; i < LD2402_GATE_COUNT; i++) {
		if (thr == NULL) {
			printk(" %2d", i);
		} else {
			printk(" %2u", (unsigned int)ld2402_energy_to_db(thr[i]));
		}
	}
	printk("\n");
}

static void print_params(const struct ld2402_params *p)
{
	static const char *const interference[] = {"not checked", "none", "DETECTED"};
	uint32_t pi = p->power_interference;

	printk("[PARAM] max distance      : %u.%u m (raw %u)\n",
	       p->max_distance / 10, p->max_distance % 10, p->max_distance);
	printk("[PARAM] disappear delay   : %u s\n", p->disappear_delay_s);
	printk("[PARAM] power interference: %s\n", (pi < 3) ? interference[pi] : "?");
	print_gate_row("gate", NULL);
	print_gate_row("motion dB", p->motion_threshold);
	print_gate_row("micro dB", p->micro_threshold);
	printk("[PARAM] gate n = %d*n .. %d*(n+1) cm; threshold dB = 10*log10(raw)\n",
	       LD2402_GATE_SIZE_CM, LD2402_GATE_SIZE_CM);
}

/* One config session: version -> parameter dump -> engineering mode. */
static int configure_radar(void)
{
	struct ld2402_params params;
	char fw[16] = "unknown";
	int ret;
	int end_ret;

	ret = ld2402_config_begin();
	if (ret != 0) {
		return ret;
	}

	(void)ld2402_read_version(fw, sizeof(fw));
	printk("[SENSOR] LD2402 firmware %s\n", fw);

	if (ld2402_read_params(&params) == 0) {
		print_params(&params);
	} else {
		printk("[PARAM] parameter read failed\n");
	}

	ret = ld2402_set_engineering_mode();

	/* always leave config mode, otherwise the radar stops reporting */
	end_ret = ld2402_config_end();
	return (ret != 0) ? ret : end_ret;
}

static bool start_radar(void)
{
	for (int attempt = 1; attempt <= STARTUP_RETRIES; attempt++) {
		int ret = configure_radar();

		if (ret == 0) {
			printk("[SENSOR] engineering mode ON\n");
			return true;
		}
		printk("[SENSOR] config attempt %d/%d failed (%d)\n", attempt, STARTUP_RETRIES, ret);
		k_msleep(STARTUP_RETRY_MS);
	}

	printk("[SENSOR] falling back to normal (ASCII) mode - no gate energy\n");
	return false;
}

/* ------------------------------------------------------------------------ */
/* Auto threshold calibration (long press on calib-button)                   */
/* ------------------------------------------------------------------------ */

static void send_calib_status(uint8_t state, uint8_t value)
{
	struct radar_msg msg;

	memset(&msg, 0, sizeof(msg));
	msg.magic = RADAR_MSG_MAGIC;
	msg.uptime_ms = k_uptime_get_32();
	msg.flags = RADAR_FLAG_SENSOR_OK;
	msg.calib_state = state;
	msg.calib_pct = value;
	(void)send_msg(&msg);
}

/* Blocking: ~10 s countdown + generation time. Radar reports pause meanwhile. */
static bool run_auto_threshold(void)
{
	struct ld2402_params params;
	int64_t deadline;
	uint8_t percent = 0;
	int ret;

	for (int s = CALIB_COUNTDOWN_S; s > 0; s--) {
		printk("[CALIB] leave the detection area: %d s\n", s);
		send_calib_status(RADAR_CALIB_COUNTDOWN, (uint8_t)s);
		k_msleep(1000);
	}

	ret = ld2402_config_begin();
	if (ret != 0) {
		printk("[CALIB] config begin failed (%d)\n", ret);
		return false;
	}

	ret = ld2402_auto_threshold_start(CALIB_COEFF_X10, CALIB_COEFF_X10, CALIB_COEFF_X10);
	if (ret != 0) {
		printk("[CALIB] start failed (%d)\n", ret);
		goto out_end_config;
	}
	printk("[CALIB] started (coefficients %d.%d)\n", CALIB_COEFF_X10 / 10, CALIB_COEFF_X10 % 10);

	deadline = k_uptime_get() + CALIB_TIMEOUT_MS;
	while (percent < 100) {
		k_msleep(CALIB_POLL_MS);

		if (ld2402_auto_threshold_progress(&percent) == 0) {
			printk("[CALIB] progress %u%%\n", percent);
			send_calib_status(RADAR_CALIB_RUNNING, percent);
		}
		if (k_uptime_get() > deadline) {
			printk("[CALIB] timeout\n");
			ret = -ETIMEDOUT;
			goto out_end_config;
		}
	}

	ret = ld2402_save_params();
	printk("[CALIB] save to radar flash: %s (%d)\n", (ret == 0) ? "OK" : "FAILED", ret);

	if (ld2402_read_params(&params) == 0) {
		print_params(&params);
	}

	/* re-assert the output mode before leaving config mode */
	(void)ld2402_set_engineering_mode();

out_end_config:
	(void)ld2402_config_end();
	return ret == 0;
}

static void show_calib_result(bool ok)
{
	int64_t until = k_uptime_get() + CALIB_RESULT_SHOW_MS;

	/* keep sending so the display core does not report LINK: FAIL */
	while (k_uptime_get() < until) {
		send_calib_status(ok ? RADAR_CALIB_DONE : RADAR_CALIB_FAILED, 0);
		k_msleep(PERIODIC_SEND_MS);
	}
}

#if HAS_CALIB_BUTTON
static int64_t press_start_ms;
static bool press_handled;

static void calib_button_init(void)
{
	if (!gpio_is_ready_dt(&calib_button) ||
	    gpio_pin_configure_dt(&calib_button, GPIO_INPUT) != 0) {
		printk("[CALIB] button not available\n");
		return;
	}
	printk("[CALIB] hold the button %d s to run auto threshold\n",
	       CALIB_LONG_PRESS_MS / 1000);
}

/* Polled from the main loop (<= 500 ms period) - no ISR needed for a long press. */
static bool calib_long_press_detected(int64_t now)
{
	if (gpio_pin_get_dt(&calib_button) <= 0) {
		press_start_ms = 0;
		press_handled = false;
		return false;
	}
	if (press_start_ms == 0) {
		press_start_ms = now;
	}
	if (!press_handled && (now - press_start_ms) >= CALIB_LONG_PRESS_MS) {
		press_handled = true;
		return true;
	}
	return false;
}
#else
static void calib_button_init(void)
{
	printk("[CALIB] no calib-button alias - auto threshold disabled\n");
}

static bool calib_long_press_detected(int64_t now)
{
	ARG_UNUSED(now);
	return false;
}
#endif /* HAS_CALIB_BUTTON */

/* ------------------------------------------------------------------------ */
/* Radar message                                                             */
/* ------------------------------------------------------------------------ */

static void build_msg(struct radar_msg *msg, const struct ld2402_report *rep,
		      uint16_t filtered_cm, const struct radar_trend *trend, bool sensor_ok)
{
	memset(msg, 0, sizeof(*msg));
	msg->magic = RADAR_MSG_MAGIC;
	msg->uptime_ms = k_uptime_get_32();

	if (!sensor_ok) {
		return;
	}

	msg->flags |= RADAR_FLAG_SENSOR_OK;
	msg->presence = rep->presence;
	/* the radar keeps the last distance when the target is gone - clear it */
	msg->distance_cm = (rep->presence == LD2402_PRESENCE_NONE) ? 0 : filtered_cm;
	msg->direction = trend->direction;
	msg->speed_cms = trend->speed_cms;

	if (rep->has_energy) {
		msg->flags |= RADAR_FLAG_ENERGY;
		for (int i = 0; i < RADAR_GATE_COUNT; i++) {
			msg->motion_db[i] = ld2402_energy_to_db(rep->motion_energy[i]);
			msg->micro_db[i] = ld2402_energy_to_db(rep->micro_energy[i]);
		}
	}
}

static bool should_send(const struct radar_msg *now, const struct radar_msg *last,
			int64_t elapsed_ms)
{
	if (now->presence != last->presence || now->flags != last->flags ||
	    now->direction != last->direction) {
		return true;
	}
	if (abs((int)now->distance_cm - (int)last->distance_cm) >= DIST_DELTA_CM) {
		return true;
	}
	return elapsed_ms >= PERIODIC_SEND_MS;
}

static void log_status(const struct radar_msg *msg, uint16_t raw_cm)
{
	struct ld2402_stats st;
	int peak_gate = 0;

	for (int i = 1; i < RADAR_GATE_COUNT; i++) {
		if (msg->motion_db[i] > msg->motion_db[peak_gate]) {
			peak_gate = i;
		}
	}

	ld2402_get_stats(&st);
	printk("[SENSOR] %-5s dist=%3ucm (raw %3u) %-8s %+4dcm/s peak=%2udB@g%-2d"
	       " | sent=%u bin=%u txt=%u bad=%u ovf=%u%s\n",
	       presence_name(msg->presence), (unsigned int)msg->distance_cm,
	       (unsigned int)raw_cm, direction_name(msg->direction), (int)msg->speed_cms,
	       (unsigned int)msg->motion_db[peak_gate], peak_gate,
	       tx_seq, st.binary_frames, st.ascii_lines, st.bad_frames, st.rx_overflows,
	       (msg->flags & RADAR_FLAG_SENSOR_OK) ? "" : " [NO SENSOR]");
}

/* ------------------------------------------------------------------------ */
/* Main                                                                      */
/* ------------------------------------------------------------------------ */

int main(void)
{
	struct ld2402_report latest = {0};
	struct radar_msg last_sent = {0};
	struct radar_median median;
	struct radar_trend trend;
	uint16_t filtered_cm = 0;
	int64_t last_report_ms = -SENSOR_TIMEOUT_MS;
	int64_t last_send_ms = 0;
	int64_t last_log_ms = 0;

	printk("\n=== LD2402 AMP: sensor core (%s) ===\n", CONFIG_BOARD_TARGET);

	if (ld2402_init(radar_uart) != 0) {
		printk("[SENSOR] LD2402 UART not ready - check alias ld2402-uart\n");
		return 0;
	}
	if (radar_link_tx_init() != 0) {
		printk("[SENSOR] inter-core link init failed\n");
		return 0;
	}

	start_radar();
	calib_button_init();
	radar_median_reset(&median);
	radar_trend_reset(&trend);

	while (true) {
		struct ld2402_report rep;
		struct radar_msg msg;
		int64_t now;
		bool sensor_ok;

		if (ld2402_read_report(&rep, K_MSEC(REPORT_WAIT_MS)) == 0) {
			bool present = (rep.presence != LD2402_PRESENCE_NONE);

			latest = rep;
			last_report_ms = k_uptime_get();

			if (present) {
				filtered_cm = radar_median_update(&median, rep.distance_cm);
			} else {
				radar_median_reset(&median);
				filtered_cm = 0;
			}
			radar_trend_update(&trend, last_report_ms, filtered_cm, present);
		}

		now = k_uptime_get();

		if (calib_long_press_detected(now)) {
			bool ok = run_auto_threshold();

			show_calib_result(ok);
			radar_median_reset(&median);
			radar_trend_reset(&trend);
			last_report_ms = k_uptime_get(); /* grace period after config mode */
			continue;
		}

		sensor_ok = (now - last_report_ms) < SENSOR_TIMEOUT_MS;
		if (!sensor_ok) {
			radar_median_reset(&median);
			radar_trend_reset(&trend);
		}
		build_msg(&msg, &latest, filtered_cm, &trend, sensor_ok);

		if (should_send(&msg, &last_sent, now - last_send_ms)) {
			if (send_msg(&msg) == 0) {
				last_sent = msg;
				last_send_ms = now;
			}
		}

		if (now - last_log_ms >= LOG_PERIOD_MS) {
			log_status(&msg, latest.distance_cm);
			last_log_ms = now;
		}
	}

	return 0;
}
