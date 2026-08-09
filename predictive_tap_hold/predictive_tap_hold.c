// Copyright 2025 Joschua Gandert (@jgandert)
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "predictive_tap_hold.h"

#ifndef PTH_DISABLED
    #ifdef NO_ACTION_TAPPING
    #    error "This feature requires action tapping. Please update your config.h"
    #endif

    #if !defined(TAPPING_TERM) && !defined(TAPPING_TERM_PER_KEY)
    #    error "Predictive Tap-Hold requires TAPPING_TERM = 0 or TAPPING_TERM_PER_KEY to be defined. Please update your config.h"
    #elif defined(TAPPING_TERM) && !defined(TAPPING_TERM_PER_KEY) && TAPPING_TERM != 0
    #    error "Predictive Tap-Hold requires TAPPING_TERM = 0 or TAPPING_TERM_PER_KEY to be defined. Please update your config.h"
    #endif
#endif /* ifndef PTH_DISABLED */
/*
Here is the structure of a record for reference:

keyrecord_t record {
    keyevent_t event {
        keypos_t key { // Position in matrix
            uint8_t col;
            uint8_t row;
        }
        bool     pressed; // True if pressed, false if released
        uint16_t time;    // Timestamp of the event
    }

#ifndef NO_ACTION_TAPPING
    tap_t tap {
        bool    interrupted : 1; // If another key was pressed during tap-hold sequence
        bool    reserved2 : 1;
        bool    reserved1 : 1;
        bool    reserved0 : 1;
        uint8_t count : 4; // 0 for hold, 1+ for tap
    }
#endif
#if defined(COMBO_ENABLE) || defined(REPEAT_KEY_ENABLE)
    uint16_t keycode;
#endif
}
*/

// Standard C library equivalents often used in QMK code
#ifndef MIN
#    define MIN(x, y) (((x) < (y)) ? (x) : (y))
#endif
#ifndef MAX
#    define MAX(x, y) (((x) > (y)) ? (x) : (y))
#endif
#ifndef ABS
#    define ABS(x) (((x) < 0) ? -(x) : (x))
#endif

// Safe division macro (avoids division by zero)
// This was also used in the genetic algorithm (symbolic regression).
#ifndef SD
#    define SD(x, y) (((y) == 0) ? (x) : ((x) / (y)))
#endif

#define ATOM_SIDE_TO_STR(side) (((side) == PTH_ATOM_LEFT) ? "L" : ((side) == PTH_ATOM_RIGHT) ? "R" : ((side) == PTH_ATOM_OPPOSITE) ? "O" : ((side) == PTH_ATOM_SAME) ? "S" : "?")

// Never use more than one get_keycode_string call in a macro. Each call uses
// the same buffer, so in the end you'd get the same result for both calls.
#if defined(PTH_DEBUG) && defined(CONSOLE_ENABLE)
#    define PTH_LOG(first_arg) print("PTH: " first_arg "\n");
#    define PTH_LOGF(fmt, first_arg, ...) uprintf("PTH: " fmt "\n", first_arg, ##__VA_ARGS__);

/**
 * @brief Converts a full 8-bit encoded side value into a human-readable string.
 *
 * The format is "PO" if the upper 4 bits (user_bits) are zero.
 * Otherwise, the format is "PO+X", where:
 * - 'P' is the character for the path atom's side.
 * - 'O' is the character for the other atom's side.
 * - 'X' is the decimal value of the user_bits.
 *
 * @note This function uses a static buffer and is not thread-safe.
 *       The returned pointer is only valid until the next call to this function.
 *
 * @param full_side The 8-bit encoded value.
 * @return A constant character pointer to the formatted string.
 */
static const char* side_to_str(uint8_t full_side) {
    // A static buffer is used to store the result. It is reused on each call.
    static char buffer[8]; // Sufficient for "OS+15\0"

    // Extract the string representation for the path atom side (bits 2-3)
    const char* pth_side_str = ATOM_SIDE_TO_STR(PTH_GET_PTH_ATOM_SIDE(full_side));

    // Extract the string representation for the other atom side (bits 0-1)
    const char* other_side_str = ATOM_SIDE_TO_STR(PTH_GET_OTHER_ATOM_SIDE(full_side));

    // Extract the value of the user bits (bits 4-7)
    uint8_t user_bits = (full_side >> 4) & 0b1111;

    // Conditionally format the string based on the value of user_bits
    if (user_bits == 0) {
        // If user_bits is 0, omit the "+<number>" part
        sprintf(buffer, "%s%s", pth_side_str, other_side_str);
    } else {
        // If user_bits is not 0, include it in the output
        sprintf(buffer, "%s%s+%u", pth_side_str, other_side_str, user_bits);
    }

    return buffer;
}

#else
#    define PTH_LOG(...) ((void)0);
#    define PTH_LOGF(...) ((void)0);
#    define side_to_str(full_side) ("")
#endif // PTH_DEBUG & CONSOLE_ENABLE

#define STATUS_TO_STR(status) (((status) == PTH_IDLE) ? "IDLE" : ((status) == PTH_PRESSED) ? "PRESSED" : ((status) == PTH_SECOND_PRESSED) ? "SECOND_PRESSED" : ((status) == PTH_DECIDED_TAP) ? "DECIDED_TAP" : ((status) == PTH_DECIDED_HOLD) ? "DECIDED_HOLD" : "UNKNOWN")

// Used for turning tap-hold releases into taps after the actual tap-hold key
// has been released. For example, if LCTL_T(KC_E) is pressed, C_S_T(KC_A) is
// pressed, and LCTL_T(KC_E) is released, then we are already back to the IDLE
// state (i.e. we are ready for the next PTH) and so we still need some way of
// knowing how previously pressed tap-hold keys have to be send (tap or hold).
// Hold is the default, so we only need to store those that should be tap.
//
// INFO:
// * This must not be larger than 8 because the bitmask is an uint8_t.
// * If you use values less than 8, you'll have to mask off bits.
#define RELEASE_AS_TAP_POSITIONS_SIZE 8

// This is the size of the release record array.
// Release records that don't fit will be directly released.
//
// INFO:
// * This must not be larger than 8 because the bitmask is an uint8_t.
// * If you use values less than 8, you'll have to mask off bits.
#define RELEASE_RECORD_SIZE 8

// Maximum duration (ms) considered valid for timers and prediction heuristics.
// Durations longer than this will be essentially capped, as the task
// function will mark timers running longer than this as "maxed out".
//
// This should be in sync with the one used in training and must be < 32,767!
// More is not possible because 16-bit timers are used, which wrap around.
#define MS_MAX_DUR_FOR_TIMERS 4096

// Sentinel value for an empty key position
#define EMPTY_KEYPOS (keypos_t){.col = 0xFF, .row = 0xFF}

static inline bool keypos_eq(keypos_t p1, keypos_t p2) {
    return p1.col == p2.col && p1.row == p2.row;
}

// Static variables for state tracking
// ----------------------------------------------------------------------------
static pth_status_t pth_status      = PTH_IDLE;
static pth_status_t pth_prev_status = PTH_IDLE;

static uint16_t    pth_keycode                  = KC_NO;
static uint16_t    pth_tap_code_instead_of_hold = KC_NO;
static keyrecord_t pth_record                   = {.event = {.key = EMPTY_KEYPOS}};
static uint16_t    pth_press_timer              = 0;
static bool        pth_press_timer_max_reached  = false;
static uint8_t     pth_atomic_side              = 0;
static uint8_t     pth_side_user_bits           = 0;

static bool    pth_was_held_instantly         = false;
static bool    second_was_held_instantly      = false;
static bool    instant_layer_was_active       = false;
static uint8_t layer_before_instant_layer_tap = 0;

static bool        has_second                     = false;
static keyrecord_t second_record                  = {.event = {.key = EMPTY_KEYPOS}};
static uint16_t    second_keycode                 = KC_NO;
static uint16_t    second_press_timer             = 0;
static bool        second_press_timer_max_reached = false;
static bool        second_is_tap_hold             = false;
static bool        second_is_same_side_as_pth     = false;
static bool        second_to_be_released          = false;

static int16_t timeout_for_forcing_choice       = 0;
static bool    has_chosen_after_timeout_reached = false;

static uint16_t min_overlap_dur_for_hold = 0;

// -- State captured specifically for prediction --
static uint16_t pth_press_to_second_press_dur           = 0;
static uint16_t pth_press_to_second_release_dur         = 0;
static uint16_t pth_second_dur                          = 0;
static uint16_t pth_second_press_to_third_press_dur     = 0;
static int16_t  pth_prev_prev_press_to_prev_press_dur   = -1;
static int16_t  pth_prev_press_to_pth_press_dur         = -1;
static int16_t  pth_prev_prev_overlap_dur               = -1;
static int16_t  pth_prev_overlap_dur                    = -1;
static float    pth_press_to_press_w_avg                = 0;
static float    pth_overlap_w_avg                       = 0;
static uint16_t key_release_before_pth_to_pth_press_dur = 0;

// -- State about previous presses and releases --
static uint16_t prev_press_keycode               = KC_NO;
static uint16_t cur_press_keycode                = KC_NO;
static uint8_t  down_count                       = 0;
static uint16_t overlap_timer                    = 0;
static bool     overlap_timer_max_reached        = false;
static uint16_t press_to_press_timer             = 0;
static bool     press_to_press_timer_max_reached = false;
static uint16_t release_timer                    = 0;
static bool     release_timer_max_reached        = false;
static int16_t  prev_press_to_press_dur          = -1;
static int16_t  cur_press_to_press_dur           = -1;
static int16_t  prev_overlap_dur                 = -1;
static int16_t  cur_overlap_dur                  = -1;

