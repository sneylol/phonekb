#pragma once

#include <furi.h>
#include <furi_hal_usb.h>
#include <furi_hal_usb_hid.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Phone Keyboard's own USB HID interface (keyboard + mouse + media keys).
 * Same device as the firmware's usb_hid, plus it receives the PC's keyboard
 * lights. Use with furi_hal_usb_set_config(&pkb_usb_hid, NULL).
 */
extern FuriHalUsbInterface pkb_usb_hid;

bool pkb_hid_is_connected(void);

/** Keyboard lights from the PC: HID_KB_LED_NUM / HID_KB_LED_CAPS / HID_KB_LED_SCROLL */
uint8_t pkb_hid_get_led_state(void);

/** Number of LED updates received from the PC (for diagnostics). */
uint32_t pkb_hid_get_led_reports(void);

void pkb_hid_set_state_callback(HidStateCallback cb, void* ctx);

bool pkb_hid_kb_press(uint16_t button);
bool pkb_hid_kb_release(uint16_t button);
bool pkb_hid_kb_release_all(void);

bool pkb_hid_mouse_move(int8_t dx, int8_t dy);
bool pkb_hid_mouse_press(uint8_t button);
bool pkb_hid_mouse_release(uint8_t button);
bool pkb_hid_mouse_scroll(int8_t delta);

bool pkb_hid_consumer_key_press(uint16_t button);
bool pkb_hid_consumer_key_release(uint16_t button);
bool pkb_hid_consumer_key_release_all(void);

#ifdef __cplusplus
}
#endif
