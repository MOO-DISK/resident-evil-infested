#include "ZombieModeInternal.h"
#include "../../DebugPrint.h"

// ============================================================================
// ZombieMessages.cpp - text that does not stop the game (port-added).
//
// The original freezes the room for most messages: set_message_display clears
// the g_message_flags bits its caller names (0xff for a locked door, a desk,
// "no ink ribbon", an examined object...), which stops the player's state
// machine (bit 0x01), the monsters (0x04), the effects (0x08), and waits for
// Action/Cancel to close the text. In a multiplayer match that is a free
// pause on the copy that runs the room.
//
// In the mode such a message is "passive": the flags are left as they are, so
// the survivor keeps full control and the room keeps running; the text still
// types out on the room's message line at the bottom of the screen, turns its
// pages and goes away by itself after a reading time for its length. The
// buttons do not touch it (Action is still the check button).
//
// A message that asks Yes/No (tag 8: take an item, use a key, save) needs the
// answer, so the survivor is held - but not the room: instead of the caller's
// mask only 0x0100 (the control: game_loop cuts the pad to the menu bits, as
// when a script takes the control) and 0x0040 (no other check, door or menu
// starts) are cleared. The player's state machine keeps running with no
// movement input, so the survivor settles into the idle pose, and its STATE
// keeps going out; monsters, effects and events carry on. A grab or death
// while the question is up answers No. Closing it raises back only those two
// bits, so nothing the room changed meanwhile is rolled back.
//
// Messages raised inside the menus, during a door transition, or with no
// pause asked for keep the original behaviour - except an examine event's.
//
// Examine events (zm_evt_is_examine: an action press on a painting, a statue,
// a shelf...) take the control themselves (message flag 0x100), cut to a
// close-up (cut_lock_set), show a message with no pause that waits for a
// press, then cut back to a fixed camera and give the control back. In the
// mode their message is passive too, and the control comes back as it starts.
// The close-up stays until the survivor moves or turns: then the camera goes
// back to the one before it (and on to the one for where the survivor stands,
// zm_fix_camera_at), and the event's own camera commands are dropped for the
// rest of its run - its fixed cut back would land on a stale camera.
//
// A passive message gives way to a different one at once, and to a door or
// the inventory opening (game_loop waits on g_menu_choice_id 0x80 before
// either). The same message is not restarted while it is up, nor for
// ZM_MSG_REPEAT_MS after: walk-in zones probe every frame.
// ============================================================================

#define ZM_MSG_BASE_MS    1500   // reading time per page: this
#define ZM_MSG_CHAR_MS      60   //   plus this per character,
#define ZM_MSG_MIN_MS     2000   //   within these bounds
#define ZM_MSG_MAX_MS     7000
#define ZM_MSG_REPEAT_MS  1000
#define ZM_MSG_PROMPT_MASK 0x0140   // what a Yes/No prompt clears


static bool           s_passive = false;
static bool           s_prompt = false;    // a Yes/No prompt holding only the control
static unsigned short s_id = 0;
static unsigned int   s_waitStart = 0;     // when the shown page finished typing; 0 = not yet
static signed char    s_evtSlot = -1;      // event slot whose SCD command is running
static bool           s_examine[8];        // per event slot: an examine event
static int            s_closeSlot = -1;    // the event that cut to a close-up
static unsigned char  s_closeCam, s_prevCam, s_closeStage, s_closeRoom;
static bool           s_closeReleased = false;
static unsigned short s_lastId = 0xFFFF;   // the last passive message, and when it ended
static unsigned int   s_lastEnd = 0;

// Does the message ask Yes/No? Walks the encoding UpdateMessageDisplay reads:
// tag 1 n (n = 0 ends the text), tags 3/4/5/6 and the 0xf8-0xfa glyph pages
// take one byte, tag 8 is the prompt.
static bool zm_msg_has_prompt(const unsigned char* p)
{
    for (int n = 0; n < 2048; ++n) {
        unsigned char c = p[n];
        switch (c) {
        case 1:
            if (p[n + 1] == 0) return false;
            ++n;
            break;
        case 8:
            return true;
        case 3: case 4: case 5: case 6:
        case 0xf8: case 0xf9: case 0xfa:
            ++n;
            break;
        default:
            break;
        }
    }
    return false;
}

