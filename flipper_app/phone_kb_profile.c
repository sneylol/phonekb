#include "phone_kb_profile.h"

#include <furi.h>
#include <furi_hal_version.h>
#include <gap.h>
#include <ble/core/ble_defs.h>
#include <ble/core/ble_std.h>
#include <ble/core/auto/ble_types.h>
#include <furi_ble/event_dispatcher.h>
#include <furi_ble/gatt.h>
#include <services/dev_info_service.h>
#include <services/battery_service.h>

// The SDK doesn't ship the HCI transport headers, so mirror the few
// definitions the GATT event handler needs (same layout as ST's hci_tl.h).
typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t data[1];
} PhoneKbHciUartPckt;

typedef struct __attribute__((packed)) {
    uint8_t evt;
    uint8_t plen;
    uint8_t data[1];
} PhoneKbHciEventPckt;

typedef struct __attribute__((packed)) {
    uint16_t ecode;
    uint8_t data[1];
} PhoneKbEvtBlecoreAci;

#define PHONE_KB_ACI_GATT_ATTRIBUTE_MODIFIED_VSEVT_CODE (0x0C01U)

#define RX_DATA_LEN_MAX (486)

// 128-bit UUIDs, little-endian byte order
static const Service_UUID_t phone_kb_service_uuid = {
    .Service_UUID_128 =
        {0x00, 0x00, 0xfe, 0x60, 0xcc, 0x7a, 0x48, 0x2a, 0x98, 0x4a, 0x7f, 0x2e, 0xd5, 0xb3, 0xe5, 0x8f}};
#define PHONE_KB_CHAR_UUID(n) \
    {0x00, 0x00, 0xfe, (n), 0x8e, 0x22, 0x45, 0x41, 0x9d, 0x4c, 0x21, 0xed, 0xae, 0x82, 0xed, 0x19}

typedef enum {
    PhoneKbCharRx = 0,
    PhoneKbCharAuth,
    PhoneKbCharStatus,
    PhoneKbCharConfig,
    PhoneKbCharProgress,
    PhoneKbCharInfo,
    PhoneKbCharCount,
} PhoneKbCharId;

