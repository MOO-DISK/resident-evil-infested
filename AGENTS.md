# Resident Evil 1 for PC Decompilation Project

## What this is

A decompilation of the 1997 Resident Evil 1 PC release (the USA build, as shipped
by GOG — same binary as the 1997 retail disc). The code is reconstructed as a
Win32 C++ project that reproduces the original binary's behaviour, not a
redesign: original function and global addresses stay in comments, and the
original's quirks are preserved rather than cleaned up.

## Current state

- **USA port: function-complete and playable.** 1800 of 1801 in-scope game
  functions are implemented; the single remainder
  (`ent_setanim_walkto_helper`, `0x0040c3d0`) is documented. The Release build
  has been play-tested end to end by the user. Remaining work is bug fixes and
  new features — not finding missing functions.
- **JPN (Biohazard, MediaKite release): partial.** The Japanese text encoding,
  the message / item-name / item-description / font tables and the save/load
  screen strings are ported, generated into `src/game/JpnTextTables.cpp` and
  selected at runtime with `config.ini [Assets] Version=JPN`. Everything else
  that release has and the USA build does not is **not ported yet**.
- **Linux port: playable.** A native 32-bit binary (`-m32`) shares every game
  translation unit and swaps the platform layer plus the renderer backend
  (SDL2 + OpenGL 3.3 core, ffmpeg for FMV). Verified running on Ubuntu and
  CachyOS, keyboard and gamepad. See `docs/LINUX_PORT.md`.
- **Zombie mod / asymmetric multiplayer (branch `playable_zombie`): active
  work.** See "Zombie mod" below — that is what current sessions work on.

## Project goal

The USA release is no longer the end of the project. The goal now is to **add
features from other RE1 releases** on top of this port, starting with the
Japanese MediaKite version. Extend behaviour; keep the existing USA behaviour
as the default so the two do not silently diverge.

On the `playable_zombie` branch the current goal is the zombie mod: a 1 director
vs 3 survivors multiplayer mode built on the port (see the section below).


## Tools (`tools/`)

`tools/` holds decompilation tooling and the verifiers whose artifacts live
there; `tests/` holds the test harness (`check_platform_boundary.py`,
`compile_linux.sh`, `rgba_to_png.py`). Keep that split.

Most tools read the original binaries (`assets/ResidentEvil.exe`,
`assets/Biohazard.exe`) or the shipped assets (`assets/USA|JPN/...`) and print
decoded data — they are how a table is confirmed against the original instead
of guessed. Run them from the repo root.

**Extraction from the original binaries**
- `decode_re1.py` — decode the game's custom text encoding (`PrintFormattedText` / `PrintText8x14` strings).
- `decode_msg_table.py` — decode the global message table from `ResidentEvil.exe` for comparison with the `STR()` sources in `Globals.cpp`.
- `extract_global_messages.py`, `extract_item_descriptions.py` (table at `0x004C6160`), `extract_string_table.py` — pull specific tables out of the original binary.
- `mine_effect_tables.py`, `gen_effect_c_tables.py` — mine the billboard-effect tables and emit them as C initializer lists for `EffectSystem.cpp`.
- `mine_room_scd.py` — dump and decode an RDT's per-frame SCD room script.
- `scd_widths.py` — derive SCD command argument widths from the original's dispatch table (`0x4c1110`).
- `progress_report.py` — how many original functions are implemented in `src/` (needs a Ghidra function-entry dump; see its header).
- `gen_ps1_audio_manifest.py` — name every PS1 sound effect, voice clip and BGM track after the PC `.wav` it replaces (PC slot tables for the candidates, audio correlation to confirm) and emit the asset migrator's `Ps1AudioManifest.h`. Needs a raw PS1 image.

**Verifiers — run them after touching what they cover**
- `verify_msg_encoding.py` — replicates the `STR()` `Encoded` constructor and compares the encoded global messages against the original bytes.
- `verify_msg_fixes.py` — same idea for the fixed `STR()` sources, byte for byte.
- `verify_dc_item_models.py` — cross-checks the Director's Cut item-view mapping (`ItemModels.cpp`) against the generated DC item lookup and the shipped `ITEM_M2` art, and checks `g_ItemsImageBuffer` covers the DC sprite sheet.
- `verify_dc_entity_models.py` — the DC model table (`0x8008d03c`, via the Ghidra bridge) resolved to file names through the disc's ENEMY directory, entity id `0x16`'s handler read out of all 14 PS1 stage overlays, and the models `dc/EntityModels.cpp` names against the overlay.
- `verify_dc_save_mode.py` — recomputes every `BioCardLayout` offset from the struct and checks it against each field's own comment, the DC mode byte → flag-bit mapping against the PS1 load path, and the overlay font's four CLUT rows against the base glyph block and the PS1 colour columns.

**Asset and file inspection**
- `dump_tim.py` — parse a PSX TIM and write a viewable PPM.
- `dump_ivm.py` — parse an `.IVM` item view (TIM + TMD), render its texture page to a PNG and print both headers.
- `dump_item_pix.py` — render rows of an item sprite sheet (`ITEM_ALL.PIX` / `Item_all_dc.pix`) through `STATUS.TIM`'s palette as a PNG grid.
- `dump_esp_sprite.py` — decode a sprite region of an effspr TIM (pixels + CLUT, with the STP bit).
- `pak_view.py` — view `.pak` background images (LZW → TIM → PNG/PPM).
- `dump_room_masks.py` — per-camera room-mask (overlay) sprite tables from RDTs.
- `dump_init_doors.py` — init-script `door_set` records and other room-action entries.
- `dump_init_scd.py` — an RDT's initialization SCD (header `+0x60`).
- `dor_disasm.py`, `door_script_parse.py` — disassemble / parse the `.dor` door-animation scripts.
- `evt_disasm.py` — an RDT's SCD *event* scripts (the cutscene VM, not the command stream `mine_room_scd.py` handles).
- `sim_zone_walk.py` — simulate the zone-graph walkers with the original's exact memory model over real RDT zone tables.
- `estimate_zombie_route_time.py` — turns each seed's fastest escape into seconds (room walks at run speed from the RDT door/pickup positions, fixed door/pickup/crest costs; solo and a coordinated team); report `tools/zombie_route_time_report.json`.
- `gen_spawn_spots.py` — three monster spawn spots per mansion room (floor flood-fill inside the collision outline and camera zones, away from doors) and each room's monster cap by its floor area (`CAP_STEPS`); writes the zombie mod's `src/game/mods/ZombieSpawnSpots.cpp` (generated - do not edit by hand).

