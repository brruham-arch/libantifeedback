#include <string.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <link.h>
#include <cstdint>
#include "mod/amlmod.h"

MYMOD(brruham.antifeedback, AntiFeedback, 1.0, brruham)

// ── BASS types ────────────────────────────────────────────────────────────────
typedef unsigned int DWORD;
typedef unsigned int HRECORD;
typedef unsigned int HCHANNEL;
typedef int          BOOL;
typedef void         RECORDPROC;

typedef struct {
    DWORD freq; DWORD chans; DWORD flags; DWORD ctype;
    DWORD origres; DWORD plugin; DWORD sample; const char* filename;
} BASS_CHANNELINFO;

#define BASS_ATTRIB_VOL     2
#define BASS_CTYPE_STREAM   0x10000
#define BASS_ACTIVE_PLAYING 1
#define SV_FREQ             48000
#define SV_CHANS            1
#define MAX_SV_CHANNELS     64

static HCHANNEL        g_sv_channels[MAX_SV_CHANNELS];
static int             g_sv_count   = 0;
static HRECORD         g_rec_handle = 0;
static int             g_mic_active = 0;
static volatile int    g_running    = 1; // FIX 1: flag stop thread
static pthread_mutex_t g_mutex      = PTHREAD_MUTEX_INITIALIZER;

static HRECORD (*orig_RecordStart)(DWORD, DWORD, DWORD, RECORDPROC*, void*) = nullptr;
static BOOL    (*orig_ChannelPlay)(DWORD, BOOL)                              = nullptr;
static BOOL    (*orig_ChannelFree)(DWORD)                                    = nullptr;
static BOOL    (*fn_ChannelGetInfo)(DWORD, BASS_CHANNELINFO*)                = nullptr;
static BOOL    (*fn_ChannelSetAttribute)(DWORD, DWORD, float)                = nullptr;
static DWORD   (*fn_ChannelIsActive)(DWORD)                                  = nullptr;

// ── GTA Text Rendering types ──────────────────────────────────────────────────
struct CRGBA { unsigned char r, g, b, a; };
typedef unsigned short gw;

typedef void (*fn_PS)(float, float, const gw*);
typedef void (*fn_SC)(CRGBA*);
typedef void (*fn_SS)(float);
typedef void (*fn_SO)(unsigned char);
typedef void (*fn_SD)(signed char);
typedef void (*fn_SF)(unsigned char);
typedef void (*fn_SE)(signed char);
typedef void (*fn_HD)();

#define OFF_PS  0x5AA191u
#define OFF_SC  0x5AAFC9u
#define OFF_SS  0x5AB109u
#define OFF_SO  0x5AB305u
#define OFF_SD  0x5A8A6Du
#define OFF_SF  0x5AB14Du
#define OFF_SE  0x5AB27Du
#define OFF_HD  0x43A659u

static fn_PS gPS  = nullptr;
static fn_SC gSC  = nullptr;
static fn_SS gSS  = nullptr;
static fn_SO gSO  = nullptr;
static fn_SD gSD  = nullptr;
static fn_SF gSF  = nullptr;
static fn_SE gSE  = nullptr;
static fn_HD gOHD = nullptr;

static gw   g_wide_normal[256] = {};
static gw   g_wide_muted [256] = {};
static bool g_wm_ready         = false;

#define T_PTR(a) ((a) | 1u)

// ── Helpers ───────────────────────────────────────────────────────────────────
static void tw(const char* s, gw* d, int m) {
    int i = 0;
    while (*s && i < m - 1) d[i++] = (gw)(unsigned char)*s++;
    d[i] = 0;
}

// ── Watermark ─────────────────────────────────────────────────────────────────
static void draw_watermark() {
    if (!gPS || !gSC || !gSS) return;

    const float X = 150.0f;
    const float Y = 340.0f;

    const gw* txt = g_mic_active ? g_wide_muted : g_wide_normal;

    if (gSF) gSF(1);
    if (gSD) gSD(0);
    if (gSE) gSE(1);

    // shadow
    CRGBA shadow = {0, 0, 0, 180};
    gSC(&shadow);
    gSS(2.0f);
    if (gSO) gSO(0);
    gPS(X + 1.5f, Y + 1.5f, txt);

    // teks utama — hijau normal, merah saat muted
    if (gSE) gSE(0);
    CRGBA color = g_mic_active ? CRGBA{255, 80, 80, 255} : CRGBA{80, 255, 80, 255};
    gSC(&color);
    gSS(2.0f);
    if (gSO) gSO(0);
    gPS(X, Y, txt);
}

