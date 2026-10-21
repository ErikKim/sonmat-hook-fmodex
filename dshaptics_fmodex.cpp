// dshaptics_fmodex.dll - log every sound a game plays through FMOD Ex (fmodex64.dll shipped beside the game:
// Sekiro, Dark Souls 3 ...). One line per sound, in the Unreal adapter's shape so the engine needs nothing new:
//     ue:<sound name>|<name of the bank it came from>
// FMOD Ex is a separate DLL with a public C++ API, so the hook goes on an exported function found by its
// exported name - no byte patterns, nothing read from the game's own (packed) executable:
//     FMOD_RESULT FMOD::System::playSound(FMOD_CHANNELINDEX channelid, FMOD::Sound *sound, bool paused, FMOD::Channel **channel)
// The sound's name is the wave's file name inside the bank (FromSoftware keeps the original names:
// "bank/ntc_main/hit/iron-cut-iron.wav"), read with FMOD::Sound::getName.
// In the running game the banks are opened without names (every sound reads "(null)", first real run
// 2026-10-11), so a nameless sound is written by where it sits instead:
//     ue:#<sounds in its bank>.<its index in the bank, from 0>.<its length in samples>|
// tools/fmod_ids.py makes the table from the user's own bank files that turns this back into the name.
// Whose sound it is: FromSoftware's names say what a sound is (foot-soil, swing-katana), not who makes it, so
// every enemy's step would count as mine. The owner field carries how far from the listener the sound is,
//     ue:<name>|d<centimetres>
// taken from Channel::set3DAttributes between playSound(paused) and Channel::setPaused(false) - the event
// system starts a sound paused, places it, then lets it go. A sound never placed (2D) has an empty owner; one
// never unpaused through this call is written 80 ms after it was started, with whatever is known by then.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <string>
#include <cmath>
#include "MinHook.h"

typedef int (*PlaySound_t)(void* sys, int channelid, void* sound, bool paused, void** channel);
typedef int (*GetName_t)(void* sound, char* name, int len);
typedef int (*GetParent_t)(void* sound, void** parent);
static PlaySound_t g_play = nullptr;
static GetName_t g_name = nullptr;
static GetParent_t g_parent = nullptr;
static HANDLE g_log = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_lock;
typedef int (*GetNum_t)(void* sound, int* n);
typedef int (*GetSub_t)(void* sound, int index, void** sub);
typedef int (*GetLen_t)(void* sound, unsigned int* len, unsigned int unit);
static GetNum_t g_num = nullptr;
static GetSub_t g_sub = nullptr;
static GetLen_t g_len = nullptr;
static std::unordered_map<void*, std::string> g_where;      // sound -> "#num.index.len" (guarded by g_lock)

// A bank's sounds stay loaded while the bank is: the place of each is looked up once and remembered.
static std::string where_of(void* sound) {
    EnterCriticalSection(&g_lock);
    auto it = g_where.find(sound);
    std::string had = it != g_where.end() ? it->second : "";
    LeaveCriticalSection(&g_lock);
    void* parent = nullptr; int num = 0, idx = -1; unsigned int len = 0;
    if (g_len) g_len(sound, &len, 2);                        // FMOD_TIMEUNIT_PCM
    if (!had.empty()) {                                      // same address, another sound (its bank was replaced)?
        char tail[24]; snprintf(tail, sizeof tail, ".%u", len);
        size_t n = strlen(tail);
        if (had.size() > n && had.compare(had.size() - n, n, tail) == 0) return had;
    }
    if (g_parent && g_num && g_sub && g_parent(sound, &parent) == 0 && parent && parent != sound && g_num(parent, &num) == 0) {
        for (int i = 0; i < num && i < 20000; i++) {
            void* sub = nullptr;
            if (g_sub(parent, i, &sub) == 0 && sub == sound) { idx = i; break; }
        }
    }
    char buf[64]; snprintf(buf, sizeof buf, "#%d.%d.%u", num, idx, len);
    EnterCriticalSection(&g_lock);
    if (g_where.size() > 60000) g_where.clear();
    g_where[sound] = buf;
    LeaveCriticalSection(&g_lock);
    return buf;
}


