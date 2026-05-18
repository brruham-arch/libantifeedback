#include <string.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <jni.h>
#include "mod/amlmod.h"

MYMOD(brruham.antifeedback, AntiFeedback, 1.0, brruham)

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
static pthread_mutex_t g_mutex      = PTHREAD_MUTEX_INITIALIZER;

static HRECORD (*orig_RecordStart)(DWORD, DWORD, DWORD, RECORDPROC*, void*) = nullptr;
static BOOL    (*orig_ChannelPlay)(DWORD, BOOL)                              = nullptr;
static BOOL    (*orig_ChannelFree)(DWORD)                                    = nullptr;
static BOOL    (*fn_ChannelGetInfo)(DWORD, BASS_CHANNELINFO*)                = nullptr;
static BOOL    (*fn_ChannelSetAttribute)(DWORD, DWORD, float)                = nullptr;
static DWORD   (*fn_ChannelIsActive)(DWORD)                                  = nullptr;

// ── Notifikasi Android ────────────────────────────────────────────────────────
static void show_notification(const char* title, const char* text) {
    JNIEnv* env = aml->GetJNIEnvironment();
    jobject ctx = aml->GetAppContextObject();
    if (!env || !ctx) return;

    // NotificationManager
    jclass    clsCtx   = env->FindClass("android/content/Context");
    jmethodID midGSS   = env->GetMethodID(clsCtx, "getSystemService",
                             "(Ljava/lang/String;)Ljava/lang/Object;");
    jstring   svcName  = env->NewStringUTF("notification");
    jobject   nm       = env->CallObjectMethod(ctx, midGSS, svcName);
    env->DeleteLocalRef(svcName);
    if (!nm || env->ExceptionCheck()) { env->ExceptionClear(); return; }

    // NotificationChannel (Android 8+)
    jclass    clsNM    = env->GetObjectClass(nm);
    jstring   chanId   = env->NewStringUTF("antifeedback_ch");

    // Cek Android versi via Build.VERSION.SDK_INT
    jclass    clsBuild = env->FindClass("android/os/Build$VERSION");
    jfieldID  fidSDK   = env->GetStaticFieldID(clsBuild, "SDK_INT", "I");
    jint      sdkInt   = env->GetStaticIntField(clsBuild, fidSDK);

    if (sdkInt >= 26) {
        jclass    clsNC    = env->FindClass("android/app/NotificationChannel");
        jstring   chanName = env->NewStringUTF("AntiFeedback");
        // IMPORTANCE_DEFAULT = 3
        jmethodID midNC    = env->GetMethodID(clsNC, "<init>",
                                 "(Ljava/lang/String;Ljava/lang/CharSequence;I)V");
        jobject   channel  = env->NewObject(clsNC, midNC, chanId, chanName, 3);
        env->DeleteLocalRef(chanName);

        jmethodID midCreateCh = env->GetMethodID(clsNM, "createNotificationChannel",
                                    "(Landroid/app/NotificationChannel;)V");
        env->CallVoidMethod(nm, midCreateCh, channel);
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(channel);
    }

    // Notification.Builder atau NotificationCompat — pakai Notification.Builder
    jclass    clsBuilder;
    jobject   builder;
    if (sdkInt >= 26) {
        clsBuilder = env->FindClass("android/app/Notification$Builder");
        jmethodID midBI = env->GetMethodID(clsBuilder, "<init>",
                              "(Landroid/content/Context;Ljava/lang/String;)V");
        builder = env->NewObject(clsBuilder, midBI, ctx, chanId);
    } else {
        clsBuilder = env->FindClass("android/app/Notification$Builder");
        jmethodID midBI = env->GetMethodID(clsBuilder, "<init>",
                              "(Landroid/content/Context;)V");
        builder = env->NewObject(clsBuilder, midBI, ctx);
    }
    env->DeleteLocalRef(chanId);

    if (!builder || env->ExceptionCheck()) { env->ExceptionClear(); return; }

    // Set title, text, icon, auto-cancel
    jstring jTitle = env->NewStringUTF(title);
    jstring jText  = env->NewStringUTF(text);

    // android.R.drawable.ic_dialog_info = 0x01080020
    auto setTitle = env->GetMethodID(clsBuilder, "setContentTitle",
                        "(Ljava/lang/CharSequence;)Landroid/app/Notification$Builder;");
    auto setText  = env->GetMethodID(clsBuilder, "setContentText",
                        "(Ljava/lang/CharSequence;)Landroid/app/Notification$Builder;");
    auto setIcon  = env->GetMethodID(clsBuilder, "setSmallIcon",
                        "(I)Landroid/app/Notification$Builder;");
    auto setAC    = env->GetMethodID(clsBuilder, "setAutoCancel",
                        "(Z)Landroid/app/Notification$Builder;");
    auto setOngoing = env->GetMethodID(clsBuilder, "setOngoing",
                          "(Z)Landroid/app/Notification$Builder;");

    env->CallObjectMethod(builder, setTitle, jTitle);
    env->CallObjectMethod(builder, setText,  jText);
    env->CallObjectMethod(builder, setIcon,  0x01080020); // ic_dialog_info
    env->CallObjectMethod(builder, setAC,    (jboolean)0);
    env->CallObjectMethod(builder, setOngoing, (jboolean)1); // persistent
    if (env->ExceptionCheck()) env->ExceptionClear();

    env->DeleteLocalRef(jTitle);
    env->DeleteLocalRef(jText);

    // Build notifikasi
    jmethodID midBuild = env->GetMethodID(clsBuilder, "build",
                             "()Landroid/app/Notification;");
    jobject notif = env->CallObjectMethod(builder, midBuild);
    if (!notif || env->ExceptionCheck()) { env->ExceptionClear(); return; }

    // notify(id, notification)
    jmethodID midNotify = env->GetMethodID(clsNM, "notify",
                              "(ILandroid/app/Notification;)V");
    env->CallVoidMethod(nm, midNotify, (jint)1001, notif);
    if (env->ExceptionCheck()) env->ExceptionClear();

    env->DeleteLocalRef(notif);
    env->DeleteLocalRef(builder);
    env->DeleteLocalRef(nm);
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
        if (g_sv_channels[i] == h) { g_sv_channels[i] = g_sv_channels[--g_sv_count]; break; }
    }
    pthread_mutex_unlock(&g_mutex);
}
static int is_sv_channel(DWORD handle) {
    if (!fn_ChannelGetInfo) return 0;
    BASS_CHANNELINFO info; memset(&info, 0, sizeof(info));
    if (!fn_ChannelGetInfo(handle, &info)) return 0;
    return ((info.ctype & BASS_CTYPE_STREAM) != 0) && (info.freq == SV_FREQ) && (info.chans == SV_CHANS);
}
static void set_sv_volume(float vol) {
    if (!fn_ChannelSetAttribute) return;
    pthread_mutex_lock(&g_mutex);
    for (int i = 0; i < g_sv_count; i++) fn_ChannelSetAttribute(g_sv_channels[i], BASS_ATTRIB_VOL, vol);
    pthread_mutex_unlock(&g_mutex);
}

// ── Hooks ─────────────────────────────────────────────────────────────────────
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
    return orig_ChannelFree(handle);
}

// ── Polling thread ────────────────────────────────────────────────────────────
static void* mic_poll_thread(void*) {
    int last_mic = 0;
    while (1) {
        usleep(50000);
        if (!fn_ChannelIsActive || g_rec_handle == 0) continue;
        int mic_on = (fn_ChannelIsActive(g_rec_handle) == BASS_ACTIVE_PLAYING);
        if (mic_on && !last_mic)      { g_mic_active = 1; set_sv_volume(0.0f); }
        else if (!mic_on && last_mic) { g_mic_active = 0; set_sv_volume(1.0f); }
        last_mic = mic_on;
    }
    return nullptr;
}

// ── Entry points ──────────────────────────────────────────────────────────────
ON_MOD_PRELOAD() {
    memset(g_sv_channels, 0, sizeof(g_sv_channels));
    g_sv_count = 0; g_rec_handle = 0; g_mic_active = 0;
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

    aml->ShowToast(false, "[AntiFeedback] Aktif");
    show_notification("AntiFeedback", "Mod aktif - speaker otomatis mute saat mic ON");
}
