/*
 * radar_link_ipm.c - radar_link over the ESP32-S3 soft IPM (ipm0).
 *
 * Driver facts (drivers/ipm/ipm_esp32.c):
 *   - Each direction has its own shared-memory half (0x400 / 2 = 512 bytes).
 *   - ipm_send() copies the payload into the other core's half and raises a
 *     FROM_CPU interrupt there. The receive callback runs in ISR context and
 *     gets a pointer into that shared memory, NOT the payload size.
 *   - The next ipm_send() overwrites the same buffer, so the receiver must
 *     copy the data inside the callback. Our sender transmits at most one
 *     message per radar frame (~165 ms), so there is no overwrite race.
 */
#include "radar_link.h"

#include <errno.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/ipm.h>
#include <zephyr/sys/printk.h>

#define RADAR_IPM_ID  0x0042 /* arbitrary 16-bit message id */

static const struct device *const ipm = DEVICE_DT_GET(DT_NODELABEL(ipm0));
static struct k_msgq *rx_queue;

int radar_link_tx_init(void)
{
	if (!device_is_ready(ipm)) {
		printk("[LINK] ipm0 not ready\n");
		return -ENODEV;
	}
	if (ipm_max_data_size_get(ipm) < (int)sizeof(struct radar_msg)) {
		printk("[LINK] IPM payload limit too small\n");
		return -EMSGSIZE;
	}
	return 0;
}

int radar_link_send(const struct radar_msg *msg)
{
	/* wait=1: spin until the shared-memory lock is free */
	return ipm_send(ipm, 1, RADAR_IPM_ID, msg, sizeof(*msg));
}

static void ipm_rx_callback(const struct device *dev, void *user_data,
			    uint32_t id, volatile void *data)
{
	struct radar_msg msg;

	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	if (id != RADAR_IPM_ID) {
		return;
	}

	/* ISR context: copy out of shared memory, validate, hand off. */
	memcpy(&msg, (const void *)data, sizeof(msg));
	if (msg.magic != RADAR_MSG_MAGIC) {
		return;
	}
	(void)k_msgq_put(rx_queue, &msg, K_NO_WAIT);
}

int radar_link_rx_init(struct k_msgq *queue)
{
	if (!device_is_ready(ipm)) {
		return -ENODEV;
	}

	rx_queue = queue;
	ipm_register_callback(ipm, ipm_rx_callback, NULL);
	return 0;
}
