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
 *   0x08 len bytes...    type UTF-8 text (len <= 255; characters may span packets),
 *                        on the PC layout the phone set (US or German)
 *   0x09 mods key        key down (hold)
 *   0x0A mods key        key up
 *   0x0B len bytes...    like 0x08, but typed like a person (humanize.c): human
 *                        rhythm and pauses at the CONFIG speed, optional typos
 *   0x0C                 stored text: start a new one (clears the old)
 *   0x0D len bytes...    stored text: more UTF-8 text (saved to the SD card)
 *   0x0E flags           stored text: done (flags bit 0: humanized). From now on a
 *                        Caps Lock press on the PC while idle makes the Flipper type
 *                        it by itself, even with the phone locked or disconnected.
 *                        Used once, then cleared.
 */

#include <stdarg.h>
#include <furi.h>
#include <furi_hal_bt.h>
#include <furi_hal_random.h>
#include <furi_hal_usb.h>
#include <furi_hal_usb_hid.h>
#include <gui/gui.h>
#include <input/input.h>
#include <notification/notification_messages.h>
#include <bt/bt_service/bt.h>
#include <storage/storage.h>

#include "phone_kb_profile.h"
#include "layout_de.h"
#include "humanize.h"
#include "phone_kb_usb.h"

#define TAG              "PhoneKB"
#define KEYS_PATH        APP_DATA_PATH(".phone_kb.keys")
#define TOKEN_PATH       APP_DATA_PATH(".phone_kb.token")
#define EVENT_LOG_PATH   APP_DATA_PATH("events.log")
#define STORED_PATH      APP_DATA_PATH("stored_text.txt")
#define STORED_MAX       (64 * 1024)
#define EVENT_LOG_MAX    (16 * 1024)
#define RX_BUF_SIZE      2048
#define CHUNK_SIZE       128
// A link must present the right secret within this time, or it is dropped
// (so a stray or rejected device can't block the phone from connecting)
#define AUTH_TIMEOUT_MS    5000
#define REJECTED_LINGER_MS 1500 // long enough for the page to read "rejected"

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
    OpTextHuman = 0x0B,
    OpStoreBegin = 0x0C,
    OpStoreData = 0x0D,
    OpStoreEnd = 0x0E,
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
    uint32_t bytes_done; // RX bytes typed or cleared (+ rx_dropped = PROGRESS for the phone)

    // Typing can take minutes with a long letter delay, so it must be stoppable
    uint32_t stop_seen; // last stop request handled (see ble_profile_phone_kb_get_stop_count)
    uint32_t release_seen; // last "release everything" request handled
    bool typing_aborted; // drop the rest of what's queued
    bool exit_requested; // Back pressed while typing
    bool forget_requested; // Hold Left pressed while typing
    uint32_t keys_sent;
    char last_text[22];

    // parser state
    uint8_t pkt[260];
    uint16_t pkt_len;
    uint32_t utf8_cp; // UTF-8 character being decoded (may span packets)
    uint8_t utf8_left; // continuation bytes still expected

    Humanizer hum; // human-like timing for OpTextHuman
    uint32_t prev_hold; // how long the previous humanized key was held (ms)
    volatile bool thinking; // in a humanized "thinking" pause (shown on screen)

    // Caps Lock on the PC as a pause key while typing
    bool caps_led; // last Caps Lock state the PC reported
    bool caps_paused; // paused with Caps Lock
    bool typing_active; // in the middle of typing a text packet
    uint8_t start_requests; // Caps Lock presses while idle: the phone starts typing its clipboard
    uint32_t start_request_tick; // when the last start request was made (0 = none pending)

    // Stored text (sent by the phone in advance): typed on Caps Lock without the phone
    Storage* storage;
    File* store_file; // open while the phone is sending a stored text
    uint32_t store_len;
    bool stored_ready;
    bool stored_human;
    bool stored_start; // Caps Lock pressed: type the stored text (main loop)
    bool stored_caps_before; // Caps Lock state before that press
    bool stored_typing; // typing the stored text (doesn't need the phone)
    uint32_t own_caps_until; // tick until which a Caps Lock change is our own key press
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
// The page only puts whole packets in a write, so a write that doesn't fit is
// dropped as a whole: storing part of it would cut a packet and the parser would
// then read the following bytes (mouse moves, keys) as text.
static void phone_kb_rx_cb(const uint8_t* data, uint16_t size, void* context) {
    PhoneKb* app = context;
    // Only this thread writes, so free space can only grow until the send
    if(furi_stream_buffer_spaces_available(app->rx) < size) {
        app->rx_dropped += size;
        return;
    }
    furi_stream_buffer_send(app->rx, data, size, 0);
}

