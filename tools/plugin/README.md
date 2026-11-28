# tools/plugin

The native plugin API's test and example (`runtime/plugin/`).

| File | What it is |
|---|---|
| `probe.sh` | Builds `probe_plugin.c` as the mod `plugin-probe` (`build/mods/plugin-probe/plugin/`) and runs `build/mkw --probe-plugin` with it on: a call through main's PLT reaches the plugin's override by name, an indirect call to the game's definition its override by address |
| `example.sh` | Builds `example_plugin.c` into an importable mod, as a folder (`build/mods-import/example-plugin/`) and as its `.zip`, the two shapes the launcher's Mods page imports (Linux `.so`) |
| `example_plugin.c` | A plugin that changes nothing in the game: a notice when the game code is loaded and a box with the versions in a corner (`notice`, `overlay_set`, `overlay_show`), and an options page (`mkw_mod_page`) with every `MkwPage` widget, in English and Brazilian Portuguese: the box on or off, its corner, the notice's text and a list of names to add and remove, read back in the game with `setting_get` |
| `overlay.sh` | Builds `overlay_plugin.c` into an importable mod (`build/mods-import/overlay-example/`, and its `.zip`): the Linux `.so`, or with `dll` and a MinGW compiler the Windows `.dll` |
| `overlay_plugin.c` | The overlay of API version 5, in C with no UI library: a card at the top right drawn with `MkwOverlay.triangles` and a texture of its own (`texture_create`), placed under the runtime's FPS display (`MkwOverlay.perf`); a page opened by Shift+Tab or L + R + D-pad down (`key_state`, `input_filter`) that takes the input (`overlay_capture`), shows the game's live picture in a box (`overlay_picture`) and what the keyboard, the mouse and the pad send it; with API 6 the runtime's Sound tab listed on the page and changed with the arrows (`settings_tabs`, `settings_rows`, `settings_set`) and F1 / Esc / the pad's menu shortcut opening the page instead of the runtime's menu (`menu_redirect`); and with `MKW_OVERLAY_EXAMPLE_FAULT=1` a fault on purpose, which the runtime survives (`overlay_on_fault`) |
| `probe_plugin.c` | The plugin it loads: one of each hook, chained to the runtime's own handler and to the game's original. Also the smallest example of a plugin |
