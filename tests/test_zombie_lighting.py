"""CPU regression tests for persistent room lighting; never launches the game."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from unittest.mock import patch

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tools"))
import evaluate_zombie_routes as routes
source = (root / "src/game/mods/ZombieRoomSync.cpp").read_text()
production = source[source.index("#define RS_STUDY_LIT"):source.index("// The rooms whose system flags")]
fixture = r"""
#include <cassert>
#include <cstddef>
#include <cstdio>
#define STAGE_MANSION_RETURN_1F 5
#define STAGE_MANSION_RETURN_2F 6
#define ROOM_SMALL_DINING 15
#define ROOM_MANSION_1F_STUDY 25
#define ROOM_TROPHY_ROOM 21
#define MSF_CAMERA_LOCK 1
#define MSF_CAMERA_DEFER 2
#define MSF_CAMERA_REDRAW 4
#define MSF_ROOM_TRANSITION 8
unsigned int g_ScenarioFlags[4],g_ScenarioFlags2[8],g_SysFlags[2],g_roomItemsFlags[8];
unsigned char *g_ScdOpcodes;
int g_main_state_flags, g_roomTransitionBusy, g_stageId,g_roomId;
unsigned char g_roomCameraId;
short g_SpecialRoomLightState,g_SpecialRoomLightDelta;
struct CAM_SWITCH_ZONE {short camTo,camFrom;short quad[8];};
CAM_SWITCH_ZONE zones[10];
struct Rdt {unsigned char cameras_count=8; unsigned char* cam_switch_zones=(unsigned char*)zones;} rdt;
Rdt* g_RdtPointer=&rdt;
void* g_CurrentRdtDataTypePtr;
int flames,pickups,redraws;
bool armed=true;
bool zombie_mode_armed(){return armed;}
int Flg_ck(int base,unsigned int bit){return (((unsigned int*)base)[bit/32] & (0x80000000u>>(bit%32)))!=0;}
void Flg_on(int base,unsigned int bit){((unsigned int*)base)[bit/32]|=0x80000000u>>(bit%32);}
void FUN_00473f10(int* base,unsigned int bit){((unsigned int*)base)[bit/32]&=~(0x80000000u>>(bit%32));}
void dbg_printf(const char*,...){}
void StMask(int,int){}
void display_room_camera_bg(){redraws++;}
int cmd_room_lights_set(){return 1;}
int cmd_effect_spawn(){flames++;return 1;}
int cmd_room_action_reset(){if(g_ScdOpcodes[1]==4)pickups++;return 1;}
int cmd_bullet_effect_spawn(){return 1;}
int cmd_bullet_effect_clear(){return 1;}
"""
checks = r"""
int main(){
 for(int i=0;i<8;i++)zones[i].camFrom=(short)i;
 zones[8].camFrom=-1;
 // Study starts dark; local switch persists, then survives room reload.
 g_stageId=5;g_roomId=25;g_roomCameraId=2;rs_light_room();assert(!s_lightOn);
 FUN_00473f10((int*)g_SysFlags,10);rs_light_frame();
 assert(Flg_ck((int)g_ScenarioFlags2,0xFA)&&g_roomCameraId==0);
 Flg_on((int)g_SysFlags,10);g_roomCameraId=2;rs_light_room();
 assert(!Flg_ck((int)g_SysFlags,10)&&g_roomCameraId==0);
 // Remote STORY update changes the local switch state without echoing it back.
 FUN_00473f10((int*)g_ScenarioFlags2,0xFA);rs_light_frame();
 assert(Flg_ck((int)g_SysFlags,10)&&g_roomCameraId==2);
 rs_light_frame();assert(!Flg_ck((int)g_ScenarioFlags2,0xFA));
 // Native ROOM70F0 init selects camera 3 only in the lit branch (with
 // flames and reward). With the candle flag clear, entry must stay dark.
 g_stageId=6;g_roomId=15;g_roomCameraId=0;rs_light_room();
 assert(!s_lightOn&&g_roomCameraId==0&&flames==0&&pickups==0);
 rs_light_frame();assert(g_roomCameraId==0&&flames==0&&pickups==0);
 // Each dark camera stays in the dark group on entry, even if an old
 // camera from the lit group was restored by a close-up.
 for(int i=0;i<3;i++){
  g_roomCameraId=(unsigned char)i;rs_light_room();assert(g_roomCameraId==i);
  g_roomCameraId=(unsigned char)(i+3);rs_light_room();assert(g_roomCameraId==i);
 }
 g_roomCameraId=0;
 // Remote candle update unlocks an untaken reward and spawns each flame once.
 Flg_on((int)g_roomItemsFlags,12);Flg_on((int)g_ScenarioFlags,10);rs_light_frame();
 assert(g_roomCameraId==3&&flames==3&&pickups==1);
 rs_light_frame();assert(flames==3&&pickups==1);
 g_roomCameraId=0;rs_light_room();assert(g_roomCameraId==3&&flames==3);
 // Camera groups map in both directions; locked close-ups wait for release.
 for(int kind=1;kind<=3;kind++)for(int i=0;i<(kind==2?2:3);i++){
  s_lightKind=kind;s_lightOn=true;
  int dark=kind==2?i+2:i;
  int lit=kind==1?i+3:kind==3?i+4:i;
  g_roomCameraId=(unsigned char)dark;g_main_state_flags=MSF_CAMERA_LOCK;
  rs_light_camera(false);assert(g_roomCameraId==dark);
  g_main_state_flags=0;rs_light_camera(false);assert(g_roomCameraId==lit);
  assert(((CAM_SWITCH_ZONE*)g_CurrentRdtDataTypePtr)->camFrom==lit);
  s_lightOn=false;rs_light_camera(false);assert(g_roomCameraId==dark);
 }
 s_lightKind=3;s_lightOn=true;g_roomCameraId=7;rs_light_camera(false);assert(g_roomCameraId==7);
 puts("Lighting OK: persistent study switch, remote changes, candle reward, camera variants and close-up release");
}
"""
with tempfile.TemporaryDirectory(prefix="re1-light-tests-") as temp:
    with patch.object(routes, "adapter_source", return_value=fixture + production + checks):
        exe = routes.build_adapter(Path(temp))
    subprocess.run([str(exe)], check=True)
