#include "ZombieModeInternal.h"
#include "ZombieNet.h"
#include "../../DebugPrint.h"
#include <cstring>

// ============================================================================
// ZombieRoomSync.cpp - port-added: a room's puzzle state shared by the
// survivors standing in it.
//
// Each copy runs its own room, so two kinds of puzzle state stayed on the copy
// whose survivor made it:
//   - pushed objects (the 2F dining room's statue, the armor room's two, the
//     2F study's, the libraries'): their positions live in the copy's own
//     g_omodel_table. update_room_objects runs the rooms' object-probe zones
//     (probe flag 0x04) against them every frame, so a copy that has the
//     statue where the pusher put it reaches the puzzle's result by itself -
//     the fall, the armor room's flags 0x20 / 0x21 and its reveal;
//   - the large gallery's portrait order (ROOM6170): system flags (bank 4,
//     0x12-0x1F) each press event tests and sets, so presses made on two
//     copies never chained.
// Both now travel as ZM_EV_ROOMSYNC between the survivors' copies:
//   { 1 object, stage | room << 8, slot, x, z, original x, original z }
//   { 2 flags,  stage | room << 8, byte, bits set, bits cleared }
//   { 3 reset,  stage | room << 8 }
// A survivor's own pushes are sent (at most every RS_SEND_MS while it pushes,
// and where the object stopped); objects moved by the copy's own events are
// not - every copy runs those. Every survivor copy keeps the latest state of
// each mansion room. Entering a room another survivor is in, a copy takes that
// state over (an object only where the room's init put it where the pusher's
// init did); entering an empty one, the room starts afresh as the original's
// does - its init puts the statues back and clears the system flags - and the
// copy tells the others to forget the old state (reset).
//
// The gallery solved on another copy: that copy plays the reveal (event 21,
// which also places its player); here the room is brought to the state the
// solved room's init would give - the panel aside (omodel 0 as the solved
// init's omodel_set), event 22 re-arming the portraits and freeing the reward's
// zone - so the reward can be taken without leaving the room.
//
// The director's copy takes no part: these scenes would turn its camera.
//
// The gallery's six portrait questions also change places each game
// (rs_gallery_shuffle): which painting asks which question comes from the
// scenario seed, so every copy agrees. The answers keep their order - it is
// the questions' own events (14-19) that test it.
// ============================================================================

extern void Flg_on(int baseAddr, unsigned int bitIndex);              // 0x00473ef0 CmdFunctions.cpp
extern void ScdEventEntry_Create(unsigned int slot, int scriptIndex); // 0x0041d650 RoomEvents.cpp

#define RS_OBJECTS   8          // g_omodel_table
#define RS_ROOMS     32
#define RS_FLAG_BYTES 8         // g_SysFlags
#define RS_SEND_MS   150

enum { RS_OP_OBJECT = 1, RS_OP_FLAGS = 2, RS_OP_RESET = 3 };


// Lighting is world state, including when nobody remains in the room. The
// native candle (bank 0, 0x0A) and trophy (bank 1, 0x44) bits already travel
// through STORY and the BioCard reconnect snapshot. The study's system bit
// 0x0A is temporary, so bank 1 bit 0xFA stores its persistent "light on" state.
#define RS_STUDY_LIT 0xFA
extern int cmd_room_lights_set(void);
extern int cmd_effect_spawn(void);
extern int cmd_room_action_reset(void);
extern int cmd_bullet_effect_spawn(void);
extern int cmd_bullet_effect_clear(void);