struct FV { float x, y, z; };
typedef int (*Set3D_t)(void* ch, const FV* pos, const FV* vel);
typedef int (*SetPaused_t)(void* ch, bool paused);
typedef int (*Listener_t)(void* sys, int listener, const FV* pos, const FV* vel, const FV* fwd, const FV* up);
static Set3D_t g_set3d = nullptr;
static SetPaused_t g_setPaused = nullptr;
static Listener_t g_listener = nullptr;
struct Pending { std::string name; bool placed; FV pos; ULONGLONG t; };
static std::unordered_map<void*, Pending> g_pending;        // started paused, not let go yet (guarded by g_lock)
static FV g_ear = {0, 0, 0};
static bool g_hasEar = false;

static void emit(const Pending& p) {                         // g_lock held
    char line[700]; int n;
    if (p.placed && g_hasEar) {
        float dx = p.pos.x - g_ear.x, dy = p.pos.y - g_ear.y, dz = p.pos.z - g_ear.z;
        double d = sqrt((double)dx * dx + (double)dy * dy + (double)dz * dz);
        n = snprintf(line, sizeof line, "ue:%s|d%d\n", p.name.c_str(), (int)(d * 100 > 9999999 ? 9999999 : d * 100));
    } else n = snprintf(line, sizeof line, "ue:%s|\n", p.name.c_str());
    DWORD w; WriteFile(g_log, line, (DWORD)n, &w, nullptr);
}

static void flush_old() {                                    // g_lock held
    if (g_pending.empty()) return;
    ULONGLONG now = GetTickCount64();
    for (auto it = g_pending.begin(); it != g_pending.end();)
        if (now - it->second.t > 80) { emit(it->second); it = g_pending.erase(it); } else ++it;
}

static int hk_play(void* sys, int channelid, void* sound, bool paused, void** channel) {
    std::string label;
    if (sound && g_name && g_log != INVALID_HANDLE_VALUE) {
        char name[260] = "";
        if (g_name(sound, name, 256) != 0 || !name[0] || !strcmp(name, "(null)")) label = where_of(sound);
        else {
            for (char* p = name; *p; p++) if (*p == '|' || *p == '\n' || *p == '\r') *p = '_';
            label = name;
        }
    }
    int r = g_play(sys, channelid, sound, paused, channel);
    if (!label.empty()) {
        Pending p{label, false, {0, 0, 0}, GetTickCount64()};
        EnterCriticalSection(&g_lock);
        flush_old();
        if (r == 0 && paused && channel && *channel && g_setPaused) g_pending[*channel] = p;
        else emit(p);
        LeaveCriticalSection(&g_lock);
    }
    return r;
}

static int hk_set3d(void* ch, const FV* pos, const FV* vel) {
    if (pos) {
        EnterCriticalSection(&g_lock);
        auto it = g_pending.find(ch);
        if (it != g_pending.end()) { it->second.pos = *pos; it->second.placed = true; }
        LeaveCriticalSection(&g_lock);
    }
    return g_set3d(ch, pos, vel);
}

static int hk_setPaused(void* ch, bool paused) {
    EnterCriticalSection(&g_lock);
    if (!paused) {
        auto it = g_pending.find(ch);
        if (it != g_pending.end()) { emit(it->second); g_pending.erase(it); }
    }
    flush_old();
    LeaveCriticalSection(&g_lock);
    return g_setPaused(ch, paused);
}

static int hk_listener(void* sys, int listener, const FV* pos, const FV* vel, const FV* fwd, const FV* up) {
    if (pos && listener == 0) { g_ear = *pos; g_hasEar = true; }
    return g_listener(sys, listener, pos, vel, fwd, up);
}