// Runs in the BLE event thread (tiny stack). The first secret sent from a Connect
// the user tapped is adopted (saved later by the main loop); after that it must
// match. Automatic reconnects never claim an unlinked Flipper, so after
// "Hold Left" the old phone's page can't silently re-link itself.
static bool phone_kb_auth_cb(
    const uint8_t token[PHONE_KB_TOKEN_LEN],
    bool user_initiated,
    void* context) {
    PhoneKb* app = context;
    bool ok;
    if(!app->has_token) {
        if(!user_initiated) {
            app->rejected = true;
            return false;
        }
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
        pkb_hid_kb_release_all();
        pkb_hid_mouse_release(HID_MOUSE_BTN_LEFT);
        pkb_hid_mouse_release(HID_MOUSE_BTN_RIGHT);
        pkb_hid_mouse_release(HID_MOUSE_BTN_WHEEL);
    }
    view_port_update(app->view_port);
}

// ---------------------------------------------------------------- HID side

// ---- Caps Lock as a pause key ----
// The PC tells every keyboard when Caps Lock turns on or off (keyboard LEDs).
// While the Flipper is typing, or has text waiting, each Caps Lock press pauses
// or resumes it. When it's idle, Caps Lock is just Caps Lock.
static bool caps_lock_on(void) {
    return (pkb_hid_get_led_state() & HID_KB_LED_CAPS) != 0;
}

static void caps_check(PhoneKb* app) {
    bool on = caps_lock_on();
    if(on == app->caps_led) return;
    app->caps_led = on;
    // Our own Caps Lock press (phone's Caps button, or restoring Caps Lock after a text)
    if((int32_t)(app->own_caps_until - furi_get_tick()) > 0) {
        event_log("Caps Lock %s (own key press)", on ? "on" : "off");
        return;
    }
    if(ble_profile_phone_kb_is_paused(app->profile)) { // the phone's Pause rules
        event_log("Caps Lock %s (phone paused: ignored)", on ? "on" : "off");
        return;
    }
    bool busy = app->typing_active || app->caps_paused ||
                furi_stream_buffer_bytes_available(app->rx) > 0;
    if(!busy) {
        if(app->stored_ready) {
            // A stored text is waiting: the Flipper types it by itself (main loop)
            app->stored_start = true;
            app->stored_caps_before = !on;
            event_log("Caps Lock %s: typing the stored text", on ? "on" : "off");
            return;
        }
        // Idle: ask the phone to start typing its clipboard (it decides)
        app->start_requests++;
        app->start_request_tick = furi_get_tick() | 1;
        event_log("Caps Lock %s: start request", on ? "on" : "off");
        return;
    }
    app->caps_paused = !app->caps_paused;
    event_log("Caps Lock %s: %s", on ? "on" : "off", app->caps_paused ? "PAUSED" : "resumed");
    view_port_update(app->view_port);
}

static bool is_paused(PhoneKb* app) {
    return app->caps_paused || ble_profile_phone_kb_is_paused(app->profile);
}

// Press Caps Lock ourselves (caps_check knows it's ours) and let the PC update the light
static void press_own_caps(PhoneKb* app) {
    app->own_caps_until = furi_get_tick() + 400;
    pkb_hid_kb_press(HID_KEYBOARD_CAPS_LOCK);
    furi_delay_ms(30);
    pkb_hid_kb_release(HID_KEYBOARD_CAPS_LOCK);
}