**Japanese (Biohazard.exe)**
- `jpn_font_table.py` — glyph map of the JPN font (`data\FONT.TIM`).
- `jpn_msg_decode.py` — decode / re-encode the JPN text tables straight from the JPN executable.
- `gen_jpn_text.py` — generate the C++ side from those two (`JpnTextTables.cpp`, `JpnFontTable.h`, and `test_str_jp.cpp`).

**Editors (open in a browser, no build)**
- `rdt_event_editor.html` — RDT event editor; `node tools/test_rdt_editor.js` runs its headless tests (add `--quick` for assertions only).
- `save_editor.html` — PC save-file editor.

**Linux** — `package_linux.sh` and `elf_needed.py`; both are described in the Linux port section above.

**Asset migration** — `tools/asset_migrator/` is a standalone Qt 6 GUI for the
player-facing asset import: a PC tab (a USA/JPN tree from a folder or a disc
image, optional AVI→MP4) and a Director's Cut tab (a PS1 image only, so the
`.STR` CD-XA audio survives; builds the `DC/` overlay, `.BSS`→`.pak`
backgrounds and `.STR`→`.mp4`). It replaces the old `scripts/build_dc_assets.py`
and is **not** part of the game build. Its core is Qt-free C++17, so
`re1am_selftest.exe` runs it headlessly; see `tools/asset_migrator/README.md`.

`test_str_jp.cpp` is **generated** by `gen_jpn_text.py` — do not edit it by hand.

## Code style

- The original is C++ compiled into a Win32 game. The decomp follows that style
  and keeps the original's structure.
- Strictly 32-bit: no 64-bit libraries, types or arithmetic assumptions.
- Capcom's **Marni System** is a DirectX 5 wrapper over the PSYQ (PS1 SDK). On
  modern Windows it is backed by a DX11 / XInput layer (`MarniDX`,
  `MarniXInput`); on Linux by OpenGL + SDL2. Both keep the original Marni method
  set — see `docs/MARNI_SYSTEM.md`.

## Platform boundary

`src/game/` must stay OS-agnostic: no `<windows.h>`, no Win32 API calls.
Everything OS-specific sits behind `src/platform/` (`platform.h` plus `win32/`
and `linux/`). `tests/check_platform_boundary.py` enforces this in CI — run it
before committing.

## Linux port

- `CMakeLists.txt` is the source-of-truth translation-unit list for Linux;
  `Game.vcxproj` for Windows. **Keep both in sync when adding a file.**
- Backends: `src/platform/linux/{platform,input,audio,video,config,crash,main,stubs}.cpp`
  and `src/marni/MarniDX_GL.cpp` + `src/marni/MarniGLFuncs.{h,cpp}` (hand-rolled
  GL loader, no glad/GLEW).
- Build on a 64-bit host with 32-bit support (WSL Ubuntu 24.04 is the reference
  environment):
  ```
  cmake -S . -B build/linux -DCMAKE_BUILD_TYPE=Release
  cmake --build build/linux -j
  ```
  Dependencies and the Arch/CachyOS package names are in the README.
- The binary can be launched from anywhere: assets and saves come from
  `config.ini` (`[Assets] Path`, `[Save] Path`), resolved against the
  **executable's** directory. Defaults are `<exe dir>/USA` and
  `<exe dir>/SAVE` (case-resolved against the filesystem).
- Headless test hooks (`src/platform/linux/main.cpp`):
  `--press <SDL scancode> <frame> [hold]`, `--capture <file> [frames]` (writes
  `RE1CAP <w> <h>\n` + RGBA; convert with `python3 tests/rgba_to_png.py`), and
  `RE1_DEBUGLOG=1` to append trace output to `re1_debug.log`. A fatal signal
  writes a symbolized `crash.log`.
- Portable bundle for other distributions: `bash tools/package_linux.sh` →
  `dist/residentevil-<version>-linux-x86/` (binary + bundled 32-bit libraries +
  launcher). Game data is never bundled.
- `python3 tools/elf_needed.py <binary>` prints the ELF class and `DT_NEEDED`
  order — use it when a run fails with "error while loading shared libraries".

## VTable calling conventions

When rewriting code that calls through raw vtable pointers (not C++ virtual
methods), check the calling convention used by the vtable's adapter layer:

- **CMarniDirect3D** vtables use `__cdecl` wrappers (`self` as first stack arg)
- **CMarniViewport2** vtables use `__stdcall` wrappers (callee cleans stack, `RET N`)
- **CMarniBits** vtables use `__cdecl` wrappers (`self` as first stack arg)

A mismatch (e.g. calling a `__stdcall` vtable entry with a `__cdecl` function
pointer type) causes Run-Time Check Failure #0 at runtime. See
`docs/CLASSES_AND_VTABLES.md` for the full convention table and adapter
signatures.

## Zombie mod (`src/game/mods/`)

An asymmetric multiplayer mode. Everything is gated on `g_bPlayAsZombie`
(`zombie_mode_armed()`), so the USA game is unchanged when the mode is off.

**Working with the user**
- **Never launch the game yourself.** The user play-tests by hand. Build it,
  then ask them to test. Nothing counts as fixed until they confirm it.
- Commit only when the user asks.
- Debug traces go through `RE1_DEBUGLOG=1` into `re1_debug.log`: `[shot pN]`
  shot audit, hits, monster hits and hurts. Ask the user for that log.
- Keep both `CMakeLists.txt` and `Game.vcxproj` updated. The Windows build has
  not been compiled since the multiplayer work started. Only Linux is built and
  checked with `tests/check_platform_boundary.py`.

**Design**
- **Roles** (`ZM_NET_OFF` / `ZM_NET_ZOMBIE` / `ZM_NET_SURVIVOR`):
  - Player 0 is the host and the **director**. The director possesses and places
    monsters.
  - Players 1–3 are **survivors**. Each picks a different character: Chris,
    Jill, Barry, Rebecca, Richard or Enrico (`ZombieNet.h` enum 0–5).
    `net_grant_char` keeps the picks unique.
  - Everyone plays Chris's scenario; the character picks the model and the
    perks (`ZombiePerks.cpp`): max health 140 + (toughness - 8) * 22 (Barry 10,
    Chris/Enrico/Richard 8, Jill/Rebecca 6), a starting kit each (Barry: magnum,
    no ammo; Jill: lockpick; Richard: the sheet music), Jill, Rebecca and
    Richard can play the bar's piano (`ZombiePiano.cpp`, below), Chris +50% world ammo pick-ups (rounded down;
    dropped ammo, including death drops, gets no bonus), Jill's lockpick
    opens sword-key doors, Barry's gun shots penetrate, Rebecca's spray heals
    survivors within 3000 (HEAL event), Enrico +10% movement, Richard's radio:
    OPTIONS opens the director's map look-only (`zm_map_set_read_only`) - which
    rooms have monsters; he stands still while it is up. Everyone also keeps
    the knife.
  - Single player is director vs an AI survivor (`ZombieSurvivor.cpp`).
