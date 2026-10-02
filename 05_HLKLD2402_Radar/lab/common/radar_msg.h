/*
 * radar_msg.h - inter-core message shared by the sensor core and the
 * display core.
 *
 * The same header is used on both platforms:
 *   - ESP32-S3 : PRO_CPU (sensor)  -> APP_CPU (display) over IPM
 *   - SR110    : Cortex-M4 (sensor) -> Cortex-M55 (display) over mbox
 *
 * Layout rules:
 *   - Fixed-width fields only, ordered so that there is no implicit padding.
 *   - Both cores are little-endian, so the struct is copied as raw bytes.
 *   - Keep it <= 64 bytes (smallest IPC payload limit we rely on).
 */
#ifndef RADAR_MSG_H_
#define RADAR_MSG_H_

#include <stdint.h>

#define RADAR_MSG_MAGIC   0x4C443234u /* "LD24" - lets the receiver reject garbage */
#define RADAR_GATE_COUNT  16          /* LD2402 reports 16 gates x 0.7 m */

enum radar_presence {
	RADAR_PRESENCE_NONE   = 0, /* no target */
	RADAR_PRESENCE_MOVING = 1, /* target present (moving) */
	RADAR_PRESENCE_STILL  = 2, /* target present, stationary */
};

/* Radial direction of the target, derived from the distance trend. */
enum radar_direction {
	RADAR_DIR_UNKNOWN  = 0, /* no target or not enough samples yet */
	RADAR_DIR_STEADY   = 1, /* distance not changing significantly */
	RADAR_DIR_APPROACH = 2, /* distance decreasing (coming closer) */
	RADAR_DIR_LEAVE    = 3, /* distance increasing (moving away) */
};

/* Auto-threshold calibration state (sensor core -> display core). */
enum radar_calib {
	RADAR_CALIB_IDLE      = 0, /* normal radar display */
	RADAR_CALIB_COUNTDOWN = 1, /* calib_pct = seconds left, area must be emptied */
	RADAR_CALIB_RUNNING   = 2, /* calib_pct = progress 0..100 % */
	RADAR_CALIB_DONE      = 3, /* thresholds generated and saved to radar flash */
	RADAR_CALIB_FAILED    = 4,
};

/* radar_msg.flags */
#define RADAR_FLAG_SENSOR_OK  (1u << 0) /* sensor core received a radar report recently */
#define RADAR_FLAG_ENERGY     (1u << 1) /* motion_db/micro_db are valid (engineering mode) */

struct radar_msg {
	uint32_t magic;                       /* RADAR_MSG_MAGIC */
	uint32_t seq;                         /* increments on every send */
	uint32_t uptime_ms;                   /* sender uptime when the message was built */
	uint16_t distance_cm;                 /* median-filtered distance, 0 when no target */
	uint8_t  presence;                    /* enum radar_presence */
	uint8_t  flags;                       /* RADAR_FLAG_* */
	uint8_t  direction;                   /* enum radar_direction */
	uint8_t  calib_state;                 /* enum radar_calib */
	int16_t  speed_cms;                   /* radial speed, cm/s (negative = approaching) */
	uint8_t  calib_pct;                   /* COUNTDOWN: seconds left, RUNNING: progress % */
	uint8_t  reserved[3];                 /* explicit padding, keep zero */
	uint8_t  motion_db[RADAR_GATE_COUNT]; /* per-gate motion energy in dB (0..96) */
	uint8_t  micro_db[RADAR_GATE_COUNT];  /* per-gate micro-motion energy in dB (0..96) */
};

_Static_assert(sizeof(struct radar_msg) == 56, "radar_msg layout changed");

#endif /* RADAR_MSG_H_ */