// -- Tracks tap-hold key *positions* that should resolve as HOLD on release --
static keypos_t release_as_tap_positions[RELEASE_AS_TAP_POSITIONS_SIZE];
static uint8_t  used_release_as_tap_positions_bitmask = 0;

// -- Tracks releases of keys after PTH and before a third has been pressed. --
static keyrecord_t release_records[RELEASE_RECORD_SIZE];

// If the bit at index 'i' is 1, it means release_records[i] was released
// before the second key. If the bit is 0 instead, it was release after second.
static uint8_t is_before_second_bitmask     = 0;
static uint8_t used_release_records_bitmask = 0;

// -- Recursion guard --
static bool is_processing_record_due_to_pth = false;

// Reset and initialization
// ----------------------------------------------------------------------------

static void reset_pth_state(void) {
    // We don't need to reset timers, as the issue would be using a timer
    // before it can be used, and if we do that we have bigger issues than
    // using a timer with an invalid value. Same applies to side and others.
    pth_prev_status              = pth_status;
    pth_status                   = PTH_IDLE;
    pth_keycode                  = KC_NO;
    pth_tap_code_instead_of_hold = KC_NO;
    pth_record.event.key         = EMPTY_KEYPOS;
    pth_press_timer_max_reached  = false;

    pth_was_held_instantly         = false;
    second_was_held_instantly      = false;
    instant_layer_was_active       = false;
    layer_before_instant_layer_tap = 0;

    has_second                     = false;
    second_record.event.key        = EMPTY_KEYPOS;
    second_keycode                 = KC_NO;
    second_press_timer_max_reached = false;
    second_is_tap_hold             = false;
    second_to_be_released          = false;

    has_chosen_after_timeout_reached = false;

    min_overlap_dur_for_hold = 0;

    PTH_LOG("--------------------------------------------------------------------------------");
}

void keyboard_post_init_predictive_tap_hold(void) {
#ifdef PTH_DISABLED
    uprintf("Predictive_tap_hold is disabled");
    return;
#endif
    // since we have no data yet, just use one far in the past
    press_to_press_timer = timer_read() - MS_MAX_DUR_FOR_TIMERS;
    release_timer        = timer_read() - (MS_MAX_DUR_FOR_TIMERS - 100);
}

static uint16_t get_keycode_same_pos_in_layer(keyrecord_t* record, uint8_t layer) {
    // thanks u/pgetreuer for the suggestion
    const keypos_t pos = record->event.key;

#ifdef VIAL_ENABLE
    return dynamic_keymap_get_keycode(layer, pos.row, pos.col);
#else
    return keycode_at_keymap_location(layer, pos.row, pos.col);
#endif
}

// Utility functions
// ----------------------------------------------------------------------------
// 0b10000: the "right" flag in 5-bit mods
#define MODS_5_BIT_RL_BIT 0x10

static inline uint8_t convert_5_bit_mods_to_8_bit(uint8_t mods_5_bit) {
    if ((mods_5_bit & MODS_5_BIT_RL_BIT) == 0) {
        // left mods are same in 5 and 8-bit
        return mods_5_bit;
    }

    // disable RL bit, then shift result by 4 bits
    return (uint8_t)((mods_5_bit & ~MODS_5_BIT_RL_BIT) << 4);
}

static uint8_t get_5_bit_mods_of_mod_tap(uint16_t keycode) {
    // mod_config modifies the mod if certain settings are active (such as swapping keys)
    return mod_config(QK_MOD_TAP_GET_MODS(keycode));
}

/**
 * @brief Sets the bit at `index` in `bitmask` to `is_set`.
 */
static inline uint8_t change_bit(uint8_t bitmask, uint8_t index, bool is_set) {
    return (bitmask & ~(1U << index)) | (is_set << index);
}

static inline uint8_t clear_bit(uint8_t bitmask, uint8_t index) {
    return bitmask & ~(1U << index);
}

static inline uint8_t set_bit(uint8_t bitmask, uint8_t index) {
    return bitmask | (1U << index);
}

/**
 * @brief Determines if two keys are on the same side.
 *
 * Each side configuration determines how the key behaves if it is the PTH key,
 * and how it behaves if it is any other key. So, first we extract the
 * respective atomic side from each configuration.
 *
 * The rules are:
 * 1. If other is opposite, return false (not considered the same side).
 * 2. If other is same, return true;
 * 3. If PTH is opposite, return false.
 * 4. If PTH is same, return true;
 * 5. If both sides are equal, return true, false otherwise.
 *
 * @param other_atomic_side one of PTH_ATOM_LEFT, PTH_ATOM_RIGHT,
 *                          PTH_ATOM_OPPOSITE, PTH_ATOM_SAME
 * @return true if the resulting sides are considered the same, false otherwise.
 */
static bool is_same_side_as_pth(uint8_t other_atomic_side) {
    // Combine the two 2-bit atomic sides to form a 4-bit index (0-15).
    uint8_t index = (pth_atomic_side << 2) | other_atomic_side;

    // LUT containing the results for all 16 possible side comparisons.
    const uint16_t truth_table = 0b1011100010101001;

    // Look up the result in the table.
    return (truth_table >> index) & 1;
}

// Convenience function
static inline bool is_record_same_side_as_pth(keyrecord_t* record) {
    uint8_t other_side = PTH_GET_OTHER_ATOM_SIDE(pth_get_side(record));
    return is_same_side_as_pth(other_side);
}

// Public functions
// ----------------------------------------------------------------------------
pth_status_t pth_get_status(void) {
    return pth_status;
}

pth_status_t pth_get_prev_status(void) {
    return pth_prev_status;
}

int16_t pth_get_prev_press_to_pth_press_dur(void) {
    return pth_prev_press_to_pth_press_dur;
}

uint8_t pth_get_pth_atomic_side(void) {
    return pth_atomic_side;
}

uint8_t pth_get_pth_side_user_bits(void) {
    return pth_side_user_bits;
}

bool pth_is_second_same_side_as_pth(void) {
    return second_is_same_side_as_pth;
}

keyrecord_t pth_get_pth_record(void) {
    return pth_record;
}

keyrecord_t pth_get_second_record(void) {
    return second_record;
}

uint16_t pth_get_pth_keycode(void) {
    return pth_keycode;
}

uint16_t pth_get_second_keycode(void) {
    return second_keycode;
}

uint16_t pth_get_prev_press_keycode(void) {
    return prev_press_keycode;
}

uint16_t pth_get_second_keycode_on_same_layer_as_pth(void) {
    if (!pth_was_held_instantly || !IS_QK_LAYER_TAP(pth_keycode)) {
        return KC_NO;
    }
    return get_keycode_same_pos_in_layer(&second_record, layer_before_instant_layer_tap);
}

bool pth_is_second_tap_hold(void) {
    return second_is_tap_hold;
}

bool pth_is_processing_internal(void) {
    return is_processing_record_due_to_pth;
}

bool pth_is_tap_hold_keycode(uint16_t keycode) {
    switch (keycode) {
        case QK_MOD_TAP ... QK_MOD_TAP_MAX:
        case QK_LAYER_TAP ... QK_LAYER_TAP_MAX:
            return true;
        case QK_SWAP_HANDS ... QK_SWAP_HANDS_MAX:
            return !IS_SWAP_HANDS_KEYCODE(keycode);
    }
    return false;
}

uint8_t pth_get_all_8_bit_mods(void) {
    // TODO: Use weak_mods or not?
#ifdef NO_ACTION_ONESHOT
    return get_mods(); //| get_weak_mods();
#else
    return get_mods() | get_oneshot_mods(); //| get_weak_mods();
#endif // NO_ACTION_ONESHOT
}

uint8_t pth_get_8_bit_mods_of_mod_tap(uint16_t keycode) {
    return convert_5_bit_mods_to_8_bit(get_5_bit_mods_of_mod_tap(keycode));
}

bool pth_is_mod_tap_with_any_mods_of(uint16_t keycode, uint8_t mods_8_bit) {
    return IS_QK_MOD_TAP(keycode) && (pth_get_8_bit_mods_of_mod_tap(keycode) & mods_8_bit) != 0;
}

bool pth_default_should_hold_instantly(uint16_t keycode, keyrecord_t* record) {
#ifdef PTH_DONT_HOLD_INSTANTLY
        return false;
#endif

#ifdef CAPS_WORD_ENABLE
    // Instantly holding will result in a held tap-hold key being processed,
    // thus breaking caps words.
    if (is_caps_word_on()) {
        return false;
    }
#endif

    // It seems there are only downsides to instant holding GUI.
    const uint8_t active_mods_or_gui = pth_get_all_8_bit_mods() | MOD_MASK_GUI;
    if (pth_is_mod_tap_with_any_mods_of(keycode, active_mods_or_gui)) {
        // PTH is a mod-tap containing mods that are already active (or GUI).
        //
        // This is a workaround for QMK's behavior of reporting a modifier
        // release to the OS, if a key with multiple modifiers is released,
        // even if another key with one of those modifiers is still down.
        //
        // Example: KC_LSFT down, LCS_T(KC_E) down -> Shift and Ctrl are down,
        // ... (other presses), LCS_T(KC_E) up, KC_T down. When LCS_T(KC_E) was
        // released, QMK reported left Shift as released, even though KC_LSFT
        // is still pressed. The result is that KC_T is not uppercased.
        //
        // Of course, this workaround only helps with the instant hold, so if
        // hold is chosen, this still happens, but that's probably okay.
        //
        // A full workaround in your own keymap is to use the right version on
        // bare modifier keys (e.g. KC_RSFT), and the left version of modifiers
        // on all tap-hold keys (e.g. LCS_T(KC_A)) or vice versa. It works
        // because modifiers of different sides don't affect each other. If a
        // left mod is down, and then a right mod is up, the release of the
        // left mod will still be reported to the OS when it happens.
        return false;
    }

    return true;
}