- **No story.** The intro, title and in-game FMVs are skipped
  (`zombie_mode_skip_intro`, `cmd_fmv_set`). Story events are not started:
  `cmd_scd_event_create` asks `zombie_mode_skip_scd_event`, which scans the
  event script (and the events it starts) for a voice line or a movie and, if
  found, makes its flag writes (scenario/lock/enemy/item banks) instead.
  A monster's entrance is skipped too, voice or not: an event that takes the
  control (message flag 0x100 cleared) and drives an enemy (back passage
  Hunter, 2F small library, attic). A skipped scene's own script also has its
  main-state/message flag writes and `room_action_arm` made in order (control
  comes back - the roofed passage's room script takes it before the radio
  call) and a player left in state 8 by the room script is stood back
  (`zm_evt_release_player`). Walk-in zones (`create_room_event`, probe flags
  without 0x80) are filtered the same way (`zombie_mode_skip_room_event`);
  action-press events (stairs, puzzles) still run.
- **The return mansion.** The mode always plays stages 6/7 (0-indexed 5/6):
  `zombie_mode_new_game` sets `SCENARIO_FLAG_STAGE_VARIANT`. The first visit
  carries 14 of the mansion's 58 rooms as 4-byte stub RDTs.
- **Randomized scenario** (`ZombieRandom.cpp`): one seed per game (the host's,
  or the clock's in single player), and every copy builds the same scenario
  from the RDTs. Which mansion key opens each key-locked door (13 locks, by
  lock flag), where the keys and the four crests lie (assumed fill over the
  23 pool spots - top-level, un-armed item_model_set records that held a key,
  crest, weapon or ammo), and the weapons/ammo over the rest. Puzzle and
  one-way locks start open (all but the crest door, lock flag 23). Hooks:
  `cmd_item_model_set` → `zombie_mode_item_spot` (patches the record's item
  and quantity), `door_try_enter` → `zombie_mode_door_need`. The lobby's
  route map (Aim) shows the key doors and which key each takes; not where
  the keys lie. `[random]` lines in the debug log list the scenario.
  - Looks: a spot whose item changed shows the new item - its inventory view
    model (`item_m2/iNNv.ivm`), rescaled to a size for its kind, stood on the
    floor (turned flat when it is tallest along y), its texture on the mod's
    page counter (`zombie_mode_item_look` in `cmd_item_model_set`).
  - Route proof (`rnd_reach`): doors whose trigger a script can switch off
    follow the reviewed `kDoorGates` table (unlisted ones count as closed);
    a room counts only if the main hall is reachable back from it; keys,
    crests and puzzle tools stay out of the furniture rooms in
    `kUnverifiedRooms`. See `docs/INFESTATION_PUZZLE_ACCESS.md`.
  - New spots: 16 rooms a game get an extra pickup on one of their spawn
    spots, under a roomItems flag no mansion script uses, as an
    item_model_set record of our own run through `cmd_item_model_set`
    (`zm_random_room_loaded`, from `zombie_mode_room_spawn`); the RDT's
    `item_count` is raised so `render_room_objects` draws it. They are in the
    pool; spots left over after keys, crests and weapons get supplies.
  - Key items in leaf rooms (rooms with one neighbouring room, `s_leaf`,
    `rnd_key_item_room`; not a puzzle's room, the shotgun rooms, the
    storeroom or an unverified room): every such room gets a new pool spot
    first; keys and crests take a leaf room each when one is free, then the
    broken shotgun and the pick axe (route cost `RND_TOOL_NEAR`..`FAR`, 5..9
    crossings, `rnd_route_cost`), the sheet music and the chemical, each in
    a leaf room of its own (`rnd_place_key_items`). The bar's own sheet
    music record becomes a supply.
  - Heavy weapons come from five puzzles only (`kPuzzles`: piano bar
    alcove, greenhouse past the plants, tiger statue's red gem, large
    gallery portraits, armor room), each in its own reward slot.
    `rnd_rank_puzzles` orders them by route cost to the puzzle room through
    the room of the item it takes (keys picked up on the way; Richard's
    sheet music ignored): the nearest two a grenade launcher or flamethrower
    each, the next two the Python, the farthest the rocket launcher - never
    the piano. Loads: Python 6, launcher 6 explosive, flamethrower 240,
    rocket launcher 4. No heavy weapon lies on the floor. Tier 1 (handgun,
    shotgun) lies anywhere. Ammo lies only for the heavy weapons in the game
    (magnum rounds also with Barry, the three grenade kinds, fuel 120).
    `rnd_weapons_refresh` deals the floor's weapons and ammo again while
    Barry's presence changes, until every seated survivor's STATE has come
    (`rnd_party`). `[random] puzzle` lines.
    The flamethrower's reload keeps its full count in the mode
    (`menu_ammo_loaded` in MainMenu.cpp; the original keeps 7 bits).
  - Ammo doubling: up to 48 more new spots (a room takes up to 3 new spots,
    one per spawn spot) are held back for ammo only; `rnd_fill_ammo` puts as
    many ammo pickups on them as the scenario already placed (`[random] ammo:`
    in the debug log) and leaves the rest empty. Pickups themselves are not
    doubled.
  - Shotgun puzzle (`ZombieShotgun.cpp`): first-visit trap/living RDTs restored
    under return-stage IDs. No keys/crests in either room. One broken shotgun
    and one Pick Axe lie in distinct keyless rooms 3–8 door crossings from the
    hall. USE the broken shotgun at the empty mounting plate; Action at the
    passage-side trapped door with the pick axe rescues living occupants and
    the rescuer through a black-screen transition. Host-authorized event 31,
    reusable pick axe, 60-second ceiling timer and persistent broken door.
    State and committed replacement/rescue reconcile on reconnect. CPU tests:
    `tests/test_zombie_shotgun.py`; user play-testing still required.
- **Win condition:** a survivor opening the storeroom's (stage 1 room 0x1B)
  door out to the courtyard requests an escape from the host (`zm_is_last_door`).
  Only the host confirms the final result: escape=0, timeout=1, all-dead=2
  (`zm_all_survivors_dead`; departed survivors no longer count). Received
  escape requests precede timeout/all-dead checks in the same host update;
  later requests cannot reverse a confirmed outcome. Survivors wait for the
  host's result, including its elapsed time. WIN=9 requests are private
  `{player,0,exit room,health,seed low,seed high}`; confirmations are
  `{winner/-1,reason,elapsed seconds,seed low,seed high}`. Critical results
  bypass the normal inbox and are retained if game initialization is unfinished.
  Seed checks exclude old-match results. Untested by the user.
- **Dead survivors stay in the match:** `zombie_mode_hold_death` keeps
  game_loop from ending the game on a survivor's death; its copy keeps
  sending STATE.
- **Spectating** (`ZombieSpectate.cpp`): 4 s after its death a survivor's
  copy watches a living survivor (left/right picks the next one): it loads
  that survivor's room by a door record of its own, as the map jump does,
  and the camera follows the watched position (`zombie_mode_camera_target`,
  `zm_fix_camera_at` on arrival). A door taken by the watched survivor is
  followed after 400 ms. From the start of the watching its STATE is frozen
  on the corpse (`zm_net_freeze_state`: stage, room, spot, pose) with flag
  0x10, so the body stays where it fell on every copy; STATE also carries
  the room the copy has loaded (`viewStage` / `viewRoom`). A spectating
  player is never "here" (`zm_player_here`) and never the owner (with
  nobody else in the room, its copy runs nothing); a room's owner sends
  ENEMIES for a spectator watching it (`zm_spectated_here`). It counts gone
  from the corpse's room once it has left it (`zm_spec_room_exit`, after the
  room capture), or at once when someone else is there to run it. Its pad
  is cleared (the hidden player entity stays put) and its story-flag watch
  is off (`zm_world_story_rebase`). `[spec]` lines in the debug log.
