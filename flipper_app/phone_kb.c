/**
 * Phone Keyboard for Flipper Zero (Momentum firmware)
 *
 * Phone  --BLE-->  Flipper  --USB HID-->  PC
 *
 * No Bluetooth pairing / PIN. After connecting, the phone writes a 16-byte
 * secret to the AUTH characteristic. The first secret the Flipper ever sees is
 * saved to the SD card; after that only a phone with the same secret is
 * accepted. Hold Left to forget it. See phone_kb_profile.h.
 *
 * The phone then writes small binary packets to the RX characteristic.
 * Several packets may be concatenated in one BLE write.
 *
 *   0x01 mods key        tap a key (mods = standard HID modifier bitmask)
 *   0x02 dx dy           mouse move (int8 each)
 *   0x03 btn             mouse button down (1=left 2=right 4=middle)
 *   0x04 btn             mouse button up
 *   0x05 delta           vertical scroll (int8)
 *   0x06                 release everything
 *   0x07 lo hi           consumer key tap (volume, play/pause, ...)
 *   0x08 len bytes...    type ASCII text (US layout, len <= 255)
 *   0x09 mods key        key down (hold)
 *   0x0A mods key        key up
 */

#include <stdarg.h>
#include <furi.h>
#include <furi_hal_bt.h>
#include <furi_hal_usb.h>
#include <furi_hal_usb_hid.h>
#include <gui/gui.h>
#include <input/input.h>
#include <notification/notification_messages.h>
#include <bt/bt_service/bt.h>
#include <storage/storage.h>

#include "phone_kb_profile.h"

#define TAG              "PhoneKB"
#define KEYS_PATH        APP_DATA_PATH(".phone_kb.keys")
#define TOKEN_PATH       APP_DATA_PATH(".phone_kb.token")
#define EVENT_LOG_PATH   APP_DATA_PATH("events.log")
#define EVENT_LOG_MAX    (16 * 1024)
#define RX_BUF_SIZE      2048
#define CHUNK_SIZE       128

typedef enum {
    OpTap = 0x01,
    OpMouseMove = 0x02,
    OpMouseDown = 0x03,
    OpMouseUp = 0x04,
    OpScroll = 0x05,
    OpReleaseAll = 0x06,
    OpConsumer = 0x07,
    OpText = 0x08,
    OpKeyDown = 0x09,
    OpKeyUp = 0x0A,
} Op;

typedef struct {
    Gui* gui;
    ViewPort* view_port;
    FuriMessageQueue* input_queue;
    NotificationApp* notifications;
    Bt* bt;
    FuriHalBleProfileBase* profile;
    FuriStreamBuffer* rx;
    FuriHalUsbInterface* usb_prev;

    volatile bool ble_connected;

    // Secret of the phone this Flipper is linked to
    uint8_t token[PHONE_KB_TOKEN_LEN];
    volatile bool has_token;
    volatile bool token_save_pending;
    volatile bool rejected; // last phone sent the wrong secret
    volatile uint32_t rx_dropped; // bytes lost because the buffer was full

    // Typing can take minutes with a long letter delay, so it must be stoppable
    uint32_t stop_seen; // last stop request handled (see ble_profile_phone_kb_get_stop_count)
    bool typing_aborted; // drop the rest of what's queued
    bool exit_requested; // Back pressed while typing
    bool forget_requested; // Hold Left pressed while typing
    uint32_t keys_sent;
    char last_text[22];

    // parser state
    uint8_t pkt[260];
    uint16_t pkt_len;
} PhoneKb;

// ---------------------------------------------------------------- BLE side

// Small text log on the SD card (apps_data/<app>/events.log), written only from
// the app thread. While the app runs, USB is a keyboard, so there's no console:
// this is how a freeze or a refused phone can be diagnosed afterwards.
static void event_log(const char* fmt, ...) {
    char line[96];
    int n = snprintf(line, sizeof(line), "%lu ", (unsigned long)furi_get_tick());
    va_list args;
    va_start(args, fmt);
    n += vsnprintf(line + n, sizeof(line) - n - 1, fmt, args);
    va_end(args);
    if(n > (int)sizeof(line) - 2) n = sizeof(line) - 2;
    line[n++] = '\n';

    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    if(storage_file_open(file, EVENT_LOG_PATH, FSAM_WRITE, FSOM_OPEN_APPEND)) {
        storage_file_write(file, line, n);
    }
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

static void event_log_start(void) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FileInfo info;
    if(storage_common_stat(storage, EVENT_LOG_PATH, &info) == FSE_OK && info.size > EVENT_LOG_MAX) {
        storage_simply_remove(storage, EVENT_LOG_PATH);
    }
    furi_record_close(RECORD_STORAGE);
}

