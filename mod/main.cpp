#include <string.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <jni.h>

#define EXPORT __attribute__((visibility("default")))

// ── JavaVM global (diisi di JNI_OnLoad) ──────────────────────────────────────
static JavaVM* g_jvm = nullptr;

// ── Toast: dijalankan di dedicated thread dengan Looper sendiri ───────────────
struct ToastArgs { char msg[256]; };

static void* toast_thread(void* arg) {
    ToastArgs* ta = (ToastArgs*)arg;
    if (!g_jvm) { delete ta; return nullptr; }

    JNIEnv* env = nullptr;
    if (g_jvm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
        delete ta; return nullptr;
    }

    // Looper.prepare() agar Toast bisa show di thread ini
    jclass    clsLooper = env->FindClass("android/os/Looper");
    jmethodID midPrep   = env->GetStaticMethodID(clsLooper, "prepare", "()V");
    env->CallStaticVoidMethod(clsLooper, midPrep);
    if (env->ExceptionCheck()) env->ExceptionClear(); // abaikan jika sudah ada looper

    // Ambil context via ActivityThread
    jclass    clsAT     = env->FindClass("android/app/ActivityThread");
    jmethodID midCurAT  = env->GetStaticMethodID(clsAT, "currentActivityThread",
                              "()Landroid/app/ActivityThread;");
    jmethodID midGetApp = env->GetMethodID(clsAT, "getApplication",
                              "()Landroid/app/Application;");
    jobject at      = env->CallStaticObjectMethod(clsAT, midCurAT);
    jobject context = env->CallObjectMethod(at, midGetApp);

    // Toast.makeText(context, msg, LENGTH_SHORT).show()
    jclass    clsToast = env->FindClass("android/widget/Toast");
    jmethodID midMake  = env->GetStaticMethodID(clsToast, "makeText",
                             "(Landroid/content/Context;Ljava/lang/CharSequence;I)"
                             "Landroid/widget/Toast;");
    jmethodID midShow  = env->GetMethodID(clsToast, "show", "()V");

    jstring jmsg  = env->NewStringUTF(ta->msg);
    jobject toast = env->CallStaticObjectMethod(clsToast, midMake, context, jmsg, 0);
    env->CallVoidMethod(toast, midShow);
    env->DeleteLocalRef(jmsg);

    g_jvm->DetachCurrentThread();
    delete ta;
    return nullptr;
}

static void show_toast(const char* msg) {
    if (!g_jvm) return;
    ToastArgs* ta = new ToastArgs();
    strncpy(ta->msg, msg, sizeof(ta->msg) - 1);
    ta->msg[sizeof(ta->msg) - 1] = '\0';
    pthread_t tid;
    if (pthread_create(&tid, nullptr, toast_thread, ta) == 0)
        pthread_detach(tid);
    else
        delete ta;
}

// ── Tipe dasar BASS ──────────────────────────────────────────────────────────
typedef unsigned int DWORD;
typedef unsigned int HRECORD;
typedef unsigned int HCHANNEL;
typedef int          BOOL;
typedef void         RECORDPROC;

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

#define BASS_ATTRIB_VOL     2
#define BASS_CTYPE_STREAM   0x10000
#define BASS_ACTIVE_PLAYING 1
#define SV_FREQ             48000
#define SV_CHANS            1

// ── State global ─────────────────────────────────────────────────────────────
#define MAX_SV_CHANNELS 64

static HCHANNEL        g_sv_channels[MAX_SV_CHANNELS];
static int             g_sv_count   = 0;
static HRECORD         g_rec_handle = 0;
static int             g_mic_active = 0;
static pthread_mutex_t g_mutex      = PTHREAD_MUTEX_INITIALIZER;

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
    if (g_sv_count < MAX_SV_CHANNELS)
        g_sv_channels[g_sv_count++] = h;
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
    for (int i = 0; i < g_sv_count; i++)
        fn_ChannelSetAttribute(g_sv_channels[i], BASS_ATTRIB_VOL, vol);
    pthread_mutex_unlock(&g_mutex);
}

// ── Hooks ─────────────────────────────────────────────────────────────────────
static HRECORD hook_RecordStart(DWORD freq, DWORD chans, DWORD flags,
                                 RECORDPROC* proc, void* user) {
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
    return orig_ChannelFree(handle);
}

// ── Polling thread ────────────────────────────────────────────────────────────
static void* mic_poll_thread(void*) {
    int last_mic = 0;
    while (1) {
        usleep(50000);
        if (!fn_ChannelIsActive || g_rec_handle == 0) continue;

        int mic_on = (fn_ChannelIsActive(g_rec_handle) == BASS_ACTIVE_PLAYING);

        if (mic_on && !last_mic) {
            g_mic_active = 1;
            set_sv_volume(0.0f);
        } else if (!mic_on && last_mic) {
            g_mic_active = 0;
            set_sv_volume(1.0f);
        }
        last_mic = mic_on;
    }
    return nullptr;
}

// ── Entry points ──────────────────────────────────────────────────────────────
extern "C" {

// JNI_OnLoad dipanggil saat .so di-dlopen — tangkap JavaVM di sini
EXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    g_jvm = vm;
    return JNI_VERSION_1_6;
}

EXPORT void* __GetModInfo() {
    static const char* info = "antifeedback|1.0|Auto mute SV saat mic ON|brruham";
    return (void*)info;
}

EXPORT void OnModPreLoad() {
    memset(g_sv_channels, 0, sizeof(g_sv_channels));
    g_sv_count   = 0;
    g_rec_handle = 0;
    g_mic_active = 0;
}

EXPORT void OnModLoad() {
    void* hDobby = dlopen("libdobby.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hDobby) return;

    auto resolver  = (void*(*)(const char*, const char*))
                        dlsym(hDobby, "DobbySymbolResolver");
    auto dobbyHook = (int(*)(void*, void*, void**))
                        dlsym(hDobby, "DobbyHook");
    if (!resolver || !dobbyHook) return;

    void* hBASS = dlopen("libBASS.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hBASS) return;

    struct { const char* name; void** out; } syms[] = {
        { "BASS_RecordStart",        (void**)&orig_RecordStart       },
        { "BASS_ChannelPlay",        (void**)&orig_ChannelPlay       },
        { "BASS_ChannelFree",        (void**)&orig_ChannelFree       },
        { "BASS_ChannelGetInfo",     (void**)&fn_ChannelGetInfo      },
        { "BASS_ChannelSetAttribute",(void**)&fn_ChannelSetAttribute },
        { "BASS_ChannelIsActive",    (void**)&fn_ChannelIsActive     },
    };
    for (auto& s : syms) {
        void* addr = resolver("libBASS.so", s.name);
        if (!addr) return;
        *s.out = addr;
    }

    struct { void* addr; void* hook; void** orig; } hooks[] = {
        { (void*)orig_RecordStart, (void*)hook_RecordStart, (void**)&orig_RecordStart },
        { (void*)orig_ChannelPlay, (void*)hook_ChannelPlay, (void**)&orig_ChannelPlay },
        { (void*)orig_ChannelFree, (void*)hook_ChannelFree, (void**)&orig_ChannelFree },
    };
    for (auto& hk : hooks) {
        if (dobbyHook(hk.addr, hk.hook, hk.orig) != 0) return;
    }

    pthread_t tid;
    if (pthread_create(&tid, nullptr, mic_poll_thread, nullptr) == 0)
        pthread_detach(tid);

    show_toast("[AntiFeedback] Aktif");
}

} // extern "C"
