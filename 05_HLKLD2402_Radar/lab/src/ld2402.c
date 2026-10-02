/*
 * ld2402.c - minimal HLK-LD2402 24 GHz radar driver.
 *
 * Layers (top to bottom):
 *   public API     : ld2402_read_report(), configuration commands
 *                    (version, engineering mode, parameters, auto threshold)
 *   command layer  : ld2402_command() - send a command frame, wait for its ACK
 *   frame parser   : parser_feed()    - byte-wise state machine, all 3 formats
 *   RX byte source : rx_get_byte()    - UART IRQ + ring buffer, or polling
 *
 * Only the thread that calls the public API consumes RX bytes, so the parser
 * needs no locking. The UART ISR only produces bytes into the ring buffer.
 */
#include "ld2402.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/ring_buffer.h>

/* ------------------------------------------------------------------------ */
/* Protocol constants (manual V1.08, chapter 5)                              */
/* ------------------------------------------------------------------------ */

#define DATA_HEADER   0xF4F3F2F1u /* as shifted into a 32-bit window */
#define CMD_HEADER    0xFDFCFBFAu

static const uint8_t DATA_FOOTER[4] = {0xF8, 0xF7, 0xF6, 0xF5};
static const uint8_t CMD_FOOTER[4]  = {0x04, 0x03, 0x02, 0x01};

#define CMD_READ_VERSION   0x0000
#define CMD_WRITE_PARAMS   0x0007
#define CMD_READ_PARAMS    0x0008
#define CMD_AUTO_THRESHOLD 0x0009
#define CMD_AUTO_PROGRESS  0x000A
#define CMD_SET_MODE       0x0012
#define CMD_SAVE_PARAMS    0x00FD
#define CMD_END_CONFIG     0x00FE
#define CMD_ENABLE_CONFIG  0x00FF
#define ACK_BIT            0x0100 /* ACK command word = sent command | 0x0100 */

#define MODE_ENGINEERING   0x00000004u

/* presence(1) + distance(2) + 32 gates x 4 bytes = 131 = 0x83 */
#define ENERGY_FRAME_LEN   (1 + 2 + (2 * LD2402_GATE_COUNT * 4))

#define MAX_PAYLOAD        160
#define MAX_ASCII_LINE     32
#define ACK_TIMEOUT_MS     500
#define SAVE_TIMEOUT_MS    3000  /* flash write takes longer than other commands */

#define PARAM_MAX_DISTANCE     0x0001
#define PARAM_DISAPPEAR_DELAY  0x0004
#define PARAM_POWER_INTERF     0x0005
#define PARAM_MOTION_THR_BASE  0x0010
#define PARAM_MICRO_THR_BASE   0x0030
#define PARAMS_PER_READ        8     /* 8 IDs = 16-byte value, fits the TX frame */

/* ------------------------------------------------------------------------ */
/* Frame parser                                                              */
/* ------------------------------------------------------------------------ */

enum parser_state {
	ST_SYNC,      /* hunting for a binary header or an ASCII line start */
	ST_LEN_LO,
	ST_LEN_HI,
	ST_PAYLOAD,
	ST_FOOTER,
	ST_ASCII,
};

enum frame_kind {
	KIND_DATA,
	KIND_ACK,
};

enum parse_event {
	EVT_NONE,
	EVT_REPORT, /* parser.report holds a new radar report */
	EVT_ACK,    /* parser.ack_* hold a new command ACK */
};

struct parser {
	enum parser_state state;
	enum frame_kind kind;
	uint32_t window;     /* last 4 bytes seen in ST_SYNC */
	uint16_t len;
	uint16_t pos;
	uint8_t footer_pos;
	uint8_t payload[MAX_PAYLOAD];
	char line[MAX_ASCII_LINE];
	uint8_t line_len;

	/* outputs */
	struct ld2402_report report;
	uint16_t ack_word;
	uint16_t ack_status;
	const uint8_t *ack_data; /* points into payload[] */
	uint16_t ack_data_len;
};

static struct parser s_parser;
static struct ld2402_stats s_stats;

static uint16_t get_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void parser_reset(struct parser *p)
{
	p->state = ST_SYNC;
	p->window = 0;
	p->line_len = 0;
}

