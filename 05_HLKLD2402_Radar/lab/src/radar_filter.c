/*
 * radar_filter.c - sliding median filter (see radar_filter.h).
 */
#include "radar_filter.h"

#include <string.h>

void radar_median_reset(struct radar_median *f)
{
	memset(f, 0, sizeof(*f));
}

uint16_t radar_median_update(struct radar_median *f, uint16_t sample)
{
	uint16_t sorted[RADAR_MEDIAN_WINDOW];

	f->samples[f->head] = sample;
	f->head = (uint8_t)((f->head + 1) % RADAR_MEDIAN_WINDOW);
	if (f->count < RADAR_MEDIAN_WINDOW) {
		f->count++;
	}

	/* insertion sort of at most 5 values - order of the ring does not matter */
	memcpy(sorted, f->samples, f->count * sizeof(sorted[0]));
	for (int i = 1; i < f->count; i++) {
		uint16_t key = sorted[i];
		int j = i - 1;

		while (j >= 0 && sorted[j] > key) {
			sorted[j + 1] = sorted[j];
			j--;
		}
		sorted[j + 1] = key;
	}

	return sorted[f->count / 2];
}