extern const uint8_t pth_side_layout[MATRIX_ROWS][MATRIX_COLS] PROGMEM;

uint8_t pth_get_side_from_layout(keypos_t pos) {
    return (uint8_t)pgm_read_byte(&pth_side_layout[pos.row][pos.col]);
}

// Weakly defined functions
// ----------------------------------------------------------------------------
__attribute__((weak)) uint8_t pth_get_side(keyrecord_t* record) {
    return pth_get_side_from_layout(record->event.key);
}

#ifdef PTH_FAST_STREAK_TAP_ENABLE
__attribute__((weak)) bool pth_is_fast_streak_tap_key(uint16_t keycode) {
    if ((get_mods() & (MOD_MASK_CG | MOD_BIT_LALT)) != 0) {
        return false; // Disable when non-Shift (and non-right-Alt) mods are active.
    }

    switch (get_tap_keycode(keycode)) {
        case KC_A ... KC_Z:
        case KC_SPC:
        case KC_DOT:
        case KC_COMM:
        case KC_SCLN:
        case KC_SLSH:
            return true;
    }
    return false;
}

__attribute__((weak)) bool pth_predict_fast_streak_tap(void) {
    return (pth_is_fast_streak_tap_key(pth_keycode) && pth_is_fast_streak_tap_key(prev_press_keycode) && pth_prev_status != PTH_DECIDED_HOLD && pth_prev_press_to_pth_press_dur < 125);
}
#endif // PTH_FAST_STREAK_TAP_ENABLE

#ifndef PTH_DONT_HOLD_INSTANTLY
__attribute__((weak)) bool pth_should_hold_instantly(uint16_t keycode, keyrecord_t* record) {
    return pth_default_should_hold_instantly(keycode, record);
}

__attribute__((weak)) bool pth_second_should_hold_instantly(uint16_t second_keycode, keyrecord_t* second_record) {
    return pth_should_hold_instantly(second_keycode, second_record);
}
#endif // PTH_DONT_HOLD_INSTANTLY

__attribute__((weak)) bool pth_should_choose_tap_when_second_is_same_side_press(void) {
    // If this is a non-tap-hold same-side second, then that implies key roll.
    // We consider whether second is tap-hold, even if an instant layer tap is
    // active, so that it would be possible to use that, and then activate
    // a mod tap on that new layer.
    return !second_is_tap_hold;
}

__attribute__((weak)) bool pth_should_choose_tap_when_second_is_same_side_release(void) {
    // We haven't made a choice, the second key is on same side as PTH, and
    // the second key is released before a third is pressed.
    // It is highly likely that this is a key roll, so choose tap.
    PTH_LOG("  Same-side second key release implies roll.");
    return true;
}

__attribute__((weak)) int16_t pth_get_timeout_for_forcing_choice(void) {
    return 700;
}

__attribute__((weak)) pth_status_t pth_get_forced_choice_after_timeout(void) {
    if (has_second) {
        return PTH_IDLE;
    }
    return PTH_DECIDED_HOLD;
}

__attribute__((weak)) bool pth_should_neutralize_mods(uint8_t mod_5_bit) {
    // We want to neutralize mods (Alt and Gui) unless they include Shift and
    // Ctrl, because 1. they don't need to be neutralized, and 2. because doing
    // so for Ctrl leads to control characters being output in some consoles.
    // We use the left version, as we don't care whether it's left or right.
    return (mod_5_bit & (MOD_LCTL | MOD_LSFT)) == 0;
}

__attribute__((weak)) uint16_t pth_get_code_to_be_registered_instead_when_hold_chosen(void) {
    return KC_NO;
}

__attribute__((weak)) bool pth_should_register_as_hold_when_same_side(uint16_t keycode, keyrecord_t* record) {
    return true;
}

// Key handling
// ----------------------------------------------------------------------------
static void process_record_with_new_time(keyrecord_t* record) {
    record->event.time              = timer_read();
    is_processing_record_due_to_pth = true;
    process_record(record);
    is_processing_record_due_to_pth = false;
}

static void process_register_record(keyrecord_t* record) {
    record->event.pressed = true;
    process_record_with_new_time(record);
}

static void process_unregister_record(keyrecord_t* record) {
    record->event.pressed = false;
    process_record_with_new_time(record);
}

static inline void set_record_to_tap(keyrecord_t* record) {
    record->tap.interrupted = true;
    record->tap.count       = 1;
}

static inline void set_record_to_hold(keyrecord_t* record) {
    record->tap.count = 0;
}

static void process_register_record_as_hold(keyrecord_t* record) {
    set_record_to_hold(record);
    record->event.pressed = true;
    process_record_with_new_time(record);
}

static void process_unregister_record_as_hold(keyrecord_t* record) {
    set_record_to_hold(record);
    record->event.pressed = false;
    process_record_with_new_time(record);
}

static void process_register_record_as_tap(keyrecord_t* record) {
    set_record_to_tap(record);
    record->event.pressed = true;
    process_record_with_new_time(record);
}

static void process_unregister_record_as_tap(keyrecord_t* record) {
    set_record_to_tap(record);
    record->event.pressed = false;
    process_record_with_new_time(record);
}

// will only be called if PTH was not held instantly
static void register_pth_hold(void) {
    if (pth_tap_code_instead_of_hold == KC_NO) {
        process_register_record_as_hold(&pth_record);

        // Users must know that if the second was held instantly, but the PTH
        // was not, then we will not re-register the second, i.e. second will
        // be down before the PTH key, even though PTH was actually first.
        // As a result, second will not be affected by a PTH layer change.
        //
        // This has the benefit of allowing you to press an MT and an LT, which
        // are both on the same layer, in any order, given that you configure
        // pth_should_hold_instantly so that it returns false for the LT.
        if (has_second && !second_was_held_instantly && IS_QK_LAYER_TAP(pth_keycode)) {
            // If PTH was held instantly, then second is already from the right
            // layer. If second was held instantly, then it still is, so no
            // update is needed. Here, none of that is true and the PTH key is
            // an LT, so second is out of date, as the layer wasn't active yet.
            second_keycode     = get_keycode_same_pos_in_layer(&second_record, QK_LAYER_TAP_GET_LAYER(pth_keycode));
            second_is_tap_hold = pth_is_tap_hold_keycode(second_keycode);
        }
    } else {
        register_code16(pth_tap_code_instead_of_hold);
    }
}

static void unregister_pth_hold(void) {
    if (pth_tap_code_instead_of_hold == KC_NO) {
        process_unregister_record_as_hold(&pth_record);
    } else {
        unregister_code16(pth_tap_code_instead_of_hold);
    }
}

/*
 * We will call this in those situations where we will or can create
 * a tap that is so short that the OS might ignore it.
 */
static void send_and_wait(void) {
    send_keyboard_report();
#if TAP_CODE_DELAY > 0
    wait_ms(TAP_CODE_DELAY);
#endif
}

// Handling of tap-hold releases that should be taps
// ----------------------------------------------------------------------------
/**
 * @return true if the position existed in the array and was removed
 */
static bool remove_pos_from_tap_releases(keypos_t pos) {
    uint8_t to_check_bitmask = used_release_as_tap_positions_bitmask;
    while (to_check_bitmask != 0) {
        // This counts trailing zeros in an unsigned int. Note that it is
        // undefined for a value of 0. The result is also equal to the index of
        // the least significant set bit. (e.g. the index is 2 for 0101 0100)
        uint8_t lsb_set_index = __builtin_ctz(to_check_bitmask);

        if (keypos_eq(release_as_tap_positions[lsb_set_index], pos)) {
            used_release_as_tap_positions_bitmask = clear_bit(used_release_as_tap_positions_bitmask, lsb_set_index);
            return true;
        }

        // Clear the bit, so next iteration we get the next lowest.
        to_check_bitmask = clear_bit(to_check_bitmask, lsb_set_index);
    }

    return false;
}

static void add_pos_to_tap_releases(keypos_t pos) {
    // Never call __builtin_ctz(~x) directly, as ~x will be an int, so the result
    // will be larger than what you expect. For example:
    // 255 = 0b1111_1111
    // ~255 = 0b11111111111111111111111100000000
    // So the result of ctz(~255) will be 0b1000 = 8 instead of 0
    // To prevent that, store the result of ~x in an uint8_t variable.
    uint8_t empty_release_as_tap_positions_bitmask = ~used_release_as_tap_positions_bitmask;
    if (empty_release_as_tap_positions_bitmask == 0) {
        PTH_LOGF("  There was not enough space to store (%u, %u) in release_as_tap_positions.", pos.col, pos.row);
        return;
    }

    uint8_t lsb_set_index = __builtin_ctz(empty_release_as_tap_positions_bitmask);

    release_as_tap_positions[lsb_set_index] = pos;
    used_release_as_tap_positions_bitmask   = set_bit(used_release_as_tap_positions_bitmask, lsb_set_index);
}