- **Reviving** (`ZombieSpectate.cpp`, multiplayer survivors only): hold Action
  within 800 units of a frozen corpse for 5 s, consuming a First Aid Spray;
  Rebecca takes 2 s and can also use a green herb. Returns at 25% max health
  (Rebecca: 50%). No deadline after death; once per survivor per match.
  Release, hurt or grab cancels. REVIVE=21 carries `{target player, health}`.
  The host reserves a target before the item is consumed: negative health is
  a private reservation request, zero cancels/refuses, and the positive commit
  is broadcast (including an echo to the reviver). Simultaneous revivers spend
  only the winner's item.
  The target returns from spectating to the corpse room, unfreezes STATE and
  runs player state 0 to restore normal control; the stats death time clears.
  A revivable corpse still counts dead for the director's all-dead win.
  Tuning constants are together in ZombieSpectate.cpp. Untested by the user.
- **End screen** (`ZombieStats.cpp`): every copy fades to white and shows
  YOU WIN! / YOU LOSE / GAME OVER, how the match ended and the stats - the
  director's points spent and earned, monsters bought and lost, traps, hits
  on survivors (`zm_econ_stats`); each survivor's fate (died at m:ss /
  escaped / alive), hits, kills and damage taken. Each copy sends its own
  player's numbers once the match is over (STATS). Action (after 4 s) or a
  minute leaves it through the death fade, without the death screen
  (`zombie_mode_match_over` in die_state).
- **Game clock:** 20 minutes (`ZM_TIME_LIMIT_MS`) from the moment every
  survivor is in (`zm_survivors_in_ms`), shown bottom right on every copy
  (the hall countdown sits a line above it). The director's copy (and single
  player) keeps it and sends CLOCK `{seconds left}` every 5 s; a survivor's
  copy counts down from the last one. At zero the director wins: WIN
  `{-1, 1, ...}` (an escape is `{winner, 0, ...}`), a "TIME IS UP" banner, then the
  same end.
- **Disconnect recovery** (`ZombieNet.cpp`, `ZombieReconnect.cpp`): five seconds
  without gameplay packets starts a 30-second grace period. All copies pause
  simulation and gameplay timers while continuing network polling. The original
  client reconnects automatically; a crashed survivor uses New Game → REJOIN
  SURVIVOR 1/2/3 on the same computer/save directory. Saved seat credentials
  reclaim only that seat in that seeded match. A new connection epoch rejects
  delayed packets from the old process. The host sends a retried chunked snapshot
  with the player's last received inventory, room/position, health/death, stats,
  rescue usage, shared flags/box, monsters and drops. Pending committed pickups,
  drops and rescues are reconciled before restoration; transient animations and
  menus restart. Resume waits for the restored room to finish loading. On expiry
  the seat is removed and the host drops its carried supplies at the last known
  position (subject to existing drop-list/room capacity). No host migration or
  host crash persistence: clients wait 30 seconds, then show CONNECTION LOST.
  CPU regressions: `tests/test_zombie_reconnect.py` and
  `tests/test_zombie_reconnect_snapshot.py`. User play-testing still required.
- **Networking** (`ZombieNet.cpp`) is a UDP star through the host. Per-link
  reliable events carry a source and a destination, and the host relays them.
  - Packets: HELLO, WELCOME, GO, STATE, PING, BYE, FLAGS, ENEMIES, READY, START.
  - Event kinds: HIT=1, GRAB=2, GRAB_END=3, ROSTER=4, SOUND=5, FX=6, PSND=7,
    PHURT=8, WIN=9, BURST=10, HEAL=11, STUN=12, STORY=13, CREDIT=14, TRAP=15, BOX=16, CLOCK=17, DROP=18, DROP_TAKE=19, STATS=20, REVIVE=21, PIANO=32. FX, SOUND, PSND, BURST, HEAL and STUN carry
    `stage|room<<8`. PICKUP=30 is the host's exclusive pickup transaction.
  - **Bump `ZM_NET_VERSION` (currently 58) whenever a packet layout changes.**
  - Lobby version mismatches are decoded from the stable magic/version/type
    prefix, before parsing the full packet layout. The client shows GAME VERSION
    MISMATCH with an update hint; a nonresponding host shows address/port/version
    troubleshooting guidance. Quick debug remains developer-only.
  - WELCOME / GO carry the host's scenario seed (`zm_net_seed`), the lobby
    phase, the veto flags and the survivors' votes; a survivor's HELLO carries
    its character, its vote and the seed's low byte (a vote counts only for
    the map it was cast on). Picks stay open after GO until that survivor
    sends READY; the host no longer waits at the start barrier.
