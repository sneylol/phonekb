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
 */
extern const FuriHalBleProfileTemplate* const ble_profile_phone_kb;

#define PHONE_KB_TOKEN_LEN 16

typedef enum {
    PhoneKbAuthWaiting = 0,
    PhoneKbAuthOk = 1,
    PhoneKbAuthRejected = 2,
} PhoneKbAuthStatus;

/** Called in the BLE thread with command bytes from an accepted phone. */
typedef void (*PhoneKbRxCallback)(const uint8_t* data, uint16_t size, void* context);

/** Called in the BLE thread with the secret the phone sent. Return true to accept it. */
typedef bool (*PhoneKbAuthCallback)(const uint8_t token[PHONE_KB_TOKEN_LEN], void* context);

void ble_profile_phone_kb_set_callbacks(
    FuriHalBleProfileBase* profile,
    PhoneKbRxCallback rx_callback,
    PhoneKbAuthCallback auth_callback,
    void* context);

/** Forget whether the current link is accepted. Call on every connect and disconnect. */
void ble_profile_phone_kb_reset_auth(FuriHalBleProfileBase* profile);

PhoneKbAuthStatus ble_profile_phone_kb_get_auth(FuriHalBleProfileBase* profile);

#ifdef __cplusplus
}
#endif
