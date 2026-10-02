/*
 * ld2402.h - minimal HLK-LD2402 24 GHz radar driver (UART, 115200 8N1).
 *
 * Protocol reference: Hi-Link "HLK-LD2402 User Manual" V1.08, chapter 5.
 *
 * Supported input formats (auto-detected, can be mixed on the wire):
 *   1. Normal mode   : ASCII lines "OFF\r\n" / "distance:123\r\n"
 *   2. Engineering   : binary frame
 *        F4 F3 F2 F1 | len(2) | presence(1) distance(2) energy(32 x 4) | F8 F7 F6 F5
 *   3. Command ACK   : binary frame
 *        FD FC FB FA | len(2) | cmd|0x0100 (2) status(2) data(N)    | 04 03 02 01
 *
 * Single-instance driver on purpose (one radar per board in this lab).
 */
#ifndef LD2402_H_
#define LD2402_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>

#define LD2402_GATE_COUNT    16
#define LD2402_GATE_SIZE_CM  70

enum ld2402_presence {
	LD2402_PRESENCE_NONE   = 0,
	LD2402_PRESENCE_MOVING = 1, /* normal (ASCII) mode always reports this when present */
	LD2402_PRESENCE_STILL  = 2, /* only distinguishable in engineering mode */
};

struct ld2402_report {
	uint8_t  presence;     /* enum ld2402_presence */
	uint16_t distance_cm;
	bool     has_energy;   /* true only for engineering-mode frames */
	uint32_t motion_energy[LD2402_GATE_COUNT]; /* raw linear energy */
	uint32_t micro_energy[LD2402_GATE_COUNT];  /* raw linear energy */
};

struct ld2402_stats {
	uint32_t binary_frames;  /* engineering frames decoded */
	uint32_t ascii_lines;    /* normal-mode lines decoded */
	uint32_t bad_frames;     /* length/footer errors */
	uint32_t rx_overflows;   /* ring buffer full (IRQ mode only) */
};

struct ld2402_params {
	uint32_t max_distance;        /* 0x0001, 0.1 m units (7..100) */
	uint32_t disappear_delay_s;   /* 0x0004, seconds */
	uint32_t power_interference;  /* 0x0005, 0 = not checked, 1 = none, 2 = detected */
	uint32_t motion_threshold[LD2402_GATE_COUNT]; /* 0x0010..0x001F, linear */
	uint32_t micro_threshold[LD2402_GATE_COUNT];  /* 0x0030..0x003F, linear */
};

/* Bind the driver to an already-configured UART (115200 8N1) and start RX. */
int ld2402_init(const struct device *uart);

/*
 * Configuration session. Every command below must be issued between
 * ld2402_config_begin() and ld2402_config_end(); the radar stops reporting
 * while it is in configuration mode.
 */
int ld2402_config_begin(void);
int ld2402_config_end(void);

int ld2402_read_version(char *out, size_t out_len);
int ld2402_set_engineering_mode(void);
int ld2402_read_params(struct ld2402_params *out);

/*
 * Automatic threshold generation (manual 5.2.9 / 5.2.10). The detection area
 * must be empty while it runs. Coefficients are x10 (30 = 3.0, range 10..200).
 */
int ld2402_auto_threshold_start(uint16_t trigger_x10, uint16_t hold_x10, uint16_t micro_x10);
int ld2402_auto_threshold_progress(uint8_t *percent);

/* Persist parameters to the radar's flash (0x00FD, fallback: rewrite 0x003F). */
int ld2402_save_params(void);

/* Block until the next radar report arrives or the timeout expires (-EAGAIN). */
int ld2402_read_report(struct ld2402_report *out, k_timeout_t timeout);

/* Convert a raw energy value to dB as defined by the manual: 10 * log10(raw). */
uint8_t ld2402_energy_to_db(uint32_t raw);

void ld2402_get_stats(struct ld2402_stats *out);

#endif /* LD2402_H_ */