static unsigned char kLightCandle[] = {
    0x46, 0x00, 0xBE, 0x14, 0x30, 0xF8, 0xBB, 0x22, 0xC8, 0xC8, 0xA0, 0x00, 0x7B, 0x15, 0x16, 0x22, 0x30, 0xF8, 0x9A, 0x22, 0xC8, 0xC8, 0xA0, 0x00, 0x3F, 0x15, 0x00, 0x23, 0x30, 0xF8, 0xCA, 0x0B, 0xA0, 0xA0, 0xF0, 0x00, 0xDF, 0x10, 0xCC, 0x04, 0xCC, 0x04, 0xEC, 0x03
};
static unsigned char kLightStudyOff[] = {
    0x46, 0x00, 0x20, 0x12, 0x8E, 0xF9, 0x40, 0x24, 0x46, 0x46, 0x46, 0x00, 0xF0, 0x23, 0x74, 0x18, 0x30, 0xF8, 0x99, 0x24, 0x46, 0x46, 0x46, 0x00, 0xF0, 0x23, 0x30, 0x0C, 0x30, 0xF8, 0xF0, 0x23, 0x46, 0x46, 0x46, 0x00, 0xF0, 0x23, 0x20, 0x03, 0x20, 0x03, 0x20, 0x03
};
static unsigned char kLightStudyOn[] = {
    0x46, 0x00, 0x60, 0x0E, 0x8E, 0xF9, 0xF8, 0x1B, 0x5A, 0x5A, 0x5A, 0x00, 0x70, 0x17, 0x28, 0x0F, 0x30, 0xF8, 0xF0, 0x1E, 0x82, 0x82, 0x82, 0x00, 0xEF, 0x19, 0x20, 0x0D, 0x30, 0xF8, 0xF0, 0x1E, 0x6F, 0x6F, 0x6F, 0x00, 0x9F, 0x14, 0x70, 0x04, 0x60, 0x04, 0x10, 0x04
};
static unsigned char kLightTrophyOff[] = {
    0x46, 0x00, 0x12, 0x26, 0xF8, 0xF3, 0xF8, 0x19, 0x38, 0x38, 0x38, 0x00, 0xC8, 0x2D, 0xA9, 0x26, 0x16, 0xF4, 0x50, 0x1C, 0x38, 0x38, 0x38, 0x00, 0x70, 0x2B, 0x0B, 0x27, 0x48, 0xF4, 0x08, 0x19, 0xB4, 0xB4, 0xB4, 0x00, 0xA0, 0x23, 0xA0, 0x01, 0xA0, 0x01, 0xA0, 0x01
};
static unsigned char kLightTrophyOn[] = {
    0x46, 0x00, 0x12, 0x26, 0xF8, 0xF3, 0xF8, 0x19, 0xC8, 0xC8, 0xC8, 0x00, 0x00, 0x19, 0xB1, 0x14, 0x16, 0xF4, 0x70, 0x1F, 0xC8, 0xC8, 0xC8, 0x00, 0xF0, 0x19, 0x73, 0x14, 0x48, 0xF4, 0x50, 0x1C, 0xC8, 0xC8, 0xC8, 0x00, 0xF0, 0x19, 0x30, 0x05, 0x40, 0x03, 0x70, 0x01
};
static int s_lightKind;
static bool s_lightOn, s_studySys;

static void rs_light_cmd(unsigned char* command, int (*fn)(void))
{
    unsigned char* saved = g_ScdOpcodes;
    g_ScdOpcodes = command;
    fn();
    g_ScdOpcodes = saved;
}

static bool rs_light_on(void)
{
    if (s_lightKind == 1) return Flg_ck((int)g_ScenarioFlags, 0x0A) != 0;
    if (s_lightKind == 2) return Flg_ck((int)g_ScenarioFlags2, RS_STUDY_LIT) != 0;
    return Flg_ck((int)g_ScenarioFlags2, 0x44) == 0;
}

