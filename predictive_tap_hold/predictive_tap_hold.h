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

#pragma once

#include "quantum.h"

/**
 * If you'd like to debug PTH, define PTH_DEBUG in your config.h, and set
 * CONSOLE_ENABLE = yes (and KEYCODE_STRING_ENABLE = yes) in your rules.mk
 */
#ifdef PTH_DEBUG
#    include "print.h"
#    include <string.h>
#endif

#ifdef VIAL_ENABLE
#    include "dynamic_keymap.h"
#else
#    include "keymap_introspection.h"
#endif

// Feature Flags
//=============================================================================
// You can add the below define statements to your config.h for customization.

// Note that in the following we refer to main tap-hold key as the PTH key.

/**
 * If you add this, PTH will be reset directly after a tap was chosen.
 * By default, we immediately set the PTH state to DECIDED_TAP, turning all
 * subsequent overlapping tap-hold keys (i.e. the PTH is still down) into taps.
 * Note, however, that the default has the advantage of making typing feel
 * faster, and because non-mods (KC_E) overlapping mods (KC_LSFT) happens
 * very rarely (0.73 % in the training data), it will almost always be correct.
 */
// #    define PTH_RESET_IMMEDIATELY_WHEN_TAP_CHOSEN

/**
 * Add this to enable Fast Streak Tap.
 */
// #    define PTH_FAST_STREAK_TAP_ENABLE

/**
 * Like PTH_RESET_IMMEDIATELY_WHEN_TAP_CHOSEN but only for Fast Streak Tap.
 */
// #    define PTH_FAST_STREAK_TAP_RESET_IMMEDIATELY

/**
 * Only basic, unmodified HID keycodes work, not KC_NO or KC_TRNS.
 * Avoid F24 because GUI + F24 triggers a screenshot on Windows.
 */
#ifndef PTH_INSTANT_MOD_TAP_SUPPRESSION_KEY
#    define PTH_INSTANT_MOD_TAP_SUPPRESSION_KEY KC_F23
#endif

/**
 * If a PTH is pressed and then another key on the opposite side is pressed,
 * a prediction function estimates the minimum required overlap duration
 * (PTH and second key being down at the same time). A timer is also started.
 * When the duration has passed, before a choice is made, then we choose hold.
 *
 * The following is true for both of the following constants: If their value is
 * small, we could get more accidental holds (meant to be taps), but correct
 * holds could be faster. If they're large, correct holds could be slower, and
 * we could get more accidental taps, but we could get more correct taps too.
 * But in the end, they're just guardrails for the prediction function.
 *
 * Values should match those used in training!
 *
 * Chosen because 99.9 % of mod-first intersection durations are shorter.
 * The same is true for non-mod-first intersection durations.
 */
#ifndef PTH_MS_MAX_OVERLAP
#    define PTH_MS_MAX_OVERLAP 232
#endif

/**
 * At least for this long, both keys have to be pressed simultaneously.
 * Usually, intentional holds have large overlaps with the key that follows
 * (e.g. Ctrl + C), so setting this too low could lead to accidental holds.
 *
 * Chosen because more than 90 % of mod-first intersection durations are
 * longer and the majority of non-mod-first intersection durations are shorter.
 */
#ifndef PTH_MS_MIN_OVERLAP
#    define PTH_MS_MIN_OVERLAP 39
#endif

// Macros
//=============================================================================
/**
 * The atomic side bits.
 *
 * PTH_ATOM_LEFT and PTH_ATOM_RIGHT are absolute. They require a comparison.
 * PTH_ATOM_OPPOSITE and PTH_ATOM_SAME are relative sides that are resolved
 * during the comparison in `is_same_side`.
 */
#define PTH_ATOM_LEFT 0b00
#define PTH_ATOM_RIGHT 0b01
#define PTH_ATOM_OPPOSITE 0b10
#define PTH_ATOM_SAME 0b11

/**
 * @brief Encodes the two side behaviors for a key into a single uint8_t.
 *
 * Bits 3-2: Behavior when the key is in the PTH role.
 * Bits 1-0: Behavior when the key is in the "other" role.
 *
 * @param pth_role The side behavior (LEFT, RIGHT, OPPOSITE, SAME) for when this key acts as the PTH key.
 * @param other_role The side behavior (LEFT, RIGHT, OPPOSITE, SAME) for when this key acts as the "other" key.
 * @return A uint8_t value containing both encoded behaviors.
 */
