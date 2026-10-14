#include "ZombieBumpers.h"
#include "ZombieModeInternal.h"
#include "ZombieNet.h"
#include "../../platform/platform.h"
#include "../../marni/MarniDX.h"
#include "../../marni/MarniSound.h"
#include "../../DebugPrint.h"
#include <cstdio>
#include <cstring>
#include <cmath>

namespace {
const char* const kCharacters[] = { "chris", "jill", "barry", "rebecca", "richard", "enrico" };
struct Panel {
    PlatVideoClip* clip = NULL;
    MarniHandle texture = MARNI_NULL_HANDLE;
    int width = 0, height = 0;
    bool finished = false;
};
Panel s_panels[ZM_NET_MAX_PLAYERS];
int s_roster[ZM_NET_MAX_PLAYERS], s_rosterCount = 0, s_count = 0;
int s_introBank = 0, s_pendingEnd = -1;
bool s_introDone = false, s_draw = false, s_recovered = false;
unsigned int s_started = 0;

void close_panels()
{
    s_draw = false;
    MarniDX* dx = Marni_DX();
    for (int i = 0; i < ZM_NET_MAX_PLAYERS; ++i) {
        plat_video_clip_close(s_panels[i].clip);
        if (dx && s_panels[i].texture) dx->DestroyTexture(s_panels[i].texture);
        s_panels[i] = Panel();
    }
    if (s_introBank) { setSndStop(s_introBank); destroySndBank(s_introBank); s_introBank = 0; }
    s_count = 0;
}
bool asset_path(const char* relative, char* path, size_t size)
{
    char dir[1024], raw[1200];
    if (!plat_exe_dir(dir, sizeof(dir))) return false;
    snprintf(raw, sizeof(raw), "%s/infested/%s", dir, relative);
    const char* normalized = plat_normalize_path(raw, path, size);
    if (normalized != path) snprintf(path, size, "%s", normalized);
    return true;
}
void capture_roster()
{
    s_rosterCount = 0;
    if (zm_game_role() == ZM_NET_OFF) {
        s_roster[s_rosterCount++] = (g_playerEntity.id & 1) ? ZM_CHAR_JILL : ZM_CHAR_CHRIS;
    } else {
        for (int i = 0; i < ZM_NET_MAX_PLAYERS; ++i) {
            int ch = zm_net_char(i);
            if (ch >= 0 && ch < ZM_CHAR_COUNT) s_roster[s_rosterCount++] = ch;
        }
    }
}
bool open_panels(const char* folder, bool wesker)
{
    close_panels();
    if (!s_rosterCount) capture_roster();
    s_count = wesker ? 1 : s_rosterCount;
    bool have = false;
    for (int i = 0; i < s_count; ++i) {
        char relative[128], path[1200];
        if (wesker) snprintf(relative, sizeof(relative), "video/wesker_victory.mp4");
        else snprintf(relative, sizeof(relative), "video/%s/%s.mp4", folder, kCharacters[s_roster[i]]);
        if (asset_path(relative, path, sizeof(path))) s_panels[i].clip = plat_video_clip_open(path);
        s_panels[i].finished = !s_panels[i].clip;
        dbg_printf("[bumper] %s: %s\n", relative, s_panels[i].clip ? "loaded" : "missing or unsupported; panel skipped");
        have |= s_panels[i].clip != NULL;
    }
    return have;
}
void start(bool intro)
{
    s_started = plat_time_ms();
    s_draw = true;
    if (intro) {
        char path[1200];
        if (asset_path("audio/intro.wav", path, sizeof(path))) s_introBank = loadSndBankFromWav(path);
        if (s_introBank) {
            pan_set(s_introBank, 0);
            set_volume(s_introBank, g_SfxVolume);
            playSnd(s_introBank, 0);
        } else dbg_printf("[bumper] audio/intro.wav missing or unsupported\n");
    }
}
void idle()
{
    g_PlayerPadHeld = g_PlayerPadPressed = g_PlayerDpadHeld = g_PlayerDpadPressed = 0;
    g_button_pressed_id = 0;
    if (zm_game_role() != ZM_NET_OFF) zm_net_poll();
    Task_sleep(1);
}
bool update()
{
    unsigned int elapsed = plat_time_ms() - s_started;
    bool done = true;
    MarniDX* dx = Marni_DX();
    for (int i = 0; i < s_count; ++i) {
        Panel& p = s_panels[i];
        if (!p.clip || p.finished) continue;
        const unsigned char* rgba = NULL;
        p.finished = !plat_video_clip_frame(p.clip, (int)elapsed, &rgba, &p.width, &p.height);
        done &= p.finished;
        if (!rgba || !dx) continue;
        if (!p.texture) {
            int tw, th;
            p.texture = dx->CreateTexture(p.width, p.height, 32, rgba, &tw, &th);
        } else dx->UpdateTexturePixels(p.texture, rgba, p.width, p.height, 32);
    }
    return done;
}
} // namespace

void zm_bumpers_reset(void)
{
    close_panels();
    s_introDone = false; s_pendingEnd = -1; s_rosterCount = 0;
    s_recovered = zm_net_rejoining();
}

