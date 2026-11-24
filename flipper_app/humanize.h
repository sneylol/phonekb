#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Human-like typing timing.
 *
 * Produces, per character, the time since the previous key press ("gap") and
 * how long the key is held down, following how people actually type:
 *  - gaps vary on a right-skewed (log-normal) curve, with a per-word pace
 *  - keyboard geometry: alternating hands is fast, the same finger twice is
 *    slow, double letters are fast (real QWERTZ / QWERTY key positions)
 *  - capitals (Shift), digits, symbols and AltGr characters take longer
 *  - pauses after , ; : and longer ones after . ! ? and new lines,
 *    a hesitation at the start of words and occasional "thinking" pauses
 *  - optional typos: a neighbouring key, noticed at once or 1-2 letters
 *    later, then fixed with Backspace
 *  - optional thinking pauses: every few sentences (more likely the longer
 *    it has been), before new paragraphs, and now and then after a comma
 *
 * Pure C (no Flipper APIs) so it can be tested on a computer.
 */

typedef uint32_t (*HumanizeRandom)(void* context);

typedef struct {
    // settings
    uint16_t wpm; // target speed, words (5 characters) per minute
    bool typos;
    bool german; // QWERTZ key positions (else QWERTY)
    uint8_t think; // thinking pauses: 0 off, 1 light, 2 normal, 3 heavy

    // randomness
    HumanizeRandom random;
    void* random_context;

    // state
    uint32_t prev; // previous character typed (0 = none)
    float word_pace; // pace multiplier for the current word
    // sentence / paragraph tracking for thinking pauses
    bool had_text; // something has been typed since the last reset
    bool sentence_end; // . ! ? typed, waiting to see whether a new sentence starts
    bool clause_end; // , ; : typed
    uint8_t newlines; // line breaks in a row
    uint8_t sentences_since_pause;
    uint16_t sentence_chars; // characters in the current sentence
    uint16_t line_chars; // characters in the current line
    uint8_t burst; // words left to type a bit faster after a long pause
} Humanizer;

void humanize_init(Humanizer* h, HumanizeRandom random, void* random_context);

/** Forget the previous character (after Stop, a new text, etc.). */
void humanize_reset(Humanizer* h);

/**
 * Extra "thinking" pause before this character (ms, usually 0). Call before
 * humanize_gap_ms for every character typed.
 */
uint32_t humanize_think_ms(Humanizer* h, uint32_t cp);

/** Time from the previous key press to pressing this character (ms). Updates state. */
uint32_t humanize_gap_ms(Humanizer* h, uint32_t cp);

/** How long to hold the key down (ms), given the gap just used for it. */
uint32_t humanize_hold_ms(Humanizer* h, uint32_t gap_ms);

/**
 * Decide whether to make a typo on this character. If so, *wrong is the
 * neighbouring key typed instead and *late is how many following characters
 * get typed before the mistake is noticed (0-2).
 */
bool humanize_typo(Humanizer* h, uint32_t cp, uint32_t* wrong, uint8_t* late);

/** Pause between noticing a typo and starting to fix it (ms). */
uint32_t humanize_notice_ms(Humanizer* h);

/** Gap between Backspace presses while fixing a typo (ms). */
uint32_t humanize_backspace_gap_ms(Humanizer* h);

#ifdef __cplusplus
}
#endif
