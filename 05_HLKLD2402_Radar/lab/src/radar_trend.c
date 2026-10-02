/*
 * radar_trend.c - approach / leave detection (see radar_trend.h).
 */
#include "radar_trend.h"

#include <stdlib.h>
#include <string.h>

#include "radar_msg.h"

void radar_trend_reset(struct radar_trend *tr)
{
	memset(tr, 0, sizeof(*tr));
	tr->direction = RADAR_DIR_UNKNOWN;
}

static uint8_t newest_index(const struct radar_trend *tr)
{
	return (uint8_t)((tr->head + RADAR_TREND_WINDOW - 1) % RADAR_TREND_WINDOW);
}

/* Least-squares slope of distance over time, in cm/s. */
static int32_t estimate_speed_cms(const struct radar_trend *tr)
{
	uint8_t oldest = (uint8_t)((tr->head + RADAR_TREND_WINDOW - tr->count) % RADAR_TREND_WINDOW);
	int64_t t0 = tr->t_ms[oldest];
	int64_t n = tr->count;
	int64_t sum_x = 0, sum_y = 0, sum_xy = 0, sum_xx = 0;
	int64_t denom;

	for (uint8_t i = 0; i < tr->count; i++) {
		uint8_t idx = (uint8_t)((oldest + i) % RADAR_TREND_WINDOW);
		int64_t x = tr->t_ms[idx] - t0; /* ms */
		int64_t y = tr->dist_cm[idx];   /* cm */

		sum_x += x;
		sum_y += y;
		sum_xy += x * y;
		sum_xx += x * x;
	}

	denom = (n * sum_xx) - (sum_x * sum_x);
	if (denom == 0) {
		return 0;
	}

	/* slope [cm/ms] * 1000 = [cm/s] */
	return (int32_t)((((n * sum_xy) - (sum_x * sum_y)) * 1000) / denom);
}

static uint8_t classify(uint8_t current, int32_t speed)
{
	switch (current) {
	case RADAR_DIR_APPROACH:
		if (speed < -RADAR_TREND_EXIT_CMS) {
			return RADAR_DIR_APPROACH;
		}
		break;
	case RADAR_DIR_LEAVE:
		if (speed > RADAR_TREND_EXIT_CMS) {
			return RADAR_DIR_LEAVE;
		}
		break;
	default:
		break;
	}

	if (speed <= -RADAR_TREND_ENTER_CMS) {
		return RADAR_DIR_APPROACH;
	}
	if (speed >= RADAR_TREND_ENTER_CMS) {
		return RADAR_DIR_LEAVE;
	}
	return RADAR_DIR_STEADY;
}

/* Debounce: switch only after the same candidate was seen CONFIRM times. */
static void confirm_direction(struct radar_trend *tr, uint8_t candidate)
{
	if (candidate == tr->direction) {
		tr->pending_count = 0;
		return;
	}
	if (tr->direction == RADAR_DIR_UNKNOWN) {
		tr->direction = candidate; /* first decision after (re)start */
		tr->pending_count = 0;
		return;
	}
	if (candidate != tr->pending) {
		tr->pending = candidate;
		tr->pending_count = 0;
	}
	if (++tr->pending_count >= RADAR_TREND_CONFIRM) {
		tr->direction = candidate;
		tr->pending_count = 0;
	}
}

void radar_trend_update(struct radar_trend *tr, int64_t now_ms,
			uint16_t distance_cm, bool present)
{
	int32_t speed;

	if (!present) {
		radar_trend_reset(tr);
		return;
	}

	if (tr->count > 0) {
		uint8_t last = newest_index(tr);
		bool gap = (now_ms - tr->t_ms[last]) > RADAR_TREND_MAX_GAP_MS;
		bool jump = abs((int)distance_cm - (int)tr->dist_cm[last]) > RADAR_TREND_JUMP_CM;

		if (gap || jump) {
			radar_trend_reset(tr);
		}
	}

	tr->t_ms[tr->head] = now_ms;
	tr->dist_cm[tr->head] = distance_cm;
	tr->head = (uint8_t)((tr->head + 1) % RADAR_TREND_WINDOW);
	if (tr->count < RADAR_TREND_WINDOW) {
		tr->count++;
	}

	if (tr->count < RADAR_TREND_MIN_SAMPLES) {
		tr->direction = RADAR_DIR_UNKNOWN;
		tr->pending_count = 0;
		tr->speed_cms = 0;
		return;
	}

	speed = estimate_speed_cms(tr);
	tr->speed_cms = (int16_t)speed;
	confirm_direction(tr, classify(tr->direction, speed));
}
