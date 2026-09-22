// EntityModels.h - Director's Cut entity model selection.
//
// The DC ships its own `LoadEntityEMD` (PS1 SLUS_005.51 0x800238c8, against the
// OG's 0x8002423c) with a wider per-scenario model table - 69 entries per
// character block at stride 0x8a, where the OG has 52 at 0x68. Three things
// come out of that difference, and none is expressible as a data edit on the PC
// side, because the PC's g_emdPathTable is the OG's 53-per-block table:
//
//   - model index 26 (entity id 0x16 + 4) is EM1016 / EM1116, the Forest
//     zombie; in the PC table that index is the first em100a filler.
//   - in ADVANCED the player and three cutscene NPCs are remapped to the DC's
//     alternate-outfit models, four of which (EM1035/1040/1041/1043/104C) sit
//     at indices past the PC block, where the PC table holds the *other*
//     character block's entries.
//   - in ADVANCED, loading model 26 rewrites the entity's id to 0, which is
//     what turns the Forest zombie into an ordinary damageable zombie.
//
// All three are gated on g_bDcMode by the caller, so Mode=OG never reaches
// here. See docs/PSX_DC_ENEMY_AI.md ("The ADVANCED remap", "Forest zombie").
#pragma once

// Model-table index of the Forest zombie. Room entities are looked up at
// `entity->id + 4` (the +4 sits in the caller's branch-delay slot on the PS1;
// RoomInit.cpp passes `ENTITY->id + 4` for the same reason), so this is
// entity id 0x16.
#define DC_EMD_INDEX_FOREST_ZOMBIE  26

// The DC's alternate-outfit model indices (SLUS_005.51 0x8008d03c).
#define DC_EMD_INDEX_PLAYER_ADV     0x34  // EM1032 / EM1033
#define DC_EMD_INDEX_PLAYER3_ADV    0x35  // EM1035, both blocks
#define DC_EMD_INDEX_NPC_CHRIS_ADV  0x38  // EM1040
#define DC_EMD_INDEX_NPC_JILL_ADV   0x39  // EM1041
#define DC_EMD_INDEX_NPC_REBECCA_ADV 0x3B // EM1043
#define DC_EMD_INDEX_NPC_2C_ADV     0x44  // EM104C

// The costume-variant slot the PC table holds as em1030 / em1031.
#define DC_EMD_INDEX_COSTUME        0x33

// The DC's alternate-outfit unlock. Bit of g_ScenarioFlags (PS1
// g_gameOptionsFlags, the same bank whose 0x7B is the port's
// SCENARIO_FLAG_SECOND_PLAYTHROUGH); nothing in the port sets it yet - the DC
// owns it in its SELECT / ENDING overlays - so the branch below is inert until
// those land.
#define DC_SCENARIO_FLAG_OUTFIT_UNLOCK 0x6F

// The ADVANCED model remap (PS1 0x800238c8, first block). Returns the index to
// load; the identity outside ADVANCED and for every index the DC does not
// remap. Call it with the index LoadEntityEMD has after its costume-variant
// fixup, i.e. before the `(g_playerEntity.id & 1) * 53` block base is added.
unsigned char dc_emd_advanced_index(unsigned char modelIndex);

// Returns the DC's path for a model index, or NULL when the port's
// g_emdPathTable entry already is the DC's file. `jillBlock` is the block
// selector (0 = Chris scenario, 1 = Jill).
const char* dc_emd_path(unsigned char modelIndex, unsigned char jillBlock);

// The post-load half of the ADVANCED block (PS1 0x800238c8, after the file
// read): loading the Forest zombie's model makes the entity an ordinary
// zombie. Call it with the same index, once the model is in memory.
void dc_emd_post_load(unsigned char modelIndex);

// The DC's in-hand model for its Beretta M92FS custom (item 4, the ADVANCED
// starting handgun). The PC's g_weaponPathTable is indexed by the weapon's ITEM
// id and names the Python's model at 4, because the USA's id 4 is the
// DumDum-rounds Colt Python - so an ADVANCED session carried a Python in hand.
// The DC ships two files for it, `Players/W0F.EMW` (Chris) and `W1F.EMW`
// (Jill), and they are the plain Beretta's meshes re-textured: each is exactly
// the size of that block's W02 (24028 / 25580), which is what identifies their
// owner - the DC adds no other weapon.
//
// `charBlock` is `g_playerEntity.id & 3` (0 = Chris, 1 = Jill, 2/3 the NPC
// blocks, which hold no such weapon and have no W2F/W3F counterpart).
// Returns the replacement path, or NULL to use the table's entry.
const char* dc_weapon_model_path(int charBlock, unsigned char weaponId);
