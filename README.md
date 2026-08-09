# Modules

## List
| Name                                                                                         | Path                           | Description                                                                                                                                                                                                                                                                                                                                                                          |
|----------------------------------------------------------------------------------------------|--------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| [Predictive Tap-Hold](https://github.com/jgandert/qmk_modules/tree/main/predictive_tap_hold) | `jgandert/predictive_tap_hold` | Predictive tap-hold module that analyzes typing dynamics to provide responsive taps and accurate holds without a static `TAPPING_TERM`. It defaults to an ergonomic bilateral model, using interactions between hands to determine intent, but is fully configurable to allow for same-hand holds and other custom behaviors.                                                        |
| [Magic Layer Alt-Tab](https://github.com/jgandert/qmk_modules/tree/main/magic_layer_alt_tab) | `jgandert/magic_layer_alt_tab` | This module provides a streamlined way to switch applications. Once you are on a layer of your choice, pressing `LALT(KC_TAB)` or `LSA(KC_TAB)`, will cause this module to hold <kbd>Alt</kbd>. You can then continue tapping these keys to navigate through your applications. <kbd>Alt</kbd> is automatically released as soon as you press any other key or deactivate the layer. Optionally, the modifier can be set to <kbd>Super</kbd>/<kbd>Cmd</kbd> rather than <kbd>Alt</kbd> to support macOS or other desktop environments.|

## Add Modules to Your Build
1. Make sure your copy of QMK (or Vial-QMK) is up to date. If it is, there will be a `modules` folder. If it isn't, [update it](https://docs.qmk.fm/newbs_git_using_your_master_branch#updating-your-master-branch). If you're using QMK userspace, add a `modules` directory.
2. In your QMK or userspace directory, execute `git submodule add https://github.com/jgandert/qmk_modules.git modules/jgandert` and `git submodule update --init --recursive`
3. If there's no `keymap.json` in the same folder as the `keymap.c`, create it and add the following or whichever modules you like to add:

```json
{
  "modules": ["jgandert/magic_layer_alt_tab", "jgandert/predictive_tap_hold"]
}
```

If there's one already, you have to merge it so that it is a valid JSON file. For example, adding the PTH module to
this:

```json
{
  "maintainer": "you"
}
```

Would look like this:

```json
{
  "maintainer": "you",
  "modules": [
    "jgandert/predictive_tap_hold"
  ]
}
```

---

As an alternative to step 2. you can download this repository from GitHub and extract it so that this `README.md` is in `{qmk or userspace directory}/modules/jgandert`.

Sometimes, it is useful to run `qmk clean` before building a keymap again. For example, I just now had the situation where a config option was active that I had already disabled a while ago. After running and recompiling, the issue was gone.

Further information is available in [the official docs](https://docs.qmk.fm/features/community_modules).