static unsigned int zm_msg_page_ms(void)
{
    int chars = (int)(g_MessageCurrentPtr - g_MessagePtr);
    if (chars < 0 || chars > 512) chars = 0;
    unsigned int ms = ZM_MSG_BASE_MS + (unsigned int)chars * ZM_MSG_CHAR_MS;
    if (ms < ZM_MSG_MIN_MS) ms = ZM_MSG_MIN_MS;
    if (ms > ZM_MSG_MAX_MS) ms = ZM_MSG_MAX_MS;
    return ms;
}

static void zm_msg_finish(void)
{
    s_passive = false;
    s_waitStart = 0;
    s_lastId = s_id;
    s_lastEnd = zm_game_time_ms();
}

bool zombie_mode_message_replace(unsigned short msgId)
{
    if (!s_passive || (msgId & 0x7F) == (s_id & 0x7F)) return false;
    g_menu_choice_id &= 0x7F;
    zm_msg_finish();
    return true;
}

bool zombie_mode_message_repeat(unsigned short msgId, unsigned short pause)
{
    if (!zombie_mode_armed() || pause == 0) return false;
    return (msgId & 0x7F) == (s_lastId & 0x7F) &&
           zm_game_time_ms() - s_lastEnd < ZM_MSG_REPEAT_MS;
}

static bool zm_in_examine(void)
{
    return zombie_mode_armed() && s_evtSlot >= 0 && s_evtSlot < 8 && s_examine[s_evtSlot];
}

void zombie_mode_event_init(int slot, int script)
{
    if (slot < 0 || slot >= 8) return;
    s_examine[slot] = zombie_mode_armed() && zm_evt_is_examine(script);
    if (slot == s_closeSlot) s_closeSlot = -1;
    if (s_examine[slot]) dbg_printf("[msg] event %d (slot %d) is an examine event\n", script, slot);
}

void zombie_mode_event_cmd(int slot)
{
    s_evtSlot = (signed char)slot;
}

bool zombie_mode_cut_skip(void)
{
    return zm_in_examine() && s_closeSlot == s_evtSlot && s_closeReleased;
}

void zombie_mode_cut_closeup(unsigned char prevCam)
{
    if (!zm_in_examine()) return;
    s_closeSlot = s_evtSlot;
    s_prevCam = prevCam;
    s_closeCam = g_roomCameraId;
    s_closeStage = g_stageId;
    s_closeRoom = g_roomId;
    s_closeReleased = false;
}

void zombie_mode_cut_restored(void)
{
    if (zm_in_examine() && s_closeSlot == s_evtSlot && !s_closeReleased) s_closeSlot = -1;
}

// The examine close-up back to the room's camera: the one before it, as the
// event's current_cut_set would, then the one for where the survivor stands.
static void zm_close_release(void)
{
    extern void cut_set(void);
    g_main_state_flags &= ~MSF_CAMERA_LOCK;
    if (g_RdtPointer == NULL || g_RdtPointer->cam_switch_zones == NULL) return;
    const CAM_SWITCH_ZONE* zone = (const CAM_SWITCH_ZONE*)g_RdtPointer->cam_switch_zones;
    for (int i = 0; i < 1024; i++, zone++) {
        if ((unsigned short)zone->camFrom >= g_RdtPointer->cameras_count) return;
        if ((unsigned short)zone->camFrom != s_prevCam) continue;
        g_roomCameraId = s_prevCam;
        g_CurrentRdtDataTypePtr = (void*)zone;
        cut_set();
        break;
    }
    zm_fix_camera_at((const int*)g_playerEntity.scaMatrixData.localMatrix.t, -1);
    dbg_printf("[msg] close-up %d left for camera %d\n", (int)s_closeCam, (int)g_roomCameraId);
}

