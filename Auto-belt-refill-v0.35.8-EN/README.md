# Auto Belt Refill

A [D2RLoader](https://d2rloader.net) plugin for Diablo II: Resurrected
that keeps your potion belt filled from the inventory.

Two modes:

- **Manual refill** - press the configured fill key (or run `belt-fill` in
  the console) and every empty belt slot is refilled from the inventory,
  following each column's memory.
- **Auto-refill** - the plugin watches the belt and refills a slot as soon
  as its potion is consumed. It also reacts to state changes: entering a
  game with empty slots, and potions looted straight into the inventory.

## How it works (player view)

- Each belt **column** has a memory: an ordered list of item codes
  (refill priority). A column only ever receives potions from its list.
- The memory comes from the config file, or is **learned in-game**: place a
  potion into a column's hotkey row (the bottom row of the belt, keys 1-4), press the snapshot key,
  or let the first fill pass record it. Removals (drinking, dragging out,
  dropping, selling) never change the memory.
- A column that is fully empty keeps its memory and stays empty until the
  player re-anchors it - the plugin will never guess and stuff a different
  potion family into an empty column (this also applies to the manual fill).
- Refills go through the game's own slot-search routine (the same code path
  as shift-clicking a potion), so slot routing always matches vanilla
  behavior.

## Requirements

- D2RLoader (PluginSDK-based loader).
  **Tested with: 1.1.0-beta and 1.2.1-beta.**
- **D2R build 3.2.0 (92777). Tested with: 3.2.0 (92777) and 3.3.0 (93854)** —
  the two builds ship byte-identical code, so the same binary works on both.
  The plugin hooks internal game functions by hardcoded RVA (CanPutInBelt,
  GetFreeBeltSlot, BeltTransfer, Pickup); a different game build will fail
  the startup RVA verification and the plugin safely refuses to load.
  Re-deriving the addresses is required for other builds.

## Installation

The repo mirrors the loader's folder layout - drop the included `plugins\`
and `config\` folders into your `d2rloader\` folder (or copy the two files
to the same places):

1. `plugins\d2rl-auto-belt-refill.dll` -> `d2rloader\plugins\` (prebuilt,
   included in this repo).
2. `config\auto-belt-refill.toml` -> `d2rloader\config\` (optional: a
   default config is generated on first start if none exists).
3. Restart the game. The loader log should show
   `AutoBeltRefill vX.Y.Z loaded.`

There are **no default hotkeys**. Set `fill_key` / `snapshot_key` in the
toml if you want keys (mod players: pick keys your other mods don't use),
bind them in the game's **Options > Controls > Auto Belt Refill** category
(they show up there as unbound entries), or leave them empty and use the
console commands. In-game bindings are stored by the game itself: they
persist across restarts and always override the toml default.

## Configuration

See `config\auto-belt-refill.toml` - every key is commented. Quick summary:

| Key | Meaning |
| --- | --- |
| `enabled` | Master switch. |
| `fill_key` / `snapshot_key` | Hotkeys; `""` = unbound. |
| `auto_refill` | Watch-the-belt automatic refills. |
| `auto_poll_frames` | Poll interval in frames (2..360, default 15). |
| `prefer_large` | Prefer higher potion grades when refilling/learning. |
| `debug_logs` | Diagnostic log switch - keep `false` while playing. |
| `[column1]`..`[column4]` | Per-column `consumables` list + `locked` freeze. |

Potion codes: `hp5..hp1`, `mp5..mp1`, `rvl`, `rvs`; also `vps`, `yps`,
`wms`, `tsc`, `isc`. Mod-added consumables work by code.

## Console commands

Open the console with `` Ctrl + ` ``:

| Command | Action |
| --- | --- |
| `belt-fill` | Refill every empty belt slot per the column memories. |
| `belt-place` | Diagnostics: move one inventory item into the belt. |
| `belt-policy` | Read-only: per-column memory + current belt contents. |
| `belt-config` | Read-only: the parsed config file. |
| `belt-snapshot` | Re-record every column from its hotkey row (the bottom row of the belt). |
| `belt-reset` | Forget the learned memories; the next fill re-records them. |
| `belt-scan` | Read-only: dump belt + inventory layout. |
| `belt-watch` | Read-only: hook statistics since the last invocation. |
| `belt-verify` | Read-only: check that the hooked RVAs match this build. |

## Building from source

- Visual Studio 2022+ (MSVC, C++20), CMake >= 3.28, and the D2RLoader
  **PluginSDK** (point `AUTOBELTREFILL_SDK_DIR` at your checkout).

```bat
cmake -S . -B build -D AUTOBELTREFILL_SDK_DIR="D:/path/to/PluginSDK"
cmake --build build --config Release
```

## Notes and known limits

- The game routes potions by **family** to the leftmost matching column.
  A locked column sitting left of a same-family column intercepts the
  routing; potions it refuses are discarded by the game and never reach the
  columns to its right. Keep locked columns stocked, or unlock them, if
  lower grades must reach a right-hand column.
- Memory is per session: entering a new game re-learns the columns
  (configured lists reload from the toml).