// Runs in the BLE event thread (tiny stack): only copy bytes, no logging, never block.
static void phone_kb_rx_cb(const uint8_t* data, uint16_t size, void* context) {
    PhoneKb* app = context;
    size_t sent = furi_stream_buffer_send(app->rx, data, size, 0);
    if(sent != size) app->rx_dropped += size - sent;
}

// Runs in the BLE event thread (tiny stack). First secret ever seen is adopted
// (saved later by the main loop); after that it must match.
static bool phone_kb_auth_cb(const uint8_t token[PHONE_KB_TOKEN_LEN], void* context) {
    PhoneKb* app = context;
    bool ok;
    if(!app->has_token) {
        memcpy(app->token, token, PHONE_KB_TOKEN_LEN);
        app->has_token = true;
        app->token_save_pending = true;
        ok = true;
    } else {
        uint8_t diff = 0;
        for(size_t i = 0; i < PHONE_KB_TOKEN_LEN; i++) diff |= app->token[i] ^ token[i];
        ok = (diff == 0);
    }
    app->rejected = !ok;
    return ok;
}

static void token_load(PhoneKb* app) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    app->has_token = storage_file_open(file, TOKEN_PATH, FSAM_READ, FSOM_OPEN_EXISTING) &&
                     storage_file_read(file, app->token, PHONE_KB_TOKEN_LEN) == PHONE_KB_TOKEN_LEN;
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

static void token_save(PhoneKb* app) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    if(!storage_file_open(file, TOKEN_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS) ||
       storage_file_write(file, app->token, PHONE_KB_TOKEN_LEN) != PHONE_KB_TOKEN_LEN) {
        FURI_LOG_E(TAG, "Failed to save phone secret");
    }
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

static void token_forget(PhoneKb* app) {
    app->has_token = false;
    app->rejected = false;
    memset(app->token, 0, sizeof(app->token));
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_remove(storage, TOKEN_PATH);
    furi_record_close(RECORD_STORAGE);
}

// Called from the app thread when the phone link comes up or goes down.
static void phone_kb_link_changed(PhoneKb* app, bool connected) {
    app->ble_connected = connected;
    if(connected) {
        app->rejected = false;
        notification_internal_message(app->notifications, &sequence_set_blue_255);
    } else {
        notification_internal_message(app->notifications, &sequence_reset_blue);
        // Don't leave keys or buttons stuck down on the PC
        furi_hal_hid_kb_release_all();
        furi_hal_hid_mouse_release(HID_MOUSE_BTN_LEFT);
        furi_hal_hid_mouse_release(HID_MOUSE_BTN_RIGHT);
        furi_hal_hid_mouse_release(HID_MOUSE_BTN_WHEEL);
    }
    view_port_update(app->view_port);
}

// ---------------------------------------------------------------- HID side

// Checked between letters (and every 10 ms of a letter pause): stop typing when
// the phone taps Stop, the phone disconnects, or Back is pressed on the Flipper.
static bool typing_should_stop(PhoneKb* app) {
    uint32_t stops = ble_profile_phone_kb_get_stop_count(app->profile);
    if(stops != app->stop_seen) {
        app->stop_seen = stops;
        return true;
    }
    if(!ble_profile_phone_kb_is_connected(app->profile)) return true;
    InputEvent ev;
    while(furi_message_queue_get(app->input_queue, &ev, 0) == FuriStatusOk) {
        if(ev.key == InputKeyBack && ev.type == InputTypeShort) {
            app->exit_requested = true;
            return true;
        } else if(ev.key == InputKeyLeft && ev.type == InputTypeLong) {
            app->forget_requested = true;
            return true;
        }
    }
    return false;
}

