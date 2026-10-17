"""CPU tests of production time reward logic; never launches the game."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import evaluate_zombie_routes as routes

root = Path(__file__).resolve().parents[1]
economy = (root / "src/game/mods/ZombieEconomy.cpp").read_text()
random = (root / "src/game/mods/ZombieRandom.cpp").read_text()
mode = (root / "src/game/mods/ZombieMode.cpp").read_text()

def function(source, name):
    start = re.search(r"^(?:static )?(?:void|int|bool) " + name + r"\([^\n]*\)\n\{", source, re.M).start()
    end = source.index("{", start)
    depth = 1
    while depth:
        end += 1
        depth += (source[end] == "{") - (source[end] == "}")
    return source[start:end + 1] + "\n"

constants = ""
for filename, names in [
    ("src/game/entities/EntityCommon.h", ["ENEMY_ZOMBIE", "ENEMY_ZOMBIE_VARIANT", "ENEMY_ZOMBIE_NAKED",
      "ENEMY_CERBERUS", "ENEMY_HUNTER", "ENEMY_WEB_SPINNER", "ENEMY_CHIMERA", "ENEMY_TYRANT_2"]),
    ("src/game/Types.h", ["ITEM_BERETTA", "ITEM_CLIP", "ITEM_FLAME_ROUNDS", "ITEM_FIRST_AID_SPRAY", "ITEM_RED_HERB", "ITEM_GREEN_HERB", "ITEM_SWORD_KEY", "ITEM_HELMET_KEY", "ITEM_WIND_CREST", "ITEM_MOON_CREST", "ITEM_STAR_CREST", "ITEM_SUN_CREST"]),
]:
    text = (root / filename).read_text()
    for name in names:
        constants += re.search(r"^#define " + name + r"\s+[^\n]+", text, re.M).group() + "\n"

fixture = r"""
#include <cassert>
#include <initializer_list>
#include <cstring>
#include <cstdio>
static bool s_on=true, s_active=true, s_built=true, safe=false, authority=true, ended=false, paused=false, bumper=false;
static unsigned char s_refunded[65536/8];
static bool s_deadSeen[30];
#define ZM_WORLD_UID_NONE 0xffff
#define ENTITY_STATUS_DEAD 8
#define ZOMBIE_STATE_DIE 3
#define ZOMBIE_STATE_ATTACK 5
struct Entity { unsigned char id=0,status_flags=1,state=1,action_state=0; short health=100; };
bool zm_is_zombie_id(unsigned char id){return id==0||id==1||id==17;}
static Entity g_EnemiesList[30];
static bool s_slotCorpse[30];