// Typing always happens with Caps Lock OFF: with it on, some PCs shift every key
// (, -> ;  ( -> 8  ? -> ß), not just letters. Waits a moment first, because the
// user's own Caps Lock key may still be held down, and while it is, the PC
// ignores a second Caps Lock press. Then checks the PC really switched it off.
static void ensure_caps_off(PhoneKb* app) {
    for(int attempt = 0; attempt < 3 && caps_lock_on(); attempt++) {
        furi_delay_ms(attempt == 0 ? 350 : 250);
        if(!caps_lock_on()) break;
        press_own_caps(app);
        for(int w = 0; w < 50 && caps_lock_on(); w++) furi_delay_ms(10); // the PC's answer
    }
    if(caps_lock_on()) event_log("Caps Lock: couldn't switch it off before typing");
}

// Checked between letters (and every 10 ms of a letter pause): stop typing when
// the phone taps Stop, the phone disconnects, or Back is pressed on the Flipper.
static bool typing_should_stop(PhoneKb* app) {
    caps_check(app);
    uint32_t stops = ble_profile_phone_kb_get_stop_count(app->profile);
    if(stops != app->stop_seen) {
        app->stop_seen = stops;
        return true;
    }
    if(!app->stored_typing && !ble_profile_phone_kb_is_connected(app->profile)) return true;
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
// The delay is re-read every step, so a speed change on the phone (also while
// paused) applies right away instead of after a long pause ends.
static bool letter_pause(PhoneKb* app) {
    uint32_t waited = 0;
    for(;;) {
        uint32_t delay = ble_profile_phone_kb_get_letter_delay(app->profile);
        if(waited >= delay) return true;
        uint32_t step = delay - waited < 10 ? delay - waited : 10;
        furi_delay_ms(step);
        waited += step;
        if(typing_should_stop(app)) return false;
    }
}

static void release_everything(PhoneKb* app) {
    UNUSED(app);
    pkb_hid_kb_release_all();
    pkb_hid_consumer_key_release_all();
    pkb_hid_mouse_release(HID_MOUSE_BTN_LEFT);
    pkb_hid_mouse_release(HID_MOUSE_BTN_RIGHT);
    pkb_hid_mouse_release(HID_MOUSE_BTN_WHEEL);
}

// The phone can ask to let go of held keys/buttons even while paused (when the
// normal queue isn't being read), e.g. turning off "Hold click" during a pause.
static void check_release_request(PhoneKb* app) {
    uint32_t releases = ble_profile_phone_kb_get_release_count(app->profile);
    if(releases != app->release_seen) {
        app->release_seen = releases;
        release_everything(app);
    }
}

// Hold here while the phone has paused typing. Returns false if typing should stop.
static bool wait_while_paused(PhoneKb* app) {
    bool waited = false;
    while(is_paused(app)) {
        waited = true;
        furi_delay_ms(10);
        check_release_request(app);
        if(typing_should_stop(app)) return false;
    }
    // Resumed (e.g. with Caps Lock): make sure Caps Lock is off before typing on
    if(waited && caps_lock_on()) ensure_caps_off(app);
    return true;
}

// Feed one byte of UTF-8 text. Returns true when *cp holds a complete character.
static bool utf8_feed(PhoneKb* app, uint8_t b, uint32_t* cp) {
    if(b < 0x80) {
        app->utf8_left = 0;
        *cp = b;
        return true;
    }
    if((b & 0xC0) == 0x80) { // continuation byte
        if(!app->utf8_left) return false; // stray: skip
        app->utf8_cp = (app->utf8_cp << 6) | (b & 0x3F);
        if(--app->utf8_left) return false;
        *cp = app->utf8_cp;
        return true;
    }
    if((b & 0xE0) == 0xC0) {
        app->utf8_cp = b & 0x1F;
        app->utf8_left = 1;
    } else if((b & 0xF0) == 0xE0) {
        app->utf8_cp = b & 0x0F;
        app->utf8_left = 2;
    } else if((b & 0xF8) == 0xF0) {
        app->utf8_cp = b & 0x07;
        app->utf8_left = 3;
    } else {
        app->utf8_left = 0;
    }
    return false;
}

// Last typed text for the Flipper screen (its font is ASCII only)
static void remember_char(PhoneKb* app, uint32_t cp) {
    char c;
    if(cp >= 32 && cp < 127) c = (char)cp;
    else if(cp == 0xE4 || cp == 0xC4) c = cp == 0xE4 ? 'a' : 'A'; // ä Ä
    else if(cp == 0xF6 || cp == 0xD6) c = cp == 0xF6 ? 'o' : 'O'; // ö Ö
    else if(cp == 0xFC || cp == 0xDC) c = cp == 0xFC ? 'u' : 'U'; // ü Ü
    else if(cp == 0xDF) c = 's'; // ß
    else if(cp == '\n' || cp == '\t') c = ' ';
    else c = '?';
    size_t cur = strlen(app->last_text);
    if(cur >= sizeof(app->last_text) - 1) {
        memmove(app->last_text, app->last_text + 1, cur);
        cur--;
    }
    app->last_text[cur++] = c;
    app->last_text[cur] = '\0';
}

// Key (usage + modifiers) for a character on a US-layout PC, or NONE
static uint16_t us_key(uint32_t cp) {
    if(cp >= 128) return HID_KEYBOARD_NONE;
    if(cp == '\n' || cp == '\r') return HID_KEYBOARD_RETURN;
    if(cp == '\t') return HID_KEYBOARD_TAB;
    if(cp == '\b') return HID_KEYBOARD_DELETE; // HID "Delete" == Backspace
    return HID_ASCII_TO_KEY(cp);
}

// Key (usage + modifiers) for a character on the PC's layout, or NONE.
// *dead: a dead key (^ ´ `) that needs Space after it to type the accent itself.
static uint16_t key_for(PhoneKb* app, uint32_t cp, bool* dead) {
    uint16_t key = HID_KEYBOARD_NONE;
    *dead = false;
    if(ble_profile_phone_kb_get_layout(app->profile) == PhoneKbLayoutDe) {
        if(!layout_de_lookup(cp, &key, dead)) key = HID_KEYBOARD_NONE;
    } else {
        key = us_key(cp);
    }
    // Caps Lock on at the PC inverts letters: flip Shift so the text comes out as sent
    bool letter = (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || cp == 0x00E4 ||
                  cp == 0x00F6 || cp == 0x00FC || cp == 0x00C4 || cp == 0x00D6 || cp == 0x00DC;
    if(key != HID_KEYBOARD_NONE && letter && caps_lock_on()) key ^= KEY_MOD_LEFT_SHIFT;
    return key;
}

// Type one character on the PC's layout. Returns false if typing should stop.
static bool type_codepoint(PhoneKb* app, uint32_t cp) {
    if(typing_should_stop(app)) return false;
    if(!wait_while_paused(app)) return false;

    bool dead;
    uint16_t key = key_for(app, cp, &dead);
    if(key == HID_KEYBOARD_NONE) return true; // can't be typed on this layout: skip it
    app->prev_hold = 0;

    pkb_hid_kb_press(key);
    pkb_hid_kb_release(key);
    if(dead) {
        // Dead key (^ ´ `): Space makes it type the accent itself
        pkb_hid_kb_press(HID_KEYBOARD_SPACEBAR);
        pkb_hid_kb_release(HID_KEYBOARD_SPACEBAR);
    }
    app->keys_sent++;
    remember_char(app, cp);
    // Pause between letters; set from the phone's "Letter delay" slider
    return letter_pause(app);
}

// ---------------------------------------------------------------- humanized typing

static uint32_t hum_random(void* context) {
    UNUSED(context);
    return furi_hal_random_get();
}

// Take the phone's current Humanize settings
static void hum_sync(PhoneKb* app) {
    uint8_t wpm = ble_profile_phone_kb_get_wpm(app->profile);
    app->hum.wpm = wpm ? wpm : 55;
    app->hum.typos = ble_profile_phone_kb_get_typos(app->profile);
    app->hum.german = ble_profile_phone_kb_get_layout(app->profile) == PhoneKbLayoutDe;
    app->hum.think = ble_profile_phone_kb_get_think(app->profile);
}

// Wait in 10 ms steps, holding while paused. Returns false if typing should stop.
static bool hum_wait(PhoneKb* app, uint32_t ms) {
    while(ms > 0) {
        uint32_t step = ms < 10 ? ms : 10;
        furi_delay_ms(step);
        ms -= step;
        if(typing_should_stop(app)) return false;
        if(!wait_while_paused(app)) return false;
    }
    return true;
}

// Press one key `gap` ms after the previous press, hold it like a person.
// Returns false if typing should stop.
static bool hum_key(PhoneKb* app, uint32_t cp, uint32_t gap) {
    if(typing_should_stop(app)) return false;
    if(!wait_while_paused(app)) return false;
    bool dead;
    uint16_t key = key_for(app, cp, &dead);
    // The gap runs from the previous press; part of it was spent holding that key
    if(!hum_wait(app, gap > app->prev_hold ? gap - app->prev_hold : 0)) return false;
    if(key == HID_KEYBOARD_NONE) { // can't be typed on this layout: skip it
        app->prev_hold = 0;
        return true;
    }
    uint32_t hold = humanize_hold_ms(&app->hum, gap);
    pkb_hid_kb_press(key);
    furi_delay_ms(hold);
    pkb_hid_kb_release(key);
    if(dead) { // Space after a dead key, a quick second press
        furi_delay_ms(40 + hold / 2);
        pkb_hid_kb_press(HID_KEYBOARD_SPACEBAR);
        furi_delay_ms(hold * 3 / 4);
        pkb_hid_kb_release(HID_KEYBOARD_SPACEBAR);
    }
    app->prev_hold = hold;
    app->keys_sent++;
    if(cp == '\b') {
        size_t cur = strlen(app->last_text);
        if(cur) app->last_text[cur - 1] = '\0';
    } else {
        remember_char(app, cp);
    }
    return true;
}

static bool hum_type(PhoneKb* app, uint32_t cp) {
    uint32_t think = humanize_think_ms(&app->hum, cp);
    if(think) {
        // A thinking pause: show it on the Flipper, so a long stop is clearly on purpose
        app->thinking = true;
        view_port_update(app->view_port);
        bool ok = hum_wait(app, think);
        app->thinking = false;
        view_port_update(app->view_port);
        if(!ok) return false;
    }
    return hum_key(app, cp, humanize_gap_ms(&app->hum, cp));
}

// Type a text packet like a person, with an occasional typo that is noticed
// (at once or a letter or two later) and fixed with Backspace.
static bool hum_text(PhoneKb* app, const uint8_t* t, uint8_t len) {
    hum_sync(app);
    for(uint8_t i = 0; i < len; i++) {
        uint32_t cp;
        if(!utf8_feed(app, t[i], &cp)) continue;
        uint32_t wrong;
        uint8_t late;
        if(humanize_typo(&app->hum, cp, &wrong, &late)) {
            // "Noticed late" only over plain characters that are in this packet
            uint8_t extra = 0;
            while(extra < late && i + 1 + extra < len && t[i + 1 + extra] >= 0x20 &&
                  t[i + 1 + extra] < 0x7F)
                extra++;
            if(!hum_type(app, wrong)) return false;
            for(uint8_t k = 0; k < extra; k++)
                if(!hum_type(app, t[i + 1 + k])) return false;
            if(!hum_wait(app, humanize_notice_ms(&app->hum))) return false;
            for(uint8_t k = 0; k <= extra; k++)
                if(!hum_key(app, '\b', humanize_backspace_gap_ms(&app->hum))) return false;
            app->hum.prev = '\b';
            // the characters typed after the typo are typed again by this loop
        }
        if(!hum_type(app, cp)) return false;
    }
    return true;
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
    case OpTextHuman:
    case OpStoreData:
        return (have < 2) ? 0xFFFF : (uint16_t)(2 + p[1]);
    case OpStoreBegin:
        return 1;
    case OpStoreEnd:
        return 2;
    default:
        return 1; // unknown byte: skip it to resync
    }
}

// ---------------------------------------------------------------- stored text

static void stored_clear(PhoneKb* app) {
    if(app->store_file) {
        storage_file_close(app->store_file);
        storage_file_free(app->store_file);
        app->store_file = NULL;
    }
    app->stored_ready = false;
    app->store_len = 0;
    storage_simply_remove(app->storage, STORED_PATH);
}

// Type the stored text: Caps Lock was pressed while idle. Works without the phone.
static void stored_type(PhoneKb* app) {
    app->stored_ready = false; // used once
    File* f = storage_file_alloc(app->storage);
    if(!storage_file_open(f, STORED_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        storage_file_free(f);
        return;
    }
    app->stored_typing = true;
    app->typing_active = true;
    app->utf8_left = 0;
    humanize_reset(&app->hum);
    app->prev_hold = 0;
    view_port_update(app->view_port);

    // Type with Caps Lock off: with it on, some systems change more than letters (ß -> ?)
    ensure_caps_off(app);

    bool ok = true;
    uint8_t buf[64];
    size_t n;
    while(ok && (n = storage_file_read(f, buf, sizeof(buf))) > 0) {
        if(app->stored_human) {
            ok = hum_text(app, buf, (uint8_t)n);
        } else {
            for(size_t i = 0; i < n && ok; i++) {
                uint32_t cp;
                if(utf8_feed(app, buf[i], &cp)) ok = type_codepoint(app, cp);
            }
        }
    }
    storage_file_close(f);
    storage_file_free(f);
    storage_simply_remove(app->storage, STORED_PATH);

    app->typing_active = false;
    app->stored_typing = false;
    app->caps_paused = false;
    release_everything(app);
    // Caps Lock back to how it was before the press that started this
    if(caps_lock_on() != app->stored_caps_before) {
        press_own_caps(app);
        furi_delay_ms(80);
    }
    event_log(ok ? "stored text typed" : "stored text stopped");
    view_port_update(app->view_port);
}

// First text after a Caps Lock start request (the phone types its clipboard or box):
// make sure Caps Lock is off, whatever the phone managed to do about it.
static void caps_start_check(PhoneKb* app) {
    if(!app->start_request_tick) return;
    bool recent = furi_get_tick() - app->start_request_tick < 15000;
    app->start_request_tick = 0;
    if(recent && caps_lock_on()) ensure_caps_off(app);
}

static void execute_packet(PhoneKb* app, const uint8_t* p) {
    switch(p[0]) {
    case OpTap: {
        uint16_t key = ((uint16_t)p[1] << 8) | p[2];
        if(p[2] == HID_KEYBOARD_CAPS_LOCK) app->own_caps_until = furi_get_tick() + 400;
        pkb_hid_kb_press(key);
        pkb_hid_kb_release(key);
        app->keys_sent++;
        break;
    }
    case OpKeyDown:
        if(p[2] == HID_KEYBOARD_CAPS_LOCK) app->own_caps_until = furi_get_tick() + 400;
        pkb_hid_kb_press(((uint16_t)p[1] << 8) | p[2]);
        break;
    case OpKeyUp:
        pkb_hid_kb_release(((uint16_t)p[1] << 8) | p[2]);
        break;
    case OpMouseMove:
        pkb_hid_mouse_move((int8_t)p[1], (int8_t)p[2]);
        break;
    case OpMouseDown:
        pkb_hid_mouse_press(p[1]);
        break;
    case OpMouseUp:
        pkb_hid_mouse_release(p[1]);
        break;
    case OpScroll:
        pkb_hid_mouse_scroll((int8_t)p[1]);
        break;
    case OpReleaseAll:
        release_everything(app);
        break;
    case OpConsumer: {
        uint16_t key = p[1] | ((uint16_t)p[2] << 8);
        pkb_hid_consumer_key_press(key);
        pkb_hid_consumer_key_release(key);
        break;
    }
    case OpText: {
        app->typing_active = true;
        caps_start_check(app);
        for(uint8_t i = 0; i < p[1]; i++) {
            uint32_t cp;
            if(!utf8_feed(app, p[2 + i], &cp)) continue;
            if(!type_codepoint(app, cp)) {
                app->typing_aborted = true;
                break;
            }
        }
        app->typing_active = false;
        view_port_update(app->view_port);
        break;
    }
    case OpTextHuman:
        app->typing_active = true;
        caps_start_check(app);
        if(!hum_text(app, &p[2], p[1])) app->typing_aborted = true;
        app->typing_active = false;
        view_port_update(app->view_port);
        break;
    case OpStoreBegin:
        stored_clear(app);
        app->store_file = storage_file_alloc(app->storage);
        if(!storage_file_open(app->store_file, STORED_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
            storage_file_free(app->store_file);
            app->store_file = NULL;
            event_log("stored text: can't write to the SD card");
        }
        view_port_update(app->view_port);
        break;
    case OpStoreData:
        if(app->store_file && app->store_len + p[1] <= STORED_MAX) {
            if(storage_file_write(app->store_file, &p[2], p[1]) == p[1]) app->store_len += p[1];
        }
        break;
    case OpStoreEnd:
        if(app->store_file) {
            storage_file_close(app->store_file);
            storage_file_free(app->store_file);
            app->store_file = NULL;
            app->stored_ready = app->store_len > 0;
            app->stored_human = (p[1] & 1) != 0;
            if(app->stored_ready)
                event_log(
                    "stored text ready: %lu bytes%s",
                    (unsigned long)app->store_len,
                    app->stored_human ? ", humanized" : "");
        }
        view_port_update(app->view_port);
        break;
    default:
        break;
    }
}

// Throw away everything still queued from the phone (after Stop, a disconnect or Back)
static void clear_queue(PhoneKb* app, bool was_typing) {
    app->pkt_len = 0;
    app->utf8_left = 0;
    humanize_reset(&app->hum);
    app->prev_hold = 0;
    app->caps_paused = false; // Stop also ends a Caps Lock pause
    app->typing_active = false;
    uint8_t junk[64];
    size_t dropped = 0, got;
    while((got = furi_stream_buffer_receive(app->rx, junk, sizeof(junk), 0)) > 0) {
        dropped += got;
    }
    app->bytes_done += dropped;
    release_everything(app);
    if(was_typing || dropped) {
        event_log("typing stopped, %lu queued bytes dropped", (unsigned long)dropped);
    }
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
        clear_queue(app, true);
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
    } else if(app->stored_typing) { // doesn't need the phone
        phone = app->caps_paused ? "PAUSED (Caps Lock)" :
                app->thinking    ? "Stored text: thinking..." :
                                   "Typing stored text";
    } else if(app->ble_connected) {
        if(ble_profile_phone_kb_get_auth(app->profile) != PhoneKbAuthOk) {
            phone = "Phone: checking key...";
        } else if(app->caps_paused) {
            phone = "PAUSED (Caps Lock)";
        } else if(ble_profile_phone_kb_is_paused(app->profile)) {
            phone = "Phone: PAUSED";
        } else if(app->thinking) {
            phone = "Phone: thinking...";
        } else {
            phone = "Phone: connected";
        }
    }
    canvas_draw_str(canvas, 2, 24, phone);
    canvas_draw_str(
        canvas, 2, 35, pkb_hid_is_connected() ? "PC (USB): connected" : "PC (USB): plug in cable");

    char buf[32];
    if(app->stored_ready) {
        snprintf(buf, sizeof(buf), "Text ready: press Caps Lock");
    } else {
        snprintf(buf, sizeof(buf), "Keys sent: %lu", app->keys_sent);
    }
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

    // USB: become a keyboard + mouse (this disconnects qFlipper while running).
    // Our own copy of the firmware's keyboard, which can also receive Caps Lock.
    app->usb_prev = furi_hal_usb_get_config();
    furi_hal_usb_unlock();
    furi_check(furi_hal_usb_set_config(&pkb_usb_hid, NULL));

    humanize_init(&app->hum, hum_random, NULL);
    app->caps_led = caps_lock_on();
    token_load(app);
    event_log_start();
    app->storage = furi_record_open(RECORD_STORAGE);
    storage_simply_remove(app->storage, STORED_PATH); // a stored text never outlives the app
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
    uint32_t link_up_at = 0;
    uint32_t published_progress = 0;
    uint8_t logged_leds = 0xFF;
    uint8_t published_state = 0;
    uint32_t logged_dropped = 0;
    while(running) {
        // While paused, leave everything queued where it is
        caps_check(app);
        if(app->stored_start) {
            app->stored_start = false;
            stored_type(app);
        }
        bool paused = is_paused(app);
        size_t n = 0;
        if(paused) {
            furi_delay_ms(30);
        } else {
            n = furi_stream_buffer_receive(app->rx, chunk, sizeof(chunk), 30);
        }
        if(n > 0) {
            process_bytes(app, chunk, n);
            app->bytes_done += n;
        }

        paused = is_paused(app);
        if(paused != shown_paused) {
            shown_paused = paused;
            event_log(paused ? (app->caps_paused ? "paused (Caps Lock)" : "paused") : "resumed");
            view_port_update(app->view_port);
        }

        // Stop tapped on the phone while nothing was being typed: drop what's queued
        uint32_t stops = ble_profile_phone_kb_get_stop_count(app->profile);
        if(stops != app->stop_seen) {
            app->stop_seen = stops;
            clear_queue(app, false);
        }
        check_release_request(app);

        // Tell the phone how far we are, so it sends more exactly when there's room
        uint32_t progress = app->bytes_done + app->rx_dropped;
        uint8_t state = (app->caps_paused ? PHONE_KB_STATE_CAPS_PAUSED : 0) |
                        (app->caps_led ? PHONE_KB_STATE_CAPS_ON : 0) |
                        (app->stored_ready ? PHONE_KB_STATE_STORED : 0) |
                        (uint8_t)((app->start_requests & 0x0F) << PHONE_KB_STATE_START_SHIFT);
        if(progress != published_progress || state != published_state) {
            published_progress = progress;
            published_state = state;
            ble_profile_phone_kb_publish_progress(app->profile, progress, state);
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
            if(!ble_connected) clear_queue(app, false);
            link_up_at = furi_get_tick();
        }

        // Drop a link that doesn't present the right secret in time
        if(ble_connected) {
            uint32_t up_for = furi_get_tick() - link_up_at;
            bool drop = (auth == PhoneKbAuthWaiting && up_for > AUTH_TIMEOUT_MS) ||
                        (auth == PhoneKbAuthRejected && up_for > REJECTED_LINGER_MS);
            if(drop) {
                event_log(
                    "dropping phone link: %s", auth == PhoneKbAuthRejected ? "wrong key" : "no key sent");
                bt_disconnect(app->bt);
                furi_delay_ms(200);
                furi_hal_bt_start_advertising();
                link_up_at = furi_get_tick();
            }
        }

        uint32_t dropped = app->rx_dropped;
        if(dropped != logged_dropped) {
            logged_dropped = dropped;
            event_log("rx buffer full, %lu bytes dropped so far", (unsigned long)dropped);
        }

        uint8_t leds = pkb_hid_get_led_state();
        if(logged_leds == 0xFF || ((leds ^ logged_leds) & HID_KB_LED_NUM)) {
            logged_leds = leds;
            event_log(
                "PC keyboard lights: caps %s, num %s",
                (leds & HID_KB_LED_CAPS) ? "on" : "off",
                (leds & HID_KB_LED_NUM) ? "on" : "off");
        }

        bool usb_connected = pkb_hid_is_connected();
        if(usb_connected != usb_was_connected) {
            usb_was_connected = usb_connected;
            event_log("PC (USB) %s", usb_connected ? "active" : "suspended/unplugged");
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

    stored_clear(app);
    furi_record_close(RECORD_STORAGE);
    event_log("exit");

    // Teardown: restore everything the way we found it
    bt_disconnect(app->bt);
    furi_delay_ms(200);
    bt_keys_storage_set_default_path(app->bt);
    furi_check(bt_profile_restore_default(app->bt));
    furi_record_close(RECORD_BT);

    pkb_hid_kb_release_all();
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