// No security permissions: the phone never has to pair. Access control is
// done by the AUTH secret instead (see phone_kb_profile.h).
static const BleGattCharacteristicParams phone_kb_chars[PhoneKbCharCount] = {
    [PhoneKbCharRx] =
        {.name = "RX",
         .data_prop_type = FlipperGattCharacteristicDataFixed,
         .data.fixed.length = RX_DATA_LEN_MAX,
         .uuid.Char_UUID_128 = PHONE_KB_CHAR_UUID(0x62),
         .uuid_type = UUID_TYPE_128,
         .char_properties = CHAR_PROP_WRITE_WITHOUT_RESP | CHAR_PROP_WRITE,
         .security_permissions = ATTR_PERMISSION_NONE,
         .gatt_evt_mask = GATT_NOTIFY_ATTRIBUTE_WRITE,
         .is_variable = CHAR_VALUE_LEN_VARIABLE},
    [PhoneKbCharAuth] =
        {.name = "Auth",
         .data_prop_type = FlipperGattCharacteristicDataFixed,
         .data.fixed.length = PHONE_KB_TOKEN_LEN + 1,
         .uuid.Char_UUID_128 = PHONE_KB_CHAR_UUID(0x65),
         .uuid_type = UUID_TYPE_128,
         .char_properties = CHAR_PROP_WRITE,
         .security_permissions = ATTR_PERMISSION_NONE,
         .gatt_evt_mask = GATT_NOTIFY_ATTRIBUTE_WRITE,
         .is_variable = CHAR_VALUE_LEN_VARIABLE},
    [PhoneKbCharStatus] =
        {.name = "Status",
         .data_prop_type = FlipperGattCharacteristicDataFixed,
         .data.fixed.length = 1,
         .uuid.Char_UUID_128 = PHONE_KB_CHAR_UUID(0x66),
         .uuid_type = UUID_TYPE_128,
         .char_properties = CHAR_PROP_READ,
         .security_permissions = ATTR_PERMISSION_NONE,
         .gatt_evt_mask = GATT_DONT_NOTIFY_EVENTS,
         .is_variable = CHAR_VALUE_LEN_CONSTANT},
    [PhoneKbCharConfig] =
        {.name = "Config",
         .data_prop_type = FlipperGattCharacteristicDataFixed,
         .data.fixed.length = 8,
         .uuid.Char_UUID_128 = PHONE_KB_CHAR_UUID(0x67),
         .uuid_type = UUID_TYPE_128,
         .char_properties = CHAR_PROP_WRITE,
         .security_permissions = ATTR_PERMISSION_NONE,
         .gatt_evt_mask = GATT_NOTIFY_ATTRIBUTE_WRITE,
         .is_variable = CHAR_VALUE_LEN_VARIABLE},
    [PhoneKbCharProgress] =
        {.name = "Progress",
         .data_prop_type = FlipperGattCharacteristicDataFixed,
         .data.fixed.length = 4,
         .uuid.Char_UUID_128 = PHONE_KB_CHAR_UUID(0x68),
         .uuid_type = UUID_TYPE_128,
         .char_properties = CHAR_PROP_READ | CHAR_PROP_NOTIFY,
         .security_permissions = ATTR_PERMISSION_NONE,
         .gatt_evt_mask = GATT_DONT_NOTIFY_EVENTS,
         .is_variable = CHAR_VALUE_LEN_CONSTANT},
    [PhoneKbCharInfo] =
        {.name = "Info",
         .data_prop_type = FlipperGattCharacteristicDataFixed,
         .data.fixed.length = 1,
         .uuid.Char_UUID_128 = PHONE_KB_CHAR_UUID(0x69),
         .uuid_type = UUID_TYPE_128,
         .char_properties = CHAR_PROP_READ,
         .security_permissions = ATTR_PERMISSION_NONE,
         .gatt_evt_mask = GATT_DONT_NOTIFY_EVENTS,
         .is_variable = CHAR_VALUE_LEN_CONSTANT},
};

typedef struct {
    FuriHalBleProfileBase base;
    BleServiceDevInfo* dev_info_svc;
    BleServiceBattery* battery_svc;

    uint16_t svc_handle;
    BleGattCharacteristicInstance chars[PhoneKbCharCount];
    GapSvcEventHandler* event_handler;

    volatile PhoneKbAuthStatus auth; // set in the BLE thread
    PhoneKbAuthStatus published; // value last written to the STATUS characteristic
    volatile uint16_t letter_delay_ms;
    volatile uint32_t stop_count;
    volatile uint32_t release_count;
    volatile bool paused;
    volatile uint8_t layout;
    volatile uint8_t wpm;
    volatile bool typos;
    volatile uint8_t think;
    volatile bool link_up;
    PhoneKbRxCallback rx_callback;
    PhoneKbAuthCallback auth_callback;
    void* context;
} BleProfilePhoneKb;
_Static_assert(offsetof(BleProfilePhoneKb, base) == 0, "Wrong layout");

static void phone_kb_publish_auth(BleProfilePhoneKb* p, PhoneKbAuthStatus status) {
    p->published = status;
    uint8_t value = (uint8_t)status;
    ble_gatt_characteristic_update(p->svc_handle, &p->chars[PhoneKbCharStatus], &value);
}