/* "OFF" or "distance:123" (normal working mode, manual 5.6.1) */
static bool parse_ascii_line(struct parser *p)
{
	static const char prefix[] = "distance:";
	struct ld2402_report *r = &p->report;

	if (strcmp(p->line, "OFF") == 0) {
		memset(r, 0, sizeof(*r));
		r->presence = LD2402_PRESENCE_NONE;
		return true;
	}

	if (strncmp(p->line, prefix, sizeof(prefix) - 1) == 0) {
		memset(r, 0, sizeof(*r));
		r->presence = LD2402_PRESENCE_MOVING;
		r->distance_cm = (uint16_t)strtoul(p->line + sizeof(prefix) - 1, NULL, 10);
		return true;
	}

	return false;
}

/* Engineering frame payload: presence(1) distance(2) [motion x16][micro x16] */
static bool decode_data_frame(struct parser *p)
{
	struct ld2402_report *r = &p->report;

	if (p->len < 3) {
		return false;
	}

	memset(r, 0, sizeof(*r));
	r->presence = p->payload[0];
	r->distance_cm = get_le16(&p->payload[1]);

	if (p->len >= ENERGY_FRAME_LEN) {
		const uint8_t *e = &p->payload[3];

		for (int i = 0; i < LD2402_GATE_COUNT; i++) {
			r->motion_energy[i] = get_le32(e + (i * 4));
			r->micro_energy[i] = get_le32(e + ((LD2402_GATE_COUNT + i) * 4));
		}
		r->has_energy = true;
	}

	return true;
}

/* ACK payload: ack_word(2) status(2) data(N) */
static bool decode_ack_frame(struct parser *p)
{
	if (p->len < 4) {
		return false;
	}

	p->ack_word = get_le16(&p->payload[0]);
	p->ack_status = get_le16(&p->payload[2]);
	p->ack_data = &p->payload[4];
	p->ack_data_len = p->len - 4;
	return true;
}

static enum parse_event parser_feed(struct parser *p, uint8_t b)
{
	switch (p->state) {
	case ST_SYNC:
		p->window = (p->window << 8) | b;

		if (p->window == DATA_HEADER || p->window == CMD_HEADER) {
			p->kind = (p->window == DATA_HEADER) ? KIND_DATA : KIND_ACK;
			p->window = 0;
			p->state = ST_LEN_LO;
		} else if (b == 'O' || b == 'd') {
			/* start of "OFF" or "distance:" */
			p->line[0] = (char)b;
			p->line_len = 1;
			p->state = ST_ASCII;
		}
		break;

	case ST_ASCII:
		if (b == '\r' || b == '\n') {
			p->line[p->line_len] = '\0';
			parser_reset(p);
			if (parse_ascii_line(p)) {
				s_stats.ascii_lines++;
				return EVT_REPORT;
			}
		} else if (b >= 0x20 && b < 0x7F && p->line_len < (MAX_ASCII_LINE - 1)) {
			p->line[p->line_len++] = (char)b;
		} else {
			/* not a text line after all - keep the byte for header hunting */
			parser_reset(p);
			p->window = b;
		}
		break;

	case ST_LEN_LO:
		p->len = b;
		p->state = ST_LEN_HI;
		break;

	case ST_LEN_HI:
		p->len |= (uint16_t)(b << 8);
		if (p->len == 0 || p->len > MAX_PAYLOAD) {
			s_stats.bad_frames++;
			parser_reset(p);
		} else {
			p->pos = 0;
			p->state = ST_PAYLOAD;
		}
		break;

	case ST_PAYLOAD:
		p->payload[p->pos++] = b;
		if (p->pos == p->len) {
			p->footer_pos = 0;
			p->state = ST_FOOTER;
		}
		break;

	case ST_FOOTER: {
		const uint8_t *footer = (p->kind == KIND_DATA) ? DATA_FOOTER : CMD_FOOTER;

		if (b != footer[p->footer_pos]) {
			s_stats.bad_frames++;
			parser_reset(p);
			p->window = b;
			break;
		}

		if (++p->footer_pos < sizeof(DATA_FOOTER)) {
			break;
		}

		parser_reset(p);
		if (p->kind == KIND_DATA) {
			if (decode_data_frame(p)) {
				s_stats.binary_frames++;
				return EVT_REPORT;
			}
		} else {
			if (decode_ack_frame(p)) {
				return EVT_ACK;
			}
		}
		s_stats.bad_frames++;
		break;
	}
	}

