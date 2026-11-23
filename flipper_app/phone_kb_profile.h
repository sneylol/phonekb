#pragma once

#include <furi_ble/profile_interface.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * BLE profile for Phone Keyboard.
 *
 * No Bluetooth pairing: the phone never sees a PIN or "Pair" dialog. Instead
 * the web page keeps a random 16-byte secret and writes it to the AUTH
 * characteristic right after connecting. The app decides whether to accept it
 * (the first secret ever received is remembered, others are rejected). Until a
 * connection is accepted, every byte written to RX is dropped.
 *
 * Service: 8fe5b3d5-2e7f-4a98-2a48-7acc60fe0000 (same UUID as Flipper serial)
 *   RX     19ed82ae-ed21-4c9d-4145-228e62fe0000  write          command bytes
 *   AUTH   19ed82ae-ed21-4c9d-4145-228e65fe0000  write          16-byte secret
 *   STATUS 19ed82ae-ed21-4c9d-4145-228e66fe0000  read           0 waiting, 1 ok, 2 rejected
 *   CONFIG 19ed82ae-ed21-4c9d-4145-228e67fe0000  write          [0] = ms to wait after each typed letter
 */
extern const FuriHalBleProfileTemplate* const ble_profile_phone_kb;

#define PHONE_KB_TOKEN_LEN 16

typedef enum {
    PhoneKbAuthWaiting = 0,
    PhoneKbAuthOk = 1,
    PhoneKbAuthRejected = 2,
} PhoneKbAuthStatus;

/** Called in the BLE event thread (tiny stack!) with bytes from an accepted phone. Must not block. */
typedef void (*PhoneKbRxCallback)(const uint8_t* data, uint16_t size, void* context);

/** Called in the BLE event thread (tiny stack!) with the phone's secret. Return true to accept. Must not block. */
typedef bool (*PhoneKbAuthCallback)(const uint8_t token[PHONE_KB_TOKEN_LEN], void* context);

void ble_profile_phone_kb_set_callbacks(
    FuriHalBleProfileBase* profile,
    PhoneKbRxCallback rx_callback,
    PhoneKbAuthCallback auth_callback,
    void* context);

/**
 * Copy the current auth state into the STATUS characteristic the phone reads.
 * Call often from the app thread (BLE commands are not safe in the BLE event
 * thread). Returns the current state.
 */
PhoneKbAuthStatus ble_profile_phone_kb_sync_status(FuriHalBleProfileBase* profile);

PhoneKbAuthStatus ble_profile_phone_kb_get_auth(FuriHalBleProfileBase* profile);

#define PHONE_KB_DEFAULT_LETTER_DELAY_MS 2

/**
 * Whether a phone is connected, tracked from the BLE events directly.
 * (Momentum mntm-012's Bt service never reports "connected" to apps.)
 */
bool ble_profile_phone_kb_is_connected(FuriHalBleProfileBase* profile);

/** Pause after each letter of typed text, as last set by the phone (CONFIG). */
uint8_t ble_profile_phone_kb_get_letter_delay(FuriHalBleProfileBase* profile);

#ifdef __cplusplus
}
#endif
