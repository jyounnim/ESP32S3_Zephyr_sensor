/*
 * font5x7.h
 *
 * Minimal original 5x7 dot-matrix font for the SSD1306 text
 * renderer used across this multi-sensor lab series. Only the
 * characters this series actually prints (labels, digits,
 * units, punctuation) are defined; anything else falls back
 * to a blank glyph in font5x7_lookup().
 *
 * Layout: 5 columns per glyph, column-major, bit0 = top row,
 * 7 rows tall (fits an 8px-tall character cell with 1px gap).
 */

#ifndef FONT5X7_H_
#define FONT5X7_H_

#include <stdint.h>
#include <stddef.h>
#include <zephyr/sys/util.h> /* ARRAY_SIZE */

#define FONT5X7_WIDTH  5
#define FONT5X7_HEIGHT 7

static const uint8_t FONT_SPACE[FONT5X7_WIDTH] = {0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t FONT_BANG[FONT5X7_WIDTH] = {0x00, 0x5F, 0x5F, 0x00, 0x00};
static const uint8_t FONT_PERCENT[FONT5X7_WIDTH] = {0x21, 0x10, 0x0C, 0x02, 0x21};
static const uint8_t FONT_DASH[FONT5X7_WIDTH] = {0x08, 0x08, 0x08, 0x08, 0x08};
static const uint8_t FONT_DOT[FONT5X7_WIDTH] = {0x00, 0x60, 0x60, 0x00, 0x00};
static const uint8_t FONT_0[FONT5X7_WIDTH] = {0x3E, 0x51, 0x49, 0x45, 0x3E};
static const uint8_t FONT_1[FONT5X7_WIDTH] = {0x00, 0x42, 0x7F, 0x40, 0x00};
static const uint8_t FONT_2[FONT5X7_WIDTH] = {0x42, 0x61, 0x51, 0x49, 0x46};
static const uint8_t FONT_3[FONT5X7_WIDTH] = {0x22, 0x41, 0x49, 0x49, 0x36};
static const uint8_t FONT_4[FONT5X7_WIDTH] = {0x18, 0x14, 0x12, 0x7F, 0x10};
static const uint8_t FONT_5[FONT5X7_WIDTH] = {0x27, 0x45, 0x45, 0x45, 0x39};
static const uint8_t FONT_6[FONT5X7_WIDTH] = {0x3C, 0x4A, 0x49, 0x49, 0x30};
static const uint8_t FONT_7[FONT5X7_WIDTH] = {0x01, 0x71, 0x09, 0x05, 0x03};
static const uint8_t FONT_8[FONT5X7_WIDTH] = {0x36, 0x49, 0x49, 0x49, 0x36};
static const uint8_t FONT_9[FONT5X7_WIDTH] = {0x06, 0x49, 0x49, 0x29, 0x1E};
static const uint8_t FONT_COLON[FONT5X7_WIDTH] = {0x00, 0x36, 0x36, 0x00, 0x00};
static const uint8_t FONT_A[FONT5X7_WIDTH] = {0x7C, 0x12, 0x11, 0x12, 0x7C};
static const uint8_t FONT_B[FONT5X7_WIDTH] = {0x7F, 0x49, 0x49, 0x49, 0x36};
static const uint8_t FONT_C[FONT5X7_WIDTH] = {0x3E, 0x41, 0x41, 0x41, 0x41};
static const uint8_t FONT_D[FONT5X7_WIDTH] = {0x7F, 0x41, 0x41, 0x41, 0x3E};
static const uint8_t FONT_E[FONT5X7_WIDTH] = {0x7F, 0x49, 0x49, 0x49, 0x41};
static const uint8_t FONT_F[FONT5X7_WIDTH] = {0x7F, 0x09, 0x09, 0x09, 0x01};
static const uint8_t FONT_G[FONT5X7_WIDTH] = {0x3E, 0x41, 0x49, 0x49, 0x79};
static const uint8_t FONT_H[FONT5X7_WIDTH] = {0x7F, 0x08, 0x08, 0x08, 0x7F};
static const uint8_t FONT_I[FONT5X7_WIDTH] = {0x41, 0x41, 0x7F, 0x41, 0x41};
static const uint8_t FONT_L[FONT5X7_WIDTH] = {0x7F, 0x40, 0x40, 0x40, 0x40};
static const uint8_t FONT_M[FONT5X7_WIDTH] = {0x7F, 0x02, 0x04, 0x02, 0x7F};
static const uint8_t FONT_N[FONT5X7_WIDTH] = {0x7F, 0x02, 0x04, 0x08, 0x7F};
static const uint8_t FONT_O[FONT5X7_WIDTH] = {0x3E, 0x41, 0x41, 0x41, 0x3E};
static const uint8_t FONT_P[FONT5X7_WIDTH] = {0x7F, 0x09, 0x09, 0x09, 0x06};
static const uint8_t FONT_R[FONT5X7_WIDTH] = {0x7F, 0x09, 0x19, 0x29, 0x46};
static const uint8_t FONT_S[FONT5X7_WIDTH] = {0x46, 0x49, 0x49, 0x49, 0x31};
static const uint8_t FONT_T[FONT5X7_WIDTH] = {0x01, 0x01, 0x7F, 0x01, 0x01};
static const uint8_t FONT_U[FONT5X7_WIDTH] = {0x3F, 0x40, 0x40, 0x40, 0x3F};
static const uint8_t FONT_V[FONT5X7_WIDTH] = {0x1F, 0x20, 0x40, 0x20, 0x1F};
static const uint8_t FONT_W[FONT5X7_WIDTH] = {0x7F, 0x20, 0x18, 0x20, 0x7F};
static const uint8_t FONT_Y[FONT5X7_WIDTH] = {0x03, 0x04, 0x78, 0x04, 0x03};
static const uint8_t FONT_K[FONT5X7_WIDTH] = {0x7F, 0x08, 0x14, 0x22, 0x41};
static const uint8_t FONT_h[FONT5X7_WIDTH] = {0x7F, 0x08, 0x04, 0x04, 0x78};
static const uint8_t FONT_a[FONT5X7_WIDTH] = {0x20, 0x54, 0x54, 0x54, 0x78};

struct font5x7_entry {
	char ch;
	const uint8_t *cols;
};

/* Added for the LD2402 lab: '+', '/', 'c', 'm', 's' */
static const uint8_t FONT_PLUS[FONT5X7_WIDTH] = {0x08, 0x08, 0x3E, 0x08, 0x08};
static const uint8_t FONT_SLASH[FONT5X7_WIDTH] = {0x20, 0x10, 0x08, 0x04, 0x02};
static const uint8_t FONT_c[FONT5X7_WIDTH] = {0x38, 0x44, 0x44, 0x44, 0x20};
static const uint8_t FONT_m[FONT5X7_WIDTH] = {0x7C, 0x04, 0x18, 0x04, 0x78};
static const uint8_t FONT_s[FONT5X7_WIDTH] = {0x48, 0x54, 0x54, 0x54, 0x20};

static const struct font5x7_entry font5x7_table[] = {
	{ ' ', FONT_SPACE },
	{ '!', FONT_BANG },
	{ '%', FONT_PERCENT },
	{ '-', FONT_DASH },
	{ '.', FONT_DOT },
	{ '0', FONT_0 },
	{ '1', FONT_1 },
	{ '2', FONT_2 },
	{ '3', FONT_3 },
	{ '4', FONT_4 },
	{ '5', FONT_5 },
	{ '6', FONT_6 },
	{ '7', FONT_7 },
	{ '8', FONT_8 },
	{ '9', FONT_9 },
	{ ':', FONT_COLON },
	{ 'A', FONT_A },
	{ 'B', FONT_B },
	{ 'C', FONT_C },
	{ 'D', FONT_D },
	{ 'E', FONT_E },
	{ 'F', FONT_F },
	{ 'G', FONT_G },
	{ 'H', FONT_H },
	{ 'I', FONT_I },
	{ 'L', FONT_L },
	{ 'M', FONT_M },
	{ 'N', FONT_N },
	{ 'O', FONT_O },
	{ 'P', FONT_P },
	{ 'R', FONT_R },
	{ 'S', FONT_S },
	{ 'T', FONT_T },
	{ 'U', FONT_U },
	{ 'V', FONT_V },
	{ 'W', FONT_W },
	{ 'Y', FONT_Y },
	{ 'K', FONT_K },
	{ 'h', FONT_h },
	{ 'a', FONT_a },
	{ '+', FONT_PLUS },
	{ '/', FONT_SLASH },
	{ 'c', FONT_c },
	{ 'm', FONT_m },
	{ 's', FONT_s },
};

/* Returns the 5-byte column array for `ch`, or NULL if the
 * character isn't in the table (caller should render a blank).
 */
static inline const uint8_t *font5x7_lookup(char ch)
{
	for (size_t i = 0; i < ARRAY_SIZE(font5x7_table); i++) {
		if (font5x7_table[i].ch == ch) {
			return font5x7_table[i].cols;
		}
	}
	return NULL;
}

#endif /* FONT5X7_H_ */