	return EVT_NONE;
}

/* ------------------------------------------------------------------------ */
/* RX byte source                                                            */
/* ------------------------------------------------------------------------ */

static const struct device *s_uart;

#ifndef CONFIG_LD2402_RX_POLLING

#define RX_RING_SIZE 512

RING_BUF_DECLARE(s_rx_ring, RX_RING_SIZE);
K_SEM_DEFINE(s_rx_sem, 0, 1);

static void uart_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	/*
	 * Return type differs across Zephyr versions (int in older releases,
	 * void in newer ones), so call it as a statement and never test it.
	 */
	uart_irq_update(dev);

	while (uart_irq_rx_ready(dev)) {
		uint8_t chunk[32];
		int n = uart_fifo_read(dev, chunk, sizeof(chunk));

		if (n <= 0) {
			break;
		}
		if (ring_buf_put(&s_rx_ring, chunk, n) < (uint32_t)n) {
			s_stats.rx_overflows++;
		}
		k_sem_give(&s_rx_sem);
	}
}

static int rx_start(void)
{
	int ret = uart_irq_callback_user_data_set(s_uart, uart_isr, NULL);

	if (ret != 0) {
		return ret;
	}
	uart_irq_rx_enable(s_uart);
	return 0;
}

static int rx_get_byte(uint8_t *b, k_timepoint_t end)
{
	while (ring_buf_get(&s_rx_ring, b, 1) == 0) {
		if (k_sem_take(&s_rx_sem, sys_timepoint_timeout(end)) != 0) {
			return -EAGAIN;
		}
	}
	return 0;
}

static void rx_drain(void)
{
	uint8_t junk[32];

	while (ring_buf_get(&s_rx_ring, junk, sizeof(junk)) > 0) {
	}
}

#else /* CONFIG_LD2402_RX_POLLING */

static int rx_start(void)
{
	return 0;
}

static int rx_get_byte(uint8_t *b, k_timepoint_t end)
{
	while (uart_poll_in(s_uart, b) != 0) {
		if (sys_timepoint_expired(end)) {
			return -EAGAIN;
		}
		/* 1 ms at 115200 bps = ~11.5 bytes, below the 16-byte HW FIFO */
		k_sleep(K_MSEC(1));
	}
	return 0;
}

static void rx_drain(void)
{
	uint8_t junk;

	while (uart_poll_in(s_uart, &junk) == 0) {
	}
}

#endif /* CONFIG_LD2402_RX_POLLING */

/* ------------------------------------------------------------------------ */
/* Command layer                                                             */
/* ------------------------------------------------------------------------ */

/* FD FC FB FA | len(2) | cmd(2) value(N) | 04 03 02 01 */
static size_t build_cmd_frame(uint8_t *buf, uint16_t cmd,
			      const uint8_t *value, uint16_t value_len)
{
	uint16_t inner_len = 2 + value_len;
	size_t n = 0;

	buf[n++] = 0xFD;
	buf[n++] = 0xFC;
	buf[n++] = 0xFB;
	buf[n++] = 0xFA;
	buf[n++] = (uint8_t)(inner_len & 0xFF);
	buf[n++] = (uint8_t)(inner_len >> 8);
	buf[n++] = (uint8_t)(cmd & 0xFF);
	buf[n++] = (uint8_t)(cmd >> 8);
	if (value_len > 0) {
		memcpy(&buf[n], value, value_len);
		n += value_len;
	}
	memcpy(&buf[n], CMD_FOOTER, sizeof(CMD_FOOTER));
	n += sizeof(CMD_FOOTER);

	return n;
}

/*
 * Send one command and wait for the matching ACK. Radar data frames that
 * arrive in the meantime are parsed and silently dropped.
 * On success, s_parser.ack_data/ack_data_len point at the ACK return data.
 */
