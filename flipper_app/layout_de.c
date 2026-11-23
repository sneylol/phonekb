#include "layout_de.h"

#include <stddef.h>

#define SHIFT LAYOUT_DE_SHIFT
#define ALTGR LAYOUT_DE_ALTGR

// HID usages (position on the keyboard, named after the US key there)
#define K_A         0x04
#define K_1         0x1E
#define K_0         0x27
#define K_ENTER     0x28
#define K_BACKSPACE 0x2A
#define K_TAB       0x2B
#define K_SPACE     0x2C
#define K_MINUS     0x2D // DE: ß ? backslash
#define K_EQUAL     0x2E // DE: ´ `  (dead)
#define K_LBRACKET  0x2F // DE: ü Ü
#define K_RBRACKET  0x30 // DE: + * ~
#define K_BACKSLASH 0x31 // DE: # '
#define K_SEMICOLON 0x33 // DE: ö Ö
#define K_QUOTE     0x34 // DE: ä Ä
#define K_GRAVE     0x35 // DE: ^ (dead) °
#define K_COMMA     0x36 // DE: , ;
#define K_DOT       0x37 // DE: . :
#define K_SLASH     0x38 // DE: - _
#define K_NONUS_BS  0x64 // DE: < > |  (key left of Y on ISO keyboards)

#define LETTER(c) (K_A + ((c) - 'a'))
#define DIGIT(n)  ((n) == 0 ? K_0 : K_1 + (n) - 1)

typedef struct {
    uint32_t cp;
    uint16_t key;
    bool dead;
} DeKey;

// Everything that isn't a plain letter or digit
static const DeKey de_keys[] = {
    {' ', K_SPACE, false},
    {'\n', K_ENTER, false},
    {'\r', K_ENTER, false},
    {'\t', K_TAB, false},
    {'\b', K_BACKSPACE, false},

    // Number row with Shift
    {'!', DIGIT(1) | SHIFT, false},
    {'"', DIGIT(2) | SHIFT, false},
    {0x00A7, DIGIT(3) | SHIFT, false}, // §
    {'$', DIGIT(4) | SHIFT, false},
    {'%', DIGIT(5) | SHIFT, false},
    {'&', DIGIT(6) | SHIFT, false},
    {'/', DIGIT(7) | SHIFT, false},
    {'(', DIGIT(8) | SHIFT, false},
    {')', DIGIT(9) | SHIFT, false},
    {'=', DIGIT(0) | SHIFT, false},

    // Number row with AltGr
    {0x00B2, DIGIT(2) | ALTGR, false}, // ²
    {0x00B3, DIGIT(3) | ALTGR, false}, // ³
    {'{', DIGIT(7) | ALTGR, false},
    {'[', DIGIT(8) | ALTGR, false},
    {']', DIGIT(9) | ALTGR, false},
    {'}', DIGIT(0) | ALTGR, false},

    {0x00DF, K_MINUS, false}, // ß
    {'?', K_MINUS | SHIFT, false},
    {'\\', K_MINUS | ALTGR, false},

    {0x00B4, K_EQUAL, true}, // ´ (dead)
    {'`', K_EQUAL | SHIFT, true}, // ` (dead)

    {0x00FC, K_LBRACKET, false}, // ü
    {0x00DC, K_LBRACKET | SHIFT, false}, // Ü

    {'+', K_RBRACKET, false},
    {'*', K_RBRACKET | SHIFT, false},
    {'~', K_RBRACKET | ALTGR, false},

    {'#', K_BACKSLASH, false},
    {'\'', K_BACKSLASH | SHIFT, false},

    {0x00F6, K_SEMICOLON, false}, // ö
    {0x00D6, K_SEMICOLON | SHIFT, false}, // Ö
    {0x00E4, K_QUOTE, false}, // ä
    {0x00C4, K_QUOTE | SHIFT, false}, // Ä

    {'^', K_GRAVE, true}, // ^ (dead)
    {0x00B0, K_GRAVE | SHIFT, false}, // °

    {',', K_COMMA, false},
    {';', K_COMMA | SHIFT, false},
    {'.', K_DOT, false},
    {':', K_DOT | SHIFT, false},
    {'-', K_SLASH, false},
    {'_', K_SLASH | SHIFT, false},

    {'<', K_NONUS_BS, false},
    {'>', K_NONUS_BS | SHIFT, false},
    {'|', K_NONUS_BS | ALTGR, false},

    // AltGr letters
    {'@', LETTER('q') | ALTGR, false},
    {0x20AC, LETTER('e') | ALTGR, false}, // €
    {0x00B5, LETTER('m') | ALTGR, false}, // µ
};

// QWERTZ: Y and Z swap places
static uint16_t de_letter(char lower) {
    if(lower == 'y') return LETTER('z');
    if(lower == 'z') return LETTER('y');
    return LETTER(lower);
}

bool layout_de_lookup(uint32_t cp, uint16_t* key, bool* dead) {
    *dead = false;
    if(cp >= 'a' && cp <= 'z') {
        *key = de_letter((char)cp);
        return true;
    }
    if(cp >= 'A' && cp <= 'Z') {
        *key = de_letter((char)(cp - 'A' + 'a')) | SHIFT;
        return true;
    }
    if(cp >= '0' && cp <= '9') {
        *key = DIGIT(cp - '0');
        return true;
    }
    for(size_t i = 0; i < sizeof(de_keys) / sizeof(de_keys[0]); i++) {
        if(de_keys[i].cp == cp) {
            *key = de_keys[i].key;
            *dead = de_keys[i].dead;
            return true;
        }
    }
    return false;
}