// Handling of release records to preserve order of presses and releases
// ----------------------------------------------------------------------------
// Using an enum instead of boolean for cleaner code
typedef enum { AFTER_SECOND, BEFORE_SECOND } release_time_t;

static inline uint8_t get_to_be_released_bitmask(release_time_t release_time) {
    const bool is_before_second = release_time == BEFORE_SECOND;

    // -is_before_second = ~is_before_second + 1
    //
    // With is_before_second:
    //  true =        0000 0001,  false =        0000 0000
    // ~true =        1111 1110, ~false =        1111 1111
    // ~true + 1 =    1111 1111, ~false + 1 =    0000 0000
    // ~(~true + 1) = 0000 0000, ~(~false + 1) = 1111 1111
    //
    // So, if is_before_second is true, we get 0000 0000 for
    // ~(-is_before_second), and if it's false, we get 1111 1111.
    //
    // Let's say is_before_second_bitmask is 1001 0101.
    // Bits with 1 are for "before second" and 0 are for "after second".
    //
    // Now we XOR is_before_second_bitmask and the above result:
    // If is_before_second is true:  1001 0101 ^ 0000 0000 = 1001 0101
    // If is_before_second is false: 1001 0101 ^ 1111 1111 = 0110 1010
    //
    // We get a bitmask where only those bits are set that match the current
    // release time.
    uint8_t same_release_time_bitmask = is_before_second_bitmask ^ ~(-is_before_second);

    // Finally, we have to check if we actually have a record for those bits.
    return used_release_records_bitmask & same_release_time_bitmask;
}

static bool process_release_records(release_time_t release_time, bool wait_before_first) {
    uint8_t to_be_released_bitmask = get_to_be_released_bitmask(release_time);

    // If no bit is set, there's nothing to release.
    if (to_be_released_bitmask == 0) {
        return false;
    }

    // Already remove the bits whose records will be released now.
    used_release_records_bitmask &= ~to_be_released_bitmask;

    bool waited = !wait_before_first;

    while (to_be_released_bitmask != 0) {
        uint8_t lsb_set_index = __builtin_ctz(to_be_released_bitmask);

        if (!waited) {
            send_and_wait();
            waited = true;
        }

        process_record_with_new_time(&release_records[lsb_set_index]);

        to_be_released_bitmask = clear_bit(to_be_released_bitmask, lsb_set_index);
    }

    return waited;
}

/**
 * @brief Processes releases of the supplied kind and resets their records.
 * Will wait only if there is a release record of the supplied kind.
 * We do this to avoid any unnecessary waits.
 *
 * @return true if we waited.
 */
static bool process_release_records_and_wait_before_first(release_time_t release_time) {
    return process_release_records(release_time, true);
}

/**
 * @brief Adds a release record to the array. If the array is full, processes
 *        the record immediately.
 */
static void add_release_record(keyrecord_t* record, release_time_t release_time) {
    uint8_t has_no_record_bitmask = ~used_release_records_bitmask;
    if (has_no_record_bitmask == 0) {
        // Every bit is set. Process the record immediately as a fallback.
        process_record(record);
    }

    // the least significant bit that is not set (i.e. the last zero)
    uint8_t lsb_unset_index          = __builtin_ctz(has_no_record_bitmask);
    release_records[lsb_unset_index] = *record;

    const bool is_before_second  = release_time == BEFORE_SECOND;
    is_before_second_bitmask     = change_bit(is_before_second_bitmask, lsb_unset_index, is_before_second);
    used_release_records_bitmask = set_bit(used_release_records_bitmask, lsb_unset_index);
}

static float weighted_avg(float v3, float v4) {
    // a value < 0 should not count towards the average (and v4 is never < 0)
    if (v3 < 0) {
        return v4;
    }

    // each weight is the result of E**index divided by the sum of them all
    return 0.2689414213699951f * v3 + 0.7310585786300049f * v4;
}

// Default prediction functions (can be used in your own weak function overrides)
//=============================================================================
/**
 * Auto-generated decision tree prediction function.
 *
 * At most 7 comparisons are necessary to get a result.
 *
 * Mod:      50,599 /  68,121 (74.28 %)
 * Non-mod: 306,692 / 310,294 (98.84 %)
 * Total:   357,291 / 378,415 (94.42 %)
 *
 * @return float predicted overlap time in ms.
 */
float pth_default_get_hold_prediction_when_third_press(void) {
    // Initialize to -1 because we may not have that information yet.
    float opt_next_dur            = -1.0f;
    float opt_th_down_next_up_dur = -1.0f;

    if (second_to_be_released) {
        opt_next_dur            = (float)pth_second_dur;
        opt_th_down_next_up_dur = (float)pth_press_to_second_release_dur;
    }

    // clang-format off
return (
  pth_prev_press_to_pth_press_dur <= 759
  ? (
    opt_th_down_next_up_dur <= 150
    ? (
      pth_press_to_second_press_dur <= 170
      ? (
        pth_second_press_to_third_press_dur <= 107
        ? 0.040555656f
        : (
          opt_th_down_next_up_dur <= 109
          ? 0.14262922f
          : (
            pth_press_to_second_press_dur <= 55
            ? 0.3217576f
            : 0.8006757f
          )
        )
      )
      : (
        pth_press_to_second_press_dur <= 216
        ? (
          down_count <= 0
          ? (
            pth_second_press_to_third_press_dur <= 77
            ? 0.38718662f
            : 0.6451292f
          )
          : 0.22810061f
        )
        : (
          down_count <= 0
          ? 0.910299f
          : (
            pth_press_to_second_press_dur <= 264
            ? 0.4814815f
            : 0.8877551f
          )
        )
      )
    )
    : (
      pth_second_press_to_third_press_dur <= 145
      ? (
        pth_press_to_second_press_dur <= 92
        ? (
          down_count <= 0
          ? (
            key_release_before_pth_to_pth_press_dur <= 112
            ? 0.43078628f
            : 0.6967871f
          )
          : (
            pth_press_to_press_w_avg <= 63.602364f
            ? 0.51724136f
            : 0.16554306f
          )
        )
        : (
          down_count <= 0
          ? 0.82194614f
          : (
            pth_press_to_press_w_avg <= 105.37883f
            ? 0.64830506f
            : 0.35095447f
          )
        )
      )
      : (
        pth_press_to_second_press_dur <= 59
        ? (
          opt_next_dur <= 130
          ? 0.6714801f
          : (
            pth_prev_press_to_pth_press_dur <= 303
            ? 0.27037036f
            : 0.7083333f
          )
        )
        : 0.93728805f
      )
    )
  )
  : (
    pth_press_to_press_w_avg <= 994.01086f
    ? (
      opt_th_down_next_up_dur <= 120
      ? (
        pth_press_to_second_press_dur <= 139
        ? (
          key_release_before_pth_to_pth_press_dur <= 443
          ? 0.84f
          : (
            key_release_before_pth_to_pth_press_dur <= 1110
            ? 0.12546816f
            : 0.54545456f
          )
        )
        : 0.83798885f
      )
      : (
        pth_second_press_to_third_press_dur <= 127
        ? (
          pth_press_to_second_press_dur <= 146
          ? (
            key_release_before_pth_to_pth_press_dur <= 916
            ? 0.4074074f
            : 0.9166667f
          )
          : 0.9607843f
        )
        : 0.97471267f
      )
    )
    : (
      pth_press_to_second_press_dur <= 19
      ? 0.06451613f
      : (
        pth_prev_press_to_pth_press_dur <= 1449
        ? (
          pth_press_to_second_press_dur <= 111
          ? (
            key_release_before_pth_to_pth_press_dur <= 1777
            ? 0.6754386f
            : 0.1f
          )
          : 0.9519231f
        )
        : 0.99276936f
      )
    )
  )
);
    // clang-format on
}

/**
 * Auto-generated decision tree prediction function.
 * At most 7 comparisons are necessary to get a result.
 *
 * Mod:       741,259 /  1,057,871 (70.07 %)
 * Non-mod: 9,162,154 /  9,190,163 (99.70 %)
 * Total:   9,903,413 / 10,248,034 (96.64 %)
 *
 * @return float prediction value in [0, 1]. > 0.5f is considered hold.
 */
