/*
 * radar_link.h - one-way inter-core link: sensor core -> display core.
 *
 * Same API on both platforms, different transport underneath:
 *   radar_link_ipm.c  : ESP32-S3 PRO_CPU <-> APP_CPU, Zephyr IPM (soft IPM)
 *   radar_link_mbox.c : SR110 M4 <-> M55, Zephyr mbox
 *
 * Because of this, sensor_main.c and display_main.c are byte-for-byte
 * identical between the ESP32-S3 and SR110 projects.
 */
#ifndef RADAR_LINK_H_
#define RADAR_LINK_H_

#include <zephyr/kernel.h>
#include "radar_msg.h"

/* Sensor core side */
int radar_link_tx_init(void);
int radar_link_send(const struct radar_msg *msg);

/*
 * Display core side. Every valid message received (magic checked) is put
 * into 'queue' from ISR context with K_NO_WAIT. The queue item size must be
 * sizeof(struct radar_msg).
 */
int radar_link_rx_init(struct k_msgq *queue);

#endif /* RADAR_LINK_H_ */