static int ld2402_command(uint16_t cmd, const uint8_t *value, uint16_t value_len,
			  uint32_t timeout_ms)
{
	uint8_t frame[32];
	size_t frame_len;
	k_timepoint_t end;

	if (value_len > sizeof(frame) - 12) {
		return -EINVAL;
	}

	frame_len = build_cmd_frame(frame, cmd, value, value_len);

	rx_drain();
	parser_reset(&s_parser);

	for (size_t i = 0; i < frame_len; i++) {
		uart_poll_out(s_uart, frame[i]);
	}

	end = sys_timepoint_calc(K_MSEC(timeout_ms));

	while (true) {
		uint8_t b;

		if (rx_get_byte(&b, end) != 0) {
			return -ETIMEDOUT;
		}
		if (parser_feed(&s_parser, b) != EVT_ACK) {
			continue;
		}
		if (s_parser.ack_word != (cmd | ACK_BIT)) {
			continue; /* stale ACK of a previous command */
		}
		return (s_parser.ack_status == 0) ? 0 : -EIO;
	}
}

static void put_le16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v & 0xFF);
	p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v & 0xFF);
	p[1] = (uint8_t)((v >> 8) & 0xFF);
	p[2] = (uint8_t)((v >> 16) & 0xFF);
	p[3] = (uint8_t)((v >> 24) & 0xFF);
}

/* 0x0008: value = ID(2) x n, ACK data = value(4) x n (same order) */
static int read_param_batch(uint16_t first_id, int count, uint32_t *out)
{
	uint8_t value[PARAMS_PER_READ * 2];
	int ret;

	if (count > PARAMS_PER_READ) {
		return -EINVAL;
	}
	for (int i = 0; i < count; i++) {
		put_le16(&value[i * 2], (uint16_t)(first_id + i));
	}

	ret = ld2402_command(CMD_READ_PARAMS, value, (uint16_t)(count * 2), ACK_TIMEOUT_MS);
	if (ret != 0) {
		return ret;
	}
	if (s_parser.ack_data_len < count * 4) {
		return -EBADMSG;
	}
	for (int i = 0; i < count; i++) {
		out[i] = get_le32(s_parser.ack_data + (i * 4));
	}
	return 0;
}

/* 0x0007: value = (ID(2) + value(4)) x n - this lab writes one at a time */
static int write_param(uint16_t id, uint32_t param_value)
{
	uint8_t value[6];

	put_le16(&value[0], id);
	put_le32(&value[2], param_value);
	return ld2402_command(CMD_WRITE_PARAMS, value, sizeof(value), ACK_TIMEOUT_MS);
}

/* ------------------------------------------------------------------------ */
/* Configuration commands (public, call between config_begin/config_end)     */
/* ------------------------------------------------------------------------ */

int ld2402_config_begin(void)
{
	static const uint8_t value[] = {0x01, 0x00};

	return ld2402_command(CMD_ENABLE_CONFIG, value, sizeof(value), ACK_TIMEOUT_MS);
}

int ld2402_config_end(void)
{
	return ld2402_command(CMD_END_CONFIG, NULL, 0, ACK_TIMEOUT_MS);
}

/* ACK data: version_len(2) + version string (e.g. "v3.5.5") */
int ld2402_read_version(char *out, size_t out_len)
{
	int ret = ld2402_command(CMD_READ_VERSION, NULL, 0, ACK_TIMEOUT_MS);
	uint16_t ver_len;

	if (ret != 0) {
		return ret;
	}
	if (s_parser.ack_data_len < 2 || out_len < 2) {
		return -EBADMSG;
	}

	ver_len = get_le16(s_parser.ack_data);
	ver_len = MIN(ver_len, (uint16_t)(s_parser.ack_data_len - 2));
	ver_len = MIN(ver_len, (uint16_t)(out_len - 1));

	memcpy(out, s_parser.ack_data + 2, ver_len);
	out[ver_len] = '\0';
	return 0;
}

/* 0x0012: value = 0x0000 (2 bytes) + mode (4 bytes) */
int ld2402_set_engineering_mode(void)
{
	uint8_t value[6];

	put_le16(&value[0], 0x0000);
	put_le32(&value[2], MODE_ENGINEERING);
	return ld2402_command(CMD_SET_MODE, value, sizeof(value), ACK_TIMEOUT_MS);
}

