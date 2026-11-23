#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * German (DE/AT, QWERTZ) keyboard layout.
 *
 * Maps a Unicode code point to the HID usage (low byte) plus modifier bits
 * (high byte, same values as furi_hal_usb_hid.h: Shift = 1 << 9,
 * AltGr = Right Alt = 1 << 14) that produce it on a PC set to German.
 *
 * *dead is true for dead keys (^ ´ `): press the key, then Space, to get the
 * character itself.
 *
 * Returns false if the character can't be typed on this layout.
 */
bool layout_de_lookup(uint32_t codepoint, uint16_t* key, bool* dead);

#define LAYOUT_DE_SHIFT (1 << 9)
#define LAYOUT_DE_ALTGR (1 << 14)

#ifdef __cplusplus
}
#endif