float pth_default_get_hold_prediction_when_pth_release_after_second_press(void) {
    // clang-format off
return (
  pth_prev_press_to_pth_press_dur <= 1254
  ? (
    pth_press_to_second_press_dur <= 214
    ? (
      pth_press_to_second_press_dur <= 168
      ? (
        pth_prev_press_to_pth_press_dur <= 237
        ? 0.021824066f
        : (
          pth_press_to_second_press_dur <= 124
          ? 0.06581373f
          : (
            pth_prev_prev_press_to_prev_press_dur <= 1603
            ? 0.12980974f
            : 0.6515581f
          )
        )
      )
      : (
        key_release_before_pth_to_pth_press_dur <= 169
        ? 0.1548253f
        : (
          pth_press_to_second_press_dur <= 186
          ? (
            pth_press_to_press_w_avg <= 822.32574f
            ? 0.3386316f
            : 0.6540284f
          )
          : (
            pth_prev_press_to_pth_press_dur <= 226
            ? 0.10697675f
            : 0.53629214f
          )
        )
      )
    )
    : (
      pth_press_to_second_press_dur <= 247
      ? (
        key_release_before_pth_to_pth_press_dur <= 162
        ? (
          pth_overlap_w_avg <= 0.13447072f
          ? (
            pth_prev_prev_press_to_prev_press_dur <= 165
            ? 0.63566846f
            : 0.41175103f
          )
          : 0.24768922f
        )
        : (
          down_count <= 0
          ? (
            pth_overlap_w_avg <= 17.07778f
            ? 0.7658702f
            : 0.4507772f
          )
          : 0.08022922f
        )
      )
      : (
        down_count <= 0
        ? 0.88925225f
        : (
          pth_press_to_second_press_dur <= 312
          ? 0.26601785f
          : (
            pth_prev_press_to_pth_press_dur <= 181
            ? 0.7529976f
            : 0.23684211f
          )
        )
      )
    )
  )
  : (
    key_release_before_pth_to_pth_press_dur <= 1350
    ? (
      pth_press_to_second_press_dur <= 139
      ? (
        key_release_before_pth_to_pth_press_dur <= 1273
        ? (
          pth_prev_prev_press_to_prev_press_dur <= 1588
          ? (
            key_release_before_pth_to_pth_press_dur <= 539
            ? 0.5905512f
            : 0.25539857f
          )
          : (
            key_release_before_pth_to_pth_press_dur <= 102
            ? 0.083333336f
            : 0.8053435f
          )
        )
        : (
          pth_press_to_press_w_avg <= 1096.1167f
          ? (
            pth_press_to_second_press_dur <= 89
            ? 0.4801762f
            : 0.7108014f
          )
          : 0.42533332f
        )
      )
      : 0.89287937f
    )
    : (
      pth_press_to_second_press_dur <= 17
      ? (
        pth_prev_prev_press_to_prev_press_dur <= 146
        ? 0.01754386f
        : (
          key_release_before_pth_to_pth_press_dur <= 3116
          ? 0.04477612f
          : (
            key_release_before_pth_to_pth_press_dur <= 3243
            ? 0.5714286f
            : 0.09090909f
          )
        )
      )
      : (
        key_release_before_pth_to_pth_press_dur <= 1504
        ? 0.9103782f
        : (
          down_count <= 0
          ? 0.98845273f
          : 0.046153847f
        )
      )
    )
  )
);
    // clang-format on
}

/**
 * Auto-generated decision tree prediction function.
 * At most 7 comparisons are necessary to get a result.
 *
 * Mod:     420,158 / 435,604 (96.45 %)
 * Non-mod:  60,870 /  85,031 (71.59 %)
 * Total:   481,028 / 520,635 (92.39 %)
 *
 * @return float prediction value in [0, 1]. > 0.5f is considered hold.
 */
float pth_default_get_hold_prediction_when_pth_release_after_second_release(void) {
    uint16_t opt_next_dur            = pth_second_dur;
    uint16_t opt_th_down_next_up_dur = pth_press_to_second_release_dur;

    // clang-format off
return (
  opt_th_down_next_up_dur <= 143
  ? (
    pth_prev_press_to_pth_press_dur <= 1292
    ? (
      opt_th_down_next_up_dur <= 116
      ? 0.09534535f
      : (
        key_release_before_pth_to_pth_press_dur <= 118
        ? 0.27736303f
        : (
          pth_prev_press_to_pth_press_dur <= 174
          ? 0.08959538f
          : (
            pth_press_to_second_press_dur <= 29
            ? 0.32664755f
            : 0.65463656f
          )
        )
      )
    )
    : (
      pth_press_to_second_press_dur <= 19
      ? 0.1f
      : (
        opt_th_down_next_up_dur <= 64
        ? (
          key_release_before_pth_to_pth_press_dur <= 2050
          ? 0.0625f
          : (
            pth_press_to_press_w_avg <= 2830.7092f
            ? 0.71428573f
            : 0.5f
          )
        )
        : (
          key_release_before_pth_to_pth_press_dur <= 1244
          ? (
            opt_th_down_next_up_dur <= 107
            ? 0.33333334f
            : 0.85714287f
          )
          : 0.99616855f
        )
      )
    )
  )
  : (
    key_release_before_pth_to_pth_press_dur <= 125
    ? (
      pth_press_to_second_press_dur <= 107
      ? (
        down_count <= 0
        ? (
          pth_press_to_second_press_dur <= 77
          ? (
            key_release_before_pth_to_pth_press_dur <= 47
            ? 0.42004812f
            : 0.58709514f
          )
          : 0.70079845f
        )
        : 0.24063401f
      )
      : (
        opt_th_down_next_up_dur <= 182
        ? (
          pth_prev_prev_overlap_dur <= 0
          ? (
            opt_next_dur <= 43
            ? 0.4791367f
            : 0.8005192f
          )
          : (
            opt_next_dur <= 54
            ? 0.23857868f
            : 0.50877196f
          )
        )
        : (
          pth_press_to_second_press_dur <= 167
          ? 0.8571564f
          : (
            opt_next_dur <= 17
            ? 0.30452675f
            : 0.96995705f
          )
        )
      )
    )
    : (
      down_count <= 0
      ? (
        pth_press_to_press_w_avg <= 867.94495f
        ? 0.94516844f
        : (
          pth_press_to_second_press_dur <= 11
          ? 0.14285715f
          : 0.9992744f
        )
      )
      : (
        pth_prev_prev_press_to_prev_press_dur <= 311
        ? (
          opt_th_down_next_up_dur <= 238
          ? 0.15384616f
          : (
            pth_press_to_second_press_dur <= 175
            ? 0.43137255f
            : 0.74390244f
          )
        )
        : (
          opt_th_down_next_up_dur <= 178
          ? (
            pth_prev_press_to_pth_press_dur <= 96
            ? 0.54285717f
            : 0.0952381f
          )
          : (
            pth_prev_press_to_pth_press_dur <= 187
            ? 0.91690546f
            : 0.2f
          )
        )
      )
    )
  )
);
    // clang-format on
}

/**
 * @brief The default prediction for the minimum overlap time for a hold.
 *
 * Mod:                 991,319 /  1,496,055 (66.26 %)
 * Non-mod:           9,527,683 /  9,582,518 (99.43 %)
 * Total:            10,519,002 / 11,078,573 (94.95 %)
 *
 * @return float predicted overlap time in ms.
 */
float pth_default_get_overlap_ms_for_hold_prediction(void) {
    // clang-format off
    float guess = ABS(
        MAX(pth_press_to_second_press_dur *
                SD(20145.72453837935f,
                   20145.72453837935f -
                       (((float)pth_prev_press_to_pth_press_dur) -
                        pth_prev_prev_overlap_dur) *
                           pth_press_to_second_press_dur),
            SD(20141.63979839019f - ((pth_prev_press_to_pth_press_dur -
                                      2.0f * pth_prev_prev_overlap_dur) -
                                     pth_prev_prev_overlap_dur) *
                                        10.24699665838974f,
               pth_press_to_second_press_dur) -
                32.559018051648636f));
    // clang-format on

    return (uint16_t)guess;
}

#ifdef PTH_FAST_STREAK_TAP_ENABLE
// should be simple, as this will be called on every tap-hold press when IDLE
float pth_default_get_fast_streak_tap_prediction(void) {
    float s = ((float)pth_prev_prev_overlap_dur) - pth_prev_press_to_pth_press_dur;
    return ABS(SD(s, 4.280551301886473f - pth_prev_press_to_pth_press_dur));
}

float pth_conservative_get_fast_streak_tap_prediction(void) {
    float s = ((float)pth_prev_prev_overlap_dur) - pth_prev_press_to_pth_press_dur;
    return ABS(SD(s, s + 5.3131340976019885f * pth_overlap_w_avg));
}
#endif // PTH_FAST_STREAK_TAP_ENABLE

__attribute__((weak)) float pth_get_prediction_factor_for_hold(void) {
    // will be 1 for PTH_5H and 2 for PTH_10H, and 3 for PTH_15H
    uint8_t mp = PTH_GET_USER_BIT_ENCODED_VALUE(pth_get_pth_side_user_bits());
    if (mp == 0 || mp > 3) {
        return 1.0f;
    }
    return 1.0f - mp * 0.05f;
}

// Prediction functions
// ----------------------------------------------------------------------------
__attribute__((weak)) bool pth_predict_hold_when_third_press(void) {
    float p = pth_default_get_hold_prediction_when_third_press();
    p *= pth_get_prediction_factor_for_hold();
    return p > 0.5f;
}

__attribute__((weak)) bool pth_predict_hold_when_pth_release_after_second_press(void) {
    float p = pth_default_get_hold_prediction_when_pth_release_after_second_press();
    p *= pth_get_prediction_factor_for_hold();
    return p > 0.5f;
}

__attribute__((weak)) bool pth_predict_hold_when_pth_release_after_second_release(void) {
    float p = pth_default_get_hold_prediction_when_pth_release_after_second_release();
    p *= pth_get_prediction_factor_for_hold();
    return p > 0.5f;
}

__attribute__((weak)) uint16_t pth_predict_min_overlap_for_hold_in_ms(void) {
    float pf = pth_get_prediction_factor_for_hold();

    if (pth_is_second_same_side_as_pth()) {
        // If second is same side, we want the overlap required to be larger,
        // as it is more likely this is intended as a tap.
        pf -= 0.10f;
    }

    // a large overlap estimate makes hold less likely
    float f = 1.0f + (1.0f - pf);
    return pth_default_get_overlap_ms_for_hold_prediction() * f;
}

static bool should_neutralize_mods(uint16_t keycode, bool was_held_instantly) {
    return (was_held_instantly && IS_QK_MOD_TAP(keycode) && pth_should_neutralize_mods(get_5_bit_mods_of_mod_tap(keycode)));
}

