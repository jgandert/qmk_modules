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

#include QMK_KEYBOARD_H

#ifndef MAGIC_LAYER_ALT_TAB_LAYER
#    error "Magic Layer Alt-Tab requires MAGIC_LAYER_ALT_TAB_LAYER to be defined. Please set it the layer of your choice in your config.h"
#endif

static bool alt_tab_running = false;

static void disable_magic_layer_alt_tab(void) {
    unregister_mods(MOD_BIT(KC_LALT));
    alt_tab_running = false;
}

bool process_record_magic_layer_alt_tab(uint16_t keycode, keyrecord_t *record) {
    const bool is_alt_tab_key = keycode == LALT(KC_TAB) || keycode == LSA(KC_TAB);

    if (!is_alt_tab_key || !IS_LAYER_ON(MAGIC_LAYER_ALT_TAB_LAYER)) {
        if (alt_tab_running && keycode != MS_BTN1) {
            disable_magic_layer_alt_tab();
        }
        return true;
    }

    if (!alt_tab_running) {
        register_mods(MOD_BIT(KC_LALT));
        alt_tab_running = true;
    }

    if (record->event.pressed) {
        if (keycode == LALT(KC_TAB)) {
            tap_code16(KC_TAB);
        } else if (keycode == LSA(KC_TAB)) {
            tap_code16(LSFT(KC_TAB));
        }
    }
    return false;
}

void post_process_record_magic_layer_alt_tab(uint16_t keycode, keyrecord_t *record) {
    if (alt_tab_running && !IS_LAYER_ON(MAGIC_LAYER_ALT_TAB_LAYER)) {
        disable_magic_layer_alt_tab();
    }
}