/*
 * radar_trend.h - approach / leave detection from the LD2402 distance stream.
 *
 * The LD2402 is a 1T1R radar: it reports radial distance only (no angle, no
 * speed). The radial speed is estimated here as the least-squares slope of
 * the last few distance samples, then classified with hysteresis:
 *
 *   speed <= -ENTER  -> APPROACH      (stays until speed > -EXIT)
 *   speed >= +ENTER  -> LEAVE         (stays until speed < +EXIT)
 *   otherwise        -> STEADY
 *
 * A new direction must be seen on RADAR_TREND_CONFIRM consecutive updates
 * before it is reported (debounce against +-15 cm distance noise).
 *
 * The window is reset when the target disappears, when frames stop for a
 * while, or when the distance jumps (most likely a different target).
 */
#ifndef RADAR_TREND_H_
#define RADAR_TREND_H_

#include <stdbool.h>
#include <stdint.h>

#define RADAR_TREND_WINDOW       8    /* samples, ~1.3 s at 165 ms/frame */
#define RADAR_TREND_MIN_SAMPLES  5    /* need ~0.8 s of data before deciding */
#define RADAR_TREND_MAX_GAP_MS   600  /* frame gap that invalidates the window */
#define RADAR_TREND_JUMP_CM      100  /* single-step jump treated as target switch */
#define RADAR_TREND_ENTER_CMS    20   /* |speed| to enter APPROACH/LEAVE */
#define RADAR_TREND_EXIT_CMS     10   /* |speed| below which we fall back to STEADY */
#define RADAR_TREND_CONFIRM      3    /* consecutive updates needed to change direction */

struct radar_trend {
	int64_t  t_ms[RADAR_TREND_WINDOW];
	uint16_t dist_cm[RADAR_TREND_WINDOW];
	uint8_t  head;      /* next write index */
	uint8_t  count;     /* valid samples in the window */
	uint8_t  direction; /* enum radar_direction (reported, debounced) */
	uint8_t  pending;   /* candidate direction waiting for confirmation */
	uint8_t  pending_count;
	int16_t  speed_cms; /* last estimated radial speed */
};

void radar_trend_reset(struct radar_trend *tr);

/* Feed one radar report. 'present' = target detected in this report. */
void radar_trend_update(struct radar_trend *tr, int64_t now_ms,
			uint16_t distance_cm, bool present);

#endif /* RADAR_TREND_H_ */