#define PTH_ENCODE_KEY_SIDES(pth_role, other_role) ((((uint8_t)(pth_role) & 0b11) << 2) | ((uint8_t)(other_role) & 0b11))

// Helper macros to decode the specific role behavior from a key's configuration.
#define PTH_GET_PTH_ATOM_SIDE(encoded_val) (((encoded_val) >> 2) & 0b11)
#define PTH_GET_OTHER_ATOM_SIDE(encoded_val) ((encoded_val) & 0b11)

#define PTH_SIDE_WITHOUT_USER_BITS(encoded_val) ((encoded_val) & 0b1111)

// This extracts the number passed to PTH_TO_USER_BIT
#define PTH_GET_USER_BIT_ENCODED_VALUE(encoded_val) (((encoded_val) >> 4) & 0b1111)

// This simply sets the non-user-bits to 0, so the return value can be compared
// to a value that was acquired by calling PTH_TO_USER_BIT like PTH_5H.
#define PTH_GET_USER_BITS(encoded_val) ((encoded_val) & 0b11110000)

#define PTH_TO_USER_BITS(val) ((val) << 4)

// This is using the user bits.
#define PTH_5H PTH_TO_USER_BITS(1)
#define PTH_10H PTH_TO_USER_BITS(2)
#define PTH_15H PTH_TO_USER_BITS(3)