// Decision making functions
// ----------------------------------------------------------------------------
static void make_decision_tap(void) {
    if (pth_status >= PTH_DECIDED_TAP) {
        return;
    }

    PTH_LOGF("  -> DECIDED_TAP after %u ms", timer_elapsed(pth_press_timer));

    pth_status = PTH_DECIDED_TAP;

    if (should_neutralize_mods(pth_keycode, pth_was_held_instantly) || should_neutralize_mods(second_keycode, second_was_held_instantly)) {
        // Neutralize modifiers acting on their own (e.g. ALT).
        tap_code16(PTH_INSTANT_MOD_TAP_SUPPRESSION_KEY);
    }

    // TODO: If both held instantly, does the order ever matter?
    if (pth_was_held_instantly) {
        if (IS_QK_LAYER_TAP(pth_keycode)) {
            // PTH is LT and was held instantly, so second is outdated.
            second_keycode     = get_keycode_same_pos_in_layer(&second_record, layer_before_instant_layer_tap);
            second_is_tap_hold = pth_is_tap_hold_keycode(second_keycode);
            PTH_LOGF("  Disabling PTH instant layer. Second key will be: %s", get_keycode_string(second_keycode));
        }
        process_unregister_record_as_hold(&pth_record);
    }

    if (second_was_held_instantly) {
        process_unregister_record_as_hold(&second_record);
    }

    process_register_record_as_tap(&pth_record);
    process_release_records_and_wait_before_first(BEFORE_SECOND);

    if (!has_second) {
        return;
    }

    if (second_is_tap_hold) {
        if (!second_to_be_released) {
            // add it to array, so we can release as hold even after we have reset state
            add_pos_to_tap_releases(second_record.event.key);
        }
        set_record_to_tap(&second_record);
    }

    PTH_LOGF("  Registering second key. (layer_state=%u default_layer_state=%u)", layer_state, default_layer_state);
    process_register_record(&second_record);
    bool waited = process_release_records_and_wait_before_first(AFTER_SECOND);

    if (second_to_be_released) {
        if (!waited) {
            send_and_wait();
        }
        process_unregister_record(&second_record);
    }
}

static void make_decision_hold(void) {
    if (pth_status >= PTH_DECIDED_TAP) {
        return;
    }

    PTH_LOGF("  -> DECIDED_HOLD after %u ms", timer_elapsed(pth_press_timer));

    pth_status = PTH_DECIDED_HOLD;

    if (!pth_was_held_instantly) {
        register_pth_hold();
    }
    process_release_records(BEFORE_SECOND, pth_was_held_instantly);

    if (!has_second) {
        return;
    }

    // Users expect the following sequence to result in an uppercase A and B:
    // KC_LSFT down, LCTL_T(KC_A) down [PTH key], LSFT_T(KC_B) down, KC_LSFT up,
    // LCTL_T(KC_A) up. The algorithm predicts tap. At that point KC_LSFT is
    // already up. If we had already unregistered the Shift, then we would get
    // 'ab' instead of 'AB'. That's why we have to collect releases after and
    // before the second key is pressed, so that we can release them at the
    // right time when a choice for tap or hold is made. That way, we can't
    // preserve the duration between presses, but we can preserve the order.
    //
    // Be aware, that there is one edge case where we can't do so completely:
    //
    // PTH is pressed and instantly held, a key X is released (which we cache,
    // i.e. not process yet), second is down and instantly held. So the second
    // is already held, but in reality X was released before second is pressed.
    //
    // That said, it seems like it's not an issue, as modifiers (usually) only
    // affect keys when they're being pressed down, and not afterwards. An
    // KC_LSFT will not make an KC_E uppercase, if it was down before KC_LSFT.
    if (!second_was_held_instantly) {
        if (second_is_tap_hold) {
            if (second_is_same_side_as_pth && pth_should_register_as_hold_when_same_side(second_keycode, &second_record)) {
                // Same-side tap-hold becomes hold to allow multiple holds at
                // the same time. For consistency, we do it, even if second was
                // already released.
                set_record_to_hold(&second_record);
            } else {
                // other side becomes tap
                if (!second_to_be_released) {
                    add_pos_to_tap_releases(second_record.event.key);
                }
                set_record_to_tap(&second_record);
            }
        }

        process_register_record(&second_record);
    }

    // does not wait, if second was held instantly, as that already requires waiting
    bool waited = process_release_records(AFTER_SECOND, second_was_held_instantly);

    if (second_to_be_released) {
        if (!waited) {
            send_and_wait();
        }
        process_unregister_record(&second_record);
    }
}

static void make_user_choice_or_not(void) {
    has_chosen_after_timeout_reached = true;
    pth_status_t choice              = pth_get_forced_choice_after_timeout();
    if (choice == PTH_DECIDED_HOLD) {
        PTH_LOG("Choose hold because pressed long enough.");
        make_decision_hold();
    } else if (choice == PTH_DECIDED_TAP) {
        PTH_LOG("Choose tap because pressed long enough.");
        make_decision_tap();

#ifdef PTH_RESET_IMMEDIATELY_WHEN_TAP_CHOSEN
        add_pos_to_tap_releases(pth_record.event.key);
        reset_pth_state();
#endif
    }
}

static void store_press_to_press_and_overlap_for_pth(void) {
    // We measure from press to press, and as this is a press, we
    // don't need to do any special handling here for down_count > 0.
    pth_prev_prev_press_to_prev_press_dur = prev_press_to_press_dur;
    pth_prev_press_to_pth_press_dur       = cur_press_to_press_dur;

    // The following is necessary for consistency!
    // For example, let's go through some examples:
    //
    // x is the current PTH key
    // lower case = down / pressed
    // upper case = up / released
    //
    // cCdDx -> would provide 2 overlap values
    //
    // cCdx (d not released) -> without adding 0, this would only
    // provide 1 value from that sequence, but as other keys likely
    // have been pressed before, the prediction algorithm would have
    // access to much older overlap values that are less relevant.
    //
    // For press to press durations, we would get 2 in either case.
    uint8_t down_count_before_this = down_count - 1;

    pth_prev_prev_overlap_dur = prev_overlap_dur;
    pth_prev_overlap_dur      = cur_overlap_dur;
    if (down_count_before_this == 1) {
        // still one down, but no overlap (of course, it will overlap with this one)
        pth_prev_prev_overlap_dur = pth_prev_overlap_dur;
        pth_prev_overlap_dur      = 0;
    } else if (down_count_before_this >= 2) {
        pth_prev_prev_overlap_dur = 0;

        // there's still an overlap going on (more than 1 key down),
        // so determine duration until now and that will be the new last
        if (overlap_timer_max_reached) {
            pth_prev_overlap_dur = MS_MAX_DUR_FOR_TIMERS;
        } else {
            pth_prev_overlap_dur = timer_elapsed(overlap_timer);
        }
    }

    pth_press_to_press_w_avg = weighted_avg(pth_prev_prev_press_to_prev_press_dur, pth_prev_press_to_pth_press_dur);
    pth_overlap_w_avg        = weighted_avg(pth_prev_prev_overlap_dur, pth_prev_overlap_dur);
}

static void collect_new_press_to_press_and_overlap_duration(bool is_pressed, uint16_t cur_time) {
    if (is_pressed) {
        uint16_t p_to_p_dur;
        if (press_to_press_timer_max_reached) {
            p_to_p_dur = MS_MAX_DUR_FOR_TIMERS;
        } else {
            p_to_p_dur = TIMER_DIFF_16(cur_time, press_to_press_timer);
        }
        prev_press_to_press_dur = cur_press_to_press_dur;
        cur_press_to_press_dur  = p_to_p_dur;
        PTH_LOGF("  Storing actual press-to-press duration: %u ms", p_to_p_dur);

        press_to_press_timer             = cur_time;
        press_to_press_timer_max_reached = false;
        down_count++;
        if (down_count == 2) {
            // Two keys down at the same time, so we have an overlap
            overlap_timer             = cur_time;
            overlap_timer_max_reached = false;
        }
    } else {
        // on release
        uint16_t overlap = 0;
        // Check if an overlap was active (down_count would have been >= 2 before this release event)
        if (down_count >= 2) {
            if (overlap_timer_max_reached) {
                overlap = MS_MAX_DUR_FOR_TIMERS;
            } else {
                overlap = TIMER_DIFF_16(cur_time, overlap_timer);
            }
        }

        if (down_count > 0) {
            down_count--;
        }
        prev_overlap_dur = cur_overlap_dur;
        cur_overlap_dur  = overlap;
        PTH_LOGF("  Storing actual overlap duration: %u ms", overlap);

        // We don't want to count overlaps twice, so we set to the current time
        overlap_timer             = cur_time;
        overlap_timer_max_reached = false;

        release_timer             = cur_time;
        release_timer_max_reached = false;
    }
}