static void rs_light_apply(bool entering)
{
    unsigned char* light = NULL;
    if (s_lightKind == 1 && s_lightOn) {
        light = kLightCandle;
        if (!entering && Flg_ck((int)g_SysFlags, 0) == 0) {
            // ROOM70F0's native flames and pickup re-arm, without its camera cut.
            unsigned char flames[][12] = {
                {0x2A,0x0F,0,0,0xFE,0x1F,0xC2,0xF8,0xC8,0x1F,0,0},
                {0x2A,0x0F,0,0,0x7C,0x20,0x7A,0xF8,0x46,0x20,0,0},
                {0x2A,0x0F,0,0,0x0C,0x21,0xC2,0xF8,0xC4,0x20,0,0}
            };
            for (int i = 0; i < 3; i++) rs_light_cmd(flames[i], cmd_effect_spawn);
            Flg_on((int)g_SysFlags, 0);
            unsigned char examine[] = {0x12,1,2,0x81,0x81,0,0xFF,0,0,0};
            unsigned char pickup[] = {0x12,4,4,0x81,0,0,0,0,0,0};
            rs_light_cmd(examine, cmd_room_action_reset);
            if (Flg_ck((int)g_roomItemsFlags, 0x0C) != 0)
                rs_light_cmd(pickup, cmd_room_action_reset);
        }
    } else if (s_lightKind == 2) {
        light = s_lightOn ? kLightStudyOn : kLightStudyOff;
        if (s_lightOn) FUN_00473f10((int*)g_SysFlags, 0x0A);
        else Flg_on((int)g_SysFlags, 0x0A);
        s_studySys = !s_lightOn;
    } else if (s_lightKind == 3) {
        light = s_lightOn ? kLightTrophyOn : kLightTrophyOff;
        if (!entering && Flg_ck((int)g_roomItemsFlags, 0x1E) != 0) {
            unsigned char eye[] = {0x3D,0x0C,0x19,0,0x50,0x14,0x60,0xF0,0x44,0x2F,0,0};
            unsigned char clear[] = {0x3E,0,0,0};
            rs_light_cmd(clear, cmd_bullet_effect_clear);
            if (!s_lightOn) rs_light_cmd(eye, cmd_bullet_effect_spawn);
        }
    }
    if (light != NULL) rs_light_cmd(light, cmd_room_lights_set);
    // The local switch event may still play its native flicker afterward.
    // Arriving copies and remote changes need its settled lighting immediately.
    g_SpecialRoomLightState = (short)0xFFFF;
    g_SpecialRoomLightDelta = 0;
    dbg_printf("[roomsync] light %d %s (%s)\n", s_lightKind,
               s_lightOn ? "on" : "off", entering ? "entry" : "change");
}

// The background art has separate camera groups for each lighting state:
// ROOM70F0 dark 0-2 / lit 3-5; ROOM6190 lit 0-1 / dark 2-3;
// ROOM70F0's init explicitly selects camera 3 in the lit branch, alongside
// spawning the three flames and arming the reward (bank 0 bit 0x0A set).
// ROOM7150 dark 0-2 / lit 4-6 (3 and 7 are the eye close-ups).
// A reveal can restore its old camera after the switch has changed, so check
// whenever the camera is free, including after that reveal releases it.
static void rs_light_camera(bool entering)
{
    if (!s_lightKind || (!entering && (g_main_state_flags & MSF_CAMERA_LOCK))) return;
    int cam = g_roomCameraId, next = cam;
    if (s_lightKind == 1) {
        if (s_lightOn && cam <= 2) next += 3;
        if (!s_lightOn && cam >= 3 && cam <= 5) next -= 3;
    } else if (s_lightKind == 2) {
        if (s_lightOn && cam >= 2 && cam <= 3) next -= 2;
        if (!s_lightOn && cam <= 1) next += 2;
    } else {
        if (s_lightOn && cam <= 2) next += 4;
        if (!s_lightOn && cam >= 4 && cam <= 6) next -= 4;
    }
    if (next == cam) return;
    g_roomCameraId = (unsigned char)next;
    const CAM_SWITCH_ZONE* zone = (const CAM_SWITCH_ZONE*)g_RdtPointer->cam_switch_zones;
    for (int i = 0; i < 1024; i++, zone++) {
        if ((unsigned short)zone->camFrom >= g_RdtPointer->cameras_count) break;
        if (zone->camFrom == next) {
            g_CurrentRdtDataTypePtr = (void*)zone;
            break;
        }
    }
    if (!entering) {
        if (g_main_state_flags & MSF_CAMERA_DEFER) {
            g_main_state_flags |= MSF_CAMERA_REDRAW;
            StMask(0, 5);
        } else {
            StMask(0, 4);
            display_room_camera_bg();
        }
    }
}

static void rs_light_room(void)
{
    s_lightKind = 0;
    if (!zombie_mode_armed()) return;
    if (g_stageId == STAGE_MANSION_RETURN_2F && g_roomId == ROOM_SMALL_DINING) s_lightKind = 1;
    if (g_stageId == STAGE_MANSION_RETURN_1F && g_roomId == ROOM_MANSION_1F_STUDY) s_lightKind = 2;
    if (g_stageId == STAGE_MANSION_RETURN_2F && g_roomId == ROOM_TROPHY_ROOM) s_lightKind = 3;
    if (!s_lightKind) return;
    s_lightOn = rs_light_on();
    rs_light_apply(true);
    rs_light_camera(true);
}