#ifdef __cplusplus
extern "C" {
#endif

// Core types
//=============================================================================

typedef enum { PTH_IDLE, PTH_PRESSED, PTH_SECOND_PRESSED, PTH_DECIDED_TAP, PTH_DECIDED_HOLD } pth_status_t;

typedef enum {
    // always left
    PTH_L = PTH_ENCODE_KEY_SIDES(PTH_ATOM_LEFT, PTH_ATOM_LEFT),

    // left if this is the PTH key, right otherwise
    PTH_LR = PTH_ENCODE_KEY_SIDES(PTH_ATOM_LEFT, PTH_ATOM_RIGHT),

    // left if this is the PTH key, opposite otherwise
    PTH_LO = PTH_ENCODE_KEY_SIDES(PTH_ATOM_LEFT, PTH_ATOM_OPPOSITE),

    // and so on...
    PTH_LS = PTH_ENCODE_KEY_SIDES(PTH_ATOM_LEFT, PTH_ATOM_SAME),
    PTH_RL = PTH_ENCODE_KEY_SIDES(PTH_ATOM_RIGHT, PTH_ATOM_LEFT),
    PTH_R  = PTH_ENCODE_KEY_SIDES(PTH_ATOM_RIGHT, PTH_ATOM_RIGHT),
    PTH_RO = PTH_ENCODE_KEY_SIDES(PTH_ATOM_RIGHT, PTH_ATOM_OPPOSITE),
    PTH_RS = PTH_ENCODE_KEY_SIDES(PTH_ATOM_RIGHT, PTH_ATOM_SAME),
    PTH_OL = PTH_ENCODE_KEY_SIDES(PTH_ATOM_OPPOSITE, PTH_ATOM_LEFT),
    PTH_OR = PTH_ENCODE_KEY_SIDES(PTH_ATOM_OPPOSITE, PTH_ATOM_RIGHT),
    PTH_O  = PTH_ENCODE_KEY_SIDES(PTH_ATOM_OPPOSITE, PTH_ATOM_OPPOSITE),
    PTH_OS = PTH_ENCODE_KEY_SIDES(PTH_ATOM_OPPOSITE, PTH_ATOM_SAME),
    PTH_SL = PTH_ENCODE_KEY_SIDES(PTH_ATOM_SAME, PTH_ATOM_LEFT),
    PTH_SR = PTH_ENCODE_KEY_SIDES(PTH_ATOM_SAME, PTH_ATOM_RIGHT),
    PTH_SO = PTH_ENCODE_KEY_SIDES(PTH_ATOM_SAME, PTH_ATOM_OPPOSITE),
    PTH_S  = PTH_ENCODE_KEY_SIDES(PTH_ATOM_SAME, PTH_ATOM_SAME),
} pth_side_t;

// Side configuration (weakly defined in .c, potentially overridden by user)
//=============================================================================

// You have to overwrite this or `pth_is_on_left_hand` to define which keys are left
// and which are right. There is no default here!
/**
 * @param record The keyrecord_t associated with the event.
 * @return the encoded side configuration of this key.
 */
uint8_t pth_get_side(keyrecord_t* record);

/**
 * @brief A PROGMEM array defining the side of each key.
 * Use the same LAYOUT macro as your keymap.
 *
 * Example: `const uint8_t pth_side_layout[MATRIX_ROWS][MATRIX_COLS] PROGMEM = LAYOUT(PTH_L, PTH_L, ..., PTH_R, PTH_R);`
 */
extern const uint8_t pth_side_layout[MATRIX_ROWS][MATRIX_COLS] PROGMEM;

// Configuration functions (weakly defined in .c, override in keymap.c)
//=============================================================================
#ifdef PTH_FAST_STREAK_TAP_ENABLE
/**
 * @brief Determines if a key is eligible for Fast Streak Tap. By default, this
 *        includes letters, space, dot, comma, semicolon, and slash when
 *        neither Ctrl, Alt or GUI are active.
 */
bool pth_is_fast_streak_tap_key(uint16_t keycode);

/**
 * @brief Decides whether to resolve the PTH key as a tap due to a fast typing
 *        streak. By default, this is `true` if the current and previous keys
 *        are eligible and the press-to-press interval is short (< 125ms).
 */
bool pth_predict_fast_streak_tap(void);
#endif // PTH_FAST_STREAK_TAP_ENABLE

#ifndef PTH_DONT_HOLD_INSTANTLY
/**
 * @brief Decide if the PTH should be treated as HELD immediately on press.
 *        The provisional hold will be reverted if the final decision is TAP.
 *        This works for both MT and LT.
 *
 * By default, calls `pth_default_should_hold_instantly`.
 *
 * @return true To activate the hold part instantly (provisionally).
 * @return false To wait for standard decision logic.
 */
bool pth_should_hold_instantly(uint16_t keycode, keyrecord_t* record);

/**
 * @brief Decide if the second key in a sequence should be held instantly.
 *
 * By default, just calls `pth_should_hold_instantly`.
 */
bool pth_second_should_hold_instantly(uint16_t second_keycode, keyrecord_t* second_record);
#endif // PTH_DONT_HOLD_INSTANTLY

/**
 * @brief Decides whether to resolve PTH as TAP when a second key is pressed
 *        on the same side.
 *
 * By default, returns `true` if the second key is not a tap-hold on the current
 * layer, implying a key roll.
 */
bool pth_should_choose_tap_when_second_is_same_side_press(void);

/**
 * @brief Decides whether to resolve PTH as TAP when a second key on the same
 *        side is released before a decision is made.
 *
 * By default, returns `true`.
 *
 * This will only be called, if a choice has not been made yet. As a choice is
 * always made on the third key press, this will also not be called thereafter.
 */
bool pth_should_choose_tap_when_second_is_same_side_release(void);

/**
 * @brief Returns the timeout in ms after which a decision is forced.
 *
 * By default, returns `700`. If the returned duration has passed, and no
 * choice has been made yet, we run `pth_get_forced_choice_after_timeout`.
 *
 * Must be less than `MS_MAX_DUR_FOR_TIMERS` (about 4 seconds).
 *
 * @return -1 to disable this for this PTH key
 * @return 0 to decide immediately when the PTH is pressed
 * @return any other value to wait that long (milliseconds)
 */
int16_t pth_get_timeout_for_forcing_choice(void);

/**
 * @brief Determines the forced decision after the timeout from
 *        `pth_get_timeout_for_forcing_choice` expires. If it returns something
 *        other than `PTH_DECIDED_TAP` or `PTH_DECIDED_HOLD` (e.g. `PTH_IDLE`),
 *        we do nothing and the default logic will handle the case.
 *
 * By default, returns `PTH_DECIDED_HOLD` if no second key was pressed,
 * otherwise `PTH_IDLE`, and so nothing is done in that case.
 */
pth_status_t pth_get_forced_choice_after_timeout(void);

/**
 * @brief Decides if a mod-tap's modifiers should be "neutralized" on tap by
 *        sending a keypress (e.g., F23). Will receive 5-bit packed mods, such
 *        as `MOD_LSFT` or `MOD_RALT`, **not** `MOD_BIT(<kc>)` or `MOD_MASK_xxx`.
 *
 * By default, we'll neutralize mods if they are not only CTRL, SHIFT or a
 * combination thereof. So, Ctrl + Alt will be neutralized.
 *
 * @return true If the mod should be neutralized with the
 *              `PTH_INSTANT_MOD_TAP_SUPPRESSION_KEY`.
 * @return false If it shouldn't.
 */
bool pth_should_neutralize_mods(uint8_t mods_5_bit);

/**
 * @brief This can return a possibly modded keycode that will be send when hold
 *        is chosen, instead of activating the actual hold part of the PTH key.
 *
 * By default, this returns `KC_NO`, in which case, the hold part is activated.
 *
 * For example, you could return `C(KC_C)` when the keycode is `LT(1, KC_E)`.
 * That way, if you tap, you'd type E, and if you hold, you'd copy something.
 *
 * Note that instant hold is automatically disabled for PTH keys where this
 * returned a key other than `KC_NO`.
 *
 * Thanks to @getreuer, @filterpaper and @jweickm for the idea.
 */
uint16_t pth_get_code_to_be_registered_instead_when_hold_chosen(void);

/**
 * @brief Decides if a same-side tap-hold key (pressed after PTH) should also
 *        be resolved as a hold.
 *
 * By default, returns `true`.
 */
bool pth_should_register_as_hold_when_same_side(uint16_t keycode, keyrecord_t* record);

/**
 * @brief Decides how easy it is to achieve a hold prediction by multiplying
 *        any prediction value with this factor. In the case of the overlap
 *        prediction, we instead multiply it with (1 + (1 - factor)), as a
 *        larger overlap makes hold harder. For example, when the factor
 *        returned from here is 0.9, then we'll multiply the overlap with
 *        (1 + (1 - 0.9)) = 1.1.
 *
 * By default, returns `0.95` if `pth_get_pth_side_user_bits()` equals
 * `PTH_5H`, `0.9` for `PTH_10H`, and `0.85` for `PTH_15H`, thus making holds 5 %,
 * 10 %, and 15 % harder respectively on keys using them.
 */
float pth_get_prediction_factor_for_hold(void);

// Prediction functions (weakly defined)
//=============================================================================
/**
 * @brief Prediction function called when a third key is pressed.
 *        Override to implement custom logic.
 *
 * @return true for HOLD, false for TAP.
 */
bool pth_predict_hold_when_third_press(void);

/**
 * @brief Prediction function called when the PTH key is released after the
 *        second key was pressed (and is still down).
 *
 * @return true for HOLD, false for TAP.
 */
bool pth_predict_hold_when_pth_release_after_second_press(void);

/**
 * @brief Prediction function called when the PTH key is released after the
 *        second key was also released.
 *
 * @return true for HOLD, false for TAP.
 */
bool pth_predict_hold_when_pth_release_after_second_release(void);

/**
 * @brief Prediction function for the minimum required overlap time for a hold
 *        when the second key is on the opposite side.
 * @return Predicted overlap time in ms.
 */
uint16_t pth_predict_min_overlap_for_hold_in_ms(void);

#ifdef PTH_FAST_STREAK_TAP_ENABLE
// These might be useful in pth_predict_fast_streak_tap.

// This correctly predicted 7.49 % of tap-holds in the data to be taps.
// The remaining 92.51 % will be handled by the normal PTH logic.
// Conversely, it mispredicted 0.66 % of the training data to be taps.
float pth_default_get_fast_streak_tap_prediction(void);

// This correctly predicted 3.46 % of tap-holds in the data to be taps.
// The remaining 96.54 % will be handled by the normal PTH logic.
// Conversely, it mispredicted 0.29 % of the training data to be taps.
float pth_conservative_get_fast_streak_tap_prediction(void);
#endif // PTH_FAST_STREAK_TAP_ENABLE

// Default prediction functions (can be used in your own weak function overrides)
//=============================================================================
/**
 * @brief The default prediction when a third key is pressed.
 *
 * @return float prediction value in [0, 1]. > 0.5f is considered hold.
 */
float pth_default_get_hold_prediction_when_third_press(void);

/**
 * @brief The default prediction when the PTH key is released after the
 *        second key was pressed (and is still down).
 *
 * @return float prediction value in [0, 1]. > 0.5f is considered hold.
 */
float pth_default_get_hold_prediction_when_pth_release_after_second_press(void);

/**
 * @brief The default prediction when the PTH key is released after the
 *        second key was also released.
 *
 * @return float prediction value in [0, 1]. > 0.5f is considered hold.
 */
float pth_default_get_hold_prediction_when_pth_release_after_second_release(void);

/**
 * @brief The default prediction for the minimum overlap time for a hold.
 *
 * @return predicted overlap time in ms.
 */
float pth_default_get_overlap_ms_for_hold_prediction(void);

// Accessor functions
//=============================================================================
pth_status_t pth_get_status(void);
pth_status_t pth_get_prev_status(void);

/**
 * @return the duration from previous press to the PTH press
 */
int16_t pth_get_prev_press_to_pth_press_dur(void);

/**
 * @return PTH_ATOM_LEFT, PTH_ATOM_RIGHT, PTH_ATOM_OPPOSITE or PTH_ATOM_SAME
 */
uint8_t pth_get_pth_atomic_side(void);

/**
 * @return the user bits of the PTH's side
 */
uint8_t pth_get_pth_side_user_bits(void);

bool pth_is_second_same_side_as_pth(void);

/**
 * @brief Get the keyrecord of the currently active Predictive tap-hold (PTH) key.
 *
 * @return keyrecord_t The keyrecord, or a record with an empty key position if none active.
 */
keyrecord_t pth_get_pth_record(void);

keyrecord_t pth_get_second_record(void);

/**
 * @brief Get the keycode of the currently active Predictive tap-hold (PTH) key.
 *
 * @return uint16_t The keycode (e.g., LCTL_T(KC_A)) or KC_NO if none active.
 */
uint16_t pth_get_pth_keycode(void);
uint16_t pth_get_second_keycode(void);
uint16_t pth_get_prev_press_keycode(void);

/**
 * @return second keycode on the layer that contains the PTH key.
 *         This is useful when an instant Layer-Tap (`LT`) is active.
 *
 * @return KC_NO if PTH is not an LT or was not held instantly.
 */
uint16_t pth_get_second_keycode_on_same_layer_as_pth(void);

bool pth_is_second_tap_hold(void);

/**
 * @brief Check if the PTH module is currently processing key events. If this
 *        returns `true` inside `process_record_user`, then you know the
 *        current event comes directly from PTH.
 *
 * @return true If the event is internal to predictive_tap_hold.
 * @return false If the event originated from the normal QMK matrix scan.
 */
bool pth_is_processing_internal(void);

// Utility functions
//=============================================================================
/**
 * @brief Use this to access the pth_side_layout array comfortably.
 */
uint8_t pth_get_side_from_layout(keypos_t pos);

/**
 * @return true if the supplied keycode is a tap-hold keycode.
 */
bool pth_is_tap_hold_keycode(uint16_t keycode);

/**
 * @return all currently active mods (may include oneshot mods if enabled).
 */
uint8_t pth_get_all_8_bit_mods(void);

/**
 * @brief Get the 8-bit mods from a mod-tap keycode.
 *
 * @param keycode The mod-tap keycode.
 * @return the corresponding mods (e.g., `MOD_BIT_LCTRL` or `MOD_MASK_CTRL`).
 */
uint8_t pth_get_8_bit_mods_of_mod_tap(uint16_t keycode);

/**
 * @return true if the supplied key is mod tap that contains any of the
 *         supplied 8-bit mods.
 */
bool pth_is_mod_tap_with_any_mods_of(uint16_t keycode, uint8_t mods_8_bit);

/**
 * @brief The default implementation of `should_hold_instantly`.
 *
 * @return true, unless Caps Word is active, or PTH is a mod tap and some of
 *         the mods it contains are already active.
 */
bool pth_default_should_hold_instantly(uint16_t keycode, keyrecord_t* record);

#ifdef __cplusplus
}
#endif