int ld2402_read_params(struct ld2402_params *out)
{
	uint32_t basic[1];
	int ret;

	memset(out, 0, sizeof(*out));

	/* IDs 0x0001 / 0x0004 / 0x0005 are not contiguous - read one by one */
	ret = read_param_batch(PARAM_MAX_DISTANCE, 1, basic);
	if (ret != 0) {
		return ret;
	}
	out->max_distance = basic[0];

	ret = read_param_batch(PARAM_DISAPPEAR_DELAY, 1, basic);
	if (ret != 0) {
		return ret;
	}
	out->disappear_delay_s = basic[0];

	/* read-only status; older firmware may not know it - not fatal */
	if (read_param_batch(PARAM_POWER_INTERF, 1, basic) == 0) {
		out->power_interference = basic[0];
	}

	for (int i = 0; i < LD2402_GATE_COUNT; i += PARAMS_PER_READ) {
		ret = read_param_batch((uint16_t)(PARAM_MOTION_THR_BASE + i), PARAMS_PER_READ,
				       &out->motion_threshold[i]);
		if (ret != 0) {
			return ret;
		}
		ret = read_param_batch((uint16_t)(PARAM_MICRO_THR_BASE + i), PARAMS_PER_READ,
				       &out->micro_threshold[i]);
		if (ret != 0) {
			return ret;
		}
	}
	return 0;
}

/* 0x0009: value = trigger(2) hold(2) micro(2), each coefficient x10 */
int ld2402_auto_threshold_start(uint16_t trigger_x10, uint16_t hold_x10, uint16_t micro_x10)
{
	uint8_t value[6];

	put_le16(&value[0], trigger_x10);
	put_le16(&value[2], hold_x10);
	put_le16(&value[4], micro_x10);
	return ld2402_command(CMD_AUTO_THRESHOLD, value, sizeof(value), ACK_TIMEOUT_MS);
}

/* 0x000A: ACK data = progress(2), 0..100 */
int ld2402_auto_threshold_progress(uint8_t *percent)
{
	int ret = ld2402_command(CMD_AUTO_PROGRESS, NULL, 0, ACK_TIMEOUT_MS);
	uint16_t progress;

	if (ret != 0) {
		return ret;
	}
	if (s_parser.ack_data_len < 2) {
		return -EBADMSG;
	}
	progress = get_le16(s_parser.ack_data);
	*percent = (uint8_t)MIN(progress, 100);
	return 0;
}

/*
 * 0x00FD exists from FW 3.3.2. Older firmware saves to flash when parameter
 * 0x003F is written (manual 5.2.7), so the fallback rewrites its own value.
 */
int ld2402_save_params(void)
{
	uint32_t micro_last;
	int ret;

	ret = ld2402_command(CMD_SAVE_PARAMS, NULL, 0, SAVE_TIMEOUT_MS);
	if (ret == 0) {
		return 0;
	}

	ret = read_param_batch(PARAM_MICRO_THR_BASE + LD2402_GATE_COUNT - 1, 1, &micro_last);
	if (ret != 0) {
		return ret;
	}
	return write_param(PARAM_MICRO_THR_BASE + LD2402_GATE_COUNT - 1, micro_last);
}

/* ------------------------------------------------------------------------ */
/* Public API                                                                */
/* ------------------------------------------------------------------------ */

int ld2402_init(const struct device *uart)
{
	if (!device_is_ready(uart)) {
		return -ENODEV;
	}

	s_uart = uart;
	parser_reset(&s_parser);
	memset(&s_stats, 0, sizeof(s_stats));

	return rx_start();
}

int ld2402_read_report(struct ld2402_report *out, k_timeout_t timeout)
{
	k_timepoint_t end = sys_timepoint_calc(timeout);

	while (true) {
		uint8_t b;
		int ret = rx_get_byte(&b, end);

		if (ret != 0) {
			return ret;
		}
		if (parser_feed(&s_parser, b) == EVT_REPORT) {
			*out = s_parser.report;
			return 0;
		}
	}
}

uint8_t ld2402_energy_to_db(uint32_t raw)
{
	if (raw == 0) {
		return 0;
	}
	/* max: 10 * log10(2^32 - 1) = 96.3 dB -> fits in uint8_t */
	return (uint8_t)lroundf(10.0f * log10f((float)raw));
}

void ld2402_get_stats(struct ld2402_stats *out)
{
	*out = s_stats;
}
