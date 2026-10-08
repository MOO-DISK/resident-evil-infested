#include "ZombieModeInternal.h"
#include "ZombieNet.h"
#include "../entities/EntityCommon.h"
#include "../../DebugPrint.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern unsigned int Flg_ck(int baseAddr, unsigned int bitIndex);    // 0x00473f40 CmdFunctions.cpp

// ============================================================================
// ZombieDirectorAI.cpp - the AI director (port-added), [Mods] AiDirector.
//
// Spends the director's points on the director's copy (or single player's)
// - a host's "AI DIRECTOR" game (ZombieLobby.cpp), whose copy only watches,
// or [Mods] AiDirector as an autopilot beside a human director -
// through exactly the calls the map makes (zm_director_place_ai, the doorway
// reinforcement, the LOCK DOORS trap), so every placement rule, price, cap
// and the roster sync apply unchanged. It never takes a monster over.
//
//   Pacing - an "intensity" built from the survivors' lost health, deaths and
//     the monsters they kill, decaying over time. BUILD spends; reaching the
//     difficulty's peak switches to PEAK (nothing new) and, once it has
//     calmed, RELAX for a while before building again.
//   Where - the door graph (each room's RDT, cached here as an adjacency
//     table) searched outward from every living survivor. Rooms one to
//     `lookahead` doors ahead score, the room a survivor just left does not,
//     rooms still holding a key or crest (their next objective) score more,
//     rooms already holding monsters less.
//   What - weighted by the room's capacity and what is unlocked; points are
//     held back for a big monster that is about to unlock.
//   Pressure - a survivor that lingers in one room gets a doorway
//     reinforcement; a hurt survivor in a room with monsters gets its doors
//     locked (hard and above).
//   The opening - as soon as the game starts, part of the starting points go
//     into the rooms around the main hall (the human director's setup).
// ============================================================================

int g_zmAiDirector = 0;

struct ZmAiLevel {
    const char*  name;
    int          incomePct;      // income and starting points
    int          unlockPct;      // unlock times
    int          lookahead;      // doors ahead of a survivor it places
    unsigned int thinkMs;        // between decisions
    int          peak;           // intensity that ends a build-up
    unsigned int relaxMs;        // quiet after a peak
    unsigned int dwellMs;        // a survivor this long in one room: reinforce (0 never)
    bool         traps;
    bool         focusWeak;      // weight the hurt survivor's path up
    int          spendPerMin;    // points a minute at most (the opening aside)
    int          openingPct;     // of the starting points
};

static const ZmAiLevel kLevels[5] = {
    { "OFF",       100, 100, 0,    0,   0,     0,     0, false, false,    0,   0 },
    { "EASY",       60, 125, 2, 6000,  50, 30000,     0, false, false,  600,  50 },
    { "NORMAL",    100, 100, 2, 4000,  70, 20000, 75000, false, false,  900,  70 },
    { "HARD",      140,  80, 3, 2500,  85, 12000, 45000, true,  false, 1400,  85 },
    { "NIGHTMARE", 180,  60, 3, 1500, 100,  6000, 30000, true,  true,  2000, 100 },
};

enum { AI_BUILD = 0, AI_PEAK = 1, AI_RELAX = 2 };
static const char* const kPhaseName[3] = { "BUILD", "PEAK", "RELAX" };

#define AI_STAGES      7
#define AI_ROOMS       0x20
#define AI_MAX_LINKS   ZM_MAX_DOORS
#define AI_FAR         0x7F
#define AI_LOW_HEALTH  45          // a survivor this hurt is "weak" (Jill has 96, Chris 140)
#define AI_DECAY_PER_S 2           // intensity
#define AI_HIT_SCALE   2           // intensity per 3 health lost (x2 / 3)
#define AI_DEATH       50
#define AI_KILL        8
#define AI_SAVE_AHEAD_MS 60000     // hold points for a monster unlocking this soon