// Pause after a letter in 10 ms steps. Returns false if typing should stop.
static bool letter_pause(PhoneKb* app) {
    uint32_t left = ble_profile_phone_kb_get_letter_delay(app->profile);
    while(left > 0) {
        uint32_t step = left < 10 ? left : 10;
        furi_delay_ms(step);
        left -= step;
        if(typing_should_stop(app)) return false;
    }
    return true;
}

// Hold here while the phone has paused typing. Returns false if typing should stop.
static bool wait_while_paused(PhoneKb* app) {
    while(ble_profile_phone_kb_is_paused(app->profile)) {
        furi_delay_ms(10);
        if(typing_should_stop(app)) return false;
    }
    return true;
}

// Returns false if typing should stop.
static bool type_char(PhoneKb* app, uint8_t c) {
    if(typing_should_stop(app)) return false;
    if(!wait_while_paused(app)) return false;
    uint16_t key;
    if(c == '\n' || c == '\r') {
        key = HID_KEYBOARD_RETURN;
    } else if(c == '\t') {
        key = HID_KEYBOARD_TAB;
    } else if(c == '\b') {
        key = HID_KEYBOARD_DELETE; // HID "Delete" == Backspace
    } else {
        key = HID_ASCII_TO_KEY(c);
    }
    if(key == HID_KEYBOARD_NONE) return true;
    furi_hal_hid_kb_press(key);
    furi_hal_hid_kb_release(key);
    app->keys_sent++;
    // Pause between letters; set from the phone's "Letter delay" slider
    return letter_pause(app);
}

static void remember_text(PhoneKb* app, const uint8_t* s, uint8_t len) {
    size_t cur = strlen(app->last_text);
    for(uint8_t i = 0; i < len; i++) {
        char c = (s[i] >= 32 && s[i] < 127) ? (char)s[i] : ' ';
        if(cur >= sizeof(app->last_text) - 1) {
            memmove(app->last_text, app->last_text + 1, cur);
            cur--;
        }
        app->last_text[cur++] = c;
        app->last_text[cur] = '\0';
    }
}

static uint16_t packet_length(const uint8_t* p, uint16_t have) {
    switch(p[0]) {
    case OpTap:
    case OpMouseMove:
    case OpConsumer:
    case OpKeyDown:
    case OpKeyUp:
        return 3;
    case OpMouseDown:
    case OpMouseUp:
    case OpScroll:
        return 2;
    case OpReleaseAll:
        return 1;
    case OpText:
        return (have < 2) ? 0xFFFF : (uint16_t)(2 + p[1]);
    default:
        return 1; // unknown byte: skip it to resync
    }
}

static void execute_packet(PhoneKb* app, const uint8_t* p) {
    switch(p[0]) {
    case OpTap: {
        uint16_t key = ((uint16_t)p[1] << 8) | p[2];
        furi_hal_hid_kb_press(key);
        furi_hal_hid_kb_release(key);
        app->keys_sent++;
        break;
    }
    case OpKeyDown:
        furi_hal_hid_kb_press(((uint16_t)p[1] << 8) | p[2]);
        break;
    case OpKeyUp:
        furi_hal_hid_kb_release(((uint16_t)p[1] << 8) | p[2]);
        break;
    case OpMouseMove:
        furi_hal_hid_mouse_move((int8_t)p[1], (int8_t)p[2]);
        break;
    case OpMouseDown:
        furi_hal_hid_mouse_press(p[1]);
        break;
    case OpMouseUp:
        furi_hal_hid_mouse_release(p[1]);
        break;
    case OpScroll:
        furi_hal_hid_mouse_scroll((int8_t)p[1]);
        break;
    case OpReleaseAll:
        furi_hal_hid_kb_release_all();
        furi_hal_hid_consumer_key_release_all();
        furi_hal_hid_mouse_release(HID_MOUSE_BTN_LEFT);
        furi_hal_hid_mouse_release(HID_MOUSE_BTN_RIGHT);
        furi_hal_hid_mouse_release(HID_MOUSE_BTN_WHEEL);
        break;
    case OpConsumer: {
        uint16_t key = p[1] | ((uint16_t)p[2] << 8);
        furi_hal_hid_consumer_key_press(key);
        furi_hal_hid_consumer_key_release(key);
        break;
    }
    case OpText: {
        uint8_t typed = 0;
        while(typed < p[1]) {
            if(!type_char(app, p[2 + typed++])) {
                app->typing_aborted = true;
                break;
            }
        }
        remember_text(app, &p[2], typed);
        view_port_update(app->view_port);
        break;
    }
    default:
        break;
    }
}