static bool hook(HMODULE fm, const char* sym, void* mine, void** orig) {
    void* at = (void*)GetProcAddress(fm, sym);
    if (!at) return false;
    void* tramp = nullptr;
    if (MH_CreateHook(at, mine, &tramp) != MH_OK) return false;
    *orig = tramp;
    if (MH_EnableHook(at) != MH_OK) { *orig = nullptr; return false; }
    return true;
}

static DWORD WINAPI init(LPVOID self) {
    InitializeCriticalSection(&g_lock);
    char dir[MAX_PATH]; GetModuleFileNameA((HMODULE)self, dir, MAX_PATH);
    char* slash = strrchr(dir, '\\'); if (slash) slash[1] = 0;
    char status[MAX_PATH]; snprintf(status, sizeof status, "%sdshaptics_fmodex.log", dir);
    g_log = CreateFileW(L"C:\\Users\\Public\\dshaptics\\events_fmod.log", FILE_APPEND_DATA,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, 0, nullptr);
    const char* result = "fmodex64.dll never loaded";
    for (int attempt = 0; attempt < 100; attempt++) {           // the game may still be starting: wait for the DLL
        HMODULE fm = GetModuleHandleA("fmodex64.dll");
        if (fm) {
            void* at = (void*)GetProcAddress(fm, "?playSound@System@FMOD@@QEAA?AW4FMOD_RESULT@@W4FMOD_CHANNELINDEX@@PEAVSound@2@_NPEAPEAVChannel@2@@Z");
            g_name = (GetName_t)GetProcAddress(fm, "?getName@Sound@FMOD@@QEAA?AW4FMOD_RESULT@@PEADH@Z");
            g_parent = (GetParent_t)GetProcAddress(fm, "?getSubSoundParent@Sound@FMOD@@QEAA?AW4FMOD_RESULT@@PEAPEAV12@@Z");
            g_num = (GetNum_t)GetProcAddress(fm, "?getNumSubSounds@Sound@FMOD@@QEAA?AW4FMOD_RESULT@@PEAH@Z");
            g_sub = (GetSub_t)GetProcAddress(fm, "?getSubSound@Sound@FMOD@@QEAA?AW4FMOD_RESULT@@HPEAPEAV12@@Z");
            g_len = (GetLen_t)GetProcAddress(fm, "?getLength@Sound@FMOD@@QEAA?AW4FMOD_RESULT@@PEAII@Z");
            if (!at || !g_name) { result = "playSound / getName export not found"; break; }
            void* tramp = nullptr;
            if (MH_Initialize() != MH_OK) { result = "MinHook init failed"; break; }
            // the placing hooks first: playSound only holds a sound back when setPaused is there to let it go
            bool a3 = hook(fm, "?set3DAttributes@Channel@FMOD@@QEAA?AW4FMOD_RESULT@@PEBUFMOD_VECTOR@@0@Z", (void*)hk_set3d, (void**)&g_set3d);
            bool ls = hook(fm, "?set3DListenerAttributes@System@FMOD@@QEAA?AW4FMOD_RESULT@@HPEBUFMOD_VECTOR@@000@Z", (void*)hk_listener, (void**)&g_listener);
            bool sp = a3 && ls && hook(fm, "?setPaused@Channel@FMOD@@QEAA?AW4FMOD_RESULT@@_N@Z", (void*)hk_setPaused, (void**)&g_setPaused);
            MH_STATUS st = MH_CreateHook(at, (void*)hk_play, &tramp);
            if (st == MH_OK) { g_play = (PlaySound_t)tramp; st = MH_EnableHook(at); }
            static char msg[200];
            snprintf(msg, sizeof msg, "playSound: %s, distance: %s", st == MH_OK ? "hooked" : MH_StatusToString(st), sp ? "hooked" : "not hooked");
            result = msg;
            break;
        }
        Sleep(3000);
    }
    FILE* f = fopen(status, "w"); if (f) { fprintf(f, "%s\n", result); fclose(f); }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(h); CreateThread(nullptr, 0, init, h, 0, nullptr); }
    return TRUE;
}