static void hook_DrawAfterFade() {
    if (gOHD) ((fn_HD)gOHD)();
    if (g_wm_ready) draw_watermark();
}

// ── libGTASA base finder ──────────────────────────────────────────────────────
static int find_gtasa_base(struct dl_phdr_info* info, size_t, void* data) {
    if (strstr(info->dlpi_name, "libGTASA.so")) {
        *(uintptr_t*)data = info->dlpi_addr;
        return 1;
    }
    return 0;
}

// ── Init thread watermark ─────────────────────────────────────────────────────
static void* wm_init_thread(void*) {
    uintptr_t base = 0;
    while (base == 0) {
        dl_iterate_phdr(find_gtasa_base, &base);
        sleep(1);
    }
    sleep(5);

    void* hDobby = dlopen("libdobby.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hDobby) return nullptr;
    auto dobbyHook = (int(*)(void*, void*, void**))dlsym(hDobby, "DobbyHook");
    if (!dobbyHook) return nullptr;

    gSC = (fn_SC)T_PTR(base + OFF_SC);
    gSS = (fn_SS)T_PTR(base + OFF_SS);
    gSO = (fn_SO)T_PTR(base + OFF_SO);
    gSD = (fn_SD)T_PTR(base + OFF_SD);
    gSF = (fn_SF)T_PTR(base + OFF_SF);
    gSE = (fn_SE)T_PTR(base + OFF_SE);
    gPS = (fn_PS)T_PTR(base + OFF_PS);

    void* target = (void*)T_PTR(base + OFF_HD);
    if (dobbyHook(target, (void*)hook_DrawAfterFade, (void**)&gOHD) == 0)
        g_wm_ready = true;

    return nullptr;
}

// ── BASS helpers ──────────────────────────────────────────────────────────────
static void sv_channel_add(HCHANNEL h) {
    pthread_mutex_lock(&g_mutex);
    if (g_sv_count < MAX_SV_CHANNELS) g_sv_channels[g_sv_count++] = h;
    pthread_mutex_unlock(&g_mutex);
}
static void sv_channel_remove(HCHANNEL h) {
    pthread_mutex_lock(&g_mutex);
    for (int i = 0; i < g_sv_count; i++) {
        if (g_sv_channels[i] == h) {
            g_sv_channels[i] = g_sv_channels[--g_sv_count];
            break;
        }
    }
    pthread_mutex_unlock(&g_mutex);
}
static int is_sv_channel(DWORD handle) {
    if (!fn_ChannelGetInfo) return 0;
    BASS_CHANNELINFO info; memset(&info, 0, sizeof(info));
    if (!fn_ChannelGetInfo(handle, &info)) return 0;
    return ((info.ctype & BASS_CTYPE_STREAM) != 0)
        && (info.freq == SV_FREQ)
        && (info.chans == SV_CHANS);
}
static void set_sv_volume(float vol) {
    if (!fn_ChannelSetAttribute) return;
    pthread_mutex_lock(&g_mutex);
    for (int i = 0; i < g_sv_count; i++)
        fn_ChannelSetAttribute(g_sv_channels[i], BASS_ATTRIB_VOL, vol);
    pthread_mutex_unlock(&g_mutex);
}

// ── BASS hooks ────────────────────────────────────────────────────────────────
static HRECORD hook_RecordStart(DWORD freq, DWORD chans, DWORD flags, RECORDPROC* proc, void* user) {
    HRECORD h = orig_RecordStart(freq, chans, flags, proc, user);
    g_rec_handle = h;
    return h;
}
static BOOL hook_ChannelPlay(DWORD handle, BOOL restart) {
    BOOL r = orig_ChannelPlay(handle, restart);
    if (is_sv_channel(handle)) {
        sv_channel_add(handle);
        if (g_mic_active && fn_ChannelSetAttribute)
            fn_ChannelSetAttribute(handle, BASS_ATTRIB_VOL, 0.0f);
    }
    return r;
}
static BOOL hook_ChannelFree(DWORD handle) {
    sv_channel_remove(handle);
    // FIX 2: reset g_rec_handle kalau handle mic yang di-free
    if (handle == g_rec_handle) g_rec_handle = 0;
    return orig_ChannelFree(handle);
}

// ── Mic polling thread ────────────────────────────────────────────────────────
static void* mic_poll_thread(void*) {
    int last_mic = 0;
    while (g_running) { // FIX 1: cek flag, bukan while(1)
        usleep(50000);
        if (!fn_ChannelIsActive || g_rec_handle == 0) continue;

        // FIX 3: validasi handle masih valid sebelum akses
        BASS_CHANNELINFO info; memset(&info, 0, sizeof(info));
        if (!fn_ChannelGetInfo || !fn_ChannelGetInfo(g_rec_handle, &info)) {
            g_rec_handle = 0; // handle sudah invalid, reset
            continue;
        }

        int mic_on = (fn_ChannelIsActive(g_rec_handle) == BASS_ACTIVE_PLAYING);
        if (mic_on && !last_mic)      { g_mic_active = 1; set_sv_volume(0.0f); }
        else if (!mic_on && last_mic) { g_mic_active = 0; set_sv_volume(1.0f); }
        last_mic = mic_on;
    }
    return nullptr;
}

// ── AML Entry points ──────────────────────────────────────────────────────────
ON_MOD_PRELOAD() {
    memset(g_sv_channels, 0, sizeof(g_sv_channels));
    g_sv_count   = 0;
    g_rec_handle = 0;
    g_mic_active = 0;
    g_running    = 1;
    g_wm_ready   = false;

    // FIX 4: teks berbeda untuk tiap state
    tw("[AntiFeedback]",    g_wide_normal, 256);
    tw("[AntiFeedback]", g_wide_muted,  256);
}

ON_MOD_LOAD() {
    void* hDobby = dlopen("libdobby.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hDobby) return;
    auto resolver  = (void*(*)(const char*, const char*))dlsym(hDobby, "DobbySymbolResolver");
    auto dobbyHook = (int(*)(void*, void*, void**))dlsym(hDobby, "DobbyHook");
    if (!resolver || !dobbyHook) return;

    void* hBASS = dlopen("libBASS.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hBASS) return;

    struct { const char* name; void** out; } syms[] = {
        { "BASS_RecordStart",         (void**)&orig_RecordStart       },
        { "BASS_ChannelPlay",         (void**)&orig_ChannelPlay       },
        { "BASS_ChannelFree",         (void**)&orig_ChannelFree       },
        { "BASS_ChannelGetInfo",      (void**)&fn_ChannelGetInfo      },
        { "BASS_ChannelSetAttribute", (void**)&fn_ChannelSetAttribute },
        { "BASS_ChannelIsActive",     (void**)&fn_ChannelIsActive     },
    };
    for (auto& s : syms) {
        void* addr = resolver("libBASS.so", s.name);
        if (!addr) return;
        *s.out = addr;
    }

    struct { void* addr; void* hook; void** orig; } bass_hooks[] = {
        { (void*)orig_RecordStart, (void*)hook_RecordStart, (void**)&orig_RecordStart },
        { (void*)orig_ChannelPlay, (void*)hook_ChannelPlay, (void**)&orig_ChannelPlay },
        { (void*)orig_ChannelFree, (void*)hook_ChannelFree, (void**)&orig_ChannelFree },
    };
    for (auto& hk : bass_hooks) {
        if (dobbyHook(hk.addr, hk.hook, hk.orig) != 0) return;
    }

    pthread_t tid_mic;
    if (pthread_create(&tid_mic, nullptr, mic_poll_thread, nullptr) == 0)
        pthread_detach(tid_mic);

    pthread_t tid_wm;
    if (pthread_create(&tid_wm, nullptr, wm_init_thread, nullptr) == 0)
        pthread_detach(tid_wm);

    aml->ShowToast(false, "[AntiFeedback] Aktif");
}

ON_MOD_UNLOAD() {
    g_running  = 0; // signal mic_poll_thread untuk stop
    g_wm_ready = false;
}