// Runs in the "BleEventWorker" thread, which has a tiny stack (~1.3 KB):
// no logging, no BLE commands, nothing that blocks. Only flip flags and copy bytes.
static BleEventAckStatus phone_kb_event_handler(void* event, void* context) {
    BleProfilePhoneKb* p = context;
    PhoneKbHciEventPckt* event_pckt = (PhoneKbHciEventPckt*)(((PhoneKbHciUartPckt*)event)->data);

    // A new or dropped link must present the secret again. Done here, in the
    // same thread that handles AUTH writes, so it can never land after them.
    // Not acked: GAP still needs to see these events.
    if(event_pckt->evt == HCI_DISCONNECTION_COMPLETE_EVT_CODE) {
        p->auth = PhoneKbAuthWaiting;
        p->link_up = false;
        p->paused = false;
        return BleEventNotAck;
    }
    if(event_pckt->evt == HCI_LE_META_EVT_CODE &&
       (event_pckt->data[0] == HCI_LE_CONNECTION_COMPLETE_SUBEVT_CODE ||
        event_pckt->data[0] == HCI_LE_ENHANCED_CONNECTION_COMPLETE_SUBEVT_CODE)) {
        p->auth = PhoneKbAuthWaiting;
        // data[1] is the HCI status: 0 = the connection really came up
        p->link_up = (event_pckt->data[1] == 0);
        return BleEventNotAck;
    }

    if(event_pckt->evt != HCI_VENDOR_SPECIFIC_DEBUG_EVT_CODE) return BleEventNotAck;
    PhoneKbEvtBlecoreAci* blecore_evt = (PhoneKbEvtBlecoreAci*)event_pckt->data;
    if(blecore_evt->ecode != PHONE_KB_ACI_GATT_ATTRIBUTE_MODIFIED_VSEVT_CODE) return BleEventNotAck;

    aci_gatt_attribute_modified_event_rp0* mod =
        (aci_gatt_attribute_modified_event_rp0*)blecore_evt->data;

    if(mod->Attr_Handle == p->chars[PhoneKbCharRx].handle + 1) {
        // Commands are only accepted after this link presented the right secret
        if(p->auth == PhoneKbAuthOk && p->rx_callback) {
            p->rx_callback(mod->Attr_Data, mod->Attr_Data_Length, p->context);
        }
        return BleEventAckFlowEnable;
    }

    if(mod->Attr_Handle == p->chars[PhoneKbCharAuth].handle + 1) {
        bool ok = false;
        uint16_t len = mod->Attr_Data_Length;
        if((len == PHONE_KB_TOKEN_LEN || len == PHONE_KB_TOKEN_LEN + 1) && p->auth_callback) {
            // Older pages send only the secret: treat that as a user-initiated connect
            bool user_initiated = (len == PHONE_KB_TOKEN_LEN) || (mod->Attr_Data[PHONE_KB_TOKEN_LEN] & 1);
            ok = p->auth_callback(mod->Attr_Data, user_initiated, p->context);
        }
        p->auth = ok ? PhoneKbAuthOk : PhoneKbAuthRejected;
        return BleEventAckFlowEnable;
    }

    if(mod->Attr_Handle == p->chars[PhoneKbCharConfig].handle + 1) {
        if(p->auth == PhoneKbAuthOk && mod->Attr_Data_Length >= 1) {
            uint16_t ms = mod->Attr_Data[0];
            if(mod->Attr_Data_Length >= 2) ms |= (uint16_t)mod->Attr_Data[1] << 8;
            p->letter_delay_ms = ms;
            if(mod->Attr_Data_Length >= 3) {
                if(mod->Attr_Data[2] & 1) p->stop_count++;
                if(mod->Attr_Data[2] & 4) p->release_count++;
                p->paused = (mod->Attr_Data[2] & 2) != 0;
            }
            if(mod->Attr_Data_Length >= 4) p->layout = mod->Attr_Data[3];
            if(mod->Attr_Data_Length >= 6) {
                p->typos = (mod->Attr_Data[4] & 2) != 0;
                p->think = (mod->Attr_Data[4] >> 2) & 3;
                if(mod->Attr_Data[5]) p->wpm = mod->Attr_Data[5];
            }
        }
        return BleEventAckFlowEnable;
    }

    return BleEventNotAck;
}