void zombie_mode_message_frame(void)
{
    if (s_closeSlot < 0) return;
    if (!zombie_mode_armed() || g_stageId != s_closeStage || g_roomId != s_closeRoom ||
        g_ScdEventTable[s_closeSlot].active == 0) {
        s_closeSlot = -1;
        return;
    }
    if (s_closeReleased) return;
    // Someone else has the camera now: not ours to give back.
    if ((g_main_state_flags & MSF_CAMERA_LOCK) == 0 || g_roomCameraId != s_closeCam) {
        s_closeSlot = -1;
        return;
    }
    if ((g_message_flags & 0x0101) != 0x0101) return;
    if ((g_PlayerDpadHeld & (ZM_PAD_FORWARD | ZM_PAD_BACK | ZM_PAD_TURN_A | ZM_PAD_TURN_B)) == 0) return;
    s_closeReleased = true;
    zm_close_release();
}

unsigned short zombie_mode_message_pause(unsigned short msgId, unsigned short pause,
                                         const unsigned char* text)
{
    s_passive = false;
    s_prompt = false;
    s_waitStart = 0;
    const bool examine = zm_in_examine();
    if (!zombie_mode_armed() || (pause == 0 && !examine) || text == NULL) return pause;
    if ((g_main_state_flags & (MSF_MENU_ACTIVE | MSF_DOOR_TRANSITION)) != 0) return pause;
    // g_openMenuFlag 0: game_loop's frame loop is running, no door or menu
    // pending. (MSF_GAMEPLAY_ACTIVE is no test: game_loop clears it once the
    // room is up and raises it only to request a room change.)
    if (g_openMenuFlag != 0) return pause;
    if (zm_msg_has_prompt(text)) {
        if (pause == 0) return 0;    // an examine event's question: its script holds the control
        s_prompt = true;
        dbg_printf("[msg] 0x%02X Yes/No: only the control held\n", msgId);
        return ZM_MSG_PROMPT_MASK;
    }
    s_passive = true;
    s_id = msgId;
    // An examine event took the control for its message: give it back now.
    if (examine && (g_message_flags & 0x0100) == 0) {
        g_message_flags |= 0x0100;
    }
    dbg_printf("[msg] 0x%02X shown without the pause\n", msgId);
    return 0;
}

bool zombie_mode_message_prompt_cancel(void)
{
    if (!s_prompt) return false;
    if (g_playerEntity.isBeingAttackedFlag == 0 && g_playerEntity.health >= 0) return false;
    g_menu_choice_id |= 1;   // the cursor on No
    dbg_printf("[msg] Yes/No prompt answered No: survivor grabbed or dead\n");
    return true;
}

bool zombie_mode_message_prompt_end(void)
{
    if (!s_prompt) return false;
    s_prompt = false;
    g_message_flags |= (unsigned short)(g_messageFlagsBackup & ZM_MSG_PROMPT_MASK);
    return true;
}

bool zombie_mode_message_is_passive(void)
{
    return s_passive;
}

bool zombie_mode_message_page_done(bool pressed)
{
    if (!s_passive) return pressed;
    unsigned int now = zm_game_time_ms();
    if (s_waitStart == 0) {
        s_waitStart = now ? now : 1;
        return false;
    }
    if (now - s_waitStart < zm_msg_page_ms()) return false;
    s_waitStart = 0;
    return true;
}

bool zombie_mode_message_end(void)
{
    if (!s_passive) return false;
    zm_msg_finish();
    return true;
}

// The mod's own text. A pause is asked for in the room, so it is passive
// there like any room message - with none it would be the original's
// no-pause message, which waits for a button and is not given up to the
// inventory or a door (a frozen survivor). Under a menu or a door it is
// asked with none, as before.
static const unsigned char* s_customText = NULL;

const unsigned char* zombie_mode_message_custom(void)
{
    return s_customText;
}

void zm_message_show(const unsigned char* text)
{
    bool room = (g_main_state_flags & (MSF_MENU_ACTIVE | MSF_DOOR_TRANSITION)) == 0 && g_openMenuFlag == 0;
    s_customText = text;
    set_message_display(0xFB, room ? 0xFF : 0);
    s_customText = NULL;
}

void zombie_mode_message_drop(void)
{
    if (!s_passive || (g_menu_choice_id & 0x80) == 0) return;
    g_menu_choice_id &= 0x7F;
    zm_msg_finish();
}