- **Game start flow** (`ZombieLobby.cpp`): the host hosts and the survivors
  join; the host advances (Enter) to the map review - every copy builds the
  seed's scenario and shows the route map: key doors (blue) and where the
  keys and crests lie (purple); the director's also shows every new pickup
  (teal). Each side may veto once (Aim; the
  survivors' veto needs every survivor), which re-rolls the seed. The host
  can press Enter to start when every survivor has accepted (Action), or after
  90 s even without all acceptances. The
  director plays at once; the survivors get the character select screen and
  all spawn in the main hall when its two minutes run out (a pick can change
  until then).
- **Survivors' map:** OPTIONS in the game opens the route map for every
  survivor (look-only: key doors, keys and crests, the survivors' rooms);
  Richard's radio adds the monster counts. OPTIONS no longer opens the options
  screen for survivors.
- **Story flags:** `g_ScenarioFlags` / `g_ScenarioFlags2` are not merged
  (scripts set and clear them). A survivor's copy diffs both banks every
  frame (`zm_world_story_watch`) and sends each changed byte as STORY
  `{bank, byte, set, clear}`; the others apply it. Per-player bits (stage
  variant, outfit, Yawn poison, lockpick, radio, menu latch, ...) stay local.
  This carries e.g. the 2F dining statue (bank 0 bit 0x0B, read by ROOM7020
  and ROOM6050). A copy already in the room sees the change on re-entry.
  `[story]` lines go to the debug log.
- **Shared item box:** the box starts empty (`zm_world_box_reset` at the
  armed new game; `bio_card.dat` would fill it) and is one box for every
  survivor. `ZombieBox.cpp` arbitrates atomic inventory/box swaps on the host.
  BOX carries `{operation,token,box slot,expected item|qty,offered item|qty,
  inventory slot,seed low,seed high}`. Conflicting requests refresh the slot
  and leave inventory unchanged. Reliable receipts and semantic retries award
  once; outstanding receipts reconcile a crashed client's inventory before
  rejoin or timeout loot recovery. Death drops and spectator jumps wait for the
  transfer. Host STATE includes all 48 box slots plus a revision; stale updates
  cannot roll back newer contents and missed broadcasts are repaired. Normal
  single-player box behavior is unchanged. `tests/test_zombie_box.py` covers
  races, swaps, stale requests, retries and reconnects. User testing required.
- **Exclusive pickups** (`ZombiePickups.cpp`, multiplayer): floor pickups use
  their global roomItems flag, drops their stable uid. PICKUP=30 carries
  `{operation,token,world/drop,flag/uid,stage|room<<8,item|qty<<8,seed low,seed high}`.
  The host reserves one claimant, then confirms a commit before inventory is
  awarded. Competing attempts cannot duplicate the item. Cancellations and
  disconnected/ten-second abandoned reservations release uncommitted items;
  retries are idempotent. Critical transactions/drop creation bypass the effects
  inbox. The pickup task yields through confirmation with live monster updates;
  input, drop-slot reuse and spectator room changes wait until it completes.
  An award arriving after death joins the corpse's inventory drops. Maps and
  documents retain their original collection paths. The shared item box uses
  the separate atomic swap protocol above. Untested by the user.
- **Dropped items** (`ZombieDrops.cpp`): a survivor's inventory command box
  has a fourth row, DROP (MainMenu.cpp `menu_item_drop` / `menu_draw_drop_row`:
  status.tim has no DROP button, so it is the CHECK button's edges with a
  letter-free column tiled over the word, and "DROP" in the 8x14 font). The
  item leaves the inventory (as an item-box deposit) and lies where the
  survivor stood for the rest of the game. Every copy keeps the list (DROP
  `{uid, item|qty<<8, x, y, z, angle, stage|room<<8}`, uid = player << 12 |
  sequence); in the loaded room each is a pickup of our own like the
  randomizer's new spots - room action slots `ZM_DROP_SLOT_FIRST` (88)..127,
  item models after the first 24, up to 40 a room - placed at room load or,
  dropped while the copy is in the room, once no menu is open. A placed
  drop's roomItems flag comes from a pool the mansion and scenario leave
  free; it is each copy's own (left out of the FLAGS merge). Its flag
  cleared on an accepted pickup removes its local model. Multiplayer consumption
  is confirmed by the host's PICKUP transaction; survivors cannot broadcast an
  unreserved DROP_TAKE to consume another copy's drop.
  Items without a view model (ids 0x4D+) cannot be dropped. `[drop]` lines.
  A survivor's death drops its whole inventory (its own copy, the first
  frame its health is below 0, `zm_drops_on_death`): in a ring 600 out round
  the body, a spot in a wall falling back to the body's own.
  - Engine widening for it: `g_RoomActionTable` 24 → 128 entries
    (`ROOM_ACTION_ENTRIES`; room init clears the new ones outright),
    `g_item_model_table` 8 → 64 (`ROOM_ITEM_MODELS`). The randomizer's new
    spots stay below slot 88 and model 24.
- **Feeding zombies:** a fatal bite leaves the living zombie feeding over the
  corpse (the grab's status bit 0x08 is cleared). Feeding survives room capture
  and ownership changes, restored directly in the all-fours pose; FEED=25 is a reliable roster companion, and GRAB_END
  carries the fatal-bite feeding cue. The feeding loop references only its own
  joints. On arrival it feeds for at least 4 seconds, then stands if a living
  survivor comes within 1200 units. It remains targetable while down; nonlethal
  weapon hits and director possession make it stand, then AI or pad control
  resumes. Lethal hits retain the death path. Untested by the user.
- **Safe rooms:** a room whose scripts set an item box (`room_action_set`,
  handler 0x08 - in the mansion the 1F save room 0x00 and the storeroom 0x18;
  the main hall's typewriter alone does not count) is closed to the director
  for the whole game: no placing or traps, no map jump, no door
  (`zm_director_safe_room`, from `zm_random_room_safe`). Drawn dark on its map.
- **Occupied rooms:** map purchases of a normal zombie or an unlocked Hunter
  request a doorway reinforcement (`ZombieReinforcements.cpp`), once per minute
  globally at normal cost and within the room cap. The room owner chooses the
  nearest safe real door to any living survivor, excluding that survivor's
  entrance (carried in STATE) - except in a leaf room (every real door leads
  to one neighbouring room, `rf_leaf`), where the way in is the entrance.
  Untested by the user. A one-second door warning precedes revalidation;
  the host then places/pays and the monster holds its entrance for 500 ms.
  Failed requests spend no points or cooldown. Other monsters remain refused;
  safe rooms and protected halls stay excluded. Multiplayer only, untested by
  the user. Traps and map jumps there are still allowed. REINFORCE=29 carries
  request, reply, warning and entrance-hold cues; introduced in protocol 40.
- **Roster sync:** a survivor's roster starts when it joins (`zm_net_join`)
  and is not reset at game start, so placements made during character select
  survive; only the copy that runs a room (the owner) captures it to the
  roster on leaving.
- **Main-hall lock:** the director cannot place a monster in, jump to or walk
  into the main hall or the 2F main hall (`zm_is_hall`) until 60 s after every
  survivor has spawned (or 150 s after the start). A HUD line counts it down.
  The 2F main hall takes only zombies and Hunters, placed or walked in
  (`zm_hall_2f_refuses`): its stairs trip up the dogs.
- **Director's points** (`ZombieEconomy.cpp`; the director's copy and single
  player only): 500 / 800 / 1000 to start with 1 / 2 / 3 survivors
  (500 in single player), then 1 a second with one survivor alive, 2 with
  two, 4 with three (`zm_econ_rate`), +50 when its possessed monster hits
  a survivor (at most one per 1.5 s; a remote zombie grab counts when the
  victim's copy takes it), +25 when any of its other monsters does (at most
  once per 2 s a monster; seen by whichever copy runs the room - a survivor's
  copy sends CREDIT to the director), and 25% of a monster's price back when it dies
  (locally after its update, or from another copy's ROSTER capture: not alive
  with health < 0). Shown top right (the notes moved under it).
  - Prices: zombie 100, green 125, naked 150 (+25% health, on every copy, in
    `zm_world_after_update`), Cerberus 200, web spinner 150,
    Chimera 400, Hunter 500, Tyrant 1800.
  - Unlocks after every survivor is in (`zm_survivors_in_ms`): Cerberus 3 min,
    Hunter 10, Chimera 12, Tyrant 15.
  - Room caps: the generated `cap` in `ZombieSpawnSpots.cpp` (main hall 5,
    1-4 elsewhere, 2 for a room not in the table), +1 at 6 min and +1 at 10 -
    except a cap set by hand (gen_spawn_spots.py `CAP_OVERRIDES`,
    `ZM_SPAWN_CAP_FIXED`: the bathroom off the trap passage holds 1 all game).
    `EXCLUDED_AREAS` keeps spots out of the piano bar's alcove.
    Hunters and Chimeras use two slots, Tyrants three, other monsters one.
    A Tyrant can be bought into an empty room even when its cap is below three;
    further placements must fit the normal cap. Counted by `zm_room_monster_slots`;
    another copy's kills in a room only
    count once its owner leaves. Monsters carried through doors are not capped.
  - The placed Tyrant uses the rooftop model (id 0x10, 600 HP), with its
    entrance and scripted final-battle health latch skipped in the mod.
    Possessable: Run+Forward sprints, Action swipes, Run+Forward+Action does
    the charge's wide sweep, and Aim+Action slashes. The low-health finisher can chain into an
    impale. Tyrant hurt reactions and impales use TYRANT_REACT=23 (stable
    attacker uid, victim reaction and grab anchor); the owner holds the same
    victim through the attack. TYRANT_TRAIL=24 carries a visual-only ribbon
    start cue to the other copies in the room; puppets draw it from the net
    joint pose. Each Tyrant keeps its own ribbon history and claw-effect
    state. Untested by the user.
- **Director's traps** (`ZombieTraps.cpp`): on the map's type list after the
  monsters (ids from `ZM_TRAP_FIRST` 0xF0), set on the selected room with Aim.
  - LOCK DOORS (100 points, 1 min cooldown): for 10 s every door of the room
    and every neighbour's door into it is shut for survivors (camera-only
    records excepted). TRAP `{id, stage|room<<8, duration/100}` goes to every
    copy, timed from arrival. `door_try_enter` asks `zombie_mode_door_trapped`
    (lock click, "THE DOOR IS JAMMED - N S"); the single-player AI survivor's
    `zm_door_usable` refuses them. Not allowed in the main hall while it is
    closed. The director's body is not held.
- **No starting monsters:** the rooms' own enemy_set records are refused
  (`zombie_mode_enemy_spawn`, ids below `NPC_ENTITIES_IDS`); every monster is
  one the director places.
- **One room per copy.** Each copy runs only its own room.
  - The **room owner** is the lowest present player index (the director, if
    present). It runs the room's monsters and sends ENEMIES. Everyone else shows
    puppets.
  - When ownership changes, the old owner sends the new one an adopt snapshot.
    When an owner leaves, its puppets revert to AI.
- **Monster ids (uids)** map remote slots through `s_localOf` / `s_ownerOf`.
  - Below 16: a script slot.
  - From 0x100: a roster extra. The director mints 0x100+, survivors 0x4000+.
  - 0xFFFD: the director's own body (`s_puppetZombie`).
- **Roster** (`ZombieWorld.cpp`): 1024 entries that persist monsters across
  rooms, including ones the director creates. `ZM_HEALTH_FRESH` is 0x7FFF.
  Extras spawn at room load in uid order.
- **Survivor stand-ins** sit in entity slots 27–29 (`ZM_SURVIVOR_SLOT_BASE` 26
  + player index).
  - Chris, Jill, Barry and Rebecca use player models char10–13. Richard (id
    0x27) and Enrico (0x28) use the NPC models em1027/em1028, Chris's animations
    and Chris's weapon textures.
  - They are posed from net STATE, with `ResetJointTransforms` on first show.
    Their weapon EMW goes into joint 14.
  - They block the local player (`ResolveEntityScaCollision`).
- **Combat across copies.** `zm_target_begin` / `zm_target_end` wrap each
  monster update and put the nearest survivor into `g_playerEntity`.
  - A zombie grabbing a remote survivor hands off by GRAB/GRAB_END: the
    victim's copy runs the bite.
  - Other monsters' hits send PHURT. The victim's copy applies the damage, with a
    700 ms cooldown.
  - Projectiles and effects are replayed from FX. Replayed effects do no damage
    (`zombie_mode_effect_damage_blocked`).
  - Burst joints (magnum head shots, rocket gore: joint flags 0x28 with the draw
    bit cleared) go with HIT as a joint mask and from the owner as BURST; the
    head-explosion cue (`Snd_em(6)`) travels with the burst, not as SOUND.
    A hidden puppet (the director's body on a survivor's copy while the
    director is elsewhere, `zombie_mode_hide_entity`) takes no weapon hits;
    the body coming into view alive has any burst joint made whole
    (`zm_burst_clear`, which frees the joint's trail slot).
- **Possession** (`zm_monster_update`). The director can drive zombies, plus
  Cerberus, Hunter and Chimera through a hand-built controller. The pad drives
  movement, and the type's own engine function runs its attacks. Other monster
  types cannot be possessed.
  - When the possessed monster dies, the director takes the next monster in the
    room, else jumps to the nearest room with one (door-graph search, 4 doors
    deep, `zm_possessed_died`). Game over only if none is in reach.
  - Possessed monsters go through doors with Action (Aim+Action is still the
    second attack), locked or not (`zombie_mode_door_ignores_lock` in
    `door_try_enter`; the lock stays for the survivors). A door into a stub
    room keeps its lock. A non-zombie travels as itself: it is re-spawned as a
    roster extra in the new room (`s_carriedId`), not as the zombie body.
- **Director controls:**
  - START switches monster. Action attacks; Aim+Action is the second attack.
  - Aim: a white-coat zombie lies down / gets up; a naked or green zombie
    vomits (the zombie's own `zombie_vomiting`, behaviour 6).
  - OPTIONS opens the map (`ZombieMap.cpp`): rooms where a scenario key or
    crest still lies (its roomItems flag not cleared) are purple, over the
    monsters' orange (`zm_random_room_key_left`); the mansion's 1F and 2F side by
    side (the rooms up to the back-exit door), every room with a room file
    selectable. B1 is drawn under 1F only in the return mansion - the first
    mansion's B1 RDTs (stage 2 rooms 0x1A-0x1C) are 4-byte stubs. On the
    map: the arrows move to the nearest room in that direction (on the drawn
    map, 1F's right edge leads to 2F), Run opens the monster-type list
    (up/down, Action or Run to keep), Aim places one, Action jumps there,
    OPTIONS closes.
  - Mod text goes through `zm_text_encode` before `PrintText8x14`: the 8x14
    sheet draws glyph byte - 36, so only letters, digits, `:` `;` `?` and
    space are ASCII; `<` drew `,` and `>` drew `!`.
  - Jumping to a room takes over its first monster; a room without one is
    refused (the map stays open and notes "NO POSSESSABLE MONSTERS IN THAT
    ROOM").
- **Placement and start positions:**
  - Placed monsters in rooms that aren't loaded wait at `g_zmSpawnSpots`.
    `ZombieSpawnSpots.cpp` is generated by `tools/gen_spawn_spots.py`; don't
    edit it by hand.
  - Survivors start in a triangle in the main hall around (17000, 8500).
  - The director starts inside the back exit.
- **Piano** (`ZombiePiano.cpp`, multiplayer survivors): Jill, Rebecca or
  Richard USE the sheet music at the bar's piano (`zm_piano_use` from
  MainMenu.cpp's item USE; the original's voiced Rebecca scenes never start)
  and play for 15 s. The USE closes the menu (`zm_piano_menu_finished`); the
  playing starts on the first frame the menu is closed, posed as Jill's own
  bar (ROOM60F1 event 6) does: placed two feet right of her spot and six
  inches closer to the keys, at (10100, 7500), facing them, with room
  collision off while playing (player flags bit 4, as room scripts use it:
  the piano's boundary box at x 10127 would push them back to ~9690) (angle 0, set directly), then
  animations 0x37, 0x38 / 0x39 in turn (state 8 behavior 1) from Jill's room
  file - the mode loads Chris's rooms, whose player plays with one hand - by
  pointing jointMoveData2/3 at ROOM60F1's player animation pair while
  playing (Chris's scene is the fallback without that file). Stopping or
  finishing stands them back up unless a grab has the player. A HUD timer,
  the tune (`sound/bgm_2b.wav`, 24.5 s, the whole piece from the first-visit
  bar's music group 0x0E - bgm_11 / bgm_1f are the practice that breaks off;
  the return bar has no music - in the piano's own bank, on every copy
  wherever it is while anyone plays; it ends with the playing - faded out
  over 1 s when finished, cut when interrupted; the room's music is stopped under it and resumed
  2 s after it stops or runs out, unless someone starts again), and
  the director's copy shows "PIANO BEING PLAYED IN THE BAR" with the count.
  Moving, turning, aiming, running, a hit, a grab or leaving the bar stops it
  (start again from zero; the sheet music is kept). Finished: the sheet music
  is used up, DONE goes to the host, which sets ScenarioFlags2 0xA2 (the
  original's "wall open" - the room's init puts the wall away on every entry)
  and sends OPEN; a copy in the bar plays event 9 (the wall sinking).
  PIANO=32 `{op, player, stage, room, 0, 0, seed low, seed high}`: 1 start /
  2 stop / 5 finished survivor -> all, 3 done survivor -> host, 4 open
  host -> all. The
  wall stays closed until then (`rnd_open_piano_room` waits for the flag).
  CPU test `tests/test_zombie_piano.py`. Untested by the user.
- **Doors:** every door animation in the mode ends after `ZM_DOOR_FRAMES`
  (20 frames, about 0.7 s, `zombie_mode_door_frames` in DoorSystem.cpp's
  DoorAnimLoop), counted from the script gate opening (the sound load), not
  from the start, so a slower load does not shorten the shown opening; 60
  frames if the gate never opens. Door scripts play at 5x after the approach
  is skipped; stairs (kai01-04) at 2x with the cut stretched to 50 frames.
  The held-button skip is off, so every player's transition
  takes the same time. The AI survivor's off-screen door cost uses the same
  constant. Untested by the user.
- **Door stun:** a survivor coming into a room stuns the monsters within
  about 6 feet (`ZM_STUN_RADIUS` 2200) of where it stands, for 5 s
  (`ZM_STUN_MS`). The survivor's copy sends STUN `{x, z, stage|room}` on its
  first frame with control in the new room (`zm_stun_frame`) - its STATE names
  the room while it is still loading, so the owner cannot judge the position
  from that. The room owner stuns its monsters in reach (`zm_stun_at`); a
  survivor that owns the room stuns its own. Each monster can be stunned at most
  once per 15 s: the five-second stun is followed by ten seconds of immunity
  (`ZM_STUN_IMMUNITY_MS`, from its last stun's end). The freeze is
  the engine's own: bit 0x0004 of `g_message_flags` is cleared around a stunned
  monster's update in state 1 (`zm_stun_before_update`), but only after the
  monster has run two updates in the room, so it has a pose. Hit reactions,
  deaths and grabs that have started still play. A stunned monster is not
  solid. The director's possessed body is stunned too. `[stun]` lines go to the
  debug log.
- **Texture pages** (`src/marni/TexturePages.h`) are widened to 128 banks.
  - TSB page bits 0–4 hold the low bits and bits 9–11 the high bits. Always go
    through `TexPageFromTsb` / `TexPageTsbBits`.
  - The engine's own page and CLUT counters stay as in the original. Mod models
    load inside `zm_ext_begin` / `zm_ext_end`, on a separate counter: pages from
    0x20 and CLUT rows from 0x40. Moving the shared counters breaks room masks.
  - 1000 TMD entity slots (`TMD_ENTITY_SLOT_COUNT`). `g_DataBuffer` has
    +0x400000.
- **Engine hooks.** The mod calls `zombie_mode_*` functions (declared in
  `ZombieMode.h`) from:
  - models and textures: EntityModelLoader, RoomInit, TmdAnimation,
    Marni3DObject, PathTrail, ObjectManager
  - effects, combat and sound: EffectSystem, WeaponDamage, SoundSystem
  - scripts and entities: CmdFunctions, EntityCommon
  - startup screens: LogosScreen, TitleScreen, CharacterSelectionScreen.

  Barry's char12 model has 15 objects, so EntityModelLoader guards the 16th
  joint.

**Files**

| File | Contents |
|---|---|
| `ZombieMode.cpp` | the mode's main file: the director's body, room hooks, owner sync, stand-ins, placement, map jump, monster controller, FX/PSND/PHURT, target swap, ext texture counter, cooldown, skins |
| `ZombieSurvivor.cpp` | the AI survivor, the RDT door/spawn cache, `zs_net_frame` / `zs_ride_frame`, survivor start positions, HUD |
| `ZombieWorld.cpp` | the roster, extras and uid↔slot mapping, flag merge |
| `ZombieNet.cpp/.h` | the 4-player link |
| `ZombieLobby.cpp` | the menu (Single / Host / Join / Address / Port) and the lobby screen. A survivor presses Action for the character select screen: the list, the highlighted character's name, toughness, description, items and perk (`zm_perk_describe`), and its model turning on the right, drawn like the options menu's player (`options_render_entity`, the player entity borrowed through `zombie_mode_preview_skin` - no game is loaded yet). The host presses Enter to start, which calls `lobby_start_game` → `game_start` |
| `ZombieMap.cpp` | the director's map overlay; areas come from the map TIM palettes |
| `ZombiePerks.cpp` | the survivors' per-character perks and starting kits |
| `ZombieRandom.cpp` | the randomized scenario: keys onto locks, keys/crests/weapons onto item spots, puzzle locks opened |
| `ZombieDrops.cpp` | the survivors' dropped items: the list, the in-room pickups, the inventory's DROP |
| `ZombiePiano.cpp` | the bar's piano: who can play, the 15 s playing and its interruptions, the director's alert, the wall opening |
| `ZombieTraps.cpp` | the director's traps: LOCK DOORS (the room's doors shut for survivors) |
| `ZombieSpectate.cpp` | a dead survivor watching the living ones: the follow, the frozen corpse STATE, the HUD |
| `ZombieStats.cpp` | the end of a match: each player's stats, their exchange (STATS) and the white end screen |
| `ZombieEconomy.cpp` | the director's points: prices, unlock times, room caps, refunds and hit bonuses, the points HUD |
| `ZombieNav.cpp` | navigation |
| `ZombieModeInternal.h` | shared declarations between the mod files |
| `ZombieMode.h` | the hooks the engine calls |

**Open items**
- Untested by the user:
  - monster possession and speeds
  - survivor collision
  - projectile replay
  - texture widening
  - Richard and Enrico
  - the lobby
  - the room-leave cooldown
  - start positions and spawn spots
  - the stair-ride fix.
- Shots sometimes don't hit zombies with 3 players in the main hall. A shot
  audit is in place; waiting for the user's log.
- Untested by the user (this round): the Enrico crash fix (`LoadEntityModel`
  moves the joints' animation objects past a TMD that ends late), death
  hand-over, the win condition, the story-event filter, burst sync, the extra
  sound groups, monsters through doors.
- Untested by the user: a dead survivor's inventory dropped round the body,
  dropping items (the DROP row's look - it may touch
  the equipped-weapon panel - drops in a room another survivor is in, part
  pickups), the widened room tables, the 20-minute game clock and the director's win,
  the ammo doubling (more pickups), the per-survivor
  point rate, the shared item box, safe rooms closed to the
  director, the director's in-game purple key rooms, the director ignoring
  door locks, the 2F main hall (closed with the hall, zombies and Hunters
  only), the headless director's-body fix.
- Untested by the user (economy round): the points, prices, unlock times,
  room caps, refunds, hit bonus, naked-zombie health, the placed Tyrant,
  the other monsters' +25 hit credit, the LOCK DOORS trap.
- Untested by the user: an AI zombie's grab handed to another copy no longer
  leaves it flagged dead on the room owner (status bit 0x08 from
  `zombie_attack`'s first step - it dropped out of the monster count and the
  START switch, was refunded and vanished when the room was left); a
  survivor's stand-in re-dresses when that player's pick changes after the
  room loaded (`zm_standin_refresh`).
- Untested by the user: the player code's per-character tables (`id & 1`:
  aim heights, fire frames, walk footfalls, muzzle flash, turn rates, knife
  offsets) follow the body worn (`zombie_mode_player_body`: Jill and Rebecca
  1, everyone else 0) instead of the scenario's Chris - Jill's straight aim
  read as aiming down against Chris's aim heights.
- Untested by the user: skipped scenes giving the control back (roofed
  passage), the back passage's Hunter entrance (walk-in) and the other
  monster entrances skipped.
- Untested by the user: the puzzle weapons (ranked by solve distance, ammo
  only for those, the rocket launcher's 4 shots, fuel refilling the
  flamethrower), key items in leaf rooms, Richard's sheet music and 162 HP. Tested: the grenade launcher
  on Chris and the flamethrower on Jill work (their own W06 / W15 files,
  never held in the original game).
- Untested by the user: spectating after death (the follow through doors,
  the camera, the corpse staying put on the other copies, the monsters
  shown in a room only the watched survivor is in); the auto-aim no longer
  locking onto the hidden director's-body puppet (`zombie_mode_hide_entity`
  in `player_find_aim_target` / `player_reticle_enemy`).
- Untested by the user: the director's win when every survivor is dead, dead
  survivors kept in the match, the white end screen and its stats on every
  copy (single player too: the AI survivor's death is now the director's win
  instead of the death screen).
- Known gaps:
  - the director's own game over (its possessed monster died with no other
    in reach) still ends only the director's copy, without the end screen
  - about 26 monsters per room; 8 sound groups per room (the room's 4 plus the
    mod's 4, `Room_SetupZombieSoundGroup`)
  - paired kill animations other than the Tyrant's impale are not synced;
    the Tyrant synchronization and possession still need user play-testing
  - mod models skip the death-fade tint
  - possessed non-zombies cannot use stairs or in-room steps.
- Long-term:
  - a server-authoritative director host
  - randomized scenarios (route, keys, weapons) with a route map shown before
    the game.

## Game design

- Stage directories and room RDT files are 1-indexed, but `g_stageId` is
  0-indexed. Most stage tests use the 0-index form — use the 0-index stage
  constants in `src/game/Types.h`. Some tests use the 1-index form to get the
  absolute index of mansion stages (see the same header).
- When commenting about a stage, use the 1-index number or its name; when
  documenting a memory index, use hex with the 0-index notation.
- **Task-based game logic**: gameplay is organized into tasks scheduled every
  frame (`docs/TASK_SCHEDULER.md`). The engine depends on this system, so adapt
  it exactly — including the naked-assembly stack switch on MSVC.


## Build

- **Windows**: `build.bat` (Release) / `build_debug.bat` (Debug), MSVC v145
  (VS 2026) locally; CI uses v143. `WholeProgramOptimization` **must stay
  disabled** in Release — `/GL` + `/LTCG` miscompiles the task scheduler's naked
  assembly and causes intermittent crashes at room load.
- **Linux**: see the Linux port section above.