// Core Processing Function (State Machine)
// ----------------------------------------------------------------------------
bool process_record_predictive_tap_hold(uint16_t keycode, keyrecord_t* record) {
#ifdef PTH_DISABLED
    return true;
#endif
    // Initial checks - don't handle internal events or non-key events
    if (is_processing_record_due_to_pth || !IS_KEYEVENT(record->event)) {
        return true; // let the processing continue
    }

    const bool cur_is_pressed = record->event.pressed;
    PTH_LOGF("Key %s is %s (side=%s) - Status: %s", get_keycode_string(keycode), cur_is_pressed ? "DOWN" : "UP", side_to_str(pth_get_side(record)), STATUS_TO_STR(pth_status));

#ifdef TAPPING_TERM_PER_KEY
    if (get_tapping_term(keycode, record) != 0) {
        // TODO: Investigate how this works with overlapping tap-holds, such as
        // first handled by us, second by QMK OR first by QMK, second by us
        PTH_LOG("  QMK will handle this, as the tapping term of this key is not zero.");
        return true;
    }
#endif

#ifdef TAP_DANCE_ENABLE
    if (IS_QK_TAP_DANCE(keycode)) {
        PTH_LOG("  QMK will handle this, as it's a tap dance.");
        return true;
    }
#endif

#ifdef COMBO_ENABLE
    if (IS_COMBOEVENT(record->event)) {
        PTH_LOG("  QMK will handle this, as it's a combo.");
        return true;
    }
#endif

    const uint16_t cur_time = timer_read();
    const keypos_t cur_pos  = record->event.key;

    // We collect here, even though this event may not end up being reported to
    // the OS for a while or it may be reported in a slightly different order
    // due to the instant hold functionality. The reason is that the prediction
    // functions were trained using real typing data, and so also we need to
    // provide it the durations of the real key presses.
    collect_new_press_to_press_and_overlap_duration(cur_is_pressed, cur_time);

    if (cur_is_pressed) {
        prev_press_keycode = cur_press_keycode;
        cur_press_keycode  = keycode;
    } else {
        // A key is released.

        // We don't need to check if the cur_pos is a tap-hold, because
        // it's impossible to press a key again that has not yet been released.
        // And not checking, also makes sure that if some glitch causes a
        // release with a keycode from a different layer than the associated
        // press, it will still be handled here.
        if (remove_pos_from_tap_releases(cur_pos)) {
            if (pth_status == PTH_PRESSED || pth_status == PTH_SECOND_PRESSED) {
                // We set it to tap, as it will be cached for future release.
                // See the release handling of PTH_PRESSED for an explanation.
                PTH_LOG("  Position was in tap_releases and status is PTH_PRESSED or SECOND_PRESSED, so set as tap (release will happen later).");
                set_record_to_tap(record);
            } else {
                PTH_LOG("  Position was in tap_releases, so release as tap.");
                process_unregister_record_as_tap(record);
                return false;
            }
        }
    }

    const bool is_tap_hold = pth_is_tap_hold_keycode(keycode);

    // --- State Machine Logic ---
    switch (pth_status) {
        // =============================================================================
        case PTH_IDLE:
            if (cur_is_pressed && is_tap_hold) {
                // New PTH key
                pth_status = PTH_PRESSED;

                pth_press_timer = cur_time;
                pth_keycode     = keycode;
                pth_record      = *record;

                uint8_t side       = pth_get_side(&pth_record);
                pth_side_user_bits = PTH_GET_USER_BITS(side);
                pth_atomic_side    = PTH_GET_PTH_ATOM_SIDE(side);

                if (release_timer_max_reached) {
                    key_release_before_pth_to_pth_press_dur = MS_MAX_DUR_FOR_TIMERS;
                } else {
                    key_release_before_pth_to_pth_press_dur = TIMER_DIFF_16(pth_press_timer, release_timer);
                }
                store_press_to_press_and_overlap_for_pth();

                pth_tap_code_instead_of_hold = pth_get_code_to_be_registered_instead_when_hold_chosen();
                timeout_for_forcing_choice   = pth_get_timeout_for_forcing_choice();

                PTH_LOGF("  -> PRESSED (new PTH key) after %u ms from last release. (side=%s timeout_for_forcing_choice=%u)", key_release_before_pth_to_pth_press_dur, ATOM_SIDE_TO_STR(pth_atomic_side), timeout_for_forcing_choice);

                if (pth_tap_code_instead_of_hold != KC_NO) {
                    PTH_LOGF("   Will register %s instead, if hold is chosen, so instant hold disabled.", get_keycode_string(pth_tap_code_instead_of_hold));
                }

                if (timeout_for_forcing_choice == 0) {
                    make_user_choice_or_not();
                    if (pth_status >= PTH_DECIDED_TAP) {
                        return false;
                    }
                }

#ifdef PTH_FAST_STREAK_TAP_ENABLE
                if (pth_predict_fast_streak_tap()) {
                    PTH_LOG("  Fast Streak Tap predicted.");
#    ifdef PTH_FAST_STREAK_TAP_RESET_IMMEDIATELY
                    process_register_record_as_tap(&pth_record);

                    // have to remember PTH tap release as we will reset immediately
                    add_pos_to_tap_releases(pth_record.event.key);
                    reset_pth_state();
#    else
                    make_decision_tap();
#    endif
                    return false;
                }
#endif // PTH_FAST_STREAK_TAP_ENABLE

#ifdef PTH_DONT_HOLD_INSTANTLY
                pth_was_held_instantly = false;
#else
                pth_was_held_instantly = pth_tap_code_instead_of_hold == KC_NO && pth_should_hold_instantly(pth_keycode, &pth_record);
#endif // PTH_DONT_HOLD_INSTANTLY

                if (pth_was_held_instantly) {
                    if (IS_QK_LAYER_TAP(keycode)) {
                        instant_layer_was_active       = true;
                        layer_before_instant_layer_tap = layer_switch_get_layer(pth_record.event.key);
                        PTH_LOGF("  Layer before instant layer: %u", layer_before_instant_layer_tap);
                    }
                    PTH_LOG("  Instantly holding PTH.");
                    process_register_record_as_hold(&pth_record);
                }

                return false;
            }

            break;

        // =============================================================================
        case PTH_PRESSED:
            if (cur_is_pressed) {
                // Second key pressed
                pth_status = PTH_SECOND_PRESSED;

                has_second                 = true;
                second_press_timer         = cur_time;
                second_keycode             = keycode;
                second_record              = *record;
                second_is_tap_hold         = is_tap_hold;
                second_is_same_side_as_pth = is_record_same_side_as_pth(record);

                if (pth_press_timer_max_reached) {
                    pth_press_to_second_press_dur = MS_MAX_DUR_FOR_TIMERS;
                } else {
                    pth_press_to_second_press_dur = TIMER_DIFF_16(second_press_timer, pth_press_timer);
                }

                PTH_LOGF("  -> SECOND_PRESSED after %u ms from PTH press", pth_press_to_second_press_dur);

                if (pth_was_held_instantly && instant_layer_was_active && second_keycode == KC_NO) {
                    PTH_LOG("  PTH's instant layer led to second key being KC_NO, so we choose tap.");
                    make_decision_tap();
#ifdef PTH_RESET_IMMEDIATELY_WHEN_TAP_CHOSEN
                    add_pos_to_tap_releases(pth_record.event.key);
                    reset_pth_state();
#endif
                    return false;
                }

                // Previously, this was only done, when they're on opposite
                // sides, but the overlap prediction seems to be more accurate
                // than third key prediction (far less data with third keys).
                if (second_is_tap_hold || !second_is_same_side_as_pth) {
                    min_overlap_dur_for_hold = MIN(PTH_MS_MAX_OVERLAP, MAX(PTH_MS_MIN_OVERLAP, pth_predict_min_overlap_for_hold_in_ms()));
                    PTH_LOGF("  Predicted minimum overlap for hold: %u ms", min_overlap_dur_for_hold);
                }

                if (!second_is_same_side_as_pth) {
                    PTH_LOG("  Second is opposite-side press, so we are done for now.");
                    return false;
                }

                // PTH and second are on the same side.
                // ------------------------------------

                if (pth_should_choose_tap_when_second_is_same_side_press()) {
                    PTH_LOG("  Second is same-side press and should_choose returned true.");
                    make_decision_tap();
#ifdef PTH_RESET_IMMEDIATELY_WHEN_TAP_CHOSEN
                    add_pos_to_tap_releases(pth_record.event.key);
                    reset_pth_state();
#endif
                    return false;
                }

#ifdef PTH_DONT_HOLD_INSTANTLY
                if (second_is_tap_hold){
#else
                if (second_is_tap_hold && pth_second_should_hold_instantly(second_keycode, &second_record)) {
#endif // PTH_DONT_HOLD_INSTANTLY
                    if (!instant_layer_was_active && IS_QK_LAYER_TAP(second_keycode)) {
                        // Remember the layer in case we have to undo the
                        // instant layer switch, when tap is chosen.
                        layer_before_instant_layer_tap = layer_switch_get_layer(second_record.event.key);
                        instant_layer_was_active       = true;
                        PTH_LOGF("  Layer before instant layer: %u", layer_before_instant_layer_tap);
                    }

                    PTH_LOG("  Instantly holding second.");
                    second_was_held_instantly = true;
                    process_register_record_as_hold(&second_record);
                }

                return false;
            } else {
                // A key was released
                if (keypos_eq(cur_pos, pth_record.event.key)) {
                    // PTH key released and no other key pressed yet, so resolve as tap
                    PTH_LOG("  PTH key released before second press. Resetting!");

                    make_decision_tap();
                    send_and_wait();
                    process_unregister_record_as_tap(&pth_record);
                    reset_pth_state();
                    return false;
                }

                // Keys, that are released before or after second is pressed, are
                // cached, so that we can replay them in the correct order. Users
                // rightly expect LSFT down, LCTL_T(KC_A) down, LFST up, tap chosen
                // to result in an uppercase A.
                PTH_LOG("  This BEFORE_SECOND release is cached. It will be processed later.");
                add_release_record(record, BEFORE_SECOND);
            }

            return false;

        // =============================================================================
        case PTH_SECOND_PRESSED:
            if (cur_is_pressed) {
                // Third key pressed
                if (second_press_timer_max_reached) {
                    pth_second_press_to_third_press_dur = MS_MAX_DUR_FOR_TIMERS;
                } else {
                    pth_second_press_to_third_press_dur = TIMER_DIFF_16(cur_time, second_press_timer);
                }

                // We run the following prediction, even if a minimum overlap
                // for hold was previously predicted, as the following function
                // is more likely to make a better prediction, as it has been
                // trained specifically for cases like this one (third press).
                // More importantly, it is really time to make a decision now.
                bool hold = pth_predict_hold_when_third_press();
                PTH_LOGF("  Third key pressed. Prediction: %s", hold ? "hold" : "tap");

                bool third_is_tap_hold = is_tap_hold;
                if (hold) {
                    make_decision_hold();
                } else {
                    make_decision_tap();

                    if (instant_layer_was_active) {
                        // If an instant layer was active before the tap
                        // decision was made, then the current keycode and
                        // is_tap_hold is outdated, so get the new one.
                        keycode           = get_keycode_same_pos_in_layer(record, layer_before_instant_layer_tap);
                        third_is_tap_hold = pth_is_tap_hold_keycode(keycode);
                    }
                }

                if (third_is_tap_hold) {
                    if (hold && is_record_same_side_as_pth(record) && pth_should_register_as_hold_when_same_side(keycode, record)) {
                        // Third is same-side tap-hold, so resolve as hold
                        process_register_record_as_hold(record);
                    } else {
                        // Tap was chosen, or third is on other side than PTH
                        add_pos_to_tap_releases(cur_pos);
                        process_register_record_as_tap(record);
                    }
                } else {
                    // Third is not tap-hold, but we handle this manually, as
                    // we just now registered other keys so time has passed.
                    process_record_with_new_time(record);
                }

#ifdef PTH_RESET_IMMEDIATELY_WHEN_TAP_CHOSEN
                if (!hold) {
                    add_pos_to_tap_releases(pth_record.event.key);
                    reset_pth_state();
                }
#endif
                return false;
            } else {
                // A key was released
                if (keypos_eq(cur_pos, pth_record.event.key)) {
                    // PTH key released
                    bool hold = false;

                    if (!second_is_same_side_as_pth) {
                        // second is on different side
                        if (second_to_be_released) {
                            hold = pth_predict_hold_when_pth_release_after_second_release();
                        } else {
                            hold = pth_predict_hold_when_pth_release_after_second_press();
                        }
                    }
                    PTH_LOGF("  PTH released after second. Prediction: %s - Resetting!", hold ? "hold" : "tap");

                    if (hold) {
                        make_decision_hold();
                        unregister_pth_hold();
                    } else {
                        make_decision_tap();
                        send_and_wait();
                        process_unregister_record_as_tap(&pth_record);
                    }

                    // We directly reset here, as the PTH key was released,
                    // which means that no other presses must be influenced by it,
                    // and we must be ready for a new PTH key.
                    reset_pth_state();
                    return false;
                } else if (keypos_eq(cur_pos, second_record.event.key)) {
                    PTH_LOG("  Second key released before PTH key.");
                    // Second key released
                    // This will not be set in cases where second is released
                    // after the third is pressed, but that is fine, as then
                    // the decision has already been made, and the default
                    // logic (or release records) will handle second just fine.
                    second_to_be_released = true;

                    if (second_is_same_side_as_pth && pth_should_choose_tap_when_second_is_same_side_release()) {
                        make_decision_tap();
#ifdef PTH_RESET_IMMEDIATELY_WHEN_TAP_CHOSEN
                        add_pos_to_tap_releases(pth_record.event.key);
                        reset_pth_state();
#endif
                        return false;
                    }

                    if (pth_press_timer_max_reached) {
                        pth_press_to_second_release_dur = MS_MAX_DUR_FOR_TIMERS;
                    } else {
                        pth_press_to_second_release_dur = TIMER_DIFF_16(cur_time, pth_press_timer);
                    }

                    if (second_press_timer_max_reached) {
                        pth_second_dur = MS_MAX_DUR_FOR_TIMERS;
                    } else {
                        pth_second_dur = TIMER_DIFF_16(cur_time, second_press_timer);
                    }
                    PTH_LOGF("  Second was pressed for %u ms. The duration from PTH press to this release is %u ms.", pth_second_dur, pth_press_to_second_release_dur);

                    // When the second key is released before a third is pressed (as right now),
                    // then make_decision_tap or make_decision_hold handle the release,
                    // so it may already have happened just now.
                    // Otherwise, the release will be handled later.
                    return false;
                }

                // See PTH_PRESSED release handling for further information.
                PTH_LOG("  This AFTER_SECOND release is cached. It will be processed later.");
                add_release_record(record, AFTER_SECOND);
            }

            return false;

        // =============================================================================
        case PTH_DECIDED_TAP:
            if (cur_is_pressed) {
                // Another key pressed after PTH decided tap
                if (is_tap_hold) {
                    add_pos_to_tap_releases(record->event.key);
                    process_register_record_as_tap(record);
                    return false;
                }
            } else {
                // A key was released
                if (keypos_eq(cur_pos, pth_record.event.key)) {
                    // PTH key released
                    PTH_LOG("  Releasing decided TAP key. Resetting!");

                    // As we may just now have chosen and send tap, we wait
                    // a bit to make sure the tap will definitely be accepted.
                    send_and_wait();
                    process_unregister_record_as_tap(&pth_record);
                    reset_pth_state();
                    return false;
                }
            }

            break;

        // =============================================================================
        case PTH_DECIDED_HOLD:
            if (cur_is_pressed) {
                // Another key pressed after PTH decided hold
                if (is_tap_hold) {
                    if (is_record_same_side_as_pth(record) && pth_should_register_as_hold_when_same_side(keycode, record)) {
                        // Same-hand tap-hold resolves as hold
                        process_register_record_as_hold(record);
                    } else {
                        // Opposite-hand tap-hold resolves as tap
                        add_pos_to_tap_releases(cur_pos);
                        process_register_record_as_tap(record);
                    }
                    return false;
                }
            } else {
                // A key was released
                if (keypos_eq(cur_pos, pth_record.event.key)) {
                    // PTH key released
                    PTH_LOG("  Releasing decided hold key. Resetting!");

                    unregister_pth_hold();
                    reset_pth_state();
                    return false;
                }
            }

            break;

            // =============================================================================
    }

    if (!cur_is_pressed && !second_was_held_instantly && keypos_eq(cur_pos, second_record.event.key)) {
        // As second will be pressed when the decision is made (unless held
        // instantly), it is possible that the press was just now registered.
        // So, to avoid really short taps that are not registered by the OS,
        // we add a tiny delay.
        send_and_wait();
    }

    PTH_LOG("  QMK will handle this.");
    // Hold is the default, and if a release was supposed to be a tap instead,
    // that release would have been handled already. So we do nothing special.
    return true;
}

// Housekeeping (runs constantly)
// ----------------------------------------------------------------------------
void housekeeping_task_predictive_tap_hold(void) {
#ifdef PTH_DISABLED
    return;
#endif
    uint16_t cur_time = timer_read();

    if (!release_timer_max_reached) {
        if (TIMER_DIFF_16(cur_time, release_timer) >= MS_MAX_DUR_FOR_TIMERS) {
            release_timer_max_reached = true;
        }
    }

    // overlap_timer is relevant if two or more keys are currently down.
    if (!overlap_timer_max_reached && down_count >= 2) {
        if (TIMER_DIFF_16(cur_time, overlap_timer) >= MS_MAX_DUR_FOR_TIMERS) {
            overlap_timer_max_reached = true;
        }
    }

    // This timer tracks the duration since the *last* key press.
    // press_to_press_timer is always relevant.
    if (!press_to_press_timer_max_reached) {
        if (TIMER_DIFF_16(cur_time, press_to_press_timer) >= MS_MAX_DUR_FOR_TIMERS) {
            press_to_press_timer_max_reached = true;
        }
    }

    if (pth_status == PTH_IDLE || pth_status >= PTH_DECIDED_TAP) {
        return;
    }

    // second_press_timer is relevant if a second key has been pressed in a PTH sequence.
    if (!second_press_timer_max_reached && pth_status == PTH_SECOND_PRESSED) {
        if (min_overlap_dur_for_hold > 0 && TIMER_DIFF_16(cur_time, second_press_timer) >= min_overlap_dur_for_hold) {
            PTH_LOG("Housekeeping: Overlap large enough, so choose HOLD.");
            make_decision_hold();
            return; // the rest of the checks don't matter anymore
        } else if (TIMER_DIFF_16(cur_time, second_press_timer) >= MS_MAX_DUR_FOR_TIMERS) {
            second_press_timer_max_reached = true;
        }
    }

    // pth_press_timer is relevant if a PTH sequence is active before a decision.
    if (!pth_press_timer_max_reached) { // must be PTH_PRESSED or PTH_SECOND_PRESSED
        if (TIMER_DIFF_16(cur_time, pth_press_timer) >= MS_MAX_DUR_FOR_TIMERS) {
            pth_press_timer_max_reached = true;
        } else if (!has_chosen_after_timeout_reached && timeout_for_forcing_choice > 0 && TIMER_DIFF_16(cur_time, pth_press_timer) >= timeout_for_forcing_choice) {
            make_user_choice_or_not();
        }
    }
}
