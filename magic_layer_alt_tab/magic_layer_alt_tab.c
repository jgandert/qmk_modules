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

// Windows uses Alt-tab, macOS Cmd-tab, and Linux desktops may use any or both.
// By default, the modifier key used is Alt.
// To use Super/Cmd instead of Alt, define MAGIC_LAYER_GUI_MOD in your config.h.
#ifdef MAGIC_LAYER_GUI_MOD
#   define MAGIC_MOD KC_LGUI
#   define MAGIC_COMBO LGUI(KC_TAB)
#   define MAGIC_COMBO_SHIFTED LSG(KC_TAB)
#else
#   define MAGIC_MOD KC_LALT
#   define MAGIC_COMBO LALT(KC_TAB)
#   define MAGIC_COMBO_SHIFTED LSA(KC_TAB)
#endif

static bool alt_tab_running = false;

static void disable_magic_layer_alt_tab(void) {
    unregister_mods(MOD_BIT(MAGIC_MOD));
    alt_tab_running = false;
}

bool process_record_magic_layer_alt_tab(uint16_t keycode, keyrecord_t *record) {
    const bool is_alt_tab_key = keycode == MAGIC_COMBO || keycode == MAGIC_COMBO_SHIFTED;

    if (!is_alt_tab_key || !IS_LAYER_ON(MAGIC_LAYER_ALT_TAB_LAYER)) {
        if (alt_tab_running && keycode != MS_BTN1) {
            disable_magic_layer_alt_tab();
        }
        return true;
    }

    if (!alt_tab_running) {
        register_mods(MOD_BIT(MAGIC_MOD));
        alt_tab_running = true;
    }

    if (record->event.pressed) {
        if (keycode == MAGIC_COMBO) {
            tap_code16(KC_TAB);
        } else if (keycode == MAGIC_COMBO_SHIFTED) {
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
