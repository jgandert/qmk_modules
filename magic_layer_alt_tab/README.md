# Magic Layer Alt-Tab
Activate a layer of your choice (e.g. with `MO` or `LT`) and then press one of the <kbd>Alt</kbd>-<kbd>Tab</kbd> keys.

Only while you are on that layer, and only after you press one of the two <kbd>Alt</kbd>-<kbd>Tab</kbd> keys, will <kbd>Alt</kbd> be held.

* `LALT(KC_TAB)` will tap `KC_TAB`

* `LSA(KC_TAB)` will tap `S(KC_TAB)`

When you leave said layer, or press another key on that layer, <kbd>Alt</kbd> will be released.

You may also, optionally, set <kbd>Super</kbd>/<kbd>Cmd</kbd> rather than <kbd>Alt</kbd> as a modifier, most likely if you use macOS (see Installation).

## Installation
1. Follow [**the steps described in the parent README**](../README.md) to add this repository to your QMK or userspace, and this module to your `keyboard.json`.
1. Choose a layer in your keymap. Add `LALT(KC_TAB)` and/or `LSA(KC_TAB)` to it. If you need <kbd>Super</kbd>/<kbd>Cmd</kbd> rather than <kbd>Alt</kbd>, use `LGUI(KC_TAB)` and/or `LSG(KC_TAB)` (this makes step 4 required).
1. Add `#define MAGIC_LAYER_ALT_TAB_LAYER ` plus the layer to your `config.h`. (e.g. `#define MAGIC_LAYER_ALT_TAB_LAYER 3`)
1. (*Optional*) If using <kbd>Super</kbd>/<kbd>Cmd</kbd>, also add `#define MAGIC_LAYER_GUI_MOD` in your `config.h`.