void zombie_mode_bumper_intro(void)
{
    if (!zombie_mode_armed() || s_introDone) return;
    s_introDone = true;
    // A recovered match is already running, including its original roster/clock.
    if (s_recovered || zm_net_rejoining()) { capture_roster(); return; }
    bool network = zm_game_role() != ZM_NET_OFF;
    unsigned int waitAt = plat_time_ms();
    int phase = network ? zm_net_bumper_phase(0) : 1;
    while (phase < 1 && plat_time_ms() - waitAt < 150000 && zm_net_status() != ZM_NET_EXPIRED) {
        zm_draw_centered("WAITING FOR SURVIVOR INTROS", 110, 0x7F);
        idle(); phase = zm_net_bumper_phase(0);
    }
    if (phase != 1) {
        dbg_printf("[bumper] intro barrier unavailable; skipped\n");
        zm_bumper_intro_clock_release();
        return;
    }
    capture_roster();
    bool have = open_panels("intro", false);
    phase = network ? zm_net_bumper_phase(1) : 2;
    while (phase < 2 && plat_time_ms() - waitAt < 150000 && zm_net_status() != ZM_NET_EXPIRED) {
        idle(); phase = zm_net_bumper_phase(1);
    }
    if (phase != 2) { close_panels(); zm_bumper_intro_clock_release(); return; }
    start(true);
    // Intro assets are three seconds / 72 frames at 24 fps. Every copy waits
    // the same duration even if it has no local assets; short clips hold last frame.
    if (!have) s_draw = false;
    while (plat_time_ms() - s_started < 3000) { update(); idle(); }
    s_draw = false;
    phase = network ? zm_net_bumper_phase(2) : 3;
    while (phase < 3 && plat_time_ms() - waitAt < 150000 && zm_net_status() != ZM_NET_EXPIRED) {
        idle(); phase = zm_net_bumper_phase(2);
    }
    close_panels();
    zm_bumper_intro_clock_release();
    dbg_printf("[bumper] intro complete: %d survivor panels\n", s_rosterCount);
}

void zm_bumpers_match_end(int reason)
{
    if (reason == ZM_END_ESCAPE || reason == ZM_END_ALL_DEAD) s_pendingEnd = reason;
}
bool zombie_mode_bumper_end_wait(void)
{
    if (!zombie_mode_armed() || s_pendingEnd < 0) return false;
    int reason = s_pendingEnd; s_pendingEnd = -1;
    if (!open_panels("victory", reason == ZM_END_ALL_DEAD)) { close_panels(); return false; }
    start(false);
    // End clips play through EOF. Keep shorter panels on their final frame.
    // Bound malformed/accidentally long assets so the results remain reachable.
    while (plat_time_ms() - s_started < 30000) {
        bool done = update();
        idle();
        if (done) break;
    }
    close_panels();
    dbg_printf("[bumper] ending complete: reason %d\n", reason);
    return true;
}

bool zombie_mode_bumper_draw(void)
{
    if (!s_draw || !s_count) return false;
    MarniDX* dx = Marni_DX();
    if (!dx) return false;
    DWORD bw, bh; dx->GetBackBufferSize(&bw, &bh);
    if (!bw || !bh) return false;
    float w = (float)bw, h = (float)bh;
    // 15 degrees from vertical, in physical screen pixels. Identical slope
    // for each seam preserves equal panel areas. Outer screen edges stay vertical.
    float shift = s_count > 1 ? h * 0.5f * 0.2679491924f : 0;
    float panelW = w / s_count;
    if (shift > panelW * 0.45f) shift = panelW * 0.45f;
    dx->Clear(0, 0, 0, 1);
    for (int i = 0; i < s_count; ++i) {
        Panel& p = s_panels[i];
        if (!p.texture || !p.width || !p.height) continue;
        float left = i * panelW, right = (i+1) * panelW;
        float lt = i ? left + shift : 0, lb = i ? left - shift : 0;
        float rt = i+1 < s_count ? right + shift : w;
        float rb = i+1 < s_count ? right - shift : w;
        // Crop-to-fill inside the polygon's bounding rectangle; never stretch
        // or rotate the video. UVs follow screen position, not slanted edges.
        float minX = lb, maxX = rt, boxW = maxX - minX;
        float scale = h / p.height;
        if (boxW / p.width > scale) scale = boxW / p.width;
        float imageW = p.width * scale, imageH = p.height * scale;
        float originX = (minX+maxX-imageW)*0.5f, originY = (h-imageH)*0.5f;
        float corners[4][2] = { {lt,0}, {rt,0}, {rb,h}, {lb,h} };
        const int indices[6] = { 0,1,2, 0,2,3 };
        float verts[48];
        for (int v = 0; v < 6; ++v) {
            float x = corners[indices[v]][0], y = corners[indices[v]][1];
            float* out = verts + v*8;
            out[0]=x; out[1]=y; out[2]=(x-originX)/imageW; out[3]=(y-originY)/imageH;
            out[4]=out[5]=out[6]=out[7]=1;
        }
        dx->DrawTriangles(verts, 2, p.texture, MARNI_SAMPLER_LINEAR, MARNI_BLEND_DISABLE);
    }
    for (int i = 1; i < s_count; ++i)
        dx->DrawLine(i*panelW+shift, 0, i*panelW-shift, h, h/240.0f*2.0f, 0xFFFFFFFFu);
    return true;
}