static void rs_light_frame(void)
{
    if (!s_lightKind || !zombie_mode_armed() || g_roomTransitionBusy ||
        (g_main_state_flags & MSF_ROOM_TRANSITION) != 0) return;
    if (s_lightKind == 2) {
        bool sys = Flg_ck((int)g_SysFlags, 0x0A) != 0;
        if (sys != s_studySys) {
            s_studySys = sys;
            if (sys) FUN_00473f10((int*)g_ScenarioFlags2, RS_STUDY_LIT);
            else Flg_on((int)g_ScenarioFlags2, RS_STUDY_LIT);
        }
    }
    bool on = rs_light_on();
    if (on != s_lightOn) {
        s_lightOn = on;
        rs_light_apply(false);
    }
    rs_light_camera(false);
}

// The rooms whose system flags hold puzzle progress, and which bits.
struct RsFlagRoom {
    unsigned char stage, room, first, last;
};
static const RsFlagRoom kFlagRooms[] = {
    { STAGE_MANSION_RETURN_1F, ROOM_LARGE_GALLERY, 0x12, 0x1F },   // the portraits' order
};

// The large gallery (ROOM6170): solved is ScenarioFlags 0x03; the press that
// solves it (event 20) sets system flag 0x1F with it.
#define RS_GALLERY_SOLVED     0x03
#define RS_GALLERY_SOLVE_BIT  0x1F
#define RS_GALLERY_EVENT_ARM  22
#define RS_GALLERY_PANEL_X    3440      // the solved init's omodel_set 0
#define RS_GALLERY_PANEL_Y    300
#define RS_GALLERY_PANEL_Z    3310
#define RS_GALLERY_PANEL_FLAG 0x81

struct RsObject {
    bool  moved;
    short origX, origZ;           // where the room's init put it
    short x, z;
};
struct RsRoom {
    RsObject      obj[RS_OBJECTS];
    unsigned char flags[RS_FLAG_BYTES];
    bool          used;
};
static RsRoom s_rooms[2][RS_ROOMS];

// This room load.
static bool          s_loaded;
static unsigned char s_stage, s_room;
static int           s_objCount;
static short         s_initX[RS_OBJECTS], s_initZ[RS_OBJECTS];
static int           s_sentX[RS_OBJECTS], s_sentZ[RS_OBJECTS];
static bool          s_dirty[RS_OBJECTS];
static unsigned int  s_sentAt[RS_OBJECTS];
static const RsFlagRoom* s_flagRoom;
// Read through Flg_ck / Flg_on, which take them a dword at a time.
alignas(4) static unsigned char s_flagMask[RS_FLAG_BYTES];
alignas(4) static unsigned char s_flagSeen[RS_FLAG_BYTES];
static bool          s_localSolve, s_wasSolved;

static RsRoom* rs_room(unsigned char stage, unsigned char room)
{
    if (room >= RS_ROOMS) return NULL;
    if (stage == STAGE_MANSION_RETURN_1F) return &s_rooms[0][room];
    if (stage == STAGE_MANSION_RETURN_2F) return &s_rooms[1][room];
    return NULL;
}

static bool rs_on(void)
{
    return zombie_mode_armed() && zm_game_role() == ZM_NET_SURVIVOR;
}

static bool rs_here(unsigned char stage, unsigned char room)
{
    return s_loaded && s_stage == stage && s_room == room && g_stageId == stage && g_roomId == room;
}

static unsigned char* rs_obj(int slot)
{
    if (slot < 0 || slot >= s_objCount) return NULL;
    unsigned char* obj = (unsigned char*)g_omodel_table[slot];
    return (obj != NULL && (obj[0] & 1) != 0) ? obj : NULL;
}

static void rs_place(unsigned char* obj, int x, int z)
{
    *(int*)(obj + 0x34) = x;
    *(int*)(obj + 0x3c) = z;
    *(short*)(obj + 0x6c) = (short)x;
    *(short*)(obj + 0x70) = (short)z;
}