// Throw away everything still queued from the phone (after Stop, a disconnect or Back)
static void clear_queue(PhoneKb* app) {
    app->pkt_len = 0;
    uint8_t junk[64];
    while(furi_stream_buffer_receive(app->rx, junk, sizeof(junk), 0) > 0) {
    }
    furi_hal_hid_kb_release_all();
    furi_hal_hid_consumer_key_release_all();
    furi_hal_hid_mouse_release(HID_MOUSE_BTN_LEFT);
    furi_hal_hid_mouse_release(HID_MOUSE_BTN_RIGHT);
    furi_hal_hid_mouse_release(HID_MOUSE_BTN_WHEEL);
    event_log("typing stopped, queue cleared");
}

static void process_bytes(PhoneKb* app, const uint8_t* data, size_t len) {
    for(size_t i = 0; i < len; i++) {
        app->pkt[app->pkt_len++] = data[i];
        uint16_t need = packet_length(app->pkt, app->pkt_len);
        if(app->pkt_len >= need) {
            execute_packet(app, app->pkt);
            app->pkt_len = 0;
            if(app->typing_aborted) break;
        }
    }
    if(app->typing_aborted) {
        app->typing_aborted = false;
        clear_queue(app);
    }
}

// ---------------------------------------------------------------- UI

static void draw_cb(Canvas* canvas, void* context) {
    PhoneKb* app = context;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 11, "Phone Keyboard");

    canvas_set_font(canvas, FontSecondary);
    const char* phone = "Phone: waiting...";
    if(app->rejected) {
        phone = "Phone: wrong key!";
    } else if(app->ble_connected) {
        if(ble_profile_phone_kb_get_auth(app->profile) != PhoneKbAuthOk) {
            phone = "Phone: checking key...";
        } else if(ble_profile_phone_kb_is_paused(app->profile)) {
            phone = "Phone: PAUSED";
        } else {
            phone = "Phone: connected";
        }
    }
    canvas_draw_str(canvas, 2, 24, phone);
    canvas_draw_str(
        canvas, 2, 35, furi_hal_hid_is_connected() ? "PC (USB): connected" : "PC (USB): plug in cable");

    char buf[32];
    snprintf(buf, sizeof(buf), "Keys sent: %lu", app->keys_sent);
    canvas_draw_str(canvas, 2, 46, buf);
    canvas_draw_str(canvas, 2, 56, app->last_text);

    canvas_draw_str(canvas, 58, 64, app->has_token ? "Hold < new phone" : "Any phone links");
}

static void input_cb(InputEvent* event, void* context) {
    PhoneKb* app = context;
    furi_message_queue_put(app->input_queue, event, 0);
}

// ---------------------------------------------------------------- main

