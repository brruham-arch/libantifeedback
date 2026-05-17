#include <stdint.h>
#include <string.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <android/log.h>
#include <stdio.h>
#include <stdarg.h>

// ── Log ──────────────────────────────────────────────────────────────────────
#define LOG_TAG  "libantifeedback"
#define LOGFILE  "/storage/emulated/0/antifeedback_log.txt"
#define EXPORT   __attribute__((visibility("default")))

static void logf_impl(const char* msg) {
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "%s", msg);
    FILE* f = fopen(LOGFILE, "a");
    if (f) { fprintf(f, "%s\n", msg); fclose(f); }
}
static void logff(const char* fmt, ...) {
    char buf[256];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    logf_impl(buf);
}
#define LOGI(fmt, ...) logff("[AF] " fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) logff("[AF][ERR] " fmt, ##__VA_ARGS__)

// ── Tipe dasar BASS ──────────────────────────────────────────────────────────
typedef unsigned int  DWORD;
typedef unsigned int  HRECORD;
typedef unsigned int  HCHANNEL;
typedef int           BOOL;
typedef void          RECORDPROC;

typedef struct {
    DWORD freq;
    DWORD chans;
    DWORD flags;
    DWORD ctype;
    DWORD origres;
    DWORD plugin;
    DWORD sample;
    const char* filename;
} BASS_CHANNELINFO;

// ── Konstanta BASS ───────────────────────────────────────────────────────────
#define BASS_ATTRIB_VOL       2
#define BASS_CTYPE_STREAM     0x10000
#define BASS_ACTIVE_PLAYING   1

// ── Konstanta SV (SampVoice / AZVoice) ──────────────────────────────────────
#define SV_FREQ   48000
#define SV_CHANS  1

// ── State global ─────────────────────────────────────────────────────────────
#define MAX_SV_CHANNELS 64

static HCHANNEL  g_sv_channels[MAX_SV_CHANNELS];
static int       g_sv_count    = 0;
static HRECORD   g_rec_handle  = 0;
static int       g_mic_active  = 0;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

// ── Function pointers ─────────────────────────────────────────────────────────
static HRECORD (*orig_RecordStart)(DWORD, DWORD, DWORD, RECORDPROC*, void*) = nullptr;
static BOOL    (*orig_ChannelPlay)(DWORD, BOOL)                              = nullptr;
static BOOL    (*orig_ChannelFree)(DWORD)                                    = nullptr;
static BOOL    (*fn_ChannelGetInfo)(DWORD, BASS_CHANNELINFO*)                = nullptr;
static BOOL    (*fn_ChannelSetAttribute)(DWORD, DWORD, float)                = nullptr;
static DWORD   (*fn_ChannelIsActive)(DWORD)                                  = nullptr;

// ── Helper ───────────────────────────────────────────────────────────────────
static void sv_channel_add(HCHANNEL h) {
    pthread_mutex_lock(&g_mutex);
    if (g_sv_count < MAX_SV_CHANNELS) {
        g_sv_channels[g_sv_count++] = h;
    }
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
    BASS_CHANNELINFO info;
    memset(&info, 0, sizeof(info));
    if (!fn_ChannelGetInfo(handle, &info)) return 0;
    return ((info.ctype & BASS_CTYPE_STREAM) != 0)
        && (info.freq  == SV_FREQ)
        && (info.chans == SV_CHANS);
}

static void set_sv_volume(float vol) {
    if (!fn_ChannelSetAttribute) return;
    pthread_mutex_lock(&g_mutex);
    for (int i = 0; i < g_sv_count; i++) {
        fn_ChannelSetAttribute(g_sv_channels[i], BASS_ATTRIB_VOL, vol);
    }
    pthread_mutex_unlock(&g_mutex);
}

// ── Hooks ─────────────────────────────────────────────────────────────────────
static HRECORD hook_RecordStart(DWORD freq, DWORD chans, DWORD flags,
                                 RECORDPROC* proc, void* user) {
    HRECORD h = orig_RecordStart(freq, chans, flags, proc, user);
    g_rec_handle = h;
    LOGI("RecordStart hooked, handle=%u freq=%u ch=%u", h, freq, chans);
    return h;
}

static BOOL hook_ChannelPlay(DWORD handle, BOOL restart) {
    BOOL r = orig_ChannelPlay(handle, restart);
    if (is_sv_channel(handle)) {
        sv_channel_add(handle);
        if (g_mic_active && fn_ChannelSetAttribute) {
            fn_ChannelSetAttribute(handle, BASS_ATTRIB_VOL, 0.0f);
        }
        LOGI("SV channel captured: %u (total=%d)", handle, g_sv_count);
    }
    return r;
}

static BOOL hook_ChannelFree(DWORD handle) {
    sv_channel_remove(handle);
    return orig_ChannelFree(handle);
}