static bool rs_gallery_here(void)
{
    return g_stageId == STAGE_MANSION_RETURN_1F && g_roomId == ROOM_LARGE_GALLERY;
}

static bool rs_gallery_solved(void)
{
    return Flg_ck((int)g_ScenarioFlags, RS_GALLERY_SOLVED) != 0;
}

// Another living survivor whose copy has this room loaded.
static bool rs_someone_here(void)
{
    int self = zm_net_self();
    for (int i = 0; i < ZM_NET_MAX_PLAYERS; i++) {
        if (i == self || zm_net_char(i) < 0) continue;
        const ZmNetPeerState* p = zm_net_player(i);
        if (p == NULL || !p->valid || p->dead || p->spectating) continue;
        if (p->viewStage == g_stageId && p->viewRoom == g_roomId) return true;
    }
    return false;
}

void zm_roomsync_reset(void)
{
    memset(s_rooms, 0, sizeof(s_rooms));
    s_loaded = false;
    s_lightKind = 0;
    if (zombie_mode_armed()) FUN_00473f10((int*)g_ScenarioFlags2, RS_STUDY_LIT);
}

// The unsolved gallery's portraits (room action slots 2-7) each start their
// question, events 6-11 (create_room_event, the event at entry +4). Which
// painting starts which is this game's: a permutation from the seed. The
// plaque (slot 1) and the last portrait under the reward (slot 8, event 12)
// stay. Once solved, event 22 turns the portraits into plain messages.
#define RS_GALLERY_FIRST_SLOT  2
#define RS_GALLERY_FIRST_EVENT 6
#define RS_GALLERY_PORTRAITS   6

static void rs_gallery_shuffle(void)
{
    unsigned char* slot[RS_GALLERY_PORTRAITS];
    for (int i = 0; i < RS_GALLERY_PORTRAITS; i++) {
        slot[i] = &g_RoomActionTable[(RS_GALLERY_FIRST_SLOT + i) * 12];
        // Only the room's own unsolved setup.
        if (slot[i][0] != 0x09 || slot[i][4] != RS_GALLERY_FIRST_EVENT + i) return;
    }
    unsigned char order[RS_GALLERY_PORTRAITS];
    for (int i = 0; i < RS_GALLERY_PORTRAITS; i++) order[i] = (unsigned char)i;
    unsigned int h = zm_random_seed() ^ 0x6A11E7u;
    for (int i = RS_GALLERY_PORTRAITS - 1; i > 0; i--) {
        h = h * 1103515245u + 12345u;
        int j = (int)((h >> 16) % (unsigned int)(i + 1));
        unsigned char t = order[i]; order[i] = order[j]; order[j] = t;
    }
    for (int i = 0; i < RS_GALLERY_PORTRAITS; i++) slot[i][4] = (unsigned char)(RS_GALLERY_FIRST_EVENT + order[i]);
    dbg_printf("[roomsync] gallery questions: %d %d %d %d %d %d\n",
               order[0], order[1], order[2], order[3], order[4], order[5]);
}