static FuriHalBleProfileBase* ble_profile_phone_kb_start(FuriHalBleProfileParams params) {
    UNUSED(params);
    BleProfilePhoneKb* p = malloc(sizeof(BleProfilePhoneKb));
    p->base.config = ble_profile_phone_kb;
    p->dev_info_svc = ble_svc_dev_info_start();
    p->battery_svc = ble_svc_battery_start(true);

    p->event_handler = ble_event_dispatcher_register_svc_handler(phone_kb_event_handler, p);
    furi_check(ble_gatt_service_add(
        UUID_TYPE_128, &phone_kb_service_uuid, PRIMARY_SERVICE, 18, &p->svc_handle));
    for(uint8_t i = 0; i < PhoneKbCharCount; i++) {
        ble_gatt_characteristic_init(p->svc_handle, &phone_kb_chars[i], &p->chars[i]);
    }
    p->auth = PhoneKbAuthWaiting;
    p->letter_delay_ms = PHONE_KB_DEFAULT_LETTER_DELAY_MS;
    phone_kb_publish_auth(p, PhoneKbAuthWaiting);
    uint32_t zero = 0;
    ble_gatt_characteristic_update(p->svc_handle, &p->chars[PhoneKbCharProgress], &zero);
    uint8_t version = PHONE_KB_PROTOCOL_VERSION;
    ble_gatt_characteristic_update(p->svc_handle, &p->chars[PhoneKbCharInfo], &version);
    p->wpm = 55;
    return &p->base;
}

static void ble_profile_phone_kb_stop(FuriHalBleProfileBase* profile) {
    furi_check(profile);
    furi_check(profile->config == ble_profile_phone_kb);
    BleProfilePhoneKb* p = (BleProfilePhoneKb*)profile;
    ble_svc_battery_stop(p->battery_svc);
    ble_svc_dev_info_stop(p->dev_info_svc);

    ble_event_dispatcher_unregister_svc_handler(p->event_handler);
    for(uint8_t i = 0; i < PhoneKbCharCount; i++) {
        ble_gatt_characteristic_delete(p->svc_handle, &p->chars[i]);
    }
    ble_gatt_service_delete(p->svc_handle);
}

// 7.5 ms .. 45 ms connection interval: low latency for mouse movement
#define CONNECTION_INTERVAL_MIN (0x06)
#define CONNECTION_INTERVAL_MAX (0x24)

static const GapConfig phone_kb_template_config = {
#ifdef PHONE_KB_LEGACY_GAP
    // Momentum mntm-007 and older: build with cdefines=["PHONE_KB_LEGACY_GAP"]
    .adv_service_uuid = 0xFE60,
#else
    .adv_service =
        {
            .UUID_Type = UUID_TYPE_16,
            // Not the Flipper (0x308x) UUID, so the official Flipper app
            // won't try to grab this connection.
            .Service_UUID_16 = 0xFE60,
        },
#endif
    .appearance_char = 0x03C1, // Keyboard
    // No Bluetooth pairing at all, so the phone never asks for a PIN.
    // Only phones that know the secret can send keystrokes (see AUTH).
    .bonding_mode = false,
    .pairing_method = GapPairingNone,
    .conn_param =
        {
            .conn_int_min = CONNECTION_INTERVAL_MIN,
            .conn_int_max = CONNECTION_INTERVAL_MAX,
            .slave_latency = 0,
            .supervisor_timeout = 0,
        },
};

static void ble_profile_phone_kb_get_config(GapConfig* config, FuriHalBleProfileParams params) {
    UNUSED(params);
    furi_check(config);
    memcpy(config, &phone_kb_template_config, sizeof(GapConfig));

    // Own MAC, distinct from the system profile (+0), the HID remote (+1) and
    // the older PIN-paired version of this app (^0x5B), so phones that bonded
    // with the old version don't try to reuse that stale bond.
    memcpy(config->mac_address, furi_hal_version_get_ble_mac(), sizeof(config->mac_address));
    config->mac_address[2] += 2;
    config->mac_address[0] ^= 0x5C;

    // First char of the local name is the AD type byte; keep it.
    snprintf(
        config->adv_name,
        sizeof(config->adv_name),
        "%cPhoneKB %s",
        furi_hal_version_get_ble_local_device_name_ptr()[0],
        furi_hal_version_get_name_ptr());
}

static const FuriHalBleProfileTemplate profile_callbacks = {
    .start = ble_profile_phone_kb_start,
    .stop = ble_profile_phone_kb_stop,
    .get_gap_config = ble_profile_phone_kb_get_config,
};