int32_t phone_kb_app(void* p) {
    UNUSED(p);
    PhoneKb* app = malloc(sizeof(PhoneKb));
    app->rx = furi_stream_buffer_alloc(RX_BUF_SIZE, 1);
    app->input_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->notifications = furi_record_open(RECORD_NOTIFICATION);

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, draw_cb, app);
    view_port_input_callback_set(app->view_port, input_cb, app);
    app->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    // USB: become a keyboard + mouse (this disconnects qFlipper while running)
    app->usb_prev = furi_hal_usb_get_config();
    furi_hal_usb_unlock();
    furi_check(furi_hal_usb_set_config(&usb_hid, NULL));

    token_load(app);
    event_log_start();
    event_log("start, linked phone: %s", app->has_token ? "yes" : "no");

    // BLE: swap the system profile for ours
    app->bt = furi_record_open(RECORD_BT);
    bt_disconnect(app->bt);
    furi_delay_ms(200); // let the 2nd core flush NVM
    bt_keys_storage_set_storage_path(app->bt, KEYS_PATH);
    app->profile = bt_profile_start(app->bt, ble_profile_phone_kb, NULL);
    furi_check(app->profile);
    ble_profile_phone_kb_set_callbacks(app->profile, phone_kb_rx_cb, phone_kb_auth_cb, app);
    furi_hal_bt_start_advertising();

    uint8_t chunk[CHUNK_SIZE];
    bool running = true;
    bool usb_was_connected = false;
    PhoneKbAuthStatus shown_auth = PhoneKbAuthWaiting;
    bool shown_connected = false;
    bool shown_paused = false;
    uint32_t logged_dropped = 0;
    while(running) {
        // While paused, leave everything queued where it is
        bool paused = ble_profile_phone_kb_is_paused(app->profile);
        size_t n = 0;
        if(paused) {
            furi_delay_ms(30);
        } else {
            n = furi_stream_buffer_receive(app->rx, chunk, sizeof(chunk), 30);
        }
        if(n > 0) process_bytes(app, chunk, n);

        paused = ble_profile_phone_kb_is_paused(app->profile);
        if(paused != shown_paused) {
            shown_paused = paused;
            event_log(paused ? "paused" : "resumed");
            view_port_update(app->view_port);
        }

        // Stop tapped on the phone while nothing was being typed: drop what's queued
        uint32_t stops = ble_profile_phone_kb_get_stop_count(app->profile);
        if(stops != app->stop_seen) {
            app->stop_seen = stops;
            clear_queue(app);
        }

        if(app->token_save_pending) {
            app->token_save_pending = false;
            token_save(app);
            event_log("new phone linked, key saved");
            notification_message(app->notifications, &sequence_success);
        }

        // The key check happens in the BLE thread; publish its result to the
        // phone (STATUS characteristic) and the screen from here.
        PhoneKbAuthStatus auth = ble_profile_phone_kb_sync_status(app->profile);
        if(auth != shown_auth) {
            shown_auth = auth;
            event_log(
                "key %s",
                auth == PhoneKbAuthOk       ? "accepted" :
                auth == PhoneKbAuthRejected ? "REJECTED" :
                                              "reset");
            view_port_update(app->view_port);
        }

        bool ble_connected = ble_profile_phone_kb_is_connected(app->profile);
        if(ble_connected != shown_connected) {
            shown_connected = ble_connected;
            phone_kb_link_changed(app, ble_connected);
            event_log("phone %s", ble_connected ? "connected" : "disconnected");
            // Don't keep typing for a phone that's gone
            if(!ble_connected) clear_queue(app);
        }

        uint32_t dropped = app->rx_dropped;
        if(dropped != logged_dropped) {
            logged_dropped = dropped;
            event_log("rx buffer full, %lu bytes dropped so far", (unsigned long)dropped);
        }

        bool usb_connected = furi_hal_hid_is_connected();
        if(usb_connected != usb_was_connected) {
            usb_was_connected = usb_connected;
            view_port_update(app->view_port);
        }

        InputEvent ev;
        if(app->exit_requested) running = false;
        bool forget = app->forget_requested;
        app->forget_requested = false;
        while(furi_message_queue_get(app->input_queue, &ev, 0) == FuriStatusOk) {
            if(ev.key == InputKeyBack && ev.type == InputTypeShort) running = false;
            if(ev.key == InputKeyLeft && ev.type == InputTypeLong) forget = true;
        }
        if(forget) {
            // Forget the linked phone: the next phone that connects becomes the new one
            token_forget(app);
            event_log("hold left: linked phone forgotten");
            bt_disconnect(app->bt);
            furi_delay_ms(200);
            furi_hal_bt_start_advertising();
            notification_message(app->notifications, &sequence_success);
            view_port_update(app->view_port);
        }
    }

    event_log("exit");

    // Teardown: restore everything the way we found it
    bt_disconnect(app->bt);
    furi_delay_ms(200);
    bt_keys_storage_set_default_path(app->bt);
    furi_check(bt_profile_restore_default(app->bt));
    furi_record_close(RECORD_BT);

    furi_hal_hid_kb_release_all();
    furi_hal_usb_set_config(app->usb_prev, NULL);

    notification_internal_message(app->notifications, &sequence_reset_blue);
    gui_remove_view_port(app->gui, app->view_port);
    view_port_free(app->view_port);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    furi_message_queue_free(app->input_queue);
    furi_stream_buffer_free(app->rx);
    free(app);
    return 0;
}
