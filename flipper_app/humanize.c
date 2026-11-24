#include "humanize.h"

#include <math.h>
#include <stddef.h>

// Within-word gaps are a bit shorter than the average gap, because pauses at
// word starts, punctuation and new lines make up the rest. Calibrated with the
// host test so that typical text comes out at the target WPM.
#define BASE_SHARE 0.78f
#define GAP_SIGMA  0.33f // spread of the log-normal gap distribution

// ---------------------------------------------------------------- randomness

static float uniform(Humanizer* h) {
    return (float)(h->random(h->random_context) >> 8) * (1.0f / 16777216.0f);
}

// Approximately standard normal (sum of 6 uniforms: mean 3, variance 0.5)
static float normal(Humanizer* h) {
    float s = 0;
    for(int i = 0; i < 6; i++) s += uniform(h);
    return (s - 3.0f) * 1.41421356f;
}

static float lognormal(Humanizer* h, float median, float sigma) {
    return median * powf(2.71828183f, sigma * normal(h));
}

// ---------------------------------------------------------------- keyboard geometry

#define ROWS 4

// Unshifted characters on each row, left to right (number row first)
static const uint32_t rows_us[ROWS][13] = {
    {'1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 0},
    {'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 0},
    {'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', 0, 0},
    {'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, 0, 0},
};
static const uint32_t rows_de[ROWS][13] = {
    {'1', '2', '3', '4', '5', '6', '7', '8', '9', '0', 0x00DF, 0x00B4, 0},
    {'q', 'w', 'e', 'r', 't', 'z', 'u', 'i', 'o', 'p', 0x00FC, '+', 0},
    {'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', 0x00F6, 0x00E4, '#', 0},
    {'y', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '-', 0, 0, 0},
};
// Shifted characters on the number row
static const uint32_t shifted_digits_us[10] = {'!', '@', '#', '$', '%', '^', '&', '*', '(', ')'};
static const uint32_t shifted_digits_de[10] = {'!', '"', 0x00A7, '$', '%', '&', '/', '(', ')', '='};

static uint32_t lower(uint32_t cp) {
    if(cp >= 'A' && cp <= 'Z') return cp + 32;
    if(cp == 0x00C4 || cp == 0x00D6 || cp == 0x00DC) return cp + 0x20; // Ä Ö Ü
    return cp;
}
static uint32_t upper(uint32_t cp) {
    if(cp >= 'a' && cp <= 'z') return cp - 32;
    if(cp == 0x00E4 || cp == 0x00F6 || cp == 0x00FC) return cp - 0x20; // ä ö ü
    return cp;
}
static bool is_upper(uint32_t cp) {
    return (cp >= 'A' && cp <= 'Z') || cp == 0x00C4 || cp == 0x00D6 || cp == 0x00DC;
}
static bool is_letter(uint32_t cp) {
    cp = lower(cp);
    return (cp >= 'a' && cp <= 'z') || cp == 0x00E4 || cp == 0x00F6 || cp == 0x00FC ||
           cp == 0x00DF;
}

// Row/column of the key that types cp (shift ignored). False if unknown.
static bool key_pos(const Humanizer* h, uint32_t cp, int* row, int* col) {
    const uint32_t(*rows)[13] = h->german ? rows_de : rows_us;
    const uint32_t* shifted = h->german ? shifted_digits_de : shifted_digits_us;
    uint32_t c = lower(cp);
    for(int r = 0; r < ROWS; r++) {
        for(int k = 0; k < 13 && rows[r][k]; k++) {
            if(rows[r][k] == c) {
                *row = r;
                *col = k;
                return true;
            }
        }
    }
    for(int k = 0; k < 10; k++) {
        if(shifted[k] == cp) {
            *row = 0;
            *col = k;
            return true;
        }
    }
    return false;
}

// Touch typing: which finger presses a column (0-3 left pinky..index, 4-7 right index..pinky)
static int finger(int col) {
    static const int f[13] = {0, 1, 2, 3, 3, 4, 4, 5, 6, 7, 7, 7, 7};
    return f[col];
}

static bool needs_shift(const Humanizer* h, uint32_t cp) {
    if(is_upper(cp)) return true;
    const char* s = h->german ? "!\"$%&/()=?`*'>;:_" : "~!@#$%^&*()_+{}|:\"<>?";
    for(const char* p = s; *p; p++)
        if((uint32_t)(unsigned char)*p == cp) return true;
    return h->german && (cp == 0x00A7 || cp == 0x00B0); // § °
}

static bool needs_altgr(const Humanizer* h, uint32_t cp) {
    if(!h->german) return false;
    const char* s = "@{[]}\\~|";
    for(const char* p = s; *p; p++)
        if((uint32_t)(unsigned char)*p == cp) return true;
    return cp == 0x20AC || cp == 0x00B2 || cp == 0x00B3 || cp == 0x00B5; // € ² ³ µ
}

static bool is_space(uint32_t cp) {
    return cp == ' ' || cp == '\n' || cp == '\r' || cp == '\t';
}

// ---------------------------------------------------------------- API

void humanize_init(Humanizer* h, HumanizeRandom random, void* random_context) {
    h->wpm = 55;
    h->typos = false;
    h->german = false;
    h->random = random;
    h->random_context = random_context;
    humanize_reset(h);
}

void humanize_reset(Humanizer* h) {
    h->prev = 0;
    h->word_pace = 1.0f;
}

uint32_t humanize_gap_ms(Humanizer* h, uint32_t cp) {
    uint32_t prev = h->prev;
    float wpm = h->wpm < 5 ? 5.0f : (float)h->wpm;
    float avg = 12000.0f / wpm; // ms per character at the target speed

    float g = lognormal(h, avg * BASE_SHARE, GAP_SIGMA);
    // Pauses shrink in proportion for faster typists
    float pause = powf(55.0f / wpm, 1.0f);

    // Start of a word: new pace for this word, a small hesitation, sometimes a real pause
    bool word_start = !is_space(cp) && (prev == 0 || is_space(prev));
    if(word_start) {
        h->word_pace = 0.85f + 0.33f * uniform(h);
        g *= 1.3f;
        float u = uniform(h);
        if(u < 0.025f) g += (800.0f + 1700.0f * uniform(h)) * pause; // "thinking"
    }
    g *= h->word_pace;

    // Keyboard geometry
    int pr, pc, cr, cc;
    if(prev && key_pos(h, prev, &pr, &pc) && key_pos(h, cp, &cr, &cc)) {
        if(lower(prev) == lower(cp)) {
            g *= 0.72f; // double letter
        } else if(finger(pc) == finger(cc)) {
            g *= 1.25f; // same finger, different key
        } else if((finger(pc) < 4) != (finger(cc) < 4)) {
            g *= 0.86f; // other hand was ready
        }
    }

    // Effort
    if(needs_altgr(h, cp)) {
        g = g * 1.2f + (120.0f + 120.0f * uniform(h)) * pause;
    } else if(needs_shift(h, cp)) {
        g += (needs_shift(h, prev) ? 15.0f : 50.0f + 70.0f * uniform(h)) * pause;
        if(!is_letter(cp)) g *= 1.3f;
    } else if(cp >= '0' && cp <= '9') {
        g *= 1.35f;
    } else if(cp == ' ') {
        g *= 0.85f; // thumb
    } else if(!is_letter(cp) && !is_space(cp)) {
        g *= 1.3f;
    }

    // Pauses after punctuation and line breaks
    if(prev == ',' || prev == ';' || prev == ':') g += (120.0f + 330.0f * uniform(h)) * pause;
    if(prev == '.' || prev == '!' || prev == '?') g += (350.0f + 900.0f * uniform(h)) * pause;
    if(prev == '\n') g += (600.0f + 1400.0f * uniform(h)) * pause;
    if(cp == '\n') g *= 1.3f;

    if(g < 30.0f) g = 30.0f;
    if(g > 8000.0f) g = 8000.0f;
    h->prev = cp;
    return (uint32_t)g;
}

uint32_t humanize_hold_ms(Humanizer* h, uint32_t gap_ms) {
    float wpm = h->wpm < 5 ? 5.0f : (float)h->wpm;
    float t = lognormal(h, 95.0f * powf(55.0f / wpm, 0.25f), 0.22f);
    // Fast typists let go sooner; don't hold longer than the rhythm allows
    float cap = 0.8f * (float)gap_ms;
    if(t > cap) t = cap;
    if(t < 35.0f) t = 35.0f;
    if(t > 220.0f) t = 220.0f;
    return (uint32_t)t;
}

bool humanize_typo(Humanizer* h, uint32_t cp, uint32_t* wrong, uint8_t* late) {
    if(!h->typos || !is_letter(cp) || cp == 0x00DF) return false;
    float wpm = (float)h->wpm;
    float p = 0.012f + 0.00015f * wpm; // ~2% at 55 WPM, more when typing fast
    if(uniform(h) >= p) return false;

    int r, c;
    if(!key_pos(h, cp, &r, &c) || r == 0) return false;
    const uint32_t(*rows)[13] = h->german ? rows_de : rows_us;

    // Neighbouring letter keys: left/right, and above/below (rows are staggered)
    uint32_t cand[6];
    int n = 0;
    const int dr[6] = {0, 0, -1, -1, 1, 1};
    const int dc[6] = {-1, 1, 0, 1, -1, 0};
    for(int i = 0; i < 6; i++) {
        int rr = r + dr[i], ccol = c + dc[i];
        if(rr < 1 || rr >= ROWS || ccol < 0 || ccol >= 13) continue;
        uint32_t k = rows[rr][ccol];
        if(k && is_letter(k) && k != 0x00DF) cand[n++] = k;
    }
    if(!n) return false;
    uint32_t w = cand[(int)(uniform(h) * n) % n];
    *wrong = is_upper(cp) ? upper(w) : w;

    float u = uniform(h);
    *late = u < 0.6f ? 0 : (u < 0.9f ? 1 : 2);
    return true;
}

uint32_t humanize_notice_ms(Humanizer* h) {
    return (uint32_t)(250.0f + 500.0f * uniform(h));
}

uint32_t humanize_backspace_gap_ms(Humanizer* h) {
    float t = lognormal(h, 120.0f, 0.25f);
    if(t < 60.0f) t = 60.0f;
    if(t > 300.0f) t = 300.0f;
    return (uint32_t)t;
}
