# Infested time rewards

Monster deaths add to the shared match clock once per roster UID. Zombies
count only after their controller commits a settled corpse; falling with
negative health can still lead to revival and earns no time. Roster captures
and reconnect exports use the same confirmation rule.

Rewards:

- Zombies (all three skins) and Cerberus: 5 seconds.
- Hunters, web spinners and chimeras: 10 seconds.
- Other monsters retain their existing rewards (including Yawn).

One independent time sphere spawns in each leaf room that contains none of the
four mansion keys or four crests. The entire room is excluded when any of those
eight progression items is assigned there, including a reward held behind a
boss or puzzle. Other puzzle items (lighter, sheet music, chemical, broken
shotgun, pick axe, battery, etc.) do not exclude a room.

Spheres never replace ordinary item drops or consume their model/action slots.
Each uses its own unused roomItems flag and a reviewed floor spawn position.
The seed chooses the position and a +5 or +10 reward with equal probability;
there is no longer a 10% replacement roll. Normal item generation is unchanged.

The sphere is translucent gold geometry around rotating, extruded +5/+10
glyphs. Walking within 850 units collects it without Action, an item screen,
or inventory space. The host checks living survivor positions and clears the
pickup's normal roomItems flag before adding time. That flag and the clock
use their existing synchronization/reconnect paths. A reliable host event
plays the local exploding facets and sound/glass.wav on copies in that room,
18 dB below the effects volume. Spheres render after the room background and
before entities and room masks; their gold shell uses 22% opacity. World
vertices use the same input/output Y conversions as ApplyMatrixLV so camera
cuts and tilted views keep the sphere anchored to its room position.
Every time award displays the latest bonus above the bottom-right timer:
+5/+10 for seconds, or +5:00 for a five-minute boss reward. It stays bright
for half a second, then rises slightly and fades over 1.5 seconds. A new
award replaces the previous popup. Clock updates carry the latest award,
its sequence and age so clients show the same amount without replaying it
on regular updates or after reconnecting to an old award.
Network protocol 76 requires every player to rebuild.

The post-boss cooldown freezes poison damage and its countdown without curing
it. Poison resumes once the cooldown ends. Survivor-called timeouts and their
lobby/map controls have been removed; disconnect/reconnect pauses remain.

CPU checks: `python tests/test_zombie_time_rewards.py` and
`python tests/test_zombie_time_projection.py`.
User play-testing is still required for the visuals, sound, collection while
running, simultaneous collectors, revisiting rooms and reconnecting.