// ── Polling thread ────────────────────────────────────────────────────────────
static void* mic_poll_thread(void*) {
    int last_mic = 0;
    LOGI("Polling thread started");
    while (1) {
        usleep(50000); // 50ms
        if (!fn_ChannelIsActive || g_rec_handle == 0) continue;

        int mic_on = (fn_ChannelIsActive(g_rec_handle) == BASS_ACTIVE_PLAYING);

        if (mic_on && !last_mic) {
            g_mic_active = 1;
            set_sv_volume(0.0f);
            LOGI("Mic ON -> mute %d SV channels", g_sv_count);
        } else if (!mic_on && last_mic) {
            g_mic_active = 0;
            set_sv_volume(1.0f);
            LOGI("Mic OFF -> unmute SV channels");
        }
        last_mic = mic_on;
    }
    return nullptr;
}

// ── API untuk bridge Lua (opsional) ──────────────────────────────────────────
static int   _is_mic_active(void)  { return g_mic_active; }
static int   _get_sv_count(void)   { return g_sv_count; }

struct AntiFeedbackAPI {
    int (*is_mic_active)(void);
    int (*get_sv_count)(void);
};

// ── Entry points ──────────────────────────────────────────────────────────────
extern "C" {

EXPORT AntiFeedbackAPI antifeedback_api = {
    _is_mic_active,
    _get_sv_count
};

EXPORT void* __GetModInfo() {
    static const char* info = "antifeedback|1.0|Auto mute SV saat mic ON|brruham";
    return (void*)info;
}

EXPORT void OnModPreLoad() {
    remove(LOGFILE);
    memset(g_sv_channels, 0, sizeof(g_sv_channels));
    g_sv_count   = 0;
    g_rec_handle = 0;
    g_mic_active = 0;
    logf_impl("[AF] OnModPreLoad v1.0");
}

EXPORT void OnModLoad() {
    LOGI("OnModLoad mulai");

    // Load Dobby
    void* hDobby = dlopen("libdobby.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hDobby) { LOGE("libdobby.so tidak ditemukan"); return; }

    auto resolver  = (void*(*)(const char*, const char*))
                        dlsym(hDobby, "DobbySymbolResolver");
    auto dobbyHook = (int(*)(void*, void*, void**))
                        dlsym(hDobby, "DobbyHook");

    if (!resolver || !dobbyHook) {
        LOGE("Dobby symbols tidak ditemukan");
        return;
    }

    // Load BASS
    void* hBASS = dlopen("libBASS.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hBASS) { LOGE("libBASS.so tidak ditemukan"); return; }

    // Resolve semua symbols
    struct { const char* name; void** out; } syms[] = {
        { "BASS_RecordStart",       (void**)&orig_RecordStart      },
        { "BASS_ChannelPlay",       (void**)&orig_ChannelPlay      },
        { "BASS_ChannelFree",       (void**)&orig_ChannelFree      },
        { "BASS_ChannelGetInfo",    (void**)&fn_ChannelGetInfo     },
        { "BASS_ChannelSetAttribute",(void**)&fn_ChannelSetAttribute},
        { "BASS_ChannelIsActive",   (void**)&fn_ChannelIsActive    },
    };

    for (auto& s : syms) {
        void* addr = resolver("libBASS.so", s.name);
        if (!addr) {
            LOGE("Gagal resolve: %s", s.name);
            return;
        }
        *s.out = addr;
        LOGI("Resolved %s -> %p", s.name, addr);
    }

    // Pasang hooks (3 hooks, sisanya hanya fn pointer)
    struct { void* addr; void* hook; void** orig; const char* name; } hooks[] = {
        { (void*)orig_RecordStart, (void*)hook_RecordStart,
          (void**)&orig_RecordStart, "RecordStart" },
        { (void*)orig_ChannelPlay, (void*)hook_ChannelPlay,
          (void**)&orig_ChannelPlay, "ChannelPlay" },
        { (void*)orig_ChannelFree, (void*)hook_ChannelFree,
          (void**)&orig_ChannelFree, "ChannelFree" },
    };

    for (auto& hk : hooks) {
        int r = dobbyHook(hk.addr, hk.hook, hk.orig);
        if (r != 0) {
            LOGE("DobbyHook gagal untuk %s (ret=%d)", hk.name, r);
            return;
        }
        LOGI("Hook terpasang: %s", hk.name);
    }

    // fn pointers langsung dari resolver (tidak di-hook)
    // sudah di-set lewat syms[] di atas

    // Tulis alamat API untuk bridge Lua
    FILE* af = fopen("/storage/emulated/0/antifeedback_addr.txt", "w");
    if (af) {
        fprintf(af, "%lu\n", (unsigned long)&antifeedback_api);
        fclose(af);
    }

    // Start polling thread
    pthread_t tid;
    if (pthread_create(&tid, nullptr, mic_poll_thread, nullptr) == 0) {
        pthread_detach(tid);
        LOGI("Polling thread started");
    } else {
        LOGE("pthread_create gagal");
    }

    LOGI("OnModLoad SELESAI - hooks aktif, polling berjalan");
}

} // extern "C"