// After the room's init (zombie_mode_room_spawn).
void zm_roomsync_room(void)
{
    rs_light_room();
    s_loaded = false;
    s_flagRoom = NULL;
    s_localSolve = false;
    if (zombie_mode_armed() && rs_gallery_here()) rs_gallery_shuffle();
    if (!rs_on() || g_RdtPointer == NULL) return;
    RsRoom* r = rs_room(g_stageId, g_roomId);
    bool adopt = r != NULL && rs_someone_here();
    if (r != NULL && !adopt && r->used) {
        // Nobody else is in it: it starts afresh, for every copy.
        memset(r, 0, sizeof(*r));
        zm_net_send_event8(ZM_EV_ROOMSYNC, RS_OP_RESET, (short)(g_stageId | (g_roomId << 8)), 0, 0, 0, 0, 0, 0);
    }

    s_objCount = (unsigned char)g_RdtPointer->omodel_slot_count;
    if (s_objCount > RS_OBJECTS) s_objCount = RS_OBJECTS;
    for (int i = 0; i < s_objCount; i++) {
        s_dirty[i] = false;
        s_sentAt[i] = 0;
        unsigned char* obj = rs_obj(i);
        if (obj == NULL) { s_initX[i] = s_initZ[i] = 0; s_sentX[i] = s_sentZ[i] = 0; continue; }
        s_initX[i] = *(short*)(obj + 0x6c);
        s_initZ[i] = *(short*)(obj + 0x70);
        if (adopt && r->obj[i].moved && r->obj[i].origX == s_initX[i] && r->obj[i].origZ == s_initZ[i]) {
            rs_place(obj, r->obj[i].x, r->obj[i].z);
            dbg_printf("[roomsync] object %d taken over at (%d,%d)\n", i, (int)r->obj[i].x, (int)r->obj[i].z);
        }
        s_sentX[i] = *(int*)(obj + 0x34);
        s_sentZ[i] = *(int*)(obj + 0x3c);
    }

    memset(s_flagMask, 0, sizeof(s_flagMask));
    for (unsigned int k = 0; k < sizeof(kFlagRooms) / sizeof(kFlagRooms[0]); k++) {
        if (kFlagRooms[k].stage != g_stageId || kFlagRooms[k].room != g_roomId) continue;
        s_flagRoom = &kFlagRooms[k];
        // Flg_on's own bit order builds the mask, as the story flags' do.
        for (unsigned int b = s_flagRoom->first; b <= s_flagRoom->last; b++) Flg_on((int)s_flagMask, b);
    }
    unsigned char* sys = (unsigned char*)g_SysFlags;
    if (s_flagRoom != NULL && adopt) {
        for (int b = 0; b < RS_FLAG_BYTES; b++) {
            sys[b] = (unsigned char)((sys[b] & ~s_flagMask[b]) | (r->flags[b] & s_flagMask[b]));
        }
        dbg_printf("[roomsync] puzzle flags taken over\n");
    }
    for (int b = 0; b < RS_FLAG_BYTES; b++) s_flagSeen[b] = (unsigned char)(sys[b] & s_flagMask[b]);

    s_wasSolved = rs_gallery_here() && rs_gallery_solved();
    s_stage = g_stageId;
    s_room = g_roomId;
    s_loaded = true;
}

// The large gallery solved on another copy, with this one in the room: the
// solved room's state, without the reveal's player placing.
static void rs_gallery_catch_up(void)
{
    unsigned char* panel = rs_obj(0);
    if (panel != NULL) {
        panel[0] = RS_GALLERY_PANEL_FLAG;
        rs_place(panel, RS_GALLERY_PANEL_X, RS_GALLERY_PANEL_Z);
        *(int*)(panel + 0x38) = RS_GALLERY_PANEL_Y;
        *(short*)(panel + 0x6e) = RS_GALLERY_PANEL_Y;
        s_sentX[0] = RS_GALLERY_PANEL_X;
        s_sentZ[0] = RS_GALLERY_PANEL_Z;
        s_dirty[0] = false;
    }
    ScdEventEntry_Create(9, RS_GALLERY_EVENT_ARM);
    dbg_printf("[roomsync] gallery solved by another survivor: reward freed here\n");
}