static const struct { unsigned char id; const char* name; } kNames[] = {
    { ENEMY_ZOMBIE, "ZOMBIE" }, { ENEMY_ZOMBIE_VARIANT, "GREEN ZOMBIE" },
    { ENEMY_ZOMBIE_NAKED, "NAKED ZOMBIE" }, { ENEMY_CERBERUS, "CERBERUS" },
    { ENEMY_HUNTER, "HUNTER" }, { ENEMY_CHIMERA, "CHIMERA" }, { ENEMY_TYRANT_2, "TYRANT" },
    { ENEMY_WEB_SPINNER, "WEB SPINNER" }, { ZM_TRAP_LOCK_DOORS, "LOCK DOORS" },
};

static const char* ai_name(unsigned char id)
{
    for (unsigned int i = 0; i < sizeof(kNames) / sizeof(kNames[0]); i++)
        if (kNames[i].id == id) return kNames[i].name;
    return "MONSTER";
}

// ---------------------------------------------------------------------------
// The door graph
// ---------------------------------------------------------------------------
struct AiRoom {
    bool          built;
    unsigned char count;
    unsigned char stage[AI_MAX_LINKS], room[AI_MAX_LINKS];
    unsigned char dest[AI_MAX_LINKS];   // the record's +0x0D, for the keypad / elevator test
    unsigned char lock[AI_MAX_LINKS];   // +0x0C
};
static AiRoom s_graph[AI_STAGES][AI_ROOMS];

static const AiRoom* ai_room(unsigned char stage, unsigned char room)
{
    AiRoom& r = s_graph[stage][room];
    if (r.built) return &r;
    r.built = true;
    r.count = 0;
    if (zm_random_room_stub(stage, room)) return &r;
    const ZmDoor* doors;
    int n = zm_room_doors(stage, room, &doors);
    for (int i = 0; i < n && r.count < AI_MAX_LINKS; i++) {
        if ((doors[i].flags0B & 0x80) != 0) continue;              // camera-only
        unsigned char ns, nr;
        zm_decode_dest(doors[i].dest, stage, &ns, &nr);
        if (ns >= AI_STAGES || nr >= AI_ROOMS || (ns == stage && nr == room)) continue;
        bool dup = false;
        for (int k = 0; k < r.count && !dup; k++) dup = r.stage[k] == ns && r.room[k] == nr;
        if (dup) continue;
        r.stage[r.count] = ns;
        r.room[r.count] = nr;
        r.dest[r.count] = doors[i].dest;
        r.lock[r.count] = doors[i].lock;
        r.count++;
    }
    return &r;
}

// Doors from (stage, room) outward, `max` deep: a key-locked door still shut
// on this copy counts two, the dead elevator / keypad door is no way at all.
static void ai_distances(unsigned char stage, unsigned char room, int max,
                         unsigned char dist[AI_STAGES][AI_ROOMS])
{
    memset(dist, AI_FAR, AI_STAGES * AI_ROOMS);
    if (stage >= AI_STAGES || room >= AI_ROOMS) return;
    static unsigned short queue[AI_STAGES * AI_ROOMS * 4];
    int head = 0, tail = 0;
    dist[stage][room] = 0;
    queue[tail++] = (unsigned short)(stage << 8 | room);
    while (head < tail) {
        unsigned char s = (unsigned char)(queue[head] >> 8), r = (unsigned char)queue[head];
        head++;
        int d = dist[s][r];
        if (d >= max) continue;
        const AiRoom* a = ai_room(s, r);
        for (int i = 0; i < a->count; i++) {
            if (zm_access_door_closed(s, r, a->dest[i])) continue;
            bool locked = (a->lock[i] & 0x80) != 0 && Flg_ck((int)g_LocksFlags, a->lock[i] & 0x3F) == 0;
            int nd = d + (locked ? 2 : 1);
            unsigned char ns = a->stage[i], nr = a->room[i];
            if (nd > max || nd >= dist[ns][nr]) continue;
            dist[ns][nr] = (unsigned char)nd;
            if (tail < (int)(sizeof(queue) / sizeof(queue[0]))) queue[tail++] = (unsigned short)(ns << 8 | nr);
        }
    }
}