static int s_bonus, s_lost, breaks, sent;
static unsigned int gained, s_seed=12345;
#define ZM_WORLD_UID_BODY 0xFFFE
#define ZM_ECON_REFUND_DIV 4
#define ZM_NET_MAX_PLAYERS 4
#define ZM_NET_OFF 0
#define ZM_NET_DIRECTOR 0
#define ZM_EV_TIME_PICKUP 37
bool zm_yawn_lockdown(){return false;}
int zm_econ_cost(unsigned char){return 100;}
void zm_clock_add_bonus(unsigned int ms){gained+=ms;}
void dbg_printf(const char*,...){}
static bool s_leaf[2]={true,true};
int rnd_room_index(unsigned char stage,unsigned char room){return stage==5&&room<2?room:-1;}
bool zm_random_room_safe(unsigned char,unsigned char){return safe;}
struct Spawn { unsigned char stage,room; short spot[3][3]={{2000,0,3000},{4000,0,5000},{6000,0,7000}}; };
using ZmSpawnSpots=Spawn;
#define RND_ROOMS 2
static Spawn g_zmSpawnSpots[]={{5,0},{5,1}};
static int g_zmSpawnSpotCount=2;
"""
fixture += constants
fixture += re.search(r"struct RndSpot \{.*?\n\};", random, re.S).group() + "\n"
fixture += r"""
static RndSpot s_spots[2],s_timeSpots[4];
static int s_timeSpotCount;
static unsigned char s_usedFlags[32];
const short* rnd_new_pickup_position(const Spawn& t,int i){return t.spot[i];}
static int s_spotCount=2;
static unsigned char g_roomItemsFlags[32];
static unsigned int s_timeBurst[2],s_timeVisualSeed;
static int g_stageId=5,g_roomId=0;
struct Peer {bool valid=true,dead=false,spectating=false,transitioning=false; int health=100,stage=5,room=0,x=2000,y=0,z=3000;};
static Peer peers[4];
using ZmNetPeerState=Peer;
static struct {struct {struct {int t[3];} localMatrix;} scaMatrixData;} g_playerEntity;
RndSpot rnd_time_position(const RndSpot& s){return s;}
bool zombie_mode_armed(){return s_on;}
bool zm_match_authority(){return authority;}
bool zombie_mode_match_over(){return ended;}
bool zm_net_pause_active(){return paused;}
bool zm_bumpers_hold_input(){return bumper;}
unsigned int zm_game_time_ms(){return 5000;}
int zm_game_role(){return 1;}
const Peer* zm_seat_state(int i){return peers+i;}
int zm_room_survivors(int (*)[2],int*,int){return 0;}
int Flg_ck(int base,unsigned int flag){return ((unsigned char*)base)[flag/8]&(1<<(flag%8));}
void FUN_00473f10(int* base,unsigned int flag){((unsigned char*)base)[flag/8]&=~(1<<(flag%8));}
void rnd_time_shatter(int){breaks++;}
void zm_net_send_event(int,short,short,short,short){sent++;}
"""
production = function(mode, "zm_monster_dead") + function(mode, "zm_monster_death_settled")
production += function(economy, "zm_econ_refund")
production += function(economy, "zm_econ_kill_confirmed") + function(economy, "zm_econ_after_update")
for name in ["rnd_is_key", "rnd_is_crest", "rnd_time_room_eligible", "rnd_build_time_spots", "rnd_time_seconds"]:
    production += function(random, name)
production += function(random, "zm_time_pickups_take") + function(random, "zm_time_pickups_frame")
checks = r"""
int main(){
    const unsigned char ids[]={ENEMY_ZOMBIE,ENEMY_ZOMBIE_VARIANT,ENEMY_ZOMBIE_NAKED,ENEMY_CERBERUS,ENEMY_HUNTER,ENEMY_WEB_SPINNER,ENEMY_CHIMERA,ENEMY_TYRANT_2};
    for(int i=0;i<8;i++){
        unsigned int before=gained; zm_econ_refund(ids[i],0x100+i);
        assert(gained-before==(i<4?5000u:i<7?10000u:0u));
        zm_econ_refund(ids[i],0x100+i); assert(gained-before==(i<4?5000u:i<7?10000u:0u));
    }
    for(int i=0;i<300;i++) zm_econ_refund(ENEMY_ZOMBIE,0x200+i);
    unsigned int before=gained; zm_econ_refund(ENEMY_ZOMBIE,0x100); assert(gained==before);
    s_on=false;zm_econ_refund(ENEMY_ZOMBIE,0x500);assert(gained==before);s_on=true;
    // A fall can revive even with negative health. No credit until settled.
    for(unsigned char id : { (unsigned char)ENEMY_ZOMBIE, (unsigned char)ENEMY_ZOMBIE_VARIANT, (unsigned char)ENEMY_ZOMBIE_NAKED }) {
        Entity& e=g_EnemiesList[0];e=Entity();e.id=id;
        unsigned short uid=0x600+id;
        before=gained;
        e.status_flags=9;zm_econ_after_update(&e,0,uid);assert(gained==before); // live bite/prone
        e.status_flags=1;e.health=-1;e.state=3;e.action_state=1;
        zm_econ_after_update(&e,0,uid);assert(gained==before); // dying fall
        e.action_state=2;zm_econ_after_update(&e,0,uid);assert(gained==before);
        e.health=1;e.state=1;e.action_state=0;zm_econ_after_update(&e,0,uid);assert(gained==before); // revived
        e.health=-1;e.state=3;e.action_state=3;e.status_flags=15;
        zm_econ_after_update(&e,0,uid);assert(gained==before+5000);
        zm_econ_after_update(&e,0,uid);assert(gained==before+5000); // only once
        e.health=100;e.status_flags=1;zm_econ_after_update(&e,0,uid);
    }
    before=gained;
    // A leaf room with tools stays eligible; each of the eight progression
    // items excludes the entire room, regardless of pickup/pool/puzzle type.
    s_spotCount=1;s_spots[0].stage=5;s_spots[0].room=0;s_spots[0].id=0x13;
    assert(rnd_time_room_eligible(5,0));
    const unsigned char progression[]={ITEM_SWORD_KEY,ITEM_SWORD_KEY+1,ITEM_SWORD_KEY+2,ITEM_HELMET_KEY,
        ITEM_WIND_CREST,ITEM_MOON_CREST,ITEM_STAR_CREST,ITEM_SUN_CREST};
    for(unsigned char id:progression){s_spots[0].id=id;assert(!rnd_time_room_eligible(5,0));}
    s_spots[0].id=0x13;s_spots[0].puzzle=1;
    s_leaf[0]=false;assert(!rnd_time_room_eligible(5,0));s_leaf[0]=true;
    memset(s_usedFlags,255,sizeof(s_usedFlags));s_usedFlags[31]=0x3F;
    rnd_build_time_spots();assert(s_timeSpotCount==2);
    assert(s_timeSpots[0].flag==255&&s_timeSpots[1].flag==254);
    assert(s_spots[0].id==0x13&&s_spotCount==1); // ordinary items untouched
    assert(s_timeSpots[0].qty==5||s_timeSpots[0].qty==10);
    // A key in room 1 removes only that room's sphere.
    s_spotCount=2;s_spots[1].stage=5;s_spots[1].room=1;s_spots[1].id=ITEM_WIND_CREST;
    memset(s_usedFlags,0,sizeof(s_usedFlags));rnd_build_time_spots();assert(s_timeSpotCount==1);
    RndSpot s=s_timeSpots[0];unsigned int chosen=s_seed;
    memset(s_usedFlags,0,sizeof(s_usedFlags));rnd_build_time_spots();
    assert(!memcmp(&s,&s_timeSpots[0],sizeof(s))); // seeded placement is stable
    memset(s_usedFlags,255,sizeof(s_usedFlags));rnd_build_time_spots();assert(!s_timeSpotCount);
    s_timeSpots[0]=s;s_timeSpotCount=1;
    memset(g_roomItemsFlags,255,sizeof(g_roomItemsFlags));
    for(int p=0;p<4;p++){peers[p].x=s.x;peers[p].z=s.z;}
    peers[0].dead=true;peers[1].transitioning=true;peers[2].room=1;peers[3].x=20000;
    zm_time_pickups_frame();assert(gained==before&&sent==0);
    peers[1].transitioning=false;peers[2].room=0;
    authority=false;zm_time_pickups_frame();assert(gained==before);authority=true;
    paused=true;zm_time_pickups_frame();assert(gained==before);paused=false;
    unsigned int reward=rnd_time_seconds(s)*1000u;
    zm_time_pickups_frame();assert(gained==before+reward&&sent==1&&breaks==1);
    zm_time_pickups_frame();assert(gained==before+reward&&sent==1&&breaks==1);
    // Repeated host observations and client collection messages never add time.
    short a[8]={0,(short)chosen,(short)(chosen>>16)};
    zm_time_pickups_take(a,1);assert(breaks==1);
    a[1]++;zm_time_pickups_take(a,0);assert(breaks==1);a[1]--;
    zm_time_pickups_take(a,0);assert(breaks==2&&gained==before+reward);
    puts("Time rewards OK: kill amounts, long-match deduplication, independent room spawns, eight progression exclusions, flag allocation, host collection, race and event checks");
}
"""
with tempfile.TemporaryDirectory(prefix="re1-time-tests-") as temp:
    with patch.object(routes, "adapter_source", return_value=fixture + production + checks):
        executable = routes.build_adapter(Path(temp))
    subprocess.run([str(executable)], check=True)