void zm_roomsync_frame(void)
{
    rs_light_frame();
    if (!s_loaded || !rs_on() || g_stageId != s_stage || g_roomId != s_room) return;
    if (g_roomTransitionBusy || (g_main_state_flags & MSF_ROOM_TRANSITION) != 0) return;
    RsRoom* r = rs_room(g_stageId, g_roomId);
    unsigned int now = zm_game_time_ms();
    short where = (short)(g_stageId | (g_roomId << 8));

    // Objects: this survivor's pushes go out; other moves (the copy's own
    // events, which take the control - message flag 0x100 - to play) are
    // every copy's own.
    bool pushing = ((g_main_state_flags & MSF_OBJECT_PUSH) != 0 || g_playerEntity.action_behavior == 0x10) &&
                   (g_message_flags & 0x0100) != 0;
    for (int i = 0; i < s_objCount; i++) {
        unsigned char* obj = rs_obj(i);
        if (obj == NULL) continue;
        int x = *(int*)(obj + 0x34), z = *(int*)(obj + 0x3c);
        if (x != s_sentX[i] || z != s_sentZ[i]) {
            if (pushing) {
                s_dirty[i] = true;
            } else if (!s_dirty[i]) {
                s_sentX[i] = x;
                s_sentZ[i] = z;
                continue;
            }
        }
        if (!s_dirty[i] || (pushing && now - s_sentAt[i] < RS_SEND_MS)) continue;
        s_dirty[i] = false;
        s_sentX[i] = x;
        s_sentZ[i] = z;
        s_sentAt[i] = now ? now : 1;
        zm_net_send_event8(ZM_EV_ROOMSYNC, RS_OP_OBJECT, where, (short)i, (short)x, (short)z,
                           s_initX[i], s_initZ[i], 0);
        if (r != NULL) {
            r->used = true;
            r->obj[i].moved = true;
            r->obj[i].origX = s_initX[i];
            r->obj[i].origZ = s_initZ[i];
            r->obj[i].x = (short)x;
            r->obj[i].z = (short)z;
        }
    }

    // Puzzle flags: what this copy's events changed.
    if (s_flagRoom != NULL) {
        const unsigned char* sys = (const unsigned char*)g_SysFlags;
        bool solveWas = Flg_ck((int)s_flagSeen, RS_GALLERY_SOLVE_BIT) != 0;
        for (int b = 0; b < RS_FLAG_BYTES; b++) {
            unsigned char cur = (unsigned char)(sys[b] & s_flagMask[b]);
            if (cur == s_flagSeen[b]) continue;
            unsigned char set = (unsigned char)(cur & ~s_flagSeen[b]);
            unsigned char clr = (unsigned char)(s_flagSeen[b] & ~cur);
            s_flagSeen[b] = cur;
            zm_net_send_event8(ZM_EV_ROOMSYNC, RS_OP_FLAGS, where, (short)b, set, clr, 0, 0, 0);
            if (r != NULL) {
                r->used = true;
                r->flags[b] = cur;
            }
        }
        if (!solveWas && Flg_ck((int)s_flagSeen, RS_GALLERY_SOLVE_BIT) != 0) s_localSolve = true;
    }

    if (rs_gallery_here() && !s_wasSolved && rs_gallery_solved()) {
        s_wasSolved = true;
        if (!s_localSolve) rs_gallery_catch_up();
    }
}

// ZM_EV_ROOMSYNC from another survivor's copy.
void zm_roomsync_take(const short* a, int src)
{
    if (!rs_on() || src == zm_net_self()) return;
    unsigned char stage = (unsigned char)(a[1] & 0xFF), room = (unsigned char)((unsigned short)a[1] >> 8);
    RsRoom* r = rs_room(stage, room);
    if (r == NULL) return;
    bool here = rs_here(stage, room);
    switch (a[0]) {
    case RS_OP_RESET:
        if (!here) memset(r, 0, sizeof(*r));
        break;
    case RS_OP_OBJECT: {
        int slot = a[2];
        if (slot < 0 || slot >= RS_OBJECTS) break;
        r->used = true;
        r->obj[slot].moved = true;
        r->obj[slot].x = a[3];
        r->obj[slot].z = a[4];
        r->obj[slot].origX = a[5];
        r->obj[slot].origZ = a[6];
        unsigned char* obj = here ? rs_obj(slot) : NULL;
        // Only the same object: the room's init put it where the sender's did.
        if (obj == NULL || s_initX[slot] != a[5] || s_initZ[slot] != a[6]) break;
        rs_place(obj, a[3], a[4]);
        s_sentX[slot] = a[3];
        s_sentZ[slot] = a[4];
        s_dirty[slot] = false;
        break;
    }
    case RS_OP_FLAGS: {
        int b = a[2];
        if (b < 0 || b >= RS_FLAG_BYTES) break;
        unsigned char set = (unsigned char)a[3], clr = (unsigned char)a[4];
        r->used = true;
        r->flags[b] = (unsigned char)((r->flags[b] | set) & ~clr);
        if (!here || s_flagRoom == NULL) break;
        unsigned char* sys = (unsigned char*)g_SysFlags;
        set &= s_flagMask[b];
        clr &= s_flagMask[b];
        sys[b] = (unsigned char)((sys[b] | set) & ~clr);
        s_flagSeen[b] = (unsigned char)(sys[b] & s_flagMask[b]);
        dbg_printf("[roomsync] puzzle flags from player %d: byte %d +%02X -%02X\n", src, b, set, clr);
        break;
    }
    }
}