// ---------------------------------------------------------------------------
// The survivors, as the AI sees them
// ---------------------------------------------------------------------------
struct AiSurvivor {
    bool          valid, dead;
    unsigned char stage, room;
    unsigned char prevStage, prevRoom;
    short         health;
    unsigned int  enteredMs;     // came into this room
    unsigned int  pressedMs;     // last reinforcement / trap aimed at it here
};
static AiSurvivor s_surv[ZM_NET_MAX_PLAYERS];
static int        s_hurt = 0;            // intensity x16 since the last pacing step

static void ai_read_survivors(unsigned int now)
{
    for (int i = 0; i < ZM_NET_MAX_PLAYERS; i++) {
        AiSurvivor& v = s_surv[i];
        bool valid = false, dead = false;
        unsigned char stage = 0, room = 0;
        short health = 0;
        if (zm_game_role() == ZM_NET_OFF) {
            // Single player: the AI survivor is the one survivor.
            if (i == 1) valid = zm_survivor_location(&stage, &room);
            health = g_playerEntity.health;
            dead = health < 0;
        } else if (i > 0 && zm_net_char(i) >= 0) {
            const ZmNetPeerState* p = zm_net_player(i);
            if (p != NULL && p->valid) {
                valid = true;
                stage = p->stage;
                room = p->room;
                health = p->health;
                dead = p->dead || p->spectating;
            }
        }
        if (!valid) { v.valid = false; continue; }
        if (!v.valid) {
            v.valid = true;
            v.dead = dead;
            v.stage = v.prevStage = stage;
            v.room = v.prevRoom = room;
            v.health = health;
            v.enteredMs = now;
            v.pressedMs = 0;
            continue;
        }
        if (stage != v.stage || room != v.room) {
            v.prevStage = v.stage;
            v.prevRoom = v.room;
            v.stage = stage;
            v.room = room;
            v.enteredMs = now;
            v.pressedMs = 0;
        }
        if (dead && !v.dead) s_hurt += AI_DEATH * 16;
        else if (!dead && health < v.health) s_hurt += (v.health - health) * AI_HIT_SCALE * 16 / 3;
        v.dead = dead;
        v.health = health;
    }
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static const ZmAiLevel* s_level = &kLevels[0];
static int          s_phase = AI_BUILD;
static int          s_intensity = 0;           // x16, for a smooth decay
static unsigned int s_lastMs = 0;
static unsigned int s_thinkMs = 0;
static unsigned int s_relaxUntilMs = 0;
static unsigned int s_minuteMs = 0;
static int          s_minuteSpent = 0;
static bool         s_opened = false;
static bool         s_tyrantPlaced = false;
static int          s_lostSeen = 0;
static char         s_lastAction[40];

// A game hosted as "AI DIRECTOR" takes the lobby's level; otherwise
// [Mods] AiDirector is an autopilot for the human director's copy.
int zm_ai_level(void)
{
    if (!g_bPlayAsZombie || !zombie_mode_armed() || zm_game_role() == ZM_NET_SURVIVOR) return 0;
    if (zm_ai_hosted()) return zm_net_ai_level();
    return (g_zmAiDirector > 0 && g_zmAiDirector <= 4) ? g_zmAiDirector : 0;
}

bool zm_ai_hosted(void)
{
    return zm_game_role() == ZM_NET_ZOMBIE && zm_net_ai_level() > 0;
}

const char* zm_ai_level_name(int level)
{
    return (level > 0 && level <= 4) ? kLevels[level].name : kLevels[0].name;
}

int zm_ai_income_pct(void) { return kLevels[zm_ai_level()].incomePct; }
int zm_ai_unlock_pct(void) { return kLevels[zm_ai_level()].unlockPct; }

void zm_ai_new_game(void)
{
    s_level = &kLevels[zm_ai_level()];
    memset(s_graph, 0, sizeof(s_graph));
    memset(s_surv, 0, sizeof(s_surv));
    s_hurt = 0;
    s_phase = AI_BUILD;
    s_intensity = 0;
    s_lastMs = zm_game_time_ms();
    s_thinkMs = s_lastMs;
    s_relaxUntilMs = 0;
    s_minuteMs = s_lastMs;
    s_minuteSpent = 0;
    s_opened = false;
    s_tyrantPlaced = false;
    s_lostSeen = 0;
    s_lastAction[0] = '\0';
    if (zm_ai_level() > 0) dbg_printf("[ai] director autopilot: %s\n", s_level->name);
}

static void ai_note(const char* fmt, const char* what, unsigned char stage, unsigned char room)
{
    const char* rn = DebugRoom_Name(stage, room);
    snprintf(s_lastAction, sizeof(s_lastAction), fmt, what);
    dbg_printf("[ai] %s - stage %d room %02X %s (%d pts, intensity %d, %s)\n", s_lastAction,
               (int)stage, (int)room, rn != NULL ? rn : "", zm_econ_points(), s_intensity / 16,
               kPhaseName[s_phase]);
}

// ---------------------------------------------------------------------------
// Pacing
// ---------------------------------------------------------------------------
static void ai_pacing(unsigned int now)
{
    unsigned int dt = now - s_lastMs;
    s_lastMs = now;
    // The survivors' lost health and deaths (ai_read_survivors).
    s_intensity += s_hurt;
    s_hurt = 0;
    // The monsters they kill: a fight going on.
    ZmEconStats st;
    zm_econ_stats(&st);
    if (st.lost > s_lostSeen) s_intensity += (st.lost - s_lostSeen) * AI_KILL * 16;
    s_lostSeen = st.lost;
    // Decay.
    int decay = (int)(dt * AI_DECAY_PER_S * 16 / 1000);
    s_intensity = s_intensity > decay ? s_intensity - decay : 0;
    if (s_intensity > 200 * 16) s_intensity = 200 * 16;

    int level = s_intensity / 16;
    switch (s_phase) {
    case AI_BUILD:
        if (level >= s_level->peak) {
            s_phase = AI_PEAK;
            dbg_printf("[ai] peak (intensity %d)\n", level);
        }
        break;
    case AI_PEAK:
        if (level < s_level->peak / 2) {
            s_phase = AI_RELAX;
            s_relaxUntilMs = now + s_level->relaxMs;
            dbg_printf("[ai] relax for %u s\n", s_level->relaxMs / 1000);
        }
        break;
    case AI_RELAX:
        if ((int)(now - s_relaxUntilMs) >= 0) {
            s_phase = AI_BUILD;
            dbg_printf("[ai] build\n");
        }
        break;
    }
    if (now - s_minuteMs >= 60000) {
        s_minuteMs = now;
        s_minuteSpent = 0;
    }
}

// ---------------------------------------------------------------------------
// Choosing
// ---------------------------------------------------------------------------

// Rooms scored by the survivors' ways forward. `originStage/Room` stands in
// for the survivors before any are in (the opening: the main hall).
static int s_score[AI_STAGES][AI_ROOMS];

static void ai_score_rooms(bool opening, unsigned char originStage, unsigned char originRoom)
{
    static unsigned char dist[AI_STAGES][AI_ROOMS];
    static const int kAhead[4] = { 0, 20, 35, 25 };
    memset(s_score, 0, sizeof(s_score));
    int look = s_level->lookahead;
    bool any = false;
    for (int i = 0; i < ZM_NET_MAX_PLAYERS && !opening; i++) {
        const AiSurvivor& v = s_surv[i];
        if (!v.valid || v.dead) continue;
        any = true;
        ai_distances(v.stage, v.room, look, dist);
        int weight = (s_level->focusWeak && v.health < AI_LOW_HEALTH) ? 2 : 1;
        for (int s = 0; s < AI_STAGES; s++) {
            for (int r = 0; r < AI_ROOMS; r++) {
                int d = dist[s][r];
                if (d == 0 || d > look) continue;
                int add = kAhead[d > 3 ? 3 : d];
                if (s == v.prevStage && r == v.prevRoom) add = -60;     // where it came from
                s_score[s][r] += add * weight;
            }
        }
    }
    if (!any) {
        ai_distances(originStage, originRoom, 3, dist);
        for (int s = 0; s < AI_STAGES; s++)
            for (int r = 0; r < AI_ROOMS; r++)
                if (dist[s][r] > 0 && dist[s][r] <= 3) s_score[s][r] = kAhead[dist[s][r]];
    }
    // What is in each candidate room.
    for (int s = 0; s < AI_STAGES; s++) {
        for (int r = 0; r < AI_ROOMS; r++) {
            if (s_score[s][r] <= 0) continue;
            if (zm_random_room_key_left((unsigned char)s, (unsigned char)r)) s_score[s][r] += 30;
            unsigned char ids[8], qty[8];
            bool isNew[8];
            if (zm_random_room_items((unsigned char)s, (unsigned char)r, ids, qty, isNew, 8) > 0)
                s_score[s][r] += 8;
            s_score[s][r] -= 12 * zm_room_monster_slots((unsigned char)s, (unsigned char)r);
            s_score[s][r] += rand() % 11;
        }
    }
}

// The best-scoring room some monster may go into now. False: none.
static bool ai_best_room(unsigned char* outStage, unsigned char* outRoom)
{
    int best = 0;
    for (int s = 0; s < AI_STAGES; s++) {
        for (int r = 0; r < AI_ROOMS; r++) {
            if (s_score[s][r] <= best) continue;
            // The cheapest monster as the test: it must fit at all.
            if (!zm_director_place_allowed((unsigned char)s, (unsigned char)r, ENEMY_ZOMBIE) &&
                !zm_director_place_allowed((unsigned char)s, (unsigned char)r, ENEMY_CERBERUS) &&
                !zm_director_place_allowed((unsigned char)s, (unsigned char)r, ENEMY_HUNTER)) continue;
            best = s_score[s][r];
            *outStage = (unsigned char)s;
            *outRoom = (unsigned char)r;
        }
    }
    return best > 0;
}

// Points to keep back: the price of a big monster unlocking within a minute
// that has not been bought yet (normal and above).
static int ai_reserve(void)
{
    if (s_level->incomePct < 100) return 0;
    static const unsigned char big[] = { ENEMY_HUNTER, ENEMY_CHIMERA, ENEMY_TYRANT_2 };
    int reserve = 0;
    for (unsigned int i = 0; i < sizeof(big); i++) {
        if (big[i] == ENEMY_TYRANT_2 && s_tyrantPlaced) continue;
        int left = zm_econ_unlock_left_ms(big[i]);
        if (left <= 0 || left > AI_SAVE_AHEAD_MS) continue;
        int cost = zm_econ_cost(big[i]);
        if (cost > reserve) reserve = cost;
    }
    return reserve;
}

// A monster for (stage, room), weighted by the room's capacity and the hour.
static bool ai_pick_monster(unsigned char stage, unsigned char room, bool opening, unsigned char* outId)
{
    struct { unsigned char id; int weight; } c[8];
    int n = 0;
    int cap = zm_econ_room_cap(stage, room);
    int used = zm_room_monster_slots(stage, room);
    int freeSlots = cap - used;
    bool objective = zm_random_room_key_left(stage, room);
    int points = zm_econ_points();
    int reserve = opening ? 0 : ai_reserve();

    const struct { unsigned char id; int weight; } base[] = {
        { ENEMY_ZOMBIE,          40 },
        { ENEMY_ZOMBIE_VARIANT,  25 },
        { ENEMY_ZOMBIE_NAKED,    20 },
        { ENEMY_WEB_SPINNER,     cap >= 3 ? 12 : 0 },
        { ENEMY_CERBERUS,        cap <= 3 ? 45 : 25 },          // corridors
        { ENEMY_HUNTER,          freeSlots >= 2 ? 45 : 0 },
        { ENEMY_CHIMERA,         freeSlots >= 2 ? 30 : 0 },
        { ENEMY_TYRANT_2,        (!s_tyrantPlaced && used == 0 && (objective || s_level->traps)) ? 80 : 0 },
    };
    int total = 0;
    for (unsigned int i = 0; i < sizeof(base) / sizeof(base[0]); i++) {
        if (base[i].weight <= 0) continue;
        if (!zm_director_place_allowed(stage, room, base[i].id)) continue;
        int cost = zm_econ_cost(base[i].id);
        // Small fry would eat the savings for a big one about to unlock.
        if (reserve > 0 && cost < reserve && points - cost < reserve) continue;
        c[n].id = base[i].id;
        c[n].weight = base[i].weight;
        total += c[n].weight;
        n++;
    }
    if (n == 0) return false;
    int roll = rand() % total;
    for (int i = 0; i < n; i++) {
        roll -= c[i].weight;
        if (roll < 0) { *outId = c[i].id; return true; }
    }
    *outId = c[n - 1].id;
    return true;
}

static bool ai_place_one(bool opening)
{
    unsigned char stage, room, id;
    if (!ai_best_room(&stage, &room)) return false;
    if (!ai_pick_monster(stage, room, opening, &id)) {
        s_score[stage][room] = 0;       // try elsewhere next time
        return false;
    }
    int cost = zm_econ_cost(id);
    // The minute's cap; its first purchase may go over (a Tyrant).
    if (!opening && s_minuteSpent > 0 && s_minuteSpent + cost > s_level->spendPerMin) return false;
    if (!zm_director_place_ai(stage, room, id, ai_name(id))) {
        s_score[stage][room] = 0;
        return false;
    }
    if (!opening) s_minuteSpent += cost;
    if (id == ENEMY_TYRANT_2) s_tyrantPlaced = true;
    // Spread the next one: this room is less attractive now.
    s_score[stage][room] -= 25;
    ai_note("%s PLACED", ai_name(id), stage, room);
    return true;
}

// A survivor lingering in one room: a zombie (a Hunter, hard and up) through
// one of its doors. The host checks the door and the room's owner validates it.
static bool ai_reinforce(unsigned int now)
{
    if (s_level->dwellMs == 0 || zm_game_role() != ZM_NET_ZOMBIE) return false;
    for (int i = 0; i < ZM_NET_MAX_PLAYERS; i++) {
        AiSurvivor& v = s_surv[i];
        if (!v.valid || v.dead || now - v.enteredMs < s_level->dwellMs) continue;
        if (v.pressedMs != 0 && now - v.pressedMs < s_level->dwellMs) continue;
        if (zm_random_room_safe(v.stage, v.room)) continue;
        v.pressedMs = now;
        unsigned char id = ENEMY_ZOMBIE;
        if (s_level->traps && zm_econ_unlock_left_ms(ENEMY_HUNTER) == 0 &&
            zm_econ_points() >= zm_econ_cost(ENEMY_HUNTER)) id = ENEMY_HUNTER;
        char why[48];
        if (zm_reinforce_request(v.stage, v.room, id, why, sizeof(why))) {
            s_minuteSpent += zm_econ_cost(id);
            ai_note("%s REINFORCES", ai_name(id), v.stage, v.room);
            return true;
        }
        dbg_printf("[ai] reinforcement refused: %s\n", why);
    }
    return false;
}

// A hurt survivor in a room with monsters: its doors shut for a while.
static bool ai_trap(unsigned int now)
{
    if (!s_level->traps || zm_trap_cooldown_ms(ZM_TRAP_LOCK_DOORS) > 0) return false;
    for (int i = 0; i < ZM_NET_MAX_PLAYERS; i++) {
        AiSurvivor& v = s_surv[i];
        if (!v.valid || v.dead || v.health >= AI_LOW_HEALTH) continue;
        if (zm_random_room_safe(v.stage, v.room)) continue;
        if (zm_room_monster_count(v.stage, v.room) <= 0) continue;
        if (!zm_director_place_ai(v.stage, v.room, ZM_TRAP_LOCK_DOORS, ai_name(ZM_TRAP_LOCK_DOORS))) continue;
        v.pressedMs = now;
        ai_note("%s", "DOORS LOCKED", v.stage, v.room);
        return true;
    }
    return false;
}

// The opening: as soon as the game is on, part of the starting points go into
// the rooms around the main hall (its doors' rooms, and theirs).
static void ai_opening(void)
{
    s_opened = true;
    unsigned char hallStage = (g_stageId % 5 == STAGE_MANSION_1F) ? g_stageId
        : (Flg_ck((int)&g_ScenarioFlags, SCENARIO_FLAG_STAGE_VARIANT) != 0 ? 5 : STAGE_MANSION_1F);
    int budget = zm_econ_points() * s_level->openingPct / 100;
    int spentFrom = zm_econ_points();
    ai_score_rooms(true, hallStage, ROOM_MAIN_HALL);
    for (int tries = 0; tries < 24 && spentFrom - zm_econ_points() < budget; tries++) {
        ai_place_one(true);
    }
    dbg_printf("[ai] opening: spent %d of %d\n", spentFrom - zm_econ_points(), budget);
}

void zm_ai_frame(void)
{
    if (zm_ai_level() == 0 || !zm_econ_on() || zombie_mode_match_over()) return;
    if (zm_game_role() != ZM_NET_OFF && zm_net_pause_active()) return;
    unsigned int now = zm_game_time_ms();
    ai_read_survivors(now);
    ai_pacing(now);
    if (!s_opened) {
        ai_opening();
        s_thinkMs = now;
        return;
    }
    if (now - s_thinkMs < s_level->thinkMs) return;
    s_thinkMs = now;
    // Survivors not in yet: nothing to aim at.
    if (zm_survivors_in_ms() < 0) return;
    if (ai_trap(now)) return;
    if (s_phase != AI_BUILD) return;
    if (ai_reinforce(now)) return;
    ai_score_rooms(false, 0, 0);
    ai_place_one(false);
}

// Under the points, top right: who is playing and what it did last.
void zm_ai_draw(void)
{
    if (zm_ai_level() == 0) return;
    char line[64];
    snprintf(line, sizeof(line), "AI %s %s %d", s_level->name, kPhaseName[s_phase], s_intensity / 16);
    snprintf(PRINT_TEXT_BUFFER, sizeof(PRINT_TEXT_BUFFER), "%s", line);
    int width = (int)strlen(PRINT_TEXT_BUFFER) * 8;
    zm_text_encode(PRINT_TEXT_BUFFER);
    PrintText8x14((short)(320 - 4 - width), 22, 0x7F, 0);
    if (s_lastAction[0] != '\0') {
        snprintf(PRINT_TEXT_BUFFER, sizeof(PRINT_TEXT_BUFFER), "%s", s_lastAction);
        width = (int)strlen(PRINT_TEXT_BUFFER) * 8;
        zm_text_encode(PRINT_TEXT_BUFFER);
        PrintText8x14((short)(320 - 4 - width), 36, 0x7F, 0);
    }
}
