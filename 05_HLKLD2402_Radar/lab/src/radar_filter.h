/*
 * radar_filter.h - sliding median filter for the LD2402 distance.
 *
 * A median (not a mean) rejects single-frame spikes - e.g. one frame that
 * locks onto a wall reflection - without smearing them into the result.
 * Window 5 = ~0.8 s of history at 165 ms/frame, adds ~2 frames of lag.
 */
#ifndef RADAR_FILTER_H_
#define RADAR_FILTER_H_

#include <stdint.h>

#define RADAR_MEDIAN_WINDOW  5

struct radar_median {
	uint16_t samples[RADAR_MEDIAN_WINDOW];
	uint8_t  head;
	uint8_t  count;
};

void radar_median_reset(struct radar_median *f);

/* Add one sample and return the median of the samples in the window. */
uint16_t radar_median_update(struct radar_median *f, uint16_t sample);

#endif /* RADAR_FILTER_H_ */