const FuriHalBleProfileTemplate* const ble_profile_phone_kb = &profile_callbacks;

void ble_profile_phone_kb_set_callbacks(
    FuriHalBleProfileBase* profile,
    PhoneKbRxCallback rx_callback,
    PhoneKbAuthCallback auth_callback,
    void* context) {
    furi_check(profile && (profile->config == ble_profile_phone_kb));
    BleProfilePhoneKb* p = (BleProfilePhoneKb*)profile;
    p->context = context;
    p->rx_callback = rx_callback;
    p->auth_callback = auth_callback;
}

PhoneKbAuthStatus ble_profile_phone_kb_sync_status(FuriHalBleProfileBase* profile) {
    furi_check(profile && (profile->config == ble_profile_phone_kb));
    BleProfilePhoneKb* p = (BleProfilePhoneKb*)profile;
    PhoneKbAuthStatus now = p->auth;
    if(now != p->published) phone_kb_publish_auth(p, now);
    return now;
}

PhoneKbAuthStatus ble_profile_phone_kb_get_auth(FuriHalBleProfileBase* profile) {
    furi_check(profile && (profile->config == ble_profile_phone_kb));
    return ((BleProfilePhoneKb*)profile)->auth;
}

uint16_t ble_profile_phone_kb_get_letter_delay(FuriHalBleProfileBase* profile) {
    furi_check(profile && (profile->config == ble_profile_phone_kb));
    return ((BleProfilePhoneKb*)profile)->letter_delay_ms;
}

uint32_t ble_profile_phone_kb_get_stop_count(FuriHalBleProfileBase* profile) {
    furi_check(profile && (profile->config == ble_profile_phone_kb));
    return ((BleProfilePhoneKb*)profile)->stop_count;
}

bool ble_profile_phone_kb_is_connected(FuriHalBleProfileBase* profile) {
    furi_check(profile && (profile->config == ble_profile_phone_kb));
    return ((BleProfilePhoneKb*)profile)->link_up;
}

bool ble_profile_phone_kb_is_paused(FuriHalBleProfileBase* profile) {
    furi_check(profile && (profile->config == ble_profile_phone_kb));
    return ((BleProfilePhoneKb*)profile)->paused;
}

PhoneKbLayout ble_profile_phone_kb_get_layout(FuriHalBleProfileBase* profile) {
    furi_check(profile && (profile->config == ble_profile_phone_kb));
    return (PhoneKbLayout)((BleProfilePhoneKb*)profile)->layout;
}

uint32_t ble_profile_phone_kb_get_release_count(FuriHalBleProfileBase* profile) {
    furi_check(profile && (profile->config == ble_profile_phone_kb));
    return ((BleProfilePhoneKb*)profile)->release_count;
}

void ble_profile_phone_kb_publish_progress(FuriHalBleProfileBase* profile, uint32_t bytes_done) {
    furi_check(profile && (profile->config == ble_profile_phone_kb));
    BleProfilePhoneKb* p = (BleProfilePhoneKb*)profile;
    uint8_t le[4] = {
        bytes_done & 0xFF, (bytes_done >> 8) & 0xFF, (bytes_done >> 16) & 0xFF, bytes_done >> 24};
    ble_gatt_characteristic_update(p->svc_handle, &p->chars[PhoneKbCharProgress], le);
}

uint8_t ble_profile_phone_kb_get_wpm(FuriHalBleProfileBase* profile) {
    furi_check(profile && (profile->config == ble_profile_phone_kb));
    return ((BleProfilePhoneKb*)profile)->wpm;
}

bool ble_profile_phone_kb_get_typos(FuriHalBleProfileBase* profile) {
    furi_check(profile && (profile->config == ble_profile_phone_kb));
    return ((BleProfilePhoneKb*)profile)->typos;
}

uint8_t ble_profile_phone_kb_get_think(FuriHalBleProfileBase* profile) {
    furi_check(profile && (profile->config == ble_profile_phone_kb));
    return ((BleProfilePhoneKb*)profile)->think;
}
