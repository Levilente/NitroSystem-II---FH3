#ifndef FH3MOD_PLATFORM_VERSION
#define FH3MOD_PLATFORM_VERSION "2.8.0"
#endif
#include "fh3mod.h"
#include "legacy_files.h"

#include <windows.h>
#include <intrin.h>
#include <mmsystem.h>
#include <xinput.h>

#include <atomic>
#include <unordered_set>
#include <vector>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "winmm.lib")

// ---- Forza Horizon 3 port --------------------------------------------------------------------------------
// FH3 (Xbox One build 1.0.125.0, exe of 5 Jun 2018, run through Azimuth) differs from FH2 in every address and in
// CCarDynamics' layout. Found by lining the two exes up (docs/FH3_PORT.md):
//   CCarDynamics' per-tick update: vtable slot 16 (FH2 0x12C1EA0, this, ?, ?, ?, float dt on the stack;
//   FH3 0x1D1E300, this, ?, float dt in the third argument).
//   Fields (from the vtable getters: FH2 GetTransform/GetVelocity = FH3 slots 1 / 101 / 102):
//     position   FH2 +0x10  FH3 +0x30
//     forward    FH2 +0x40  FH3 +0x90   (third row of the rotation; FH3 rows at +0x70/+0x80/+0x90)
//     velocity   FH2 +0x50  FH3 +0x10   (FH3 also keeps its length at +0x12D4, written by SetVelocity)
//   CWorldCollObjMan::Update: FH2 0x384C00 (player position at mgr+0x4F0), FH3 0x425D40 (at mgr+0x520).
// Not ported yet (left off in this build): the purchasable Nitrous Kit (every car gets [FH3] Stage from the ini),
// exhaust flames, camera shake / FOV, the NOS gauge and HUD theme, the MODS page settings.
#define NITRO_FH3 1
static std::atomic<float> g_push_scale{2.0f};   // [FH3] PowerScale
static const uintptr_t kFH3CarUpdate   = 0x1D1E300;
static const uintptr_t kFH3WorldUpdate = 0x425D40;
static const int kFH3MgrPlayerPos = 0x520;
static const int kFH3CarVel = 0x10, kFH3CarPos = 0x30, kFH3CarFwd = 0x90, kFH3CarSpeed = 0x12D4;

#ifdef __MINGW32__
// mingw: the wide secure printf is the plain underscore one there
#define swprintf_s(buf, n, ...) _snwprintf((buf), (n), __VA_ARGS__)
#endif

// XInput is loaded at run time (xinput1_4.dll, then 1.3 / 9.1.0), so the build needs no XInput import library.
namespace xin {
typedef DWORD (WINAPI *GetStateFn)(DWORD, XINPUT_STATE*);
typedef DWORD (WINAPI *SetStateFn)(DWORD, XINPUT_VIBRATION*);
static GetStateFn get_state = nullptr;
static SetStateFn set_state = nullptr;
static bool tried = false;
static void load() {
    if (tried) return;
    tried = true;
    const wchar_t* names[] = {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"};
    for (const wchar_t* n : names) {
        HMODULE m = LoadLibraryW(n);
        if (!m) continue;
        get_state = reinterpret_cast<GetStateFn>(reinterpret_cast<void*>(GetProcAddress(m, "XInputGetState")));
        set_state = reinterpret_cast<SetStateFn>(reinterpret_cast<void*>(GetProcAddress(m, "XInputSetState")));
        if (get_state) return;
    }
}
}  // namespace xin

// Configuration & State
static const FH3ModApi* g_api = nullptr;
static FH3Mod* g_mod = nullptr;

static std::atomic<bool> g_enabled{true};
static std::atomic<bool> g_debug{false};          // on-screen debug overlay (Mods page)
static std::atomic<bool> g_backfire_all_cars{true};  // "Backfire on All Cars": applied to the database at startup (needs a restart)
static std::atomic<bool> g_rumble{true};             // "Controller Rumble" while nitro burns
static std::atomic<bool> g_refund_on_rewind{true};   // "Refund Nitro on Rewind": a rewind gives back the nitro used since the point it returns to
static std::atomic<bool> g_refill_on_start{true};    // "Refill Nitro on Race Start": full tank when the car is put somewhere else
// [Settings] HudHide=1 (default) ties the opacity of the NOS frame to the nitrous kit (hidden without one) through extra
// bindings in the speedometer XAML. 0 leaves the XAML as it was: the frame stays, empty, on cars without a kit.
static std::atomic<bool> g_hud_hide{true};
static std::atomic<int> g_hud_theme{1};              // "HUD Theme": 0 = Original (yellow NOS bar), 1 = Fast & Furious (cyan); needs a restart
static std::atomic<float> g_duration{5.0f};          // tank size in seconds (5 s here; the original default was 20 s)
static std::atomic<float> g_buildup_time{0.5f};      // Spire default BuildupTime = 0.5s
static std::atomic<float> g_recharge_rate{0.2f};     // 0.2 seconds of nitro recharged per second idle
static std::atomic<float> g_camera_shake{0.65f};     // camera shake while nitro burns, 0..1
static std::atomic<float> g_fov_boost{0.10f};        // extra field of view while nitro burns, 0..0.3 (fraction of the normal FOV)
static std::atomic<bool>  g_vig_on{true};            // the pulsing blue vignette while nitro burns
static constexpr float g_vig_opacity = 0.95f;        // its overall opacity (fixed; only the on/off switch is a setting)
static constexpr float g_vig_intensity = 0.90f;      // how far in and how strongly the glow builds up from the edges (fixed)
static std::atomic<bool>  g_sl_on{true};             // NFS Unbound style speed lines flying out from the middle while nitro burns
static constexpr float g_sl_opacity = 0.60f;         // their opacity (fixed)
static std::atomic<float> g_player_speed{0.0f};      // the player's car speed in m/s, from the physics step (steers the speed of the speed lines)
static std::atomic<float> g_sound_volume{0.5f};      // boost and stop sounds, 0..1 (the game's own volume sliders don't reach them)

static std::atomic<bool> g_nitro_active{false};
static std::atomic<bool> g_depleted_lockout{false};
static std::atomic<float> g_current_fuel{5.0f};
static std::atomic<float> g_current_boost{1.0f};

static wchar_t g_ini_path[MAX_PATH] = {0};
static wchar_t g_sound_boost_path[MAX_PATH] = {0};
static wchar_t g_sound_stop_path[MAX_PATH] = {0};

// File paths initialization
static void init_paths() {
    if (g_api && g_api->azimuth_dir) {
        swprintf_s(g_ini_path, MAX_PATH, L"%ls\\fh3-mods\\NitroSystemII.ini", g_api->azimuth_dir);
        swprintf_s(g_sound_boost_path, MAX_PATH, L"%ls\\NitroSystemII\\nos_boost.wav", g_api->azimuth_dir);
        swprintf_s(g_sound_stop_path, MAX_PATH, L"%ls\\NitroSystemII\\nos_stop.wav", g_api->azimuth_dir);
    }
}

// The nitrous kit upgrade (garage part, 3 stages, kept per owned car): nu_upgrade.h
static void nu_say(const char* t) { if (g_api && g_mod) g_api->log(g_mod, t); }
#include "nu_upgrade.h"

// Nitro button binding (NitroButton in the ini). Triggers are analog, so they get their own flags.
struct ButtonName { const wchar_t* name; WORD mask; };
static constexpr WORD BTN_LT = 0x8001;
static constexpr WORD BTN_RT = 0x8002;
static const ButtonName k_buttons[] = {
    {L"LB", XINPUT_GAMEPAD_LEFT_SHOULDER}, {L"RB", XINPUT_GAMEPAD_RIGHT_SHOULDER},
    {L"A", XINPUT_GAMEPAD_A}, {L"B", XINPUT_GAMEPAD_B}, {L"X", XINPUT_GAMEPAD_X}, {L"Y", XINPUT_GAMEPAD_Y},
    {L"LS", XINPUT_GAMEPAD_LEFT_THUMB}, {L"RS", XINPUT_GAMEPAD_RIGHT_THUMB},
    {L"DPadUp", XINPUT_GAMEPAD_DPAD_UP}, {L"DPadDown", XINPUT_GAMEPAD_DPAD_DOWN},
    {L"DPadLeft", XINPUT_GAMEPAD_DPAD_LEFT}, {L"DPadRight", XINPUT_GAMEPAD_DPAD_RIGHT},
    {L"Back", XINPUT_GAMEPAD_BACK}, {L"Start", XINPUT_GAMEPAD_START},
    {L"LT", BTN_LT}, {L"RT", BTN_RT},
};
static std::atomic<WORD> g_nitro_button{XINPUT_GAMEPAD_LEFT_SHOULDER};

static int button_index() {
    for (int i = 0; i < (int)(sizeof(k_buttons) / sizeof(k_buttons[0])); ++i)
        if (k_buttons[i].mask == g_nitro_button.load()) return i;
    return 0;
}

static bool nitro_button_down(const XINPUT_GAMEPAD& pad) {
    WORD b = g_nitro_button.load();
    if (b == BTN_LT) return pad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
    if (b == BTN_RT) return pad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
    return (pad.wButtons & b) != 0;
}

// INI Persistence
static void load_config() {
    if (g_ini_path[0] == L'\0') return;

    wchar_t btn[32];
    GetPrivateProfileStringW(L"Settings", L"NitroButton", L"LB", btn, 32, g_ini_path);
    g_nitro_button.store(XINPUT_GAMEPAD_LEFT_SHOULDER);   // unknown names fall back to LB
    for (const auto& b : k_buttons)
        if (_wcsicmp(btn, b.name) == 0) g_nitro_button.store(b.mask);

    int en = GetPrivateProfileIntW(L"Settings", L"NitroEnabled", 1, g_ini_path);
    g_enabled.store(en != 0);
    g_debug.store(GetPrivateProfileIntW(L"Settings", L"DebugOverlay", 0, g_ini_path) != 0);
    g_backfire_all_cars.store(GetPrivateProfileIntW(L"Settings", L"BackfireOnAllCars", 1, g_ini_path) != 0);
    g_refill_on_start.store(GetPrivateProfileIntW(L"Settings", L"RefillOnRaceStart", 1, g_ini_path) != 0);
    g_refund_on_rewind.store(GetPrivateProfileIntW(L"Settings", L"RefundOnRewind", 1, g_ini_path) != 0);
    g_rumble.store(GetPrivateProfileIntW(L"Settings", L"Rumble", 1, g_ini_path) != 0);

    g_hud_hide.store(GetPrivateProfileIntW(L"Settings", L"HudHide", 1, g_ini_path) != 0);
    wchar_t theme[32];
    GetPrivateProfileStringW(L"Settings", L"HudTheme", L"FF", theme, 32, g_ini_path);
    g_hud_theme.store(_wcsicmp(theme, L"Original") == 0 ? 0 : 1);   // unknown values fall back to FF

    wchar_t buf[64];
    // Older versions kept a TorqueScale here; the push now comes from the nitrous kit stage bought for the car
    // ([Upgrade] Boost1..3), so that key is gone. ConfigVersion 2 also moves the tank from the old 20 s to 5 s, once.
    const bool old_config = GetPrivateProfileIntW(L"Settings", L"ConfigVersion", 0, g_ini_path) < 2;
    if (old_config) {
        WritePrivateProfileStringW(L"Settings", L"TorqueScale", nullptr, g_ini_path);
        WritePrivateProfileStringW(L"Settings", L"Duration", L"5.0", g_ini_path);
        WritePrivateProfileStringW(L"Settings", L"ConfigVersion", L"2", g_ini_path);
    }
    if (GetPrivateProfileStringW(L"Settings", L"Duration", L"5.0", buf, 64, g_ini_path) > 0) {
        float val = wcstof(buf, nullptr);
        if (val >= 5.0f && val <= 60.0f) {
            g_duration.store(val);
            g_current_fuel.store(val);
        }
    }
    if (GetPrivateProfileStringW(L"Settings", L"BuildupTime", L"0.5", buf, 64, g_ini_path) > 0) {
        float val = wcstof(buf, nullptr);
        if (val >= 0.1f && val <= 2.0f) g_buildup_time.store(val);
    }
    if (GetPrivateProfileStringW(L"Settings", L"RechargeRate", L"0.2", buf, 64, g_ini_path) > 0) {
        float val = wcstof(buf, nullptr);
        if (val >= 0.0f && val <= 5.0f) g_recharge_rate.store(val);
    }
    if (GetPrivateProfileStringW(L"Settings", L"SoundVolume", L"0.5", buf, 64, g_ini_path) > 0) {
        float val = wcstof(buf, nullptr);
        if (val >= 0.0f && val <= 1.0f) g_sound_volume.store(val);
    }
    if (GetPrivateProfileStringW(L"Settings", L"CameraShake", L"0.65", buf, 64, g_ini_path) > 0) {
        float val = wcstof(buf, nullptr);
        if (val >= 0.0f && val <= 1.0f) g_camera_shake.store(val);
    }
    if (GetPrivateProfileStringW(L"Settings", L"FovBoost", L"0.10", buf, 64, g_ini_path) > 0) {
        float val = wcstof(buf, nullptr);
        if (val >= 0.0f && val <= 0.3f) g_fov_boost.store(val);
    }
    g_vig_on.store(GetPrivateProfileIntW(L"Settings", L"VignetteEnabled", 1, g_ini_path) != 0);
    g_sl_on.store(GetPrivateProfileIntW(L"Settings", L"SpeedLinesEnabled", 1, g_ini_path) != 0);
}

static void save_config() {
    if (g_ini_path[0] == L'\0') return;

    wchar_t buf[64];
    for (const auto& b : k_buttons)
        if (b.mask == g_nitro_button.load()) WritePrivateProfileStringW(L"Settings", L"NitroButton", b.name, g_ini_path);
    WritePrivateProfileStringW(L"Settings", L"NitroEnabled", g_enabled.load() ? L"1" : L"0", g_ini_path);
    WritePrivateProfileStringW(L"Settings", L"DebugOverlay", g_debug.load() ? L"1" : L"0", g_ini_path);
    WritePrivateProfileStringW(L"Settings", L"BackfireOnAllCars", g_backfire_all_cars.load() ? L"1" : L"0", g_ini_path);
    WritePrivateProfileStringW(L"Settings", L"RefillOnRaceStart", g_refill_on_start.load() ? L"1" : L"0", g_ini_path);
    WritePrivateProfileStringW(L"Settings", L"RefundOnRewind", g_refund_on_rewind.load() ? L"1" : L"0", g_ini_path);
    WritePrivateProfileStringW(L"Settings", L"Rumble", g_rumble.load() ? L"1" : L"0", g_ini_path);
    WritePrivateProfileStringW(L"Settings", L"HudTheme", g_hud_theme.load() == 0 ? L"Original" : L"FF", g_ini_path);
    WritePrivateProfileStringW(L"Settings", L"HudHide", g_hud_hide.load() ? L"1" : L"0", g_ini_path);
    swprintf_s(buf, 64, L"%.1f", g_duration.load());
    WritePrivateProfileStringW(L"Settings", L"Duration", buf, g_ini_path);
    swprintf_s(buf, 64, L"%.2f", g_buildup_time.load());
    WritePrivateProfileStringW(L"Settings", L"BuildupTime", buf, g_ini_path);
    swprintf_s(buf, 64, L"%.2f", g_recharge_rate.load());
    WritePrivateProfileStringW(L"Settings", L"RechargeRate", buf, g_ini_path);
    swprintf_s(buf, 64, L"%.2f", g_sound_volume.load());
    WritePrivateProfileStringW(L"Settings", L"SoundVolume", buf, g_ini_path);
    swprintf_s(buf, 64, L"%.2f", g_camera_shake.load());
    WritePrivateProfileStringW(L"Settings", L"CameraShake", buf, g_ini_path);
    swprintf_s(buf, 64, L"%.2f", g_fov_boost.load());
    WritePrivateProfileStringW(L"Settings", L"FovBoost", buf, g_ini_path);
    WritePrivateProfileStringW(L"Settings", L"VignetteEnabled", g_vig_on.load() ? L"1" : L"0", g_ini_path);
    WritePrivateProfileStringW(L"Settings", L"SpeedLinesEnabled", g_sl_on.load() ? L"1" : L"0", g_ini_path);
}

// HUD themes. The HUD files live in this DLL (src\hud.rc) and are written to mediaoverride through the platform.
// The speedometer XAML and the NOS outlines are needed by both themes. The gears and the ABS/TCR/STM "on" icons
// are Fast & Furious art, so with the Original theme they aren't replaced and the game reads its own.
struct HudFile {
    const char* resource;    // name in hud.rc
    const char* media_path;  // relative to media
    bool ff_only;
};
#define HUD_GEAR(n) {"HUD_Dial_Digital_Gear_" #n, "ui/textures/Anthem/HUD/Speedometer/HUD_Dial_Digital_Gear_" #n ".swatchbin", true}
#define HUD_ICON(kind, what) {"HUD_Dial_Assist_" kind "_" what "_On", "ui/textures/data_bound/Anthem/HUD/HUD_Dial_Assist_" kind "_" what "_On.swatchbin", true}
#define HUD_OUTLINE(name) {"HUD_Nos_Outline_" name "_Bar", "ui/textures/data_bound/SpireHUD/HUD_Nos_Outline_" name "_Bar.swatchbin", false}
#ifdef NITRO_FH3_ASSET_OUTLINE   // build B: the NOS outline is the FH2 picture (swatchbin) instead of a vector line
static const HudFile k_hud_xaml = {"HUDSpeedometerControlAssets", "UI/Controls/HUDSpeedometerControl.xaml", false};
#else
static const HudFile k_hud_xaml = {"HUDSpeedometerControl", "UI/Controls/HUDSpeedometerControl.xaml", false};
#endif
static const HudFile k_hud_files[] = {
    HUD_OUTLINE("Digital"), HUD_OUTLINE("Radial"),
    HUD_GEAR(1), HUD_GEAR(2), HUD_GEAR(3), HUD_GEAR(4), HUD_GEAR(5), HUD_GEAR(6), HUD_GEAR(7), HUD_GEAR(8),
    HUD_ICON("Analogue", "ABS"), HUD_ICON("Analogue", "TCR"), HUD_ICON("Analogue", "STM"),
    HUD_ICON("Digital", "ABS"),  HUD_ICON("Digital", "TCR"),  HUD_ICON("Digital", "STM"),
};

// A file embedded in this DLL (src\hud.rc). The pointer stays valid for as long as the DLL is loaded.
#include "hud_data.h"   // the HUD, flame and sound files (generated from hud.rc by scripts/gen_hud_data.py)

static const void* embedded_file(const char* name, DWORD* size) {
    for (const auto& f : k_embedded_files) {
        if (strcmp(f.name, name) == 0) {
            if (size) *size = static_cast<DWORD>(f.size);
            return f.data;
        }
    }
    return nullptr;
}

static bool load_hud_resource(const char* name, std::string& out) {
    DWORD size = 0;
    const void* data = embedded_file(name, &size);
    if (!data) return false;
    out.assign(static_cast<const char*>(data), size);
    return true;
}

static void replace_all(std::string& s, const char* from, const char* to) {
    const size_t n = strlen(from);
    for (size_t pos = 0; (pos = s.find(from, pos)) != std::string::npos; pos += strlen(to)) s.replace(pos, n, to);
}

static void apply_hud_theme() {
    // FH3: the HUD files are FH2's (FH3's speedometer XAML differs); not handed to the game until they're ported.
    if (!g_api || !g_mod || g_api->api_version < 2) return;
    const bool ff = g_hud_theme.load() != 0;

    std::string xaml;
    if (!load_hud_resource(k_hud_xaml.resource, xaml)) {
        g_api->log(g_mod, "HUD: the speedometer XAML is missing from the DLL.");
        return;
    }
    if (!g_hud_hide.load())
        replace_all(xaml, " Opacity=\"{Binding RPMGaugeNumbers[20].YPos}\"", "");   // the kit-dependent frame opacity is off
    if (!ff) {
        // The XAML is written with the Fast & Furious cyan; Original turns the NOS bar yellow.
        replace_all(xaml, "#FF2DB2FF", "#FFF4F444");   // fill: the yellow of the stock gear box
        replace_all(xaml, "#2AEAFAFF", "#2AFFFFB0");   // tip glow
        replace_all(xaml, "#22EAF8FF", "#22FFFFB0");   // tip glow, radial
    }
    if (!g_api->set_file(g_mod, k_hud_xaml.media_path, xaml.data(), xaml.size()))
        g_api->log(g_mod, "HUD: couldn't replace the speedometer XAML (see platform.log).");

    if (NITRO_FH3) {   // FH3: the speedometer XAML (the NOS gauge) and, in build B, the FH2 outline picture
#ifdef NITRO_FH3_ASSET_OUTLINE
        std::string png;
        const char* path = "UI/textures/data_bound/SpireHUD/HUD_Nos_Outline_Radial_Bar.swatchbin";
        if (load_hud_resource("HUD_Nos_Outline_Radial_Bar", png) && g_api->set_file(g_mod, path, png.data(), png.size()))
            g_api->log(g_mod, "HUD: the FH2 NOS outline picture is handed to the game (build B).");
        else
            g_api->log(g_mod, "HUD: couldn't hand over the FH2 NOS outline picture (see platform.log).");
#endif
        g_api->log(g_mod, "HUD: the NOS gauge XAML is in place (applies when the HUD is next loaded).");
        return;
    }
    for (const auto& f : k_hud_files) {
        if (f.ff_only && !ff) {
            g_api->clear_file(g_mod, f.media_path);
            continue;
        }
        std::string data;
        if (!load_hud_resource(f.resource, data) || !g_api->set_file(g_mod, f.media_path, data.data(), data.size())) {
            char msg[256];
            snprintf(msg, sizeof(msg), "HUD: couldn't replace %s (see platform.log).", f.media_path);
            g_api->log(g_mod, msg);
        }
    }
    g_api->log(g_mod, ff ? "HUD theme: Fast & Furious" : "HUD theme: Original (yellow NOS bar)");
}

// Older versions had players copy these files into mediaoverride by hand. The platform doesn't know about them, so
// it never removes them: stale cyan gears would show with the Original theme, and the old Spire flame shaders would
// stay. A file is deleted only when it is byte-for-byte one we know (or our own embedded copy), so nothing a player
// or another mod changed is touched.
static uint64_t fnv1a(const void* data, size_t size) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < size; ++i) h = (h ^ static_cast<const unsigned char*>(data)[i]) * 0x100000001b3ULL;
    return h;
}

static bool read_whole_file(const std::wstring& path, std::string& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart < (64 << 20);
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        ok = ReadFile(f, out.data(), static_cast<DWORD>(out.size()), &got, nullptr) && got == out.size();
    }
    CloseHandle(f);
    return ok;
}

static std::wstring override_path(const wchar_t* relative) {
    return std::wstring(g_api->game_dir) + L"\\mediaoverride\\" + relative;
}

// Delete `path` if its content is exactly `size` bytes with hash `hash`; then remove its folders if they're empty.
static bool delete_if_known(const std::wstring& path, uint64_t size, uint64_t hash) {
    std::string have;
    if (!read_whole_file(path, have) || have.size() != size || fnv1a(have.data(), have.size()) != hash) return false;
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (!DeleteFileW(path.c_str())) return false;
    const std::wstring root = std::wstring(g_api->game_dir) + L"\\mediaoverride";
    for (std::wstring dir = path.substr(0, path.find_last_of(L'\\'));
         dir.size() > root.size() && RemoveDirectoryW(dir.c_str()); dir = dir.substr(0, dir.find_last_of(L'\\'))) {
    }
    return true;
}

static void remove_legacy_files() {
    if (!g_api || !g_mod || !g_api->game_dir || !g_api->game_dir[0]) return;
    int removed = 0;
    for (const auto& f : k_legacy_files)
        if (delete_if_known(override_path(f.path), f.size, f.hash)) ++removed;
    // The Fast & Furious gears and icons, left behind by hand-copying or by an older version, when the theme is Original.
    if (g_hud_theme.load() == 0) {
        for (const auto& f : k_hud_files) {
            if (!f.ff_only) continue;
            std::string wanted;
            std::wstring rel(f.media_path, f.media_path + strlen(f.media_path));
            for (auto& c : rel) if (c == L'/') c = L'\\';
            if (load_hud_resource(f.resource, wanted) &&
                delete_if_known(override_path(rel.c_str()), wanted.size(), fnv1a(wanted.data(), wanted.size())))
                ++removed;
        }
    }
    if (removed) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Removed %d old HUD/flame file(s) from mediaoverride (now built into the mod).", removed);
        g_api->log(g_mod, msg);
    }
}

// The exhaust flame: the game's own Backfire effect with more and longer-lived particles, drawn with its stock
// blue backfire material. Both themes use it.
static void apply_backfire_effect() {
    if (!g_api || !g_mod || g_api->api_version < 2) return;
    std::string xml;
    if (!load_hud_resource("Backfire", xml) || !g_api->set_file(g_mod, "Particles/Systems/Backfire.xml", xml.data(), xml.size()))
        g_api->log(g_mod, "Couldn't replace the backfire effect (see platform.log); the exhaust flames will look stock.");
}

// Physics Hook: CCarDynamics::UpdateDynamics (RVA 0x12C1EA0 in Anthem)
// Main per-tick vehicle simulation loop for all vehicles in the game
typedef bool (*CCarDynamics_UpdateDynamicsFn)(void* this_ptr, void* rdx, void* r8, uint32_t r9, float dt);
static CCarDynamics_UpdateDynamicsFn g_orig_cardynamics = nullptr;

static std::atomic<uint64_t> g_physics_frames{0};
static std::atomic<DWORD> g_phys_tid{0};   // physics hook thread (debug overlay)
static std::atomic<DWORD> g_frame_tid{0};  // game logic frame-step thread (debug overlay)
static std::atomic<uint64_t> g_boosted_frames{0};
static std::atomic<uint64_t> g_last_physics_tick{0};
static void* g_player_cardynamics = nullptr;
static std::atomic<uint64_t> g_last_speedo_tick{0}; // HUD speedometer only ticks while driving with the HUD up

// Telling the player's car from AI racers and traffic. Every simulated car has [+0x178] == 1, so that
// flag can't. The world manager (CWorldCollObjMan::Update, RVA 0x384C00, first argument) holds the
// player car's position at +0x4F0. We learn which offset inside CCarDynamics holds a world position by
// finding the one that keeps landing on it, then treat the nearest car to it as the player's.
static float g_player_pos[3] = {0.0f, 0.0f, 0.0f};
static std::atomic<uint64_t> g_player_pos_tick{0};

typedef void (*MgrUpdateFn)(uint8_t* mgr, float a, float b);
static MgrUpdateFn g_orig_mgr_update = nullptr;

static void drive_backfire_from_game_thread();   // defined with the other backfire code below
static void fh3_drive_flames();                  // FH3 flames, defined with the backfire code
extern thread_local void* t_fh3_car;

static void detour_mgr_update(uint8_t* mgr, float a, float b) {
    if (g_orig_mgr_update) {
        g_orig_mgr_update(mgr, a, b);
    }
    if (!mgr) return;

    __try {
        const float* p = (const float*)(mgr + kFH3MgrPlayerPos);
        if (std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2])) {
            g_player_pos[0] = p[0];
            g_player_pos[1] = p[1];
            g_player_pos[2] = p[2];
            g_player_pos_tick.store(GetTickCount64());
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    fh3_drive_flames();
}

static const int kScanBytes = 0x400;
static const float kCalibRadius2 = 8.0f * 8.0f;   // player position can lag the car by a frame
static const float kPlayerRadius2 = 25.0f * 25.0f;
static const float kFlameRadius2 = 6.0f * 6.0f;     // presentation car "is the player" radius when finding it
static const float kStickyRadius2 = 40.0f * 40.0f;  // ...and when it is already known: at 250+ km/h the two positions lag apart
static std::atomic<int> g_pos_offset{kFH3CarPos};          // offset of the world position inside CCarDynamics
static std::atomic<int> g_calib_report{0};         // 0 none, 1 learned, 2 failed (logged by the worker)
static std::atomic<int> g_calib_best_hits{0};

struct SeenCar {
    void* ptr;
    uint64_t tick;
};
static SeenCar g_seen[32];

static void note_car(void* car, uint64_t now) {
    SeenCar* slot = &g_seen[0];
    for (auto& s : g_seen) {
        if (s.ptr == car) { slot = &s; break; }
        if (s.tick < slot->tick) slot = &s;
    }
    slot->ptr = car;
    slot->tick = now;
}

static bool car_dist2(const void* car, int off, float* out) {
    const float* v = (const float*)((uintptr_t)car + off);
    float dx = v[0] - g_player_pos[0], dy = v[1] - g_player_pos[1], dz = v[2] - g_player_pos[2];
    float d2 = dx * dx + dy * dy + dz * dz;
    if (!std::isfinite(d2)) return false;
    *out = d2;
    return true;
}

// Only runs on the physics thread: finds the offset whose float triple keeps matching the player position.
static void calibrate_position_offset(void* car, uint64_t now) {
    static uint32_t hits[kScanBytes / 4];
    static int samples = 0;
    if (now - g_player_pos_tick.load() > 200) return;

    for (int i = 0; i < kScanBytes / 4 - 2; ++i) {
        float d2;
        if (car_dist2(car, i * 4, &d2) && d2 < kCalibRadius2) ++hits[i];
    }
    if (++samples < 4000) return;

    int best = 0;
    for (int i = 1; i < kScanBytes / 4 - 2; ++i) {
        if (hits[i] > hits[best]) best = i;
    }
    g_calib_best_hits.store((int)hits[best]);
    if (hits[best] >= 150) {
        g_pos_offset.store(best * 4);
        g_calib_report.store(1);
    } else {
        g_calib_report.store(2);
        samples = 0;
        for (auto& h : hits) h = 0;
    }
}

// True if this car is the one nearest the player position. Until the offset is learned, falls back to
// the old behaviour (any simulated car).
static std::atomic<void*> g_hud_player{nullptr};   // the car interface the HUD reads (rpm, gear): the player's, whatever is near
static std::atomic<bool> g_hud_trusted{false};
static bool is_player_car(void* car, uint64_t now) {
    if (g_hud_trusted.load()) { void* h = g_hud_player.load(); return h && (char*)car + 0x4B0 == (char*)h; }
    int off = g_pos_offset.load();
    if (off < 0) return true;
    if (now - g_player_pos_tick.load() > 200) return false;

    float mine;
    if (!car_dist2(car, off, &mine) || mine > kPlayerRadius2) return false;
    // FH3: the world manager's position is ~15 m from the car (a camera point), so traffic can come nearer to it
    // than the player's car. Once found, the player's car stays the player's while it's within 40 m.
    static void* sticky = nullptr;
    static uint64_t sticky_seen = 0;
    if (sticky && sticky != car && now - sticky_seen < 300) {
        float d;
        if (car_dist2(sticky, off, &d) && d < kStickyRadius2) return false;
    }
    if (car == sticky) { sticky_seen = now; return true; }
    for (const auto& s : g_seen) {
        if (!s.ptr || s.ptr == car || now - s.tick > 150) continue;
        float other;
        if (car_dist2(s.ptr, off, &other) && other < mine) return false;
    }
    sticky = car;
    sticky_seen = now;
    return true;
}

// Keeping the boost on the ground. Per-wheel contact isn't known, so: learn which world axis is "up",
// treat a free-fall signature on it as airborne, and push along the horizontal part of the nose only.
static std::atomic<int> g_up_axis{-1};     // 0..2 once learned
static std::atomic<int> g_up_report{0};    // 1 = just learned (logged by the worker)

// The vertical axis is the one the player's position moves least along, since roads are flatter than
// they are long. Vote once a second while the car is covering ground.
static void learn_up_axis(const void* car, uint64_t now) {
    int off = g_pos_offset.load();
    if (off < 0 || g_up_axis.load() >= 0) return;

    static uint64_t win_start = 0;
    static float lo[3], hi[3];
    static int votes[3] = {0, 0, 0};

    const float* p = (const float*)((uintptr_t)car + off);
    if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) return;

    if (win_start == 0 || now - win_start > 1000) {
        if (win_start != 0) {
            float r[3] = {hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]};
            int flattest = 0;
            for (int i = 1; i < 3; ++i) if (r[i] < r[flattest]) flattest = i;
            float mid = 0.0f; // second largest range = the smaller of the two horizontal axes
            for (int i = 0; i < 3; ++i) {
                if (i == flattest) continue;
                if (mid == 0.0f || r[i] < mid) mid = r[i];
            }
            if (mid > 40.0f && r[flattest] < 0.5f * mid) ++votes[flattest];

            int total = votes[0] + votes[1] + votes[2];
            if (total >= 3) {
                int best = 0;
                for (int i = 1; i < 3; ++i) if (votes[i] > votes[best]) best = i;
                if (votes[best] * 3 >= total * 2) {
                    g_up_axis.store(best);
                    g_up_report.store(1);
                }
            }
        }
        win_start = now;
        for (int i = 0; i < 3; ++i) lo[i] = hi[i] = p[i];
    } else {
        for (int i = 0; i < 3; ++i) {
            if (p[i] < lo[i]) lo[i] = p[i];
            if (p[i] > hi[i]) hi[i] = p[i];
        }
    }
}

// Airborne = vertical acceleration near -g for a couple of ticks in a row. Called every tick for the
// player's car, with the velocity as the physics left it (before our own push, which is horizontal).
static bool update_grounded(void* car, const float* vel, float dt, uint64_t now) {
    int up = g_up_axis.load();
    if (up < 0) return true;

    static void* tracked = nullptr;
    static float prev_vy = 0.0f;
    static uint64_t prev_t = 0;
    static int fall_ticks = 0;
    static int grace = 0;

    float vy = vel[up];
    float dt_use = (dt > 0.001f && dt < 0.1f) ? dt : 0.01667f;
    if (car != tracked || now - prev_t > 100) {
        tracked = car;
        fall_ticks = 0;
        grace = 0;
    } else {
        float ay = (vy - prev_vy) / dt_use;
        fall_ticks = (ay < -6.0f) ? fall_ticks + 1 : 0;
    }
    prev_vy = vy;
    prev_t = now;

    if (fall_ticks >= 2) grace = 6;        // airborne; keep off the throttle a moment after landing too
    else if (grace > 0) --grace;
    return fall_ticks < 2 && grace == 0;
}


// ---- FH3 research: which CCarDynamics virtual runs every physics tick? -------------------------------------
// Every slot of the car dynamics object's two vtables (the game makes TRefCountedObjectThreadSafeWithDeletionInfo<CCarDynamics>: game+0x4682578, 17 slots, and game+0x4682608
// with 474, the second used at this+0x4B0) is pointed at a small stub that counts the call, remembers rcx and jumps
// on to the game's function. The log then lists the busiest slots and how far the car they ran on is from the
// player, which tells the per-tick update apart.
struct ProbeSlot { volatile uint64_t calls; volatile uint64_t last_this; volatile uint64_t player_this; volatile uint64_t player_calls; uint64_t target; };
static ProbeSlot* g_probe = nullptr;
static int g_probe_count = 0;
static const uintptr_t kFH3CarVtables[2] = {0x4682578, 0x4682608};  // TRefCountedObjectThreadSafeWithDeletionInfo<CCarDynamics>, the class the game makes
static const int kFH3CarVtableSlots[2] = {17, 474};
static const int kFH3CarVtableThis[2] = {0, 0x4B0};

static bool install_probe(char* base) {
    g_probe_count = kFH3CarVtableSlots[0] + kFH3CarVtableSlots[1];
    g_probe = (ProbeSlot*)VirtualAlloc(nullptr, sizeof(ProbeSlot) * g_probe_count, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    unsigned char* code = (unsigned char*)VirtualAlloc(nullptr, 48 * g_probe_count, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_probe || !code) return false;
    int k = 0;
    for (int v = 0; v < 2; ++v) {
        uint64_t* vt = (uint64_t*)(base + kFH3CarVtables[v]);
        DWORD old = 0;
        if (!VirtualProtect(vt, 8 * kFH3CarVtableSlots[v], PAGE_READWRITE, &old)) return false;
        for (int i = 0; i < kFH3CarVtableSlots[v]; ++i, ++k) {
            ProbeSlot* ps = &g_probe[k];
            ps->target = vt[i];
            unsigned char* c = code + 48 * k;
            // lock inc qword [calls] ; mov [last_this], rcx ; jmp [target]   (rip-relative, absolute via movabs to be safe)
            int n = 0;
            c[n++] = 0x50;                                                              // push rax
            c[n++] = 0x48; c[n++] = 0xB8; *(uint64_t*)(c + n) = (uint64_t)ps; n += 8;   // mov rax, ps
            c[n++] = 0xF0; c[n++] = 0x48; c[n++] = 0xFF; c[n++] = 0x00;                 // lock inc qword [rax]
            c[n++] = 0x48; c[n++] = 0x89; c[n++] = 0x48; c[n++] = 0x08;                 // mov [rax+8], rcx
            c[n++] = 0x48; c[n++] = 0x3B; c[n++] = 0x48; c[n++] = 0x10;                 // cmp rcx, [rax+0x10]
            c[n++] = 0x75; c[n++] = 0x05;                                               // jne +5
            c[n++] = 0xF0; c[n++] = 0x48; c[n++] = 0xFF; c[n++] = 0x40; c[n++] = 0x18;  // lock inc qword [rax+0x18]
            c[n++] = 0x58;                                                              // pop rax
            c[n++] = 0xFF; c[n++] = 0x25; *(int32_t*)(c + n) = 0; n += 4;               // jmp [rip+0]
            *(uint64_t*)(c + n) = ps->target;                                           // 45 bytes in all
            vt[i] = (uint64_t)c;
        }
        VirtualProtect(vt, 8 * kFH3CarVtableSlots[v], old, &old);
    }
    FlushInstructionCache(GetCurrentProcess(), code, 48 * g_probe_count);
    return true;
}

// __try doesn't catch in this build (mingw), so pointers are checked before they're read.
static bool readable_ptr(const void* p, size_t n) {
    if ((uintptr_t)p < 0x10000) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    return (const char*)p + n <= (const char*)mbi.BaseAddress + mbi.RegionSize;
}

static void probe_report(char* base) {
    // the player's car: the object (seen through any slot) nearest the player position
    static uint64_t player_obj = 0;
    float best = 40.0f;
    uint64_t best_obj = 0;
    for (int k = kFH3CarVtableSlots[0]; k < g_probe_count; ++k) {
        __try {
            const char* obj = (const char*)g_probe[k].last_this - kFH3CarVtableThis[1];
            const float* p = (const float*)(obj + kFH3CarPos);
            if (!g_probe[k].last_this || !readable_ptr(p, 12)) continue;
            float dx = p[0] - g_player_pos[0], dy = p[1] - g_player_pos[1], dz = p[2] - g_player_pos[2];
            float d = sqrtf(dx * dx + dy * dy + dz * dz);
            if (d < best) best = d, best_obj = (uint64_t)obj;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (best_obj && best_obj != player_obj) {
        player_obj = best_obj;
        for (int k = 0; k < g_probe_count; ++k) {
            int v = k < kFH3CarVtableSlots[0] ? 0 : 1;
            g_probe[k].player_this = player_obj + kFH3CarVtableThis[v];
        }
        char b[96];
        snprintf(b, sizeof b, "probe: the player's car object is %p (%.1f m from the player position)", (void*)player_obj, best);
        g_api->log(g_mod, b);
        g_player_cardynamics = (void*)player_obj;
    }
    static uint64_t prev[600];
    struct Row { int k; uint64_t d; };
    Row rows[600];
    int nrows = 0;
    for (int k = 0; k < g_probe_count; ++k) {
        uint64_t c = g_probe[k].player_calls, d = c - prev[k];
        prev[k] = c;
        if (d) rows[nrows++] = {k, d};
    }
    for (int a = 0; a < nrows; ++a)
        for (int b = a + 1; b < nrows; ++b)
            if (rows[b].d > rows[a].d) { Row t = rows[a]; rows[a] = rows[b]; rows[b] = t; }
    char line[1600];
    int n = snprintf(line, sizeof line, "probe (calls on the player's car in 5 s, vtable/slot -> game+rva):");
    for (int a = 0; a < nrows && a < 60 && n < (int)sizeof line - 40; ++a) {
        int k = rows[a].k, v = k < kFH3CarVtableSlots[0] ? 0 : 1, i = v ? k - kFH3CarVtableSlots[0] : k;
        n += snprintf(line + n, sizeof line - n, " %c%d->%llX x%llu", v ? 'S' : 'P', i,
                      (unsigned long long)(g_probe[k].target - (uint64_t)base), (unsigned long long)rows[a].d);
    }
    g_api->log(g_mod, line);
}

// FH3: the physics runs at 360 Hz, and every tick takes a reference to each car's dynamics object and lets it go
// again (AddRef / Release, slots 2 and 3 of the object's second vtable, game+0x4682608, `this` at object+0x4B0;
// found by counting calls on the player's car). Release is the end of the car's tick, so the push goes there,
// before the game's Release runs (it may free the object). The time step is measured, so extra calls don't add up.
static const uintptr_t kFH3CarSecondaryVtable = 0x4682608;
static const int kFH3CarSecondaryThis = 0x4B0;
static const int kFH3ReleaseSlot = 3;
typedef uint64_t (*ReleaseFn)(void* self);
static ReleaseFn g_orig_release = nullptr;
static void nitro_tick(void* this_ptr, float dt);
static std::atomic<bool> g_tipsrc_searched{false};   // FH3 flames: the tip source search has run for this car
static void* g_game_base = nullptr;
static std::atomic<void*> g_fh3_player_deferred{nullptr};
static std::atomic<bool> g_fh3_car_drive_fwd{false};   // the player's Car update drives the flames (see the Car hook)   // FH3 flames (see the backfire code below)
static std::atomic<bool> g_fh3_tips_ok{false};
static std::atomic<void*> g_fh3_pres{nullptr};
static std::atomic<uint32_t> g_fh3_mask_pres{0};
static std::atomic<uint32_t> g_fh3_mask{0};                 // exhaust tips the game flagged for the player's car
static void update_torque(char* car);

static uint64_t detour_release(void* self) {
    char* car = (char*)self - kFH3CarSecondaryThis;
    static LARGE_INTEGER freq{};
    static LARGE_INTEGER last{};
    static void* last_car = nullptr;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    float dt = 1.0f / 360.0f;
    if (car == g_player_cardynamics && car == last_car && last.QuadPart)
        dt = (float)(now.QuadPart - last.QuadPart) / (float)freq.QuadPart;
    if (car == g_player_cardynamics) last = now, last_car = car;
    if (dt > 0.05f) dt = 1.0f / 360.0f;
    nitro_tick(car, dt);
    update_torque(car);
    t_fh3_car = nullptr;
    if (car == g_player_cardynamics) fh3_drive_flames();
    return g_orig_release(self);
}

// ---- FH3: nitro as engine torque ----------------------------------------------------------------------------------
// The engine's torque (game+0x1D27A51, on the car's engine block at object+0x5F8) is the torque curve at the current
// revs, blended with the turbo / supercharger curve, times the throttle, then times two per-car scales:
// object+0x10CC (used all over the drivetrain) and object+0x1198 (only on the engine's output). Nitro multiplies
// object+0x1198 for the player's car while it burns, and puts the game's own value back when it stops. The torque goes through the gearbox and the tyres
// like any other: wheelspin, gears and traction all apply.
static const int kFH3AddRefSlot = 2;
static std::atomic<int> g_torque_field{0x1198};
static void* g_scaled_car = nullptr;
static float g_scaled_base = 1.0f, g_scaled_value = 1.0f;
static std::atomic<float> g_torque_base_seen{0.0f};

// Called at the end of every tick of every car. The scale stays raised for as long as nitro burns (the game reads it
// on its own schedule, not inside this AddRef/Release pair - tested 1 Nov), and goes back when nitro stops or the
// player changes car.
static void update_torque(char* car) {
    if (car != g_player_cardynamics && car != g_scaled_car) return;
    const bool want = car == g_player_cardynamics && g_enabled.load() && g_nitro_active.load() && nu_level() > 0 &&
                      g_current_boost.load() > 1.0f;
    float* f = (float*)(car + g_torque_field.load());
    if (car == g_scaled_car) {
        if (*f != g_scaled_value) g_scaled_base = *f;   // the game wrote its own value meanwhile: that's the base now
        if (!want) {
            *f = g_scaled_base;
            g_scaled_car = nullptr;
            return;
        }
    } else {
        if (!want) {
            g_torque_base_seen.store(*f);
            return;
        }
        if (g_scaled_car) {   // another car was scaled: put it back first
            float* old = (float*)((char*)g_scaled_car + g_torque_field.load());
            if (*old == g_scaled_value) *old = g_scaled_base;
        }
        if (!(*f > 0.05f && *f < 10.0f)) return;
        g_scaled_car = car;
        g_scaled_base = *f;
    }
    g_torque_base_seen.store(g_scaled_base);
    g_scaled_value = g_scaled_base * (1.0f + (g_current_boost.load() - 1.0f) * g_push_scale.load());
    *f = g_scaled_value;
    g_boosted_frames.fetch_add(1, std::memory_order_relaxed);
}

static ReleaseFn g_orig_addref = nullptr;
static uint64_t detour_addref(void* self) {
    t_fh3_car = (char*)self - kFH3CarSecondaryThis;
    return g_orig_addref(self);
}

static bool install_release_hook(char* base) {
    void** slot = (void**)(base + kFH3CarSecondaryVtable) + kFH3ReleaseSlot;
    DWORD old = 0;
    if (!VirtualProtect(slot - 1, 2 * sizeof(void*), PAGE_READWRITE, &old)) return false;   // slots 2 and 3
    g_orig_release = (ReleaseFn)*slot;
    *slot = (void*)&detour_release;
    g_orig_addref = (ReleaseFn)slot[-1];
    slot[-1] = (void*)&detour_addref;

    VirtualProtect(slot - 1, 2 * sizeof(void*), old, &old);
    return true;
}

// FH3: the per-tick update takes the time step as its third argument (xmm2); the others are passed through.
typedef uint64_t (*FH3CarUpdateFn)(void* this_ptr, void* rdx, float dt, void* r9);
static FH3CarUpdateFn g_orig_car_update = nullptr;
// FH3: the same push was about six times as strong as in FH2 (2 s of nitro to 300 mph), so it's scaled down
// ([FH3] PushScale), and the real effect is measured (speed from the car's movement, before and during nitro).
static std::atomic<float> g_engine_accel{0.0f};
static std::atomic<float> g_meas_accel_boost{0.0f}, g_meas_accel_plain{0.0f}, g_meas_speed{0.0f};
static std::atomic<int> g_cars_seen{0};
static std::atomic<float> g_last_dt{0.0f};
static std::atomic<float> g_diag_player_dist{-1.0f};
static std::atomic<float> g_diag_speed_field{-1.0f};

static void nitro_tick(void* this_ptr, float dt) {

    g_physics_frames.fetch_add(1, std::memory_order_relaxed);
    g_phys_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    g_last_dt.store(dt, std::memory_order_relaxed);
    if (!this_ptr) return;

    __try {
        uint64_t now = GetTickCount64();
        g_last_physics_tick.store(now);
        note_car(this_ptr, now);

        bool mine = is_player_car(this_ptr, now);
        if (mine) {
            if (g_player_cardynamics != this_ptr) {
                nu_mark_dirty();
                g_fh3_pres.store(nullptr);
                g_tipsrc_searched.store(false);
                g_fh3_mask_pres.store(0);
                g_fh3_player_deferred.store(nullptr);   // a different car: its presentation and exhaust tips are learned again
                g_fh3_mask.store(0);
                g_fh3_tips_ok.store(false);
            }
            g_player_cardynamics = this_ptr;
            float d2;
            if (car_dist2(this_ptr, kFH3CarPos, &d2)) g_diag_player_dist.store(sqrtf(d2));
            g_diag_speed_field.store(*(const float*)((uintptr_t)this_ptr + kFH3CarSpeed));
        }

        const float* fwd = (const float*)((uintptr_t)this_ptr + kFH3CarFwd);
        float* vel = (float*)((uintptr_t)this_ptr + kFH3CarVel);

        bool grounded = true;
        if (mine) {
            // measured speed (from the position) and its change, averaged separately with and without nitro
            static float last_pos[3];
            static uint64_t last_qpc = 0;
            static float last_speed = -1.0f;
            LARGE_INTEGER q, f;
            QueryPerformanceCounter(&q);
            QueryPerformanceFrequency(&f);
            const float* pos = (const float*)((uintptr_t)this_ptr + kFH3CarPos);
            if (last_qpc) {
                float span = (float)(q.QuadPart - last_qpc) / (float)f.QuadPart;
                if (span >= 0.1f) {
                    float dx = pos[0] - last_pos[0], dy = pos[1] - last_pos[1], dz = pos[2] - last_pos[2];
                    float sp = sqrtf(dx * dx + dy * dy + dz * dz) / span;
                    if (last_speed >= 0.0f && span < 0.5f) {
                        float acc = (sp - last_speed) / span;
                        auto& slot = g_nitro_active.load() ? g_meas_accel_boost : g_meas_accel_plain;
                        slot.store(slot.load() * 0.7f + acc * 0.3f);
                    }
                    g_meas_speed.store(sp);
                    last_speed = sp;
                    last_qpc = q.QuadPart;
                    last_pos[0] = pos[0]; last_pos[1] = pos[1]; last_pos[2] = pos[2];
                }
            } else {
                last_qpc = q.QuadPart;
                last_pos[0] = pos[0]; last_pos[1] = pos[1]; last_pos[2] = pos[2];
            }
            g_player_speed.store(sqrtf(vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2]));
            learn_up_axis(this_ptr, now);
            grounded = update_grounded(this_ptr, vel, dt, now);
        }

        // FH3: nitro as more engine power. The car's own acceleration along its nose (from one tick's velocity
        // change, measured from what the previous tick left, so the nitro's own share isn't counted) is what the
        // engine and drivetrain do right now; nitro adds (boost - 1) times that, so it pushes hard in the low gears,
        // fades as drag eats the power at speed, and does nothing while you're off the throttle, shifting or braking.
        if (mine) {
            static void* tracked = nullptr;
            static float prev_along = 0.0f, accel = 0.0f;
            int up = g_up_axis.load();
            float dir[3] = {fwd[0], fwd[1], fwd[2]};
            if (up >= 0) dir[up] = 0.0f;
            float len = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
            bool level = len > 0.3f;
            if (level) { dir[0] /= len; dir[1] /= len; dir[2] /= len; }
            float along = vel[0] * dir[0] + vel[1] * dir[1] + vel[2] * dir[2];
            float dt_use = (dt > 0.0005f && dt < 0.05f) ? dt : 1.0f / 360.0f;
            if (tracked == this_ptr) {
                float a = (along - prev_along) / dt_use;
                if (a > -40.0f && a < 40.0f) accel += (a - accel) * 0.08f;   // ~30 ms smoothing; crashes ignored
            } else {
                tracked = this_ptr;
                accel = 0.0f;
            }
            g_engine_accel.store(accel);
            const float push = 0.0f;   // FH3: nitro is engine torque now (detour_addref), nothing is pushed
            prev_along = along + push;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return;
}

// Backfire Hook: Call at RVA 0x13B0DE6 in Anthem (CarPresentation11::UpdateBackfire)
typedef void (*BackfirePrepFn)(void* r15);
static BackfirePrepFn g_orig_backfire_prep = nullptr;
static std::atomic<uint32_t> g_bf_driven{0};   // routine calls we made ourselves
static std::atomic<uint32_t> g_bf_total{0};    // every hook call, nitro or not
static std::atomic<uint32_t> g_bf_calls{0};    // hook calls while nitro is on
static std::atomic<uint32_t> g_bf_matched{0};  // ...that were the player's car
static std::atomic<int> g_bf_tips{0};          // real exhaust tips on the player car
static char g_tip_diag[16] = "";               // per-tip status for the debug overlay
static std::atomic<uint32_t> g_real_mask{0};    // tips the game itself flagged in its own backfire events for the player
static std::atomic<uint32_t> g_real_events{0};  // how many such events were seen
static std::atomic<bool> g_in_drive{false};     // true while our own driver is calling the routine
static std::atomic<uintptr_t> g_mask_pres{0};   // the presentation (car) the learned tip mask belongs to

// The game only runs its backfire routine when a cooldown timer on the car's presentation has expired
// (about every 40 s for the player), so a nitro burst would get at most one flame. The player's
// presentation is the one with the player's position stored at +0x50; remember it, and while nitro is
// on keep that timer expired so the game runs the routine (and our flame hook) every frame.
static const uintptr_t kPresPosOffset = 0x50;
static const uintptr_t kTipsOffset = 0x15a0;      // 8 exhaust tip entries of 0x60 bytes in the presentation
static const uintptr_t kBackfireObjOffset = 0x18a0;   // timer object inside the presentation (r15)
static const uintptr_t kBackfireExpiryOffset = 0x18;  // next-fire time inside it
static std::atomic<uintptr_t> g_player_pres{0};
static std::atomic<uint64_t> g_player_pres_tick{0};

static void detour_backfire_prep(void* r15) {
    if (g_orig_backfire_prep) {
        g_orig_backfire_prep(r15);
    }

    g_bf_total.fetch_add(1, std::memory_order_relaxed);
    if (!r15) return;

    // r15 is CarPresentation11 + 0x18a0
    uintptr_t car_pres = (uintptr_t)r15 - kBackfireObjOffset;

    // Is this the player's presentation? It stores the player's position at +0x50.
    bool is_target = false;
    __try {
        int pos_off = g_pos_offset.load();
        uint64_t now = GetTickCount64();
        if (pos_off >= 0 && now - g_player_pos_tick.load() < 200) {
            float d2;
            if (car_dist2((const void*)car_pres, (int)kPresPosOffset, &d2) &&
                (d2 < kFlameRadius2 || (car_pres == g_player_pres.load() && d2 < kStickyRadius2))) {
                is_target = true;
                g_player_pres.store(car_pres);
                g_player_pres_tick.store(now);
            }
        } else if (pos_off < 0) {
            // Player not identified yet: fall back to any simulated car
            void* car_dyn = *(void**)(car_pres + 0x10);
            if (car_dyn && *(const int*)((uintptr_t)car_dyn + 0x178) == 1) is_target = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }

    if (!g_enabled.load() || !g_nitro_active.load()) {
        return;
    }

    g_bf_calls.fetch_add(1, std::memory_order_relaxed);

    __try {
        if (is_target) {
            g_bf_matched.fetch_add(1, std::memory_order_relaxed);
            // When the game fires its own backfire event its message already carries the right tips: leave it alone.
            // When our driver is calling, flag the tips the game has flagged for this car in its own events (the
            // routine then spawns a "BackFireInstance<N>" effect for each flagged tip whose handle slot,
            // car_pres + 0x1498 + 0x10 * N, is empty). 8 is only the size of the tip array: a slot the car doesn't
            // really have would get a flame in mid-air, so there is no fallback. Leave the handles alone: the game
            // keeps the running effect attached to the exhaust through them.
            if (g_in_drive.load()) {
                uintptr_t exhaust_start = car_pres + 0x15a0;
                int total = (int)(((uintptr_t)r15 - exhaust_start) / 0x60);
                if (total > 8) total = 8;

                uint32_t mask = (car_pres == g_mask_pres.load()) ? (g_real_mask.load() & 0xFF) : 0;
                char diag[16];
                int nvalid = 0;
                for (int i = 0; i < total; ++i) {
                    bool on = ((mask >> i) & 1) != 0;
                    *(uint8_t*)(exhaust_start + (uintptr_t)i * 0x60 + 0x40) = on ? 1 : 0;
                    diag[i] = on ? 'M' : '-';
                    if (on) ++nvalid;
                }
                diag[total] = 0;
                memcpy(g_tip_diag, diag, (size_t)total + 1);
                g_bf_tips.store(nvalid, std::memory_order_relaxed);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Safe protection
    }
}

// Entry of the game's backfire routine (RVA 0x13B0D70, vtable slot 76 of CarPresentation11). The game calls it
// when the engine sim sends a backfire message. We note which thread that is, and learn which exhaust tips the
// car really has from the tips the game itself flags for the player.
typedef uint64_t (*BackfireEntryFn)(void* a, void* b, void* c, void* d);
static BackfireEntryFn g_orig_backfire_entry = nullptr;
static std::atomic<DWORD> g_bf_tid{0};   // thread the game runs its backfire routine on

static uint64_t detour_backfire_entry(void* a, void* b, void* c, void* d) {
    g_bf_tid.store(GetCurrentThreadId());

    // A real backfire event for the player's car: the message (b) carries the flag byte (+0x40) of each tip
    // the game wants to fire, and the game only flags tips the car really has. Remember them.
    if (!g_in_drive.load() && a && b) {
        __try {
            uint64_t tnow = GetTickCount64();
            float d2;
            if (g_pos_offset.load() >= 0 && tnow - g_player_pos_tick.load() < 200 &&
                car_dist2(a, (int)kPresPosOffset, &d2) &&
                (d2 < kFlameRadius2 || ((uintptr_t)a == g_player_pres.load() && d2 < kStickyRadius2))) {
                if ((uintptr_t)a != g_mask_pres.load()) {
                    // A different car (or a reloaded one): what was learned no longer applies
                    g_real_mask.store(0);
                    g_real_events.store(0);
                    g_mask_pres.store((uintptr_t)a);
                }
                uint32_t mask = 0;
                for (int i = 0; i < 8; ++i) {
                    if (*((const uint8_t*)b + i * 0x60 + 0x40) != 0) mask |= 1u << i;
                }
                g_real_mask.fetch_or(mask);
                g_real_events.fetch_add(1);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    return g_orig_backfire_entry ? g_orig_backfire_entry(a, b, c, d) : 0;
}

// The game only calls its backfire routine when the engine sim sends a backfire message (rare), so nitro
// would get at most one flame. While nitro is on, call the routine for the player's presentation every few
// frames ourselves, with the presentation's own tips as the message; the flame hook then flags the real tips.
// Runs from the game's per-frame step, and only on the same thread the game calls the routine on.
static void drive_backfire_from_game_thread() {
    if (!g_orig_backfire_entry || !g_enabled.load() || !g_nitro_active.load()) return;

    DWORD tid = GetCurrentThreadId();
    DWORD bf_tid = g_bf_tid.load();

    if (bf_tid == 0 || tid != bf_tid) return;

    // Wait until the game has shown which tips this car really has (its own backfire events carry that).
    // No flames until then rather than flames on slots the car doesn't have.
    uint64_t tnow = GetTickCount64();
    if ((g_real_mask.load() & 0xFF) == 0 || g_mask_pres.load() != g_player_pres.load()) return;

    static std::atomic<uint64_t> last_drive{0};
    if (tnow - last_drive.load() < 40) return;
    last_drive.store(tnow);

    uintptr_t pres = g_player_pres.load();
    uint64_t now = GetTickCount64();
    if (!pres || now - g_player_pres_tick.load() > 120000 || g_pos_offset.load() < 0) return;
    if (now - g_player_pos_tick.load() > 200) return;

    alignas(16) static uint8_t message[0x400];
    __try {
        float d2;
        if (!car_dist2((const void*)pres, (int)kPresPosOffset, &d2) || d2 > kStickyRadius2) return;
        // The routine copies the 8 tip entries (0x60 bytes each) from this array into the presentation's own tips
        // before it spawns the flames, then destroys the array. An empty array would wipe the tips' positions
        // (flames spawn nowhere), so send the tips' current contents. Each entry holds a ref-counted pointer
        // (+0x48 object, +0x50 control block with the use count at +8); the destroy step releases one
        // reference per entry, so take one first, like the game's own copy does.
        memset(message, 0, sizeof(message));
        memcpy(message, (const void*)(pres + kTipsOffset), 8 * 0x60);
        for (int i = 0; i < 8; ++i) {
            uintptr_t block = *(uintptr_t*)(message + i * 0x60 + 0x50);
            if (block) _InterlockedIncrement((volatile long*)(block + 8));
        }
        g_in_drive.store(true);
        g_orig_backfire_entry((void*)pres, message, nullptr, nullptr);
        g_in_drive.store(false);
        g_bf_driven.fetch_add(1, std::memory_order_relaxed);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_in_drive.store(false);
    }
}

// Per-frame step of the game's logic thread (RVA 0xFA78A0; this is the thread the backfire routine runs on).
// Found from the call chain of a real backfire call: thread entry -> update loop -> this -> subsystem updates
// -> backfire message handler. Takes (this, bool); no floating-point arguments.
typedef uint64_t (*GameFrameFn)(void* self, void* flag);
static GameFrameFn g_orig_game_frame = nullptr;

static uint64_t detour_game_frame(void* self, void* flag) {
    uint64_t r = g_orig_game_frame ? g_orig_game_frame(self, flag) : 0;
    g_frame_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    drive_backfire_from_game_thread();
    return r;
}

// ---- FH3: exhaust flames ----------------------------------------------------------------------------------------
// FH3 splits the car's presentation in two. The simulation side talks to CCarPresentationDeferred, whose slot 93
// (game+0x1DFD9C0) copies the backfire message (8 exhaust tips of 0x50 bytes, a "fire" byte at +0x40 of each) into a
// queued command; on the presentation's own thread that command calls CarPresentation's slot 93 (game+0x1E741B0),
// which copies the tips to presentation+0x1090 and spawns a "BackFireInstance<N>" / "car_backfire" effect for every
// flagged tip whose handle (presentation+0xF08+0x10*N) is empty. Same routine as FH2's slot 76 (0x13B0D70), with
// smaller, plain (not ref-counted) tip entries.
//
// Nitro posts its own backfire messages through the deferred side, on the thread the game posts them from, so the
// game's queue does the hand-over to the presentation thread like it does for its own backfires. Only tips the game
// itself has flagged for this car are fired ([FH3] FlameTips can force a mask while testing).
static const uintptr_t kFH3BackfirePost = 0x1DFD9C0;
static const uintptr_t kFH3BackfireRoutine = 0x1E741B0;
static const int kFH3TipSize = 0x50, kFH3TipCount = 8, kFH3TipFlag = 0x40, kFH3TipBytes = 0x280;
typedef uint64_t (*FH3BfPostFn)(void* deferred, const void* tips);
typedef uint64_t (*FH3BfRoutineFn)(void* pres, const void* tips, void* r8, void* r9);
static FH3BfPostFn g_orig_fh3_post = nullptr;
static FH3BfRoutineFn g_orig_fh3_routine = nullptr;
static std::atomic<DWORD> g_fh3_post_tid{0}, g_fh3_routine_tid{0};
static std::atomic<uint32_t> g_fh3_post_calls{0}, g_fh3_post_flagged{0}, g_fh3_post_player{0};
static std::atomic<uint32_t> g_fh3_routine_calls{0}, g_fh3_routine_flagged{0}, g_fh3_driven{0};
alignas(16) static uint8_t g_fh3_player_tips[kFH3TipBytes];
static std::atomic<uint32_t> g_fh3_mask_forced{0};   // [FH3] FlameTips
static std::atomic<int> g_fh3_deferred_off{-1};      // where the car object keeps its deferred presentation
static std::atomic<bool> g_fh3_in_drive{false};
thread_local void* t_fh3_car = nullptr;       // the car whose tick is running on this thread
static std::atomic<int> g_fh3_log_budget{12};
static bool readable_ptr(const void* p, size_t n);

static uint32_t fh3_tip_mask(const uint8_t* tips) {
    uint32_t m = 0;
    for (int i = 0; i < kFH3TipCount; ++i)
        if (tips[i * kFH3TipSize + kFH3TipFlag]) m |= 1u << i;
    return m;
}

static void fh3_log_tips(const char* what, const void* obj, const uint8_t* tips) {
    if (g_fh3_log_budget.fetch_sub(1) <= 0) return;
    char b[1400];
    int n = snprintf(b, sizeof b, "flames: %s %p, thread %lu, car on this thread %p (player %p), tips:", what, obj,
                     (unsigned long)GetCurrentThreadId(), t_fh3_car, g_player_cardynamics);
    for (int i = 0; i < kFH3TipCount && n < (int)sizeof b - 120; ++i) {
        const float* f = (const float*)(tips + i * kFH3TipSize);
        n += snprintf(b + n, sizeof b - n, " [%d]%s %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f", i,
                      tips[i * kFH3TipSize + kFH3TipFlag] ? "*" : "", f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9],
                      f[10], f[11], f[12], f[13], f[14], f[15]);
    }
    g_api->log(g_mod, b);
}

// Is this deferred presentation the player's car's? The car object holds a pointer to it; find where once.
static bool fh3_is_player_deferred(void* deferred) {
    char* car = (char*)g_player_cardynamics;
    if (!car || !deferred) return false;
    if (deferred == g_fh3_player_deferred.load()) return true;
    int off = g_fh3_deferred_off.load();
    if (off >= 0) return readable_ptr(car + off, 8) && *(void**)(car + off) == deferred;
    static void* checked[16];
    static int nchecked = 0;
    for (int i = 0; i < nchecked; ++i)
        if (checked[i] == deferred) return false;
    const int span = 0x4000;
    if (!readable_ptr(car, span)) return false;
    for (int o = 0; o < span; o += 8) {
        uintptr_t v = *(uintptr_t*)(car + o);
        if (v == (uintptr_t)deferred || v == (uintptr_t)deferred - 0x10) {
            g_fh3_deferred_off.store(o);
            char b[128];
            snprintf(b, sizeof b, "flames: the player's car keeps its presentation at +0x%X (%p)", o, deferred);
            g_api->log(g_mod, b);
            return true;
        }
    }
    checked[nchecked++ % 16] = deferred;
    if (nchecked > 16) nchecked = 16;
    return false;
}


// The backfire is posted on the physics thread, from somewhere inside a car's update, so the car object it belongs to is
// still on the stack further up the call chain. A pointer into the player's CCarDynamics there means this backfire is
// the player's car's. (Only the live frames above this one are read, never stale data below.)
static bool fh3_player_on_stack(void* deferred) {
    char* car = (char*)g_player_cardynamics;
    if (!car) return false;
    NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
    uintptr_t lo = ((uintptr_t)__builtin_frame_address(0) + 7) & ~(uintptr_t)7;
    uintptr_t hi = (uintptr_t)tib->StackBase;
    if (hi > lo + 0x20000) hi = lo + 0x20000;
    static int s_nomatch_logs = 3;
    for (uintptr_t p = lo; p + 8 <= hi; p += 8) {
        uintptr_t v = *(uintptr_t*)p;
        if (v >= (uintptr_t)car && v < (uintptr_t)car + 0x3000) {
            static bool s_logged = false;
            if (!s_logged) {
                char b[200];
                snprintf(b, sizeof b, "flames: the player's car %p is on the stack of the backfire post (%p) at depth +0x%llX, pointer +0x%llX into it",
                         (void*)car, deferred, (unsigned long long)(p - lo), (unsigned long long)(v - (uintptr_t)car));
                g_api->log(g_mod, b);
                s_logged = true;
            }
            return true;
        }
    }
    if (s_nomatch_logs > 0) {
        --s_nomatch_logs;
        // not the player's car (or the car isn't on the stack in this form): list the live objects up the stack that
        // hold a position near the player, for the next round of research
        char b[1200];
        int n = snprintf(b, sizeof b, "flames: no player car on the stack of the post for %p; objects up the stack near the player position:", deferred);
        int found = 0;
        for (uintptr_t p = lo; p + 8 <= hi && found < 8 && n < (int)sizeof b - 80; p += 8) {
            uintptr_t v = *(uintptr_t*)p;
            if (v < 0x10000 || !readable_ptr((void*)v, 0x400)) continue;
            for (int o = 0; o < 0x400 - 12; o += 4) {
                float d2;
                if (car_dist2((void*)v, o, &d2) && d2 < 30.0f * 30.0f && d2 > 0.0001f) {
                    n += snprintf(b + n, sizeof b - n, " [depth +0x%llX obj %p pos at +0x%X %.1f m]", (unsigned long long)(p - lo), (void*)v, o, sqrtf(d2));
                    ++found;
                    break;
                }
            }
        }
        g_api->log(g_mod, b);
    }
    return false;
}


// ---- finding the player's presentation before the first backfire ----------------------------------------------------
// Every message the simulation queues for a car's presentation (108 kinds, the backfire is one) goes through game+0x1A30DA0
// with the outer object (deferred - 0x10) as the first argument. The first such call made from inside the player's
// car update gives the player's deferred presentation without waiting for a backfire. From it the mod looks for the
// CarPresentation (game+0x468E848 / 0x46902E0 vtable) it owns, and reads the exhaust tips the presentation already
// holds (presentation+0x1090, 8 x 0x50 bytes; the message's tip array is copied there by the backfire routine).
static const uintptr_t kFH3PostQueue = 0x1A30DA0;
static const uintptr_t kFH3DeferredVtable = 0x468B058, kFH3DeferredVtable2 = 0x468B9B8;
static std::atomic<uint32_t> g_fh3_queue_stack_hits{0};
static const uintptr_t kFH3PresVtables[2] = {0x468E848, 0x46902E0};
static const int kFH3PresTips = 0x1090;
typedef uint64_t (*FH3QueueFn)(void*, void*, void*, void*, void*, void*, void*, void*);
static FH3QueueFn g_orig_fh3_queue = nullptr;
static std::atomic<uint32_t> g_fh3_queue_calls{0};

static bool fh3_stack_has_player(uintptr_t span) {
    char* car = (char*)g_player_cardynamics;
    if (!car) return false;
    NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
    uintptr_t lo = ((uintptr_t)__builtin_frame_address(0) + 7) & ~(uintptr_t)7;
    uintptr_t hi = (uintptr_t)tib->StackBase;
    if (hi > lo + span) hi = lo + span;
    for (uintptr_t p = lo; p + 8 <= hi; p += 8) {
        uintptr_t v = *(uintptr_t*)p;
        if (v >= (uintptr_t)car && v < (uintptr_t)car + 0x3000) return true;
    }
    return false;
}

// A tip entry is a 4x4 matrix: three unit rows and a position with w = 1 (the game's own tips look like that; the
// bytes of a tip the car doesn't have are whatever was lying around).
static bool fh3_tip_valid(const uint8_t* t) {
    const float* f = (const float*)t;
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(f[i]) || fabsf(f[i]) > 100.0f) return false;
    for (int r = 0; r < 3; ++r) {
        float l = f[r * 4] * f[r * 4] + f[r * 4 + 1] * f[r * 4 + 1] + f[r * 4 + 2] * f[r * 4 + 2];
        if (l < 0.9f || l > 1.1f) return false;
    }
    return fabsf(f[15] - 1.0f) < 0.01f;
}

static uint32_t fh3_pres_mask_of(const uint8_t* tips) {
    uint32_t m = 0;
    for (int i = 0; i < kFH3TipCount; ++i)
        if (fh3_tip_valid(tips + i * kFH3TipSize)) m |= 1u << i;
    return m;
}

static uint32_t fh3_pres_mask(const uint8_t* pres) {
    uint32_t m = 0;
    for (int i = 0; i < kFH3TipCount; ++i)
        if (fh3_tip_valid(pres + kFH3PresTips + i * kFH3TipSize)) m |= 1u << i;
    return m;
}

static void fh3_find_pres(void* deferred) {
    char b[1500];
    const uintptr_t base = (uintptr_t)g_game_base;
    char* outer = (char*)deferred - 0x10;
    int n = snprintf(b, sizeof b, "flames: the player's presentation %p found before any backfire; looking for its CarPresentation:", deferred);
    for (int o = 0; o < 0x800; o += 8) {
        if (!readable_ptr(outer + o, 8)) break;
        uintptr_t v = *(uintptr_t*)(outer + o);
        if (v < 0x10000 || !readable_ptr((void*)v, 16)) continue;
        uintptr_t vt = *(uintptr_t*)v;
        if (vt == base + kFH3PresVtables[0] || vt == base + kFH3PresVtables[1]) {
            g_fh3_pres.store((void*)v);
            n += snprintf(b + n, sizeof b - n, " FOUND %p at +0x%X (vtable game+0x%llX)", (void*)v, o, (unsigned long long)(vt - base));
            break;
        }
        if (n < (int)sizeof b - 90 && vt > base && vt < base + 0x5000000)
            n += snprintf(b + n, sizeof b - n, " [+0x%X %p vt game+0x%llX]", o, (void*)v, (unsigned long long)(vt - base));
    }
    g_api->log(g_mod, b);
}


// ---- the presentation -> car link ----------------------------------------------------------------------------------
// The CarPresentation doesn't point at the car (checked on 1 Nov: no pointer to CCarDynamics or its +0x4B0 interface,
// directly or one pointer further). It does keep where the car is drawn: the offset of that position is learned once
// from a presentation known to be the player's (the first car while standing still, or the game's own backfire), and
// after that the player's presentation is the one drawn where the player's car is - after a car swap too.
static std::atomic<int> g_fh3_link_off{-1};    // offset of the drawn position (3 floats) in the CarPresentation
static std::atomic<int> g_fh3_link_off2{-1};   // offset behind the pointer at link_off (-1: the floats are in the presentation)

static bool fh3_player_pos(float out[3]) {
    const char* car = (const char*)g_player_cardynamics;
    if (!car || !readable_ptr(car + kFH3CarPos, 12)) return false;
    memcpy(out, car + kFH3CarPos, 12);
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

static bool fh3_pres_pos(const char* pres, int off, int off2, float out[3]) {
    if (off < 0 || !pres) return false;
    const char* p = pres + off;
    if (off2 >= 0) {
        if (!readable_ptr(p, 8)) return false;
        uintptr_t v = *(const uintptr_t*)p;
        if (v < 0x10000 || v > 0x7FFFFFFF0000ull) return false;
        p = (const char*)v + off2;
    }
    if (!readable_ptr(p, 12)) return false;
    memcpy(out, p, 12);
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

static float fh3_dist2(const float a[3], const float b[3]) {
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}

// Is this CarPresentation drawn where the player's car is (within 3 m: the drawing can be a frame behind)?
static bool fh3_link_matches(const char* pres, const char* /*car*/) {
    float pp[3], cp[3];
    if (!fh3_player_pos(pp) || !fh3_pres_pos(pres, g_fh3_link_off.load(), g_fh3_link_off2.load(), cp)) return false;
    return fh3_dist2(pp, cp) < 9.0f;
}

static void fh3_learn_link(const char* pres, const char* car, const char* why) {
    static int logs = 0;
    if (!pres || !car) return;
    float pp[3];
    if (!fh3_player_pos(pp)) return;
    if (g_fh3_link_off.load() >= 0) {   // known: check it, learn it again from this presentation if it doesn't hold
        bool ok = fh3_link_matches(pres, car);
        if (logs < 12) {
            ++logs;
            char b[200];
            snprintf(b, sizeof b, "link (%s): presentation %p at the player's car: %s", why, pres, ok ? "confirmed" : "DOES NOT MATCH, learning again");
            g_api->log(g_mod, b);
        }
        if (ok) return;
        g_fh3_link_off.store(-1);
    }
    int span = readable_ptr(pres, 0x2000) ? 0x2000 : readable_ptr(pres, 0x1000) ? 0x1000 : readable_ptr(pres, 0x800) ? 0x800 : 0;
    char b[1200];
    int n = snprintf(b, sizeof b, "link (%s): presentation %p, car at %.2f,%.2f,%.2f:", why, pres, pp[0], pp[1], pp[2]);
    int best = -1, best2 = -1, hits = 0;
    float bestd = 4.0f;   // 2 m
    for (int o = 0; o + 12 <= span; o += 4) {
        float cp[3];
        memcpy(cp, pres + o, 12);
        if (!std::isfinite(cp[0]) || !std::isfinite(cp[1]) || !std::isfinite(cp[2])) continue;
        float d = fh3_dist2(pp, cp);
        if (d < 4.0f) {
            ++hits;
            if (n < (int)sizeof b - 40) n += snprintf(b + n, sizeof b - n, " [+0x%X %.2fm]", o, sqrtf(d));
            if (d < bestd) { bestd = d; best = o; best2 = -1; }
        }
    }
    if (best < 0) {   // one pointer further
        for (int o = 0; o < (span < 0x800 ? span : 0x800); o += 8) {
            uintptr_t v = *(const uintptr_t*)(pres + o);
            if (v < 0x10000 || v > 0x7FFFFFFF0000ull || (v & 7) || !readable_ptr((void*)v, 0x400)) continue;
            for (int o2 = 0; o2 + 12 <= 0x400; o2 += 4) {
                float cp[3];
                memcpy(cp, (const char*)v + o2, 12);
                if (!std::isfinite(cp[0]) || !std::isfinite(cp[1]) || !std::isfinite(cp[2])) continue;
                float d = fh3_dist2(pp, cp);
                if (d < bestd) {
                    bestd = d; best = o; best2 = o2;
                    if (n < (int)sizeof b - 50) n += snprintf(b + n, sizeof b - n, " [+0x%X -> +0x%X %.2fm]", o, o2, sqrtf(d));
                }
            }
        }
    }
    if (best >= 0) {
        g_fh3_link_off2.store(best2);
        g_fh3_link_off.store(best);
        n += snprintf(b + n, sizeof b - n, " -> using +0x%X%s (%.2f m)", best, best2 >= 0 ? " and one pointer further" : "", sqrtf(bestd));
    } else {
        n += snprintf(b + n, sizeof b - n, " -> no link");
    }
    g_api->log(g_mod, b);
}

// Forgets what was learned about the player's presentation (another car, or the presentation was freed).
static void fh3_forget_player_pres() {
    g_fh3_pres.store(nullptr);
    g_tipsrc_searched.store(false);
    g_fh3_mask_pres.store(0);
    g_fh3_player_deferred.store(nullptr);
    g_fh3_mask.store(0);
    g_fh3_tips_ok.store(false);
}

static uint64_t detour_fh3_queue(void* a, void* b, void* c, void* d, void* e, void* f, void* g, void* h) {
    g_fh3_queue_calls.fetch_add(1, std::memory_order_relaxed);
    if (g_fh3_car_drive_fwd.load(std::memory_order_relaxed)) return g_orig_fh3_queue(a, b, c, d, e, f, g, h);
    // Only until the player's presentation is known (then nothing here costs anything). The stack of a car update has the
    // player's car on it for several presentations while the car is moving, so the first hit is not trusted: the same
    // presentation has to be the only one hit on 5 samples in a row (one sample per clock tick, ~15 ms).
    if (!g_fh3_player_deferred.load() && a && g_player_cardynamics && g_fh3_link_off.load() >= 0) {
        // the link is known: any message to a presentation whose CarPresentation points at the player's car
        if (readable_ptr((char*)a + 0x10, 0x10)) {
            uintptr_t vt = *(uintptr_t*)((char*)a + 0x10) - (uintptr_t)g_game_base;
            if (vt == kFH3DeferredVtable || vt == kFH3DeferredVtable2) {
                char* pres = *(char**)((char*)a + 0x18);
                if (pres && readable_ptr(pres, 8)) {
                    uintptr_t pvt = *(uintptr_t*)pres - (uintptr_t)g_game_base;
                    if ((pvt == kFH3PresVtables[0] || pvt == kFH3PresVtables[1]) && fh3_link_matches(pres, (char*)g_player_cardynamics)) {
                        void* deferred = (char*)a + 0x10;
                        g_fh3_player_deferred.store(deferred);
                        g_fh3_post_tid.store(GetCurrentThreadId());
                        g_fh3_pres.store(pres);
                        char buf[160];
                        snprintf(buf, sizeof buf, "flames: the player's presentation %p (CarPresentation %p) found through the link", deferred, (void*)pres);
                        g_api->log(g_mod, buf);
                    }
                }
            }
        }
    } else if (!g_fh3_player_deferred.load() && a && g_player_cardynamics) {
        static uint64_t last_try = 0;
        static void* cand = nullptr;
        static int cand_hits = 0;
        uint64_t now = GetTickCount64();
        if (now != last_try && readable_ptr((char*)a + 0x10, 8)) {
            last_try = now;
            // the object's own vtable is either the class's (0x468B058) or the ref-counted wrapper's that the game instantiates (0x468B9B8)
            uintptr_t vt = *(uintptr_t*)((char*)a + 0x10) - (uintptr_t)g_game_base;
            void* deferred = (char*)a + 0x10;
            if ((vt == kFH3DeferredVtable || vt == kFH3DeferredVtable2) && fh3_stack_has_player(0x2000)) {
                g_fh3_queue_stack_hits.fetch_add(1, std::memory_order_relaxed);
                if (deferred == cand) ++cand_hits; else { cand = deferred; cand_hits = 1; }
                if (cand_hits >= 5) {
                    cand_hits = 0;
                    g_fh3_player_deferred.store(deferred);
                    g_fh3_post_tid.store(GetCurrentThreadId());
                    fh3_find_pres(deferred);
                    fh3_learn_link((const char*)g_fh3_pres.load(), (const char*)g_player_cardynamics, "standing car");
                }
            }
        }
    }
    return g_orig_fh3_queue(a, b, c, d, e, f, g, h);
}

// Reads the presentation's tips (research log, a few times) and keeps the mask of the valid ones up to date.
static void fh3_poll_pres() {
    char* pres = (char*)g_fh3_pres.load();
    if (!pres || !readable_ptr(pres + kFH3PresTips, kFH3TipBytes)) return;
    uint32_t m = fh3_pres_mask((const uint8_t*)pres);
    g_fh3_mask_pres.store(m);
    static int logs = 4;
    static uint64_t last = 0;
    uint64_t now = GetTickCount64();
    if (logs > 0 && now - last > 3000) {
        --logs; last = now;
        char b[900];
        int n = snprintf(b, sizeof b, "flames: presentation %p tips (valid mask %02X, real mask %02X):", (void*)pres, m, g_fh3_mask.load());
        for (int i = 0; i < kFH3TipCount; ++i) {
            const float* f = (const float*)(pres + kFH3PresTips + i * kFH3TipSize);
            n += snprintf(b + n, sizeof b - n, " [%d]%s %.2f,%.2f,%.2f pos %.2f,%.2f,%.2f,%.2f", i, (m >> i) & 1 ? "*" : "", f[0], f[1], f[2], f[12], f[13], f[14], f[15]);
        }
        g_api->log(g_mod, b);
    }
}


// Research: who builds the backfire message? Logs the return addresses (game+RVA) on the stack above the post call.
static void fh3_log_callers(const char* what) {
    static int budget = 3;
    if (budget <= 0) return;
    --budget;
    const uintptr_t base = (uintptr_t)g_game_base;
    NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
    uintptr_t lo = ((uintptr_t)__builtin_frame_address(0) + 7) & ~(uintptr_t)7;
    uintptr_t hi = (uintptr_t)tib->StackBase;
    if (hi > lo + 0x1800) hi = lo + 0x1800;
    char b[1500];
    int n = snprintf(b, sizeof b, "flames: %s - call chain (return addresses, game+RVA @stack depth):", what);
    int found = 0;
    for (uintptr_t p = lo; p + 8 <= hi && found < 24 && n < (int)sizeof b - 40; p += 8) {
        uintptr_t v = *(uintptr_t*)p;
        if (v < base + 0x1000 || v >= base + 0x2CF0000) continue;
        const uint8_t* c = (const uint8_t*)v;
        if (!readable_ptr(c - 8, 8)) continue;
        bool call = c[-5] == 0xE8 || (c[-6] == 0xFF && (c[-5] & 0x38) == 0x10) || (c[-7] == 0xFF && (c[-6] & 0x38) == 0x10) ||
                    (c[-2] == 0xFF && (c[-1] & 0x38) == 0x10) || (c[-3] == 0xFF && (c[-2] & 0x38) == 0x10);
        if (!call) continue;
        n += snprintf(b + n, sizeof b - n, " %llX@%llX", (unsigned long long)(v - base), (unsigned long long)(p - lo));
        ++found;
    }
    g_api->log(g_mod, b);
}


// ---- where does the game keep the exhaust tips? -----------------------------------------------------------------------
// The first backfire carries the tips. Looking for those bytes in the player's car object (and in what it points to) shows
// whether the car keeps them somewhere permanent; if so the place is relative to the car object, is saved in the ini
// ([FH3] TipSrcKind/A/B) and read from the next start on, before any backfire.
static std::atomic<int> g_tipsrc_kind{0};        // 0 unknown, 1 car+A, 2 *(car+A)+B
static std::atomic<int> g_tipsrc_a{0}, g_tipsrc_b{0};

static bool fh3_scan_for(const uint8_t* start, size_t len, const uint8_t* msg, int i0, int j, const uint8_t** hit) {
    for (size_t chunk = 0; chunk < len; chunk += 0x1000) {
        size_t n = len - chunk < 0x1000 + 0x300 ? len - chunk : 0x1000 + 0x300;
        if (!readable_ptr(start + chunk, n)) continue;
        for (size_t o = chunk; o < chunk + 0x1000 && o + 0x40 <= len; o += 4) {
            if (start + o + 0x40 + (j > i0 ? (size_t)(j - i0) * kFH3TipSize : 0) > start + chunk + n) break;
            if (memcmp(start + o, msg + i0 * kFH3TipSize, 0x40) != 0) continue;
            if (j > i0 && memcmp(start + o + (size_t)(j - i0) * kFH3TipSize, msg + j * kFH3TipSize, 0x40) != 0) continue;
            *hit = start + o;
            return true;
        }
    }
    return false;
}

static bool fh3_build_locator_tips(uint8_t* out, uint32_t* mask_out);
static void fh3_compare_locator_tips(const uint8_t* msg);
static void fh3_find_tip_source(const uint8_t* msg) {
    char* car = (char*)g_player_cardynamics;
    if (!car || g_tipsrc_searched.exchange(true)) return;
    int i0 = -1, j = -1;
    for (int i = 0; i < kFH3TipCount; ++i)
        if (msg[i * kFH3TipSize + kFH3TipFlag] && fh3_tip_valid(msg + i * kFH3TipSize)) { if (i0 < 0) i0 = i; else if (j < 0) j = i; }
    if (i0 < 0) { g_api->log(g_mod, "tip source: no valid flagged tip in the message"); g_tipsrc_searched.store(false); return; }
    const uint8_t* hit = nullptr;
    char b[300];
    if (fh3_scan_for((const uint8_t*)car, 0x30000, msg, i0, j, &hit)) {
        int a = (int)((hit - (const uint8_t*)car) - (intptr_t)i0 * kFH3TipSize);
        g_tipsrc_kind.store(1); g_tipsrc_a.store(a); g_tipsrc_b.store(0);
        snprintf(b, sizeof b, "tip source: FOUND in the car object, entry 0 at car+0x%X (tip %d matched%s)", a, i0, j > i0 ? ", stride confirmed" : "");
        g_api->log(g_mod, b);
    } else {
        for (int p = 0; p < 0x2000 && !hit; p += 8) {
            if (!readable_ptr(car + p, 8)) continue;
            uintptr_t q = *(uintptr_t*)(car + p);
            if (q < 0x10000 || (q >= (uintptr_t)car && q < (uintptr_t)car + 0x30000)) continue;
            if (fh3_scan_for((const uint8_t*)q, 0x4000, msg, i0, j, &hit)) {
                int bb = (int)((hit - (const uint8_t*)q) - (intptr_t)i0 * kFH3TipSize);
                g_tipsrc_kind.store(2); g_tipsrc_a.store(p); g_tipsrc_b.store(bb);
                snprintf(b, sizeof b, "tip source: FOUND behind the pointer at car+0x%X, entry 0 at +0x%X (tip %d matched%s)", p, bb, i0, j > i0 ? ", stride confirmed" : "");
                g_api->log(g_mod, b);
            }
        }
    }
    if (!hit) {
        g_api->log(g_mod, "tip source: not found in the car object or one pointer level behind it (the tips are built on the fly)");
        fh3_compare_locator_tips(msg);
        return;
    }
    wchar_t w[16];
    swprintf(w, 16, L"%d", g_tipsrc_kind.load()); WritePrivateProfileStringW(L"FH3", L"TipSrcKind", w, g_ini_path);
    swprintf(w, 16, L"%d", g_tipsrc_a.load());    WritePrivateProfileStringW(L"FH3", L"TipSrcA", w, g_ini_path);
    swprintf(w, 16, L"%d", g_tipsrc_b.load());    WritePrivateProfileStringW(L"FH3", L"TipSrcB", w, g_ini_path);
}

// The place of entry 0 of the player's exhaust tips, if the game keeps them in the car (null otherwise).
static const uint8_t* fh3_tip_source_ptr() {
    char* car = (char*)g_player_cardynamics;
    int kind = g_tipsrc_kind.load();
    if (!car || !kind) return nullptr;
    const uint8_t* p = nullptr;
    if (kind == 1) p = (const uint8_t*)car + g_tipsrc_a.load();
    else if (readable_ptr(car + g_tipsrc_a.load(), 8)) {
        uintptr_t q = *(uintptr_t*)(car + g_tipsrc_a.load());
        if (q < 0x10000) return nullptr;
        p = (const uint8_t*)q + g_tipsrc_b.load();
    }
    return p && readable_ptr(p, kFH3TipBytes) ? p : nullptr;
}


// Research: where do the exhaust positions live? Walks the objects reachable from the player's car, presentation and
// deferred presentation (three pointer levels) and logs every place that holds the position of a flagged tip
// (the game builds them from the model's carLocator_Exhaust_001..008 points).
struct FH3Node { uintptr_t p; int parent; int off; };
static void fh3_walk_for_positions(std::vector<uintptr_t> roots, float x, float y, float z, float x2, float y2, float z2) {
    std::vector<FH3Node> nodes;
    std::unordered_set<uintptr_t> seen;
    const char* names[3] = {"car", "pres", "deferred"};
    for (size_t r = 0; r < roots.size(); ++r) {
        if (!roots[r]) continue;
        nodes.push_back({roots[r], -1 - (int)r, 0});
        seen.insert(roots[r]);
    }
    std::vector<int> depth(nodes.size(), 0);
    int hits = 0;
    uintptr_t first_hit_addr = 0;
    auto path = [&](int idx, int hit_off) {
        char b[600]; int n = 0;
        std::vector<int> chain;
        for (int i = idx; i >= 0; i = nodes[i].parent) chain.push_back(i);
        int root = -1 - nodes[chain.back()].parent;
        n += snprintf(b + n, sizeof b - n, "tip position found: %s", names[root < 0 ? 0 : root > 2 ? 2 : root]);
        for (int k = (int)chain.size() - 2; k >= 0; --k) n += snprintf(b + n, sizeof b - n, " -> [+0x%X]", nodes[chain[k]].off);
        snprintf(b + n, sizeof b - n, " : at +0x%X (node %p)", hit_off, (void*)nodes[idx].p);
        g_api->log(g_mod, b);
    };
    for (size_t i = 0; i < nodes.size() && nodes.size() < 40000 && hits < 40; ++i) {
        const uint8_t* base = (const uint8_t*)nodes[i].p;
        size_t len = 0x800;
        if (!readable_ptr(base, len)) { len = 0x200; if (!readable_ptr(base, len)) continue; }
        for (size_t o = 0; o + 12 <= len && hits < 40; o += 4) {
            const float* f = (const float*)(base + o);
            if ((fabsf(f[0] - x) < 1e-4f && fabsf(f[1] - y) < 1e-4f && fabsf(f[2] - z) < 1e-4f) ||
                (fabsf(f[0] - x2) < 1e-4f && fabsf(f[1] - y2) < 1e-4f && fabsf(f[2] - z2) < 1e-4f)) {
                if (!first_hit_addr) first_hit_addr = nodes[i].p + o;
                if (hits < 3) path((int)i, (int)o);
                ++hits;
            }
        }
        if (depth[i] >= 3) continue;
        for (size_t o = 0; o + 8 <= len; o += 8) {
            uintptr_t v = *(const uintptr_t*)(base + o);
            if (v < 0x10000 || v > 0x7FFFFFFFFFFFull || (v & 7) || seen.count(v) || !readable_ptr((void*)v, 16)) continue;
            seen.insert(v);
            nodes.push_back({v, (int)i, (int)o});
            depth.push_back(depth[i] + 1);
        }
    }
    char b[1200];
    snprintf(b, sizeof b, "tip position search done: %zu objects visited, %d hits", nodes.size(), hits);
    g_api->log(g_mod, b);
    if (first_hit_addr) {
        // the hit is the position (floats 12..14) of a 4x4 matrix: extend over the run of consecutive valid matrices
        uintptr_t m = first_hit_addr - 0x30, lo = m, hi = m;
        while (readable_ptr((void*)(lo - 0x40), 0x40) && fh3_tip_valid((const uint8_t*)(lo - 0x40)) && m - lo < 0x40 * 32) lo -= 0x40;
        while (readable_ptr((void*)(hi + 0x40), 0x40) && fh3_tip_valid((const uint8_t*)(hi + 0x40)) && hi - m < 0x40 * 32) hi += 0x40;
        int n = snprintf(b, sizeof b, "tip matrices: run of %d valid matrices from %p to %p (the hit is number %d):", (int)((hi - lo) / 0x40) + 1, (void*)lo, (void*)hi, (int)((m - lo) / 0x40));
        for (uintptr_t a = lo; a <= hi && n < (int)sizeof b - 80; a += 0x40) {
            const float* f = (const float*)a;
            n += snprintf(b + n, sizeof b - n, " [%d] %.2f,%.2f,%.2f", (int)((a - lo) / 0x40), f[12], f[13], f[14]);
        }
        g_api->log(g_mod, b);
        // which pointer in the graph leads to the run (or just before / inside it)?
        int found = 0;
        for (size_t i = 0; i < nodes.size() && found < 8; ++i) {
            const uint8_t* base = (const uint8_t*)nodes[i].p;
            size_t len = 0x800;
            if (!readable_ptr(base, len)) { len = 0x200; if (!readable_ptr(base, len)) continue; }
            for (size_t o = 0; o + 8 <= len && found < 8; o += 8) {
                uintptr_t v = *(const uintptr_t*)(base + o);
                if (v + 0x400 >= lo && v <= hi + 0x40) {
                    char t[600]; int k = 0;
                    std::vector<int> chain;
                    for (int c = (int)i; c >= 0; c = nodes[c].parent) chain.push_back(c);
                    int root = -1 - nodes[chain.back()].parent;
                    k += snprintf(t + k, sizeof t - k, "pointer to the matrices: %s", names[root < 0 ? 0 : root > 2 ? 2 : root]);
                    for (int q = (int)chain.size() - 2; q >= 0; --q) k += snprintf(t + k, sizeof t - k, " -> [+0x%X]", nodes[chain[q]].off);
                    snprintf(t + k, sizeof t - k, " : at +0x%X holds %p (= run start %+lld bytes)", (int)o, (void*)v, (long long)((intptr_t)v - (intptr_t)lo));
                    g_api->log(g_mod, t);
                    ++found;
                }
            }
        }
        if (!found) g_api->log(g_mod, "pointer to the matrices: none in the walked objects");
    }
}


// Research (small and safe, unlike the graph walk that crashed the game on 1 Nov): the first backfire's tip positions were
// found under presentation+0x7B0 -> [+0x2C8, +0x2E0, +0x2F8, +0x310 ...] (stride 0x18) in 0x1C0-byte blocks. Lists the valid
// 4x4 matrices in the first blocks, and what the table around holds.


// ---- exhaust tips from the model's locators, before any backfire ------------------------------------------------------
// presentation+0x7B0 holds a table (from +0x2B0, 0x18 bytes per entry, the block pointer last) of the car's locators; each
// block (0x1C0 bytes) has the locator's name ("carLocator_Exhaust_NNN") and its 4x4 matrix at +0x70. The game's backfire
// message uses tip N-1 for locator NNN. A car without a locator gets a shared fallback position (five blocks at the same
// place on the Urus), so only locators with a position of their own count. (Layout read from the 1 Nov logs.)
// The locator entries are {control block vtable game+0x464CD60, counts, block pointer}. On the first car of a session
// they form a sorted array at +0x2A0 of the object at presentation+0x7B0; after a car swap the object has other things
// in front (1 Nov logs: hash nodes, the array further on), so the object is searched for entries instead.
static const uintptr_t kFH3LocatorCtrlVtable = 0x464CD60;

struct FH3Locator { const uint8_t* block; const char* name; int off; };

// Lists the named locator blocks found in the object at presentation+0x7B0 (each block once).
static int fh3_find_locators(const char* pres, FH3Locator* out, int max, uintptr_t* M_out) {
    if (!pres || !readable_ptr(pres + 0x7B0, 8)) return 0;
    uintptr_t M = *(const uintptr_t*)(pres + 0x7B0);
    if (M_out) *M_out = M;
    if (M < 0x10000 || M > 0x7FFFFFFF0000ull) return 0;
    int span = 0;
    for (int sz = 0x4000; sz >= 0x400; sz >>= 1) if (readable_ptr((void*)M, sz)) { span = sz; break; }
    if (!span) return 0;
    const uintptr_t vt = (uintptr_t)g_game_base + kFH3LocatorCtrlVtable;
    int n = 0;
    for (int o = 0; o + 0x18 <= span && n < max; o += 8) {
        if (*(const uintptr_t*)(M + o) != vt) continue;
        uintptr_t q = *(const uintptr_t*)(M + o + 0x10);
        if (q < 0x10000 || q > 0x7FFFFFFF0000ull || !readable_ptr((void*)q, 0x1C0)) continue;
        const uint8_t* bl = (const uint8_t*)q;
        bool dup = false;
        for (int i = 0; i < n && !dup; ++i) dup = out[i].block == bl;
        if (dup) continue;
        const char* name = nullptr;
        for (int k = 0; k + 12 <= 0x1C0 && !name; k += 4)
            if (memcmp(bl + k, "carLocator_", 11) == 0) name = (const char*)bl + k;
        for (int k = 0; k + 8 <= 0x1C0 && !name; k += 8) {
            uintptr_t v = *(const uintptr_t*)(bl + k);
            if (v > 0x10000 && v < 0x7FFFFFFF0000ull && readable_ptr((void*)v, 32) && memcmp((void*)v, "carLocator_", 11) == 0)
                name = (const char*)v;
        }
        if (!name) continue;
        out[n++] = {bl, name, o};
    }
    return n;
}

static bool fh3_build_locator_tips(uint8_t* out, uint32_t* mask_out) {
    memset(out, 0, kFH3TipBytes);
    *mask_out = 0;
    FH3Locator loc[96];
    int nl = fh3_find_locators((const char*)g_fh3_pres.load(), loc, 96, nullptr);
    struct E { int num; const uint8_t* mat; };
    E e[32];
    int n = 0;
    for (int i = 0; i < nl && n < 32; ++i) {
        const char* name = loc[i].name;
        if (memcmp(name, "carLocator_Exhaust_", 19) != 0) continue;
        if (name[19] < '0' || name[19] > '9' || name[20] < '0' || name[20] > '9' || name[21] < '0' || name[21] > '9') continue;
        int num = (name[19] - '0') * 100 + (name[20] - '0') * 10 + (name[21] - '0');
        if (num < 1 || num > kFH3TipCount) continue;
        e[n++] = {num, loc[i].block + 0x70};
    }
    for (int i = 0; i < n; ++i) {
        const float* a = (const float*)e[i].mat;
        int same = 0;
        for (int j = 0; j < n; ++j) {
            const float* b2 = (const float*)e[j].mat;
            if (fabsf(a[12] - b2[12]) < 1e-4f && fabsf(a[13] - b2[13]) < 1e-4f && fabsf(a[14] - b2[14]) < 1e-4f) ++same;
        }
        if (same != 1 || !fh3_tip_valid(e[i].mat)) continue;
        int idx = e[i].num - 1;
        memcpy(out + idx * kFH3TipSize, e[i].mat, 0x40);
        out[idx * kFH3TipSize + kFH3TipFlag] = 1;
        *mask_out |= 1u << idx;
    }
    return *mask_out != 0;
}

static void fh3_dump_locators() {
    const char* pres = (const char*)g_fh3_pres.load();
    FH3Locator loc[96];
    uintptr_t M = 0;
    int nl = fh3_find_locators(pres, loc, 96, &M);
    char b[1800];
    int n = snprintf(b, sizeof b, "locators: CarPresentation %p, object %p at +0x7B0, %d named:", pres, (void*)M, nl);
    for (int i = 0; i < nl && n < (int)sizeof b - 90; ++i) {
        const float* f = (const float*)(loc[i].block + 0x70);
        char nm[40] = "";
        for (int k = 0; k < 36 && loc[i].name[k] >= 32 && loc[i].name[k] < 127; ++k) nm[k] = loc[i].name[k];
        const char* shortnm = strncmp(nm, "carLocator_", 11) == 0 ? nm + 11 : nm;
        n += snprintf(b + n, sizeof b - n, " [+0x%X %s %s%.2f,%.2f,%.2f]", loc[i].off, shortnm, fh3_tip_valid(loc[i].block + 0x70) ? "" : "INVALID ", f[12], f[13], f[14]);
    }
    g_api->log(g_mod, b);
}

// At the game's first real backfire: how close is what the locators give to the game's own message? (log, once per car)
static void fh3_compare_locator_tips(const uint8_t* msg) {
    alignas(16) uint8_t mine[kFH3TipBytes];
    uint32_t m = 0;
    bool ok = fh3_build_locator_tips(mine, &m);
    uint32_t real = fh3_tip_mask(msg);
    float worst = 0.0f;
    for (int i = 0; i < kFH3TipCount; ++i) {
        if (!((m & real) >> i & 1)) continue;
        const float* a = (const float*)(msg + i * kFH3TipSize);
        const float* b2 = (const float*)(mine + i * kFH3TipSize);
        for (int c = 0; c < 16; ++c) worst = fmaxf(worst, fabsf(a[c] - b2[c]));
    }
    char b[240];
    snprintf(b, sizeof b, "locator tips: %s, built mask %02X vs the game's %02X, largest difference in the shared tips %.5f -> %s",
             ok ? "built" : "could not build", m, real, worst, (ok && m == real && worst < 1e-3f) ? "MATCH" : "differs");
    g_api->log(g_mod, b);
    static int dumps = 0;
    if (!(ok && m == real) && dumps < 3) { ++dumps; fh3_dump_locators(); }
}

static uint64_t detour_fh3_post(void* deferred, const void* tips) {
    g_fh3_post_calls.fetch_add(1, std::memory_order_relaxed);
    if (!g_fh3_in_drive.load() && tips && readable_ptr(tips, kFH3TipBytes)) {
        const uint8_t* t = (const uint8_t*)tips;
        uint32_t mask = fh3_tip_mask(t);
        if (mask) g_fh3_post_flagged.fetch_add(1, std::memory_order_relaxed);
        bool player = fh3_is_player_deferred(deferred) || (t_fh3_car && t_fh3_car == g_player_cardynamics);
        if (!player && g_player_cardynamics && fh3_player_on_stack(deferred)) player = true;
        if (player) {
            g_fh3_post_player.fetch_add(1, std::memory_order_relaxed);
            g_fh3_post_tid.store(GetCurrentThreadId());
            fh3_log_callers("player's backfire post");
            fh3_find_tip_source(t);
            if (deferred != g_fh3_player_deferred.load()) {
                g_fh3_player_deferred.store(deferred);
                g_fh3_mask.store(0);
                g_fh3_pres.store(nullptr);
                fh3_find_pres(deferred);   // the CarPresentation (and its locators) of the presentation the game really posts to
                fh3_learn_link((const char*)g_fh3_pres.load(), (const char*)g_player_cardynamics, "the game's backfire");
                fh3_log_tips("player's car posts a backfire,", deferred, t);
            } else if (mask & ~g_fh3_mask.load()) {
                fh3_log_tips("player's car backfires on new tips,", deferred, t);
            }
            g_fh3_mask.fetch_or(mask);
            memcpy(g_fh3_player_tips, t, kFH3TipBytes);
            g_fh3_tips_ok.store(true);
        } else if (mask) {
            fh3_log_tips("another car backfires,", deferred, t);
        }
    }
    return g_orig_fh3_post(deferred, tips);
}

static uint64_t detour_fh3_routine(void* pres, const void* tips, void* r8, void* r9) {
    g_fh3_routine_tid.store(GetCurrentThreadId());
    g_fh3_routine_calls.fetch_add(1, std::memory_order_relaxed);
    if (tips && readable_ptr(tips, kFH3TipBytes) && fh3_tip_mask((const uint8_t*)tips))
        g_fh3_routine_flagged.fetch_add(1, std::memory_order_relaxed);
    return g_orig_fh3_routine(pres, tips, r8, r9);
}

// The backfire post reads the arena it allocates the message from out of the presentation (+0x8 of the outer object).
// After a car swap the presentation we remember is freed, and the post faults on it: check it is still the live
// presentation (vtable, arena) before every call of the game's function.
static bool fh3_deferred_alive(void* deferred) {
    char* outer = (char*)deferred - 0x10;
    if (!readable_ptr(outer, 0x20)) return false;
    uintptr_t vt = *(uintptr_t*)deferred - (uintptr_t)g_game_base;
    if (vt != kFH3DeferredVtable && vt != kFH3DeferredVtable2) return false;
    void* arena = *(void**)(outer + 8);
    return (uintptr_t)arena > 0x10000 && (uintptr_t)arena < 0x7FFFFFFF0000ull && readable_ptr(arena, 0x30);
}

// While nitro burns, post a backfire on the player's car every 40 ms (the routine only spawns on tips whose last
// flame has finished, so this keeps them burning). Runs on the thread the game posts its own backfires from.
static void fh3_drive_flames() {
    if (g_fh3_car_drive_fwd.load()) return;   // the Car update drives the flames
    if (!g_orig_fh3_post || !g_enabled.load() || !g_nitro_active.load()) return;
    if (GetCurrentThreadId() != g_fh3_post_tid.load()) return;
    // runs 360 times a second per car: everything below is paced
    static uint64_t last = 0;
    uint64_t now = GetTickCount64();
    if (now - last < 40) return;
    last = now;
    void* deferred = g_fh3_player_deferred.load();
    if (!deferred) return;
    if (!fh3_deferred_alive(deferred)) {             // the car was swapped or removed: its presentation is gone, learn the new one
        fh3_forget_player_pres();
        g_api->log(g_mod, "flames: the player's presentation went away (another car?); it will be found again.");
        return;
    }
    {   // the deferred presentation can stay while the car behind it changes: follow its CarPresentation (+0x18 of the outer object)
        char* p = *(char**)((char*)deferred + 0x8);
        if (p != g_fh3_pres.load() && p && readable_ptr(p, 8)) {
            uintptr_t pvt = *(uintptr_t*)p - (uintptr_t)g_game_base;
            if (pvt == kFH3PresVtables[0] || pvt == kFH3PresVtables[1]) {
                g_fh3_pres.store(p);
                g_fh3_tips_ok.store(false);
                g_fh3_mask.store(0);
            }
        }
    }
    alignas(16) uint8_t msg[kFH3TipBytes];
    uint32_t mask = g_fh3_mask_forced.load();
    if (g_fh3_tips_ok.load()) {                      // the game has backfired on this car: its message is the truth
        memcpy(msg, g_fh3_player_tips, kFH3TipBytes);
        if (!mask) mask = g_fh3_mask.load();
    } else {                                         // not yet: the tips built from the model's exhaust locators (once a second)
        alignas(16) static uint8_t built[kFH3TipBytes];
        static uint32_t built_mask = 0;
        static uint64_t built_tick = 0;
        static void* built_for = nullptr;
        // built once per car; only a failed build is retried (once a second) until the model's locators are there
        if (built_for != g_fh3_pres.load() || (!built_mask && now - built_tick > 1000)) {
            built_for = g_fh3_pres.load();
            built_tick = now;
            bool built_ok = fh3_build_locator_tips(built, &built_mask);
            {
                static void* dumped = nullptr;
                static int ndumps = 0;
                if (built_ok && dumped != built_for && ndumps < 4) {
                    dumped = built_for; ++ndumps;
                    char b[120];
                    snprintf(b, sizeof b, "locator tips: built mask %02X for CarPresentation %p:", built_mask, built_for);
                    g_api->log(g_mod, b);
                    fh3_dump_locators();
                }
            }
            if (!built_ok) {
                built_mask = 0;
                static void* dumped_for = nullptr, *failing_for = nullptr;
                static uint64_t failing_since = 0;
                static int dumps = 0;
                if (failing_for != built_for) { failing_for = built_for; failing_since = now; }
                if (dumped_for != built_for && dumps < 3 && now - failing_since > 3000) { dumped_for = built_for; ++dumps; fh3_dump_locators(); }
            }
        }
        if (!built_mask) return;
        memcpy(msg, built, kFH3TipBytes);
        if (!mask) mask = built_mask;
    }
    if (!(mask & 0xFF)) return;
    for (int i = 0; i < kFH3TipCount; ++i) msg[i * kFH3TipSize + kFH3TipFlag] = (mask >> i) & 1;
    g_fh3_in_drive.store(true);
    g_orig_fh3_post(deferred, msg);
    g_fh3_in_drive.store(false);
    g_fh3_driven.fetch_add(1, std::memory_order_relaxed);
}

// ---- the game's own exhaust tips (class Car) ---------------------------------------------------------------------
// The game builds its backfire message in Car's update (vtable game+0x462D160 slot 19, game+0x18B8D20): eight weak
// pointers to the car's active CarLocators at Car+0x88 (0x10 apart: the object, then its control block with the use
// count at +8), each matrix from the locator's slot 2 (game+0x1B3E3B0, the locator's matrix at +0xF0); then it posts the
// message to the deferred presentation at Car+0x73A0. Car+0x71E8 holds the car dynamics' ref-counted interface
// (CCarDynamics+0x4B0). So the player's Car is the one whose +0x71E8 is the player's dynamics + 0x4B0, and the nitro
// flames are built from the very locators the game uses - the exhausts of the fitted parts, on any car, after any swap.
static const uintptr_t kFH3CarObjUpdate = 0x18B8D20;
static const int kFH3CarExhausts = 0x88, kFH3CarDynIface = 0x71E8, kFH3CarDeferred = 0x73A0;
typedef uint64_t (*FH3CarObjUpdateFn)(void* car, float dt);
static FH3CarObjUpdateFn g_orig_fh3_carobj_update = nullptr;
static std::atomic<void*> g_fh3_player_car{nullptr};
static std::atomic<bool> g_fh3_car_drive{false};     // the player's Car has been seen: it drives the flames
static std::atomic<uint32_t> g_fh3_car_updates{0}, g_fh3_car_posts{0};

static uint32_t fh3_car_exhaust_tips(char* car, uint8_t* msg) {
    memset(msg, 0, kFH3TipBytes);
    uint32_t mask = 0;
    const uintptr_t base = (uintptr_t)g_game_base;
    for (int i = 0; i < kFH3TipCount; ++i) {
        char* obj = *(char**)(car + kFH3CarExhausts + 0x10 * i);
        char* ctrl = *(char**)(car + kFH3CarExhausts + 0x10 * i + 8);
        if (!obj || !ctrl || *(volatile int32_t*)(ctrl + 8) <= 0) continue;
        void** vt = *(void***)obj;
        if ((uintptr_t)vt < base || (uintptr_t)vt > base + 0x6000000) continue;
        typedef const float* (*LocatorMatrixFn)(void*);
        const float* m = ((LocatorMatrixFn)vt[2])(obj);
        if (!m || !readable_ptr(m, 0x40)) continue;
        memcpy(msg + i * kFH3TipSize, m, 0x40);
        msg[i * kFH3TipSize + kFH3TipFlag] = 1;
        mask |= 1u << i;
    }
    return mask;
}

// ---- Nitrous Kit shop, step 1 (research): what SQL the garage runs. sqlite3LockAndPrepare is game+0x440C0:
// (db, zSql, nBytes, prepFlags, pOld, ppStmt, pzTail). Logged only, nothing is changed yet.
typedef int (*FH3PrepareFn)(void*, const char*, int, unsigned, void*, void**, const char**);
static FH3PrepareFn g_orig_prepare = nullptr;
static int g_force_stage = 0;   // [FH3] ForceStage (1-3): every car has that stage, the shop is ignored (0 = normal)
static void fh3_save_last_garage(unsigned g) { wchar_t v[16]; nu_wide_num(v, (long)g); WritePrivateProfileStringW(L"FH3", L"LastGarage", v, g_ini_path); }
// The Nitrous Kit rows for the game database (FH3 layout: the area link is its own table, no AreaId in UpgradeTypes).
static void fh3_kit_sql(std::string& out) {
    char b[700];
    const char* icon = "GAME:\\Media\\UI\\Textures\\Data_Bound\\Upgrade_Parts\\Engine_Power\\engine_power_Icon.swatchbin";
    const char* iconlg = "GAME:\\Media\\UI\\Textures\\Data_Bound\\Upgrade_Parts\\Engine_Power\\engine_power_LG_Icon.swatchbin";
    const char* stock = "GAME:\\Media\\UI\\Textures\\Data_Bound\\Upgrade_Parts\\stock_Icon.swatchbin";
    const char* stocklg = "GAME:\\Media\\UI\\Textures\\Data_Bound\\Upgrade_Parts\\stock_LG_Icon.swatchbin";
    snprintf(b, sizeof b, "INSERT OR REPLACE INTO UpgradeTypes (id, PartEnum, IsException, DisplayOrder, EverymanUpgradeDisplayOrder, Name, Description, IconPath, ImagePath) VALUES (%d, %d, 0, 15, -1, 'Nitrous Kit', 'Nitrous oxide injection. Hold the nitro button while driving; a higher stage pushes harder.', '%s', '%s');\n", NU_TYPE_ID, NU_ENUM, icon, iconlg);
    out += b;
    snprintf(b, sizeof b, "INSERT OR REPLACE INTO UpgradeAreaForUpgradeType (id, UpgradeAreaId, UpgradeTypeId) VALUES (%d, 1, %d);\n", NU_TYPE_ID, NU_TYPE_ID);
    out += b;
    static const char* names[NU_LEVELS] = {"Stock", "Stage 1 Nitrous", "Stage 2 Nitrous", "Stage 3 Nitrous"};
    for (int l = 0; l < NU_LEVELS; ++l) {
        snprintf(b, sizeof b, "INSERT OR REPLACE INTO Upgrades (id, TypeId, IsException, Name, Level, Description, IconPath, ImagePath) VALUES (%d, '%d', 0, '%s', %d, '%s', '%s', '%s');\n",
                 NU_UPGRADE_BASE + l, NU_TYPE_ID, names[l], l, l ? "Nitrous injection." : "No nitrous system fitted.", l ? icon : stock, l ? iconlg : stocklg);
        out += b;
        
    }
}


// Enum 26 is the game's Quad Turbo slot: its level query is per engine (our rows have no engine) and the install
// is written into the whole garage row (QuadTurbo=<id>, AspirationTypeId=4). Both are answered / cleaned here.
static std::atomic<long> g_last_asp{1};
static bool fh3_rewrite_kit(const char* z, size_t len, std::string& out) {
    if (len > 40 && z[0] == 'S' && strncmp(z, "SELECT ManufacturerID FROM List_UpgradeEngineTurboQuad WHERE Id=", 64) == 0) {
        long id = atol(z + 64);
        if (id >= NU_ID_BASE && id < NU_ID_BASE + NU_LEVELS) { out = "SELECT -1 AS ManufacturerID"; return true; }
        return false;
    }
    if (len > 60 && z[0] == 'S' && strncmp(z, "SELECT 1 From Profile0_Career_PurchasedParts WHERE GarageId = ", 62) == 0) {
        const char* p = strstr(z, " AND PartEnum = 26 AND PartId = ");
        if (p) {
            long id = atol(p + 32);
            if (id == NU_ID_BASE) { out = "SELECT 1"; return true; }          // our stock entry is always owned
            if (id > NU_ID_BASE && id < NU_ID_BASE + NU_LEVELS) {            // bought at any manufacturer value
                const char* m = strstr(p, " AND ManufacturerId");
                out.assign(z, m ? (size_t)(m - z) : len);
                return true;
            }
        }
        return false;
    }
    if (len > 40 && z[0] == 'S' && strncmp(z, "SELECT Level FROM List_UpgradeEngineTurboQuad WHERE EngineId=", 61) == 0) {
        const char* p = strstr(z, " AND Id=");
        long id = p ? atol(p + 8) : 0;
        if (id >= NU_ID_BASE && id < NU_ID_BASE + NU_LEVELS) { out = "SELECT " + std::to_string(id - NU_ID_BASE) + " AS Level"; return true; }
        return false;
    }
    if (len > 60 && z[0] == 'U' && strncmp(z, "UPDATE Profile0_Career_Garage SET", 33) == 0) {
        const char* asp = strstr(z, "AspirationTypeId=");
        const char* q = strstr(z, "QuadTurbo=");
        long id = q ? atol(q + 10) : 0;
        const bool ours = id >= NU_ID_BASE && id < NU_ID_BASE + NU_LEVELS;
        if (asp && !ours) { long v = atol(asp + 17); if (v > 0 && (v != 4 || id > 0)) g_last_asp.store(v); }   // 4 (quad) only counts with a real quad turbo id
        if (!ours) return false;
        const char* w = strstr(z, " WHERE Id=");
        long gid = w ? atol(w + 10) : 0;
        if (gid > 0) { nu_set_level(gid, nu_cur_ord, (int)(id - NU_ID_BASE)); if ((unsigned)gid != nu_active_garage && !g_force_stage) { nu_activate((unsigned)gid); fh3_save_last_garage((unsigned)gid); } }
        std::string r(z, (size_t)(q - z));
        r += "QuadTurbo=-1";
        const char* rest = q + 10; while (*rest == '-' || (*rest >= '0' && *rest <= '9')) ++rest;
        if (asp && asp > rest) {
            r.append(rest, (size_t)(asp - rest));
            r += "AspirationTypeId=" + std::to_string(g_last_asp.load());
            const char* r2 = asp + 17; while (*r2 >= '0' && *r2 <= '9') ++r2;
            r += r2;
        } else r += rest;
        out = r;
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 6 && g_api && g_mod) { char b[96]; snprintf(b, sizeof b, "kit: installed stage %ld on garage %ld", id - NU_ID_BASE, gid); g_api->log(g_mod, b); }
        return true;
    }
    return false;
}


// A save that still holds one of our ids in the QuadTurbo slot crashes the game on load (no real part behind it):
// before the first statement that touches the garage table, clean those rows through the game's own sqlite3_exec.
typedef int (*FH3ExecFn)(void*, const char*, void*, void*, char**);
static std::atomic<int> g_repair_tries{0};
static std::atomic<bool> g_repaired{false};
static thread_local bool t_in_repair = false;
static void fh3_repair_save(void* db, const char* z) {
    if (g_repaired.load() || t_in_repair || !z || !g_game_base || !strstr(z, "Profile0_Career_Garage")) return;
    if (g_repair_tries.fetch_add(1) >= 6) return;
    t_in_repair = true;
    FH3ExecFn ex = (FH3ExecFn)((uintptr_t)g_game_base + 0x421D0);
    int r = ex(db, "UPDATE Profile0_Career_Garage SET QuadTurbo=-1, AspirationTypeId=1 WHERE QuadTurbo BETWEEN 900470 AND 900473", nullptr, nullptr, nullptr);
    t_in_repair = false;
    if (g_api && g_mod) { char b[96]; snprintf(b, sizeof b, "kit: save clean-up returned %d", r); g_api->log(g_mod, b); }
    if (r == 0) g_repaired.store(true);
}

static int fh3_prep(void* db, const char* z, int n, unsigned fl, void* old, void** pp, const char** tail) {
    int r = g_orig_prepare(db, z, n, fl, old, pp, tail);
    static std::atomic<int> fails{0};
    if (r != 0 && z && fails.fetch_add(1) < 40 && g_api && g_mod) {
        char b[700]; snprintf(b, sizeof b, "prepare failed (%d): %.500s", r, z);
        for (char* c = b; *c; ++c) if (*c == '\n' || *c == '\r' || *c == '\t') *c = ' ';
        g_api->log(g_mod, b);
    }
    return r;
}
static int detour_prepare(void* db, const char* z, int n, unsigned fl, void* old, void** pp, const char** tail) {
    if (!t_in_repair && !g_repaired.load()) fh3_repair_save(db, z);
    // The garage's part lists / installs go through nu_rewrite (view rows, installed marker, install = level change).
    if (z && readable_ptr(z, 64)) {
        const size_t len = n < 0 ? strlen(z) : (size_t)n;
        if (len > 24 && z[0] == 'U' && strncmp(z, "UPDATE Profile0_Career_Garage SET TopSpeed", 42) == 0) {
            const char* w = strstr(z, " WHERE Id=");   // the driven car's stats are saved: that is its garage id
            long gid = w ? atol(w + 10) : 0;
            if (gid > 0 && (unsigned)gid != nu_active_garage && !g_force_stage) { nu_activate((unsigned)gid); fh3_save_last_garage((unsigned)gid); }
        }
        std::string kitq;
        const char* use = fh3_rewrite_kit(z, len, kitq) ? kitq.c_str() : nu_rewrite(z, len);
        if (use != z) {
            if (z[0] == 'U' || strncmp(z, "SELECT PartTable", 16) == 0) {
                long gid = 0; const char* g = strstr(z, "GarageId = "); if (!g) { g = strstr(z, "WHERE Id = "); if (g) gid = atol(g + 11); } else gid = atol(g + 11);
                if (gid > 0 && (unsigned)gid != nu_active_garage && !g_force_stage) { nu_activate((unsigned)gid); fh3_save_last_garage((unsigned)gid); }
            }
            int r = fh3_prep(db, use, -1, fl, old, pp, tail);
            if (tail) *tail = z + len;
            return r;
        }
    }
    static std::atomic<int> left{900};
    if (z && left.load() > 0 && readable_ptr(z, 16)) {
        const char* keys[] = {"UpgradeParts", "UpgradeTypes", "UpgradeAreas", "PurchasedParts", "Career_Garage SET", "List_UpgradeMotorParts", "NitroSystemII", "PartEnum = 26", "900470", "900471", "900472", "900473"};
        bool hit = false;
        for (const char* k : keys) if (strstr(z, k)) { hit = true; break; }
        if (hit && ((strncmp(z, "SELECT 1 From Profile0_Career_PurchasedParts", 44) == 0 && !strstr(z, "PartEnum = 26")) || strncmp(z, "CREATE INDEX", 12) == 0 || strncmp(z, "CREATE TABLE", 12) == 0 || strstr(z, "WHERE 1=2") ||
                    strstr(z, "filedb.") || strncmp(z, "INSERT INTO [", 13) == 0)) hit = false;   // startup copying, not the garage
        if (hit && left.fetch_sub(1) > 0) {
            char b[900];
            size_t len = n < 0 ? strlen(z) : (size_t)n;
            const bool kitrel = strstr(z, "PartEnum = 26") || strstr(z, "9004") || strstr(z, "PurchasedParts") || strstr(z, "Career_Garage SET");
            if (len > (kitrel ? 3400 : 300)) len = kitrel ? 3400 : 300;
            char* big = (char*)malloc(len + 16);
            if (big) { snprintf(big, len + 16, "sql: %.*s", (int)len, z); for (char* c = big; *c; ++c) if (*c == '\n' || *c == '\r' || *c == '\t') *c = ' '; if (g_api && g_mod) g_api->log(g_mod, big); free(big); }
            (void)b;
            return fh3_prep(db, z, n, fl, old, pp, tail);
            snprintf(b, sizeof b, "sql: %.*s", (int)len, z);
            for (char* c = b; *c; ++c) if (*c == '\n' || *c == '\r' || *c == '\t') *c = ' ';
            if (g_api && g_mod) g_api->log(g_mod, b);
        }
    }
    return fh3_prep(db, z, n, fl, old, pp, tail);
}

typedef uint64_t (*HudCtrlFn)(void*, void*, void*, void*);
static HudCtrlFn g_orig_hud_ctrl = nullptr;
// The HUD speedometer controller (game+0xB26690, rcx = controller): +0x120/+0x128 is a weak pointer to the player's car.
static uint64_t detour_hud_ctrl(void* c, void* a, void* b, void* d) {
    uint64_t r = g_orig_hud_ctrl(c, a, b, d);
    if (c && readable_ptr(c, 0x130)) {
        void* obj = *(void**)((char*)c + 0x120);
        void* cb = *(void**)((char*)c + 0x128);
        g_hud_player.store(cb ? obj : nullptr);
    }
    return r;
}

static uint64_t detour_fh3_carobj_update(void* self, float dt) {
    uint64_t r = g_orig_fh3_carobj_update(self, dt);
    g_fh3_car_updates.fetch_add(1, std::memory_order_relaxed);
    char* car = (char*)self;
    char* dyn = (char*)g_player_cardynamics;
    if (!car) return r;
    {   // who is the player's Car? The HUD knows. Trust it once it has agreed with the proximity rule a hundred times.
        void* iface = *(void**)(car + kFH3CarDynIface);
        void* hud = g_hud_player.load();
        static int agree = 0, differ = 0;
        const bool prox = dyn && iface == dyn + 0x4B0;
        if (!g_hud_trusted.load() && prox && hud) {
            if (iface == hud) { if (++agree >= 100 && differ == 0) { g_hud_trusted.store(true); g_api->log(g_mod, "flames: the HUD's player car matches the Car's interface; using it to tell the player's car from the others."); } }
            else if (++differ <= 3) { char b[160]; snprintf(b, sizeof b, "flames: HUD car %p differs from the Car's interface %p (dynamics %p)", hud, iface, dyn); g_api->log(g_mod, b); }
        }
        if (g_hud_trusted.load()) { if (!hud || iface != hud) return r; dyn = (char*)iface - 0x4B0; }
        else if (!prox) return r;
    }
    void* deferred = *(void**)(car + kFH3CarDeferred);
    if (g_fh3_player_car.exchange(car) != car || g_fh3_player_deferred.load() != deferred) {
        g_fh3_player_deferred.store(deferred);
        g_fh3_post_tid.store(GetCurrentThreadId());
        alignas(16) uint8_t msg[kFH3TipBytes];
        uint32_t m = fh3_car_exhaust_tips(car, msg);
        char b[200];
        snprintf(b, sizeof b, "flames: the player's Car %p (dynamics %p), its presentation %p, exhausts %02X", car, dyn, deferred, m);
        g_api->log(g_mod, b);
    }
    g_fh3_car_drive.store(true);
    g_fh3_car_drive_fwd.store(true);
    if (!g_orig_fh3_post || !deferred || !g_enabled.load() || !g_nitro_active.load()) return r;
    static uint64_t last = 0;   // the update runs at the simulation rate: one message every 40 ms keeps the tips burning
    uint64_t now = GetTickCount64();
    if (now - last < 40) return r;
    last = now;
    alignas(16) uint8_t msg[kFH3TipBytes];
    uint32_t mask = fh3_car_exhaust_tips(car, msg);
    if (!mask) {
        // The Car looks its exhaust locators up lazily (slot 78, game+0x18BBE20, which the game calls right before a backfire
        // message): until the first backfire the eight pointers are empty. Ask it to, at most twice a second.
        static uint64_t last_resolve = 0;
        if (now - last_resolve >= 500 && readable_ptr(*(void**)car, 0x270 + 8)) {
            last_resolve = now;
            void** vt = *(void***)car;
            typedef void (*ResolveFn)(void*);
            ((ResolveFn)vt[0x270 / 8])(car);
            mask = fh3_car_exhaust_tips(car, msg);
            char b[160];
            snprintf(b, sizeof b, "flames: the Car %p resolved its exhaust locators: mask %02X", car, mask);
            g_api->log(g_mod, b);
        }
    }
    uint32_t forced = g_fh3_mask_forced.load();
    if (forced) {
        for (int i = 0; i < kFH3TipCount; ++i) if (!(forced >> i & 1)) msg[i * kFH3TipSize + kFH3TipFlag] = 0;
        mask &= forced;
    }
    if (!mask) return r;
    g_fh3_in_drive.store(true);
    g_orig_fh3_post(deferred, msg);
    g_fh3_in_drive.store(false);
    g_fh3_car_posts.fetch_add(1, std::memory_order_relaxed);
    return r;
}

static void install_fh3_flames(char* base) {
    FH3Hook* h1 = g_api->hook_function(g_mod, "CCarPresentationDeferred backfire", base + kFH3BackfirePost,
                                       (void*)detour_fh3_post, (void**)&g_orig_fh3_post);
    FH3Hook* h2 = g_api->hook_function(g_mod, "CarPresentation backfire", base + kFH3BackfireRoutine,
                                       (void*)detour_fh3_routine, (void**)&g_orig_fh3_routine);
    FH3Hook* h3 = g_api->hook_function(g_mod, "presentation post queue", base + kFH3PostQueue,
                                       (void*)detour_fh3_queue, (void**)&g_orig_fh3_queue);
    if (!(h3 && g_api->enable_hooks(g_mod, h3))) {
        g_orig_fh3_queue = nullptr;
        g_api->log(g_mod, "Could not hook the presentation queue: the first backfire is needed to find the player's presentation.");
    }
    bool ok1 = h1 && g_api->enable_hooks(g_mod, h1), ok2 = h2 && g_api->enable_hooks(g_mod, h2);
    if (!ok1) g_orig_fh3_post = nullptr;
    FH3Hook* h4 = g_api->hook_function(g_mod, "Car update (the game's exhaust tips)", base + kFH3CarObjUpdate,
                                       (void*)detour_fh3_carobj_update, (void**)&g_orig_fh3_carobj_update);
    if (!(h4 && g_api->enable_hooks(g_mod, h4)))
        g_api->log(g_mod, "Could not hook the Car update: flames fall back to the exhaust locators found by searching.");
    g_api->log(g_mod, ok1 && ok2 ? "Backfire hooked (nitro exhaust flames)." : "Could not hook the backfire: no nitro flames.");
}


// Camera Shake
// While nitro burns, the driver, hood, bumper and chase cameras shake. Each camera's view matrix (3 rows of
// right/up/forward, with the translation in the 4th column) comes from its vtable slot +0x50: 0x1034360 for the
// three mounted cameras, 0x1034430 for the three chase cameras (found through Deuquan's FH2-CameraMods, which
// shakes the same matrix). The functions are hooked rather than the slots, so the camera mod's own slot hooks
// still call through to ours. Other cameras that share the functions are left alone.
typedef void* (*ViewMatrixFn)(void* camera, float* out);
static ViewMatrixFn g_orig_view_mounted = nullptr;
static ViewMatrixFn g_orig_view_chase = nullptr;
// FH3 (same layout as FH2, found by comparing the vtables of Camera::CCamDriver / CCamHood / CCamBumperHigh / CCamFollow /
// CCamFollowLow / CCamFollowHigh): view matrix at game+0x19BB310 (mounted) and game+0x19BAE50 (chase), field of view at
// game+0x19BAB70 / game+0x19BABD0 (vtable slot 15).
static const uintptr_t k_shake_camera_vtables[] = {0x463ACB0, 0x463A9E0, 0x4630430, 0x463AF88, 0x463B250, 0x463B518};
static const uintptr_t kFH3ViewMounted = 0x19BB310, kFH3ViewChase = 0x19BAE50, kFH3FovMounted = 0x19BAB70, kFH3FovChase = 0x19BABD0;
static std::atomic<uint32_t> g_cam_view_calls{0}, g_cam_view_shaken{0}, g_cam_fov_calls{0}, g_cam_fov_widened{0};

// Nitro screen effects share one clock: an envelope that fades in over ~0.2s and out over ~0.4s while the nitro
// burns, and a time that wraps every 1000s. Everything that reads them (camera shake, FOV, the HUD vignette) is a
// function of these two, so asking several times in one frame gives the same picture.
static void fx_state(float* envelope_out, float* t_out) {
    static std::mutex mu;
    static LARGE_INTEGER freq{}, last{};
    static float envelope = 0.0f, t = 0.0f;
    std::lock_guard<std::mutex> lock(mu);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    float dt = last.QuadPart ? static_cast<float>(now.QuadPart - last.QuadPart) / static_cast<float>(freq.QuadPart) : 0.0f;
    last = now;
    if (dt < 0.0f || dt > 0.1f) dt = 0.0f;
    const float target = g_nitro_active.load() ? 1.0f : 0.0f;
    envelope += (target - envelope) * (std::min)(1.0f, dt * (target > envelope ? 5.0f : 2.5f));
    t = fmodf(t + dt, 1000.0f);
    if (envelope_out) *envelope_out = envelope;
    if (t_out) *t_out = t;
}

static void shake_view_matrix(void* camera, float* m) {
    const float strength = g_camera_shake.load();
    if (!camera || !m || !g_game_base || strength <= 0.0f) return;
    const uintptr_t vtable = *static_cast<uintptr_t*>(camera) - reinterpret_cast<uintptr_t>(g_game_base);
    bool ours = false;
    for (uintptr_t v : k_shake_camera_vtables) ours |= v == vtable;
    if (!ours) return;

    float envelope = 0.0f, t = 0.0f;
    fx_state(&envelope, &t);
    if (envelope < 0.001f) return;

    g_cam_view_calls.fetch_add(1, std::memory_order_relaxed);
    for (int i = 0; i < 12; ++i)
        if (!std::isfinite(m[i])) return;
    float* r0 = m; float* r1 = m + 4; float* r2 = m + 8;
    const float d02 = r0[0] * r2[0] + r0[1] * r2[1] + r0[2] * r2[2];
    const float d12 = r1[0] * r2[0] + r1[1] * r2[1] + r1[2] * r2[2];
    const float l2 = r2[0] * r2[0] + r2[1] * r2[1] + r2[2] * r2[2];
    if (fabsf(d02) > 0.05f || fabsf(d12) > 0.05f || l2 < 0.9f || l2 > 1.1f) return;   // not a rotation: leave it

    // A slower wobble under a fast buzz, at frequencies that don't line up. At 100 the peaks are about half a degree.
    constexpr float tau = 6.2831853f;
    const float a = envelope * strength * 0.008f;
    const float pitch = a * (0.55f * sinf(tau * 4.3f * t) + 0.30f * sinf(tau * 13.7f * t + 1.1f) + 0.15f * sinf(tau * 23.1f * t + 2.3f));
    const float yaw   = a * (0.35f * sinf(tau * 3.1f * t + 0.7f) + 0.20f * sinf(tau * 17.3f * t + 0.4f));
    const float roll  = a * (0.60f * sinf(tau * 5.9f * t + 1.9f) + 0.35f * sinf(tau * 19.7f * t + 0.2f));

    auto turn = [](float* x, float* y, float angle) {   // rotates row x towards row y
        const float c = cosf(angle), s = sinf(angle);
        for (int k = 0; k < 4; ++k) {
            const float nx = c * x[k] + s * y[k], ny = c * y[k] - s * x[k];
            x[k] = nx;
            y[k] = ny;
        }
    };
    turn(r2, r0, yaw);
    turn(r2, r1, pitch);
    turn(r0, r1, roll);
    g_cam_view_shaken.fetch_add(1, std::memory_order_relaxed);
}

static void* detour_view_mounted(void* camera, float* out) {
    void* r = g_orig_view_mounted ? g_orig_view_mounted(camera, out) : nullptr;
    shake_view_matrix(camera, out);
    return r;
}

static void* detour_view_chase(void* camera, float* out) {
    void* r = g_orig_view_chase ? g_orig_view_chase(camera, out) : nullptr;
    shake_view_matrix(camera, out);
    return r;
}

// Field of view
// Vtable slot +0x70 of the mounted and chase cameras returns the camera's field of view as an angle (the mounted
// one reads it from the camera, the chase one converts degrees to radians). While the nitro burns it is made a
// little wider, in the same fade as the shake. A scale, so it works whichever unit the camera uses.
typedef float (*FovFn)(void* camera);
static FovFn g_orig_fov_mounted = nullptr;
static FovFn g_orig_fov_chase = nullptr;

static float widen_fov(void* camera, float fov) {
    const float boost = g_fov_boost.load();
    if (boost <= 0.0f || !camera || !g_game_base || !std::isfinite(fov) || fov <= 0.0f || fov > 400.0f) return fov;
    const uintptr_t vtable = *static_cast<uintptr_t*>(camera) - reinterpret_cast<uintptr_t>(g_game_base);
    bool ours = false;
    for (uintptr_t v : k_shake_camera_vtables) ours |= v == vtable;
    if (!ours) return fov;
    float envelope = 0.0f;
    fx_state(&envelope, nullptr);
    if (envelope < 0.001f) return fov;
    g_cam_fov_widened.fetch_add(1, std::memory_order_relaxed);
    return fov * (1.0f + (std::min)(boost, 0.3f) * envelope);
}

static float detour_fov_mounted(void* camera) {
    return widen_fov(camera, g_orig_fov_mounted(camera));
}

static float detour_fov_chase(void* camera) {
    return widen_fov(camera, g_orig_fov_chase(camera));
}

// Smooth random flicker for the vignette: value noise (a fresh random value every 1/rate seconds, eased in between).
static float vig_hash(int n) {
    unsigned x = static_cast<unsigned>(n) * 747796405u + 2891336453u;
    x = ((x >> ((x >> 28) + 4)) ^ x) * 277803737u;
    x = (x >> 22) ^ x;
    return static_cast<float>(x & 0xFFFFu) / 65535.0f;
}
static float vig_noise(float t, float rate) {
    const float x = t * rate;
    const float fl = floorf(x);
    float f = x - fl;
    f = f * f * (3.0f - 2.0f * f);
    const int i = static_cast<int>(fl);
    return vig_hash(i) * (1.0f - f) + vig_hash(i + 1) * f;
}

// Speedometer Gauge Hook: HUDStatusSpeedometer tick (RVA 0xAD6070 in Anthem)
// rcx = HUDStatusSpeedometer* speedo, xmm1 = float current_rpm
typedef void (*SpeedoTickFn)(void* speedo, float rpm);
static SpeedoTickFn g_orig_speedo_tick = nullptr;

static void detour_speedo_tick(void* speedo, float rpm) {
    if (g_orig_speedo_tick) {
        g_orig_speedo_tick(speedo, rpm);
    }

    if (!speedo || !g_game_base) return;

    g_last_speedo_tick.store(GetTickCount64());

    __try {
        float fuel = g_current_fuel.load();
        float duration = g_duration.load();
        float total_ratio = (duration > 0.001f) ? (fuel / duration) : 0.0f;
        if (total_ratio < 0.0f) total_ratio = 0.0f;
        if (total_ratio > 1.0f) total_ratio = 1.0f;
        // The gauge is only there when the car being driven has the nitrous kit: without it everything is
        // emptied and the frame is made transparent (the XAML ties its opacity to item 20's YPos).
        const bool gauge_visible = g_enabled.load() && nu_level() > 0;
        if (!gauge_visible) total_ratio = 0.0f;

        // Sequential segments (Top segment drains first, down to bottom segment):
        // Segment 1 (bottom: 0.00 to 0.25)
        // Segment 2:         0.25 to 0.50
        // Segment 3:         0.50 to 0.75
        // Segment 4 (top:    0.75 to 1.00)
        auto clamp01 = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
        float s1 = clamp01((total_ratio - 0.00f) / 0.25f);
        float s2 = clamp01((total_ratio - 0.25f) / 0.25f);
        float s3 = clamp01((total_ratio - 0.50f) / 0.25f);
        float s4 = clamp01((total_ratio - 0.75f) / 0.25f);

        // Active nitrous glowing tip cap (follows the exact leading edge of the draining NOS bar):
        bool active = g_nitro_active.load() && gauge_visible;
        float glow_scale = 0.0f;
        float leading_angle = 55.87f;

        if (s4 > 0.001f) {
            leading_angle = 118.57f + s4 * 20.26f;
        } else if (s3 > 0.001f) {
            leading_angle = 97.67f + s3 * 20.26f;
        } else if (s2 > 0.001f) {
            leading_angle = 76.77f + s2 * 20.26f;
        } else {
            leading_angle = 55.87f + s1 * 20.26f;
        }
        constexpr float kFH3Rot = NITRO_FH3 ? -5.0f : 0.0f;   // the FH3 dial's bar is turned 5 degrees down
        // FH3: the four segments are drawn 17 degrees wide (gaps between them, like the digital bar), not 20.26
        constexpr float kSegW = 20.26f;   // the gaps between the four segments are cut by a clip in the XAML
        leading_angle = leading_angle - (s4 > 0.001f ? 118.57f + s4 * 20.26f : s3 > 0.001f ? 97.67f + s3 * 20.26f : s2 > 0.001f ? 76.77f + s2 * 20.26f : 55.87f + s1 * 20.26f)
                      + (s4 > 0.001f ? 118.57f + s4 * kSegW : s3 > 0.001f ? 97.67f + s3 * kSegW : s2 > 0.001f ? 76.77f + s2 * kSegW : 55.87f + s1 * kSegW);
        float dg[4] = {0, 0, 0, 0};   // digital bar: glow of the leading segment while the nitro burns
        leading_angle += kFH3Rot;

        // The glow passes are narrow RadialBar tracks (AngleAmount sets the width) centered on the
        // rotation angle, so Ratio 1.0 fills the whole track and YPos is the leading edge. The
        // XAML offsets slide each pass back so it ends on the edge.
        // The widest pass trails kGlowTrail degrees behind the edge. Near empty, park the glow where
        // its tail meets the bar start and shrink Ratio so it fades out instead of overhanging.
        const float kBarStart = 55.87f + kFH3Rot;
        const float kGlowTrail = 12.0f;
        if (active && total_ratio > 0.005f) {
            float room = leading_angle - kBarStart;
            if (room >= kGlowTrail) {
                glow_scale = 1.0f;
            } else {
                glow_scale = room / kGlowTrail;
                leading_angle = kBarStart + kGlowTrail;
            }
        }

        // Digital HUD: one continuous tip glow slides along the whole 245px bar (4 segments of 59px
        // with 3px gaps, so a 62px pitch). Segment 1's YPos carries the glow's right edge as an
        // offset from the bar end, or -1000 to park it off the bar. The analog HUD ignores it.
        const float kBarWidth = 245.0f;
        const float kSegPitch = 62.0f;
        const float kSegWidth = 59.0f;
        float tip_x = -1000.0f;
        if (active && total_ratio > 0.005f) {
            const float seg_fill[4] = {s1, s2, s3, s4};
            int lead = (s4 > 0.001f) ? 3 : (s3 > 0.001f) ? 2 : (s2 > 0.001f) ? 1 : 0;
            tip_x = lead * kSegPitch + kSegWidth * seg_fill[lead] - kBarWidth;
        }

        // The pulsing blue vignette (a full-screen overlay in the speedometer XAML, opacity tied to item 18's YPos).
        float vignette_alpha = 0.0f;
        float fx_env = 0.0f;
        if (active) {
            float envelope = 0.0f, t = 0.0f;
            fx_state(&envelope, &t);
            fx_env = envelope;
            const float pulse = 0.62f + 0.38f * (0.6f * vig_noise(t, 2.3f) + 0.4f * vig_noise(t + 37.1f, 5.7f));   // irregular, never below 62 %
            vignette_alpha = envelope * pulse;
        } else {
            float envelope = 0.0f;
            fx_state(&envelope, nullptr);
            fx_env = envelope * 0.8f;
            vignette_alpha = envelope * 0.8f;   // fades out instead of cutting
        }
        // Layer A (edge tint) gets the opacity, layer B (the wider glow) the opacity times the intensity.
        const float vig_a = g_vig_on.load() ? vignette_alpha * g_vig_opacity : 0.0f;
        const float vig_b = vig_a * g_vig_intensity;
        // Speed lines (see scripts/gen_speedlines.py): six tiers of looping streak animations, each made for a different
        // car speed. The tiers are crossfaded by the car's speed (the weights are interpolated on a log scale between the
        // speeds the tiers are made for), so the lines get faster as the car does, without a visible jump. Each tier's
        // container opacity is one HUD data item value: [15].X, [15].Y, [14].X, [14].Y, [13].X, [13].Y.
        static const float kSpeedLineTierKmh[6] = {40.0f, 70.0f, 110.0f, 165.0f, 235.0f, 320.0f};   // keep in step with TIER_KMH in the generator
        float sl_master = g_sl_on.load() ? fx_env * g_sl_opacity : 0.0f;
        float sl_tier[6] = {0, 0, 0, 0, 0, 0};
        {
            const float kmh = g_player_speed.load() * 3.6f;
            float w[6] = {0, 0, 0, 0, 0, 0};
            if (kmh <= kSpeedLineTierKmh[0]) {
                w[0] = 1.0f;
            } else if (kmh >= kSpeedLineTierKmh[5]) {
                w[5] = 1.0f;
            } else {
                for (int i = 0; i < 5; ++i) {
                    if (kmh <= kSpeedLineTierKmh[i + 1]) {
                        const float x = logf(kmh / kSpeedLineTierKmh[i]) / logf(kSpeedLineTierKmh[i + 1] / kSpeedLineTierKmh[i]);
                        w[i] = 1.0f - x;
                        w[i + 1] = x;
                        break;
                    }
                }
            }
            float amp = (g_player_speed.load() - 15.0f) / 45.0f;   // subtler at low speed
            amp = 0.45f + 0.55f * (amp < 0.0f ? 0.0f : (amp > 1.0f ? 1.0f : amp));
            sl_master *= amp;
            for (int i = 0; i < 6; ++i) {
                const float v = sl_master * w[i];
                sl_tier[i] = v < 0.004f ? 0.0f : v;
            }
            static bool sl_logged = false;   // one line per nitro burst: is the driver running?
            if (sl_master > 0.05f && !sl_logged) {
                sl_logged = true;
                char msg[160];
                snprintf(msg, sizeof(msg), "Nitro speed lines are being driven (opacity %.2f at %.0f km/h; tiers %.2f %.2f %.2f %.2f %.2f %.2f).",
                         sl_master, kmh, sl_tier[0], sl_tier[1], sl_tier[2], sl_tier[3], sl_tier[4], sl_tier[5]);
                if (g_api && g_mod) g_api->log(g_mod, msg);
            } else if (sl_master < 0.004f) {
                sl_logged = false;
            }
        }
        vignette_alpha = vig_a;
        if (vignette_alpha < 0.004f) vignette_alpha = 0.0f;
        {   // one line in the log per nitro burst, so a missing vignette can be told from a missing overlay
            static bool logged = false;
            if (vignette_alpha > 0.05f && !logged) {
                logged = true;
                if (g_api && g_mod) g_api->log(g_mod, "Nitro vignette is being driven (HUD items 18 and 19, YPos).");
            } else if (vignette_alpha == 0.0f) {
                logged = false;
            }
        }

        // Drive the 4 NOS segments via RPMGaugeNumbers[17..20] and the glowing tip cap on [16].
#if NITRO_FH3
        // FH3: the RPMGaugeNumbers list is the object at speedo+0x220 (the same +0x50 -> +0x28 -> +0x28 chain follows).
        if (!readable_ptr(speedo, 0x238) || *(uintptr_t*)speedo != (uintptr_t)g_game_base + 0x44A4318) return;
        void* arr_wrapper = *(void**)((uintptr_t)speedo + 0x220);
#else
        void* arr_wrapper = *(void**)((uintptr_t)speedo + 0x150);
#endif
        if (arr_wrapper && readable_ptr(arr_wrapper, 0x58)) {
            void* p50 = *(void**)((uintptr_t)arr_wrapper + 0x50);
            if (p50) {
                void* p28 = *(void**)((uintptr_t)p50 + 0x28);
                if (p28) {
                    void** items = *(void***)((uintptr_t)p28 + 0x28);
                    if (items) {
                        typedef void* (*GetPropFn)();
                        typedef void (*RaiseItemPropFn)(void* item, void* prop);

#if NITRO_FH3
                        GetPropFn get_xpos_prop = (GetPropFn)((uintptr_t)g_game_base + 0x11042F0);
                        GetPropFn get_ypos_prop = (GetPropFn)((uintptr_t)g_game_base + 0x1104400);
                        RaiseItemPropFn raise_item_prop = (RaiseItemPropFn)((uintptr_t)g_game_base + 0x10FA660);
#else
                        GetPropFn get_xpos_prop = (GetPropFn)((uintptr_t)g_game_base + 0xADD560);
                        GetPropFn get_ypos_prop = (GetPropFn)((uintptr_t)g_game_base + 0xADD620);
                        RaiseItemPropFn raise_item_prop = (RaiseItemPropFn)((uintptr_t)g_game_base + 0xAD1A80);
#endif

                        void* prop_xpos = get_xpos_prop();
                        void* prop_ypos = get_ypos_prop();

                        if (active && total_ratio > 0.005f) dg[s4 > 0.001f ? 3 : s3 > 0.001f ? 2 : s2 > 0.001f ? 1 : 0] = glow_scale;
                        struct SegTarget {
                            int idx;
                            float x;
                            float y;
                        } targets[8] = {
                            {17, s1, tip_x},                 // Segment 1 (Bottom); Y = digital glow offset
                            {18, s2, vignette_alpha},        // Segment 2; Y = opacity of the blue nitro vignette
                            {19, s3, vig_b < 0.004f ? 0.0f : vig_b}, // Segment 3; Y = opacity of the wider blue glow (vignette layer B)
                            {20, s4, (gauge_visible || !g_hud_hide.load()) ? 1.0f : 0.0f},   // Segment 4 (Top); Y = opacity of the NOS frame (1 = shown)
                            {16, glow_scale, leading_angle}, // Active Glowing Tip Cap (X=Ratio, Y=Angle)
#if NITRO_FH3
                            // FH3 (no speed lines in the XAML): the digital bar's glowing tip, one value per segment, only on the leading one
                            {15, 0.0f, 0.0f},
                            {14, dg[2], dg[3]},
                            {13, dg[0], dg[1]}
#else
                            {15, sl_tier[0], sl_tier[1]},    // speed line tiers 0 and 1: opacity (the stock rev numbers 13 to 15 are removed from the XAML)
                            {14, sl_tier[2], sl_tier[3]},    // tiers 2 and 3
                            {13, sl_tier[4], sl_tier[5]}     // tiers 4 and 5
#endif
                        };

                        for (const auto& t : targets) {
                            void* item = items[t.idx];
                            if (!item || !readable_ptr(item, 0x40)) continue;

                            float* p_x = (float*)((uintptr_t)item + 0x38);
                            if (fabsf(*p_x - t.x) > 0.0005f) {
                                *p_x = t.x;
                                if (prop_xpos) raise_item_prop(item, prop_xpos);
                            }

                            float* p_y = (float*)((uintptr_t)item + 0x3c);
                            if (!std::isnan(t.y) && fabsf(*p_y - t.y) > 0.005f) {
                                *p_y = t.y;
                                if (prop_ypos) raise_item_prop(item, prop_ypos);
                            }
                        }
                    }
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}


// Settings callbacks
static void set_rumble(WORD left_motor, WORD right_motor);

static void on_setting_changed(void* user, const char* key, int value) {
    if (strcmp(key, "DebugOverlay") == 0) {
        g_debug.store(value != 0);
        save_config();
        return;
    }
    if (strcmp(key, "Rumble") == 0) {
        g_rumble.store(value != 0);
        if (!value) set_rumble(0, 0);   // stop a rumble that's already going
        save_config();
        return;
    }
    if (strcmp(key, "HudHide") == 0) {
        g_hud_hide.store(value != 0);
        save_config();
        apply_hud_theme();   // written now, read by the game at the next start
        if (g_api && g_mod) g_api->log(g_mod, value ? "Hide nitro gauge without kit: on (applies after a restart)" : "Hide nitro gauge without kit: off (applies after a restart)");
        return;
    }
    if (strcmp(key, "SpeedLinesEnabled") == 0) {
        g_sl_on.store(value != 0);
        save_config();
        if (g_api && g_mod) g_api->log(g_mod, value ? "Nitro speed lines: on" : "Nitro speed lines: off");
        return;
    }
    if (strcmp(key, "VignetteEnabled") == 0) {
        g_vig_on.store(value != 0);
        save_config();
        if (g_api && g_mod) g_api->log(g_mod, value ? "Nitro vignette: on" : "Nitro vignette: off");
        return;
    }
    if (strcmp(key, "RefundOnRewind") == 0) {
        g_refund_on_rewind.store(value != 0);
        save_config();
        return;
    }
    if (strcmp(key, "RefillOnRaceStart") == 0) {
        g_refill_on_start.store(value != 0);
        save_config();
        return;
    }
    if (strcmp(key, "BackfireOnAllCars") == 0) {
        g_backfire_all_cars.store(value != 0);
        save_config();
        if (g_api && g_mod) g_api->log(g_mod, value ? "Backfire on all cars on (applies after a restart)" : "Backfire on all cars off (applies after a restart)");
        return;
    }
    (void)user;
    if (strcmp(key, "NitroEnabled") == 0) {
        g_enabled.store(value != 0);
        save_config();
        if (g_api && g_mod) {
            g_api->log(g_mod, value ? "Nitro enabled" : "Nitro disabled");
        }
    }
}

static void on_slider_changed(void* user, const char* key, float value) {
    (void)user;
    char buf[128];
    if (strcmp(key, "NitroButton") == 0) {
        constexpr int n = (int)(sizeof(k_buttons) / sizeof(k_buttons[0]));
        int idx = (int)(value + 0.5f) - 1;
        if (idx < 0) idx = 0;
        if (idx >= n) idx = n - 1;
        g_nitro_button.store(k_buttons[idx].mask);
        save_config();
        snprintf(buf, sizeof(buf), "NitroButton set to %ls (saved)", k_buttons[idx].name);
        if (g_api && g_mod) g_api->log(g_mod, buf);
    } else if (strcmp(key, "HudTheme") == 0) {
        g_hud_theme.store((int)(value + 0.5f) >= 2 ? 1 : 0);
        save_config();
        apply_hud_theme();   // the files are written now; the game picks them up when it restarts
        snprintf(buf, sizeof(buf), "HudTheme set to %s (saved, applies after a restart)", g_hud_theme.load() ? "Fast & Furious" : "Original");
        if (g_api && g_mod) g_api->log(g_mod, buf);
    } else if (strcmp(key, "Duration") == 0) {
        g_duration.store(value);
        save_config();
        snprintf(buf, sizeof(buf), "Nitro Duration set to %.1fs (saved)", value);
        if (g_api && g_mod) g_api->log(g_mod, buf);
    } else if (strcmp(key, "SoundVolume") == 0) {
        g_sound_volume.store(value);
        save_config();
        snprintf(buf, sizeof(buf), "SoundVolume set to %.0f%% (saved)", value * 100.0f);
        if (g_api && g_mod) g_api->log(g_mod, buf);
    } else if (strcmp(key, "CameraShake") == 0) {
        g_camera_shake.store(value);
        save_config();
        snprintf(buf, sizeof(buf), "CameraShake set to %.0f%% (saved)", value * 100.0f);
        if (g_api && g_mod) g_api->log(g_mod, buf);
    } else if (strcmp(key, "FovBoost") == 0) {
        g_fov_boost.store(value);
        save_config();
        snprintf(buf, sizeof(buf), "FovBoost set to %.0f%% (saved)", value * 100.0f);
        if (g_api && g_mod) g_api->log(g_mod, buf);
    } else if (strcmp(key, "BuildupTime") == 0) {
        g_buildup_time.store(value);
        save_config();
        snprintf(buf, sizeof(buf), "BuildupTime set to %.2fs (saved)", value);
        if (g_api && g_mod) g_api->log(g_mod, buf);
    } else if (strcmp(key, "RechargeRate") == 0) {
        g_recharge_rate.store(value);
        save_config();
        snprintf(buf, sizeof(buf), "RechargeRate set to %.2fx (saved)", value);
        if (g_api && g_mod) g_api->log(g_mod, buf);
    }
}

// Controller Haptics
// With Controller Rumble off only the "stop" (0, 0) goes through, so the game's own rumble is left alone.
static void set_rumble(WORD left_motor, WORD right_motor) {
    if (!g_rumble.load() && (left_motor || right_motor)) return;
    XINPUT_VIBRATION vib{};
    vib.wLeftMotorSpeed = left_motor;
    vib.wRightMotorSpeed = right_motor;
    for (DWORD i = 0; i < 4; ++i) {
        if (xin::set_state) xin::set_state(i, &vib);
    }
}

// Audio Control
// The boost sound is mixed here from the FF layers embedded in the DLL: the start burst over a bed of the two
// sustain loops taking turns, with a short crossfade between them. PlaySound can only loop a whole sound, so the
// mix is made a little longer than a full tank: the start never plays again, however big the tank is. The stop
// sound is played as it is. A nos_boost.wav / nos_stop.wav in the NitroSystemII folder next to azimuth.exe replaces
// them, for players who want their own (a custom boost sound loops whole). PlaySound has no volume of its own,
// so the samples are scaled by the Sound Volume slider. A sound is only rebuilt when something it's made from
// changes, and its buffer has to stay alive while PlaySound is still playing it.
struct NitroSound {
    const wchar_t* custom_path;
    const char* embedded_name;   // nullptr: mixed from the layers
    std::vector<char> wav;
    float volume = -1.0f;
    float seconds = -1.0f;
    bool custom = false;
};
static NitroSound g_boost_sound{g_sound_boost_path, nullptr};
static NitroSound g_stop_sound{g_sound_stop_path, "nos_stop"};

// Where a 16-bit PCM wav's samples are. samples is nullptr for anything else.
struct Pcm16 {
    int16_t* samples = nullptr;
    size_t frames = 0;
    int channels = 0;
    uint32_t rate = 0;
};

static Pcm16 find_pcm16(char* wav, size_t size) {
    Pcm16 pcm;
    if (size < 12 || memcmp(wav, "RIFF", 4) != 0 || memcmp(wav + 8, "WAVE", 4) != 0) return pcm;
    bool pcm16 = false;
    for (size_t pos = 12; pos + 8 <= size;) {
        uint32_t len;
        memcpy(&len, wav + pos + 4, 4);
        char* body = wav + pos + 8;
        const size_t avail = (std::min)(static_cast<size_t>(len), size - pos - 8);
        if (memcmp(wav + pos, "fmt ", 4) == 0 && avail >= 16) {
            uint16_t tag, channels, bits, sub = 0;
            memcpy(&tag, body, 2);
            memcpy(&channels, body + 2, 2);
            memcpy(&pcm.rate, body + 4, 4);
            memcpy(&bits, body + 14, 2);
            if (tag == 0xFFFE && avail >= 26) memcpy(&sub, body + 24, 2);   // WAVE_FORMAT_EXTENSIBLE: the real tag starts the GUID
            pcm16 = bits == 16 && channels >= 1 && channels <= 2 && (tag == 1 || (tag == 0xFFFE && sub == 1));
            pcm.channels = channels;
        } else if (memcmp(wav + pos, "data", 4) == 0) {
            if (pcm16) {
                pcm.samples = reinterpret_cast<int16_t*>(body);
                pcm.frames = avail / (2 * pcm.channels);
            }
            return pcm;
        }
        pos += 8 + static_cast<size_t>(len) + (len & 1);
    }
    return pcm;
}

static void scale_wav(std::vector<char>& wav, float volume) {
    Pcm16 pcm = find_pcm16(wav.data(), wav.size());
    for (size_t i = 0; pcm.samples && i < pcm.frames * pcm.channels; ++i)
        pcm.samples[i] = static_cast<int16_t>(lroundf(pcm.samples[i] * volume));
}

static Pcm16 embedded_pcm16(const char* name) {
    DWORD size = 0;
    const void* data = embedded_file(name, &size);
    return data ? find_pcm16(static_cast<char*>(const_cast<void*>(data)), size) : Pcm16{};
}

// The start burst over the sustains, `seconds` long, as a 48 kHz 16-bit stereo wav.
static std::vector<char> mix_boost(float seconds, float volume) {
    constexpr uint32_t rate = 48000;
    constexpr size_t fade = rate / 50;   // 20 ms crossfade between sustains
    const Pcm16 start = embedded_pcm16("nos_start");
    const Pcm16 sustains[2] = {embedded_pcm16("nos_sustain_1"), embedded_pcm16("nos_sustain_2")};
    for (const Pcm16& p : {start, sustains[0], sustains[1]})
        if (!p.samples || p.rate != rate || p.frames <= 2 * fade) return {};

    const size_t frames = static_cast<size_t>(seconds * rate);
    std::vector<float> mix(frames * 2, 0.0f);
    for (size_t pos = 0, n = 0; pos < frames; pos += sustains[n++ % 2].frames - fade) {
        const Pcm16& s = sustains[n % 2];
        for (size_t j = 0; j < s.frames && pos + j < frames; ++j) {
            float keep = 0.0f, add = 1.0f;
            if (n > 0 && j < fade) {   // equal-power crossfade from the sustain before
                const float a = (j + 0.5f) / fade * 1.5707963f;
                keep = cosf(a);
                add = sinf(a);
            }
            for (int c = 0; c < 2; ++c) {
                float& out = mix[(pos + j) * 2 + c];
                out = out * keep + s.samples[j * s.channels + (s.channels == 2 ? c : 0)] * add;
            }
        }
    }
    for (size_t j = 0; j < start.frames && j < frames; ++j)
        for (int c = 0; c < 2; ++c) mix[j * 2 + c] += start.samples[j * start.channels + (start.channels == 2 ? c : 0)];

    const uint32_t data_size = static_cast<uint32_t>(frames * 4);
    std::vector<char> wav(44 + data_size);
    auto put32 = [&](size_t at, uint32_t v) { memcpy(wav.data() + at, &v, 4); };
    auto put16 = [&](size_t at, uint16_t v) { memcpy(wav.data() + at, &v, 2); };
    memcpy(wav.data(), "RIFF", 4);
    put32(4, 36 + data_size);
    memcpy(wav.data() + 8, "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1);            // PCM
    put16(22, 2);            // stereo
    put32(24, rate);
    put32(28, rate * 4);     // bytes per second
    put16(32, 4);            // bytes per frame
    put16(34, 16);           // bits
    memcpy(wav.data() + 36, "data", 4);
    put32(40, data_size);
    int16_t* out = reinterpret_cast<int16_t*>(wav.data() + 44);
    for (size_t i = 0; i < mix.size(); ++i)
        out[i] = static_cast<int16_t>(lroundf((std::max)(-32768.0f, (std::min)(32767.0f, mix[i] * volume))));
    return wav;
}

static void play_nitro_sound(NitroSound& sound, DWORD flags) {
    const float volume = g_sound_volume.load();
    if (volume <= 0.0f) return;
    const bool custom = sound.custom_path[0] != L'\0' && GetFileAttributesW(sound.custom_path) != INVALID_FILE_ATTRIBUTES;
    const float seconds = sound.embedded_name ? 0.0f : g_duration.load() + 3.0f;
    if (sound.wav.empty() || sound.volume != volume || sound.custom != custom || (!custom && sound.seconds != seconds)) {
        sound.wav.clear();
        if (custom) {
            HANDLE f = CreateFileW(sound.custom_path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                LARGE_INTEGER size{};
                DWORD got = 0;
                if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < 256 * 1024 * 1024) {
                    sound.wav.resize(static_cast<size_t>(size.QuadPart));
                    if (!ReadFile(f, sound.wav.data(), static_cast<DWORD>(sound.wav.size()), &got, nullptr) || got != sound.wav.size())
                        sound.wav.clear();
                }
                CloseHandle(f);
            }
            if (volume < 1.0f) scale_wav(sound.wav, volume);
        } else if (!sound.embedded_name) {
            sound.wav = mix_boost(seconds, volume);
        } else {
            DWORD size = 0;
            if (const void* wav = embedded_file(sound.embedded_name, &size))
                sound.wav.assign(static_cast<const char*>(wav), static_cast<const char*>(wav) + size);
            if (volume < 1.0f) scale_wav(sound.wav, volume);
        }
        sound.volume = volume;
        sound.seconds = seconds;
        sound.custom = custom;
    }
    if (!sound.wav.empty())
        PlaySoundW(reinterpret_cast<LPCWSTR>(sound.wav.data()), NULL, flags | SND_MEMORY | SND_NODEFAULT);
}

static void start_nitro_audio() {
    PlaySoundW(NULL, NULL, 0);   // nothing may still be playing from a buffer that's about to be rebuilt
    play_nitro_sound(g_boost_sound, SND_ASYNC | SND_LOOP);
}

static void stop_nitro_audio() {
    PlaySoundW(NULL, NULL, 0);
    play_nitro_sound(g_stop_sound, SND_ASYNC);
}

// Fill the tank. One cooldown for every trigger below, since a single transition can set off several.
static void refill_nitro(const char* why, uint64_t now) {
    static uint64_t last_refill = 0;
    if (now - last_refill < 3000) return;
    last_refill = now;
    float full = g_duration.load();
    if (g_current_fuel.load() < full - 0.05f || g_depleted_lockout.load()) {
        g_current_fuel.store(full);
        g_depleted_lockout.store(false);
        char msg[128];
        snprintf(msg, sizeof(msg), "Nitro tank refilled (%s).", why);
        g_api->log(g_mod, msg);
    }
}

// Starting or leaving a race, quitting one, changing car, fast travelling and rewinding all hide the HUD speedometer
// while the car keeps simulating (a pause doesn't: physics stops with it, and on resuming the speedometer comes
// back before physics does). So: arm when the speedometer has been off for 2 s with physics running, or when
// physics resumes with it off, and act when it comes back.
//
// What to do depends on where the car ends up (seen in the logs):
//   - a rewind replays the car's recorded path backwards for several seconds, ending 40 m or more from where the
//     speedometer went off, on a spot the car passed a few seconds earlier: give back the nitro used since then;
//   - anything else (race start, quit, car change, fast travel) fills the tank.
// The path is recorded while the speedometer is on, and frozen while it is off.
struct FuelSample {
    float pos[3];
    float fuel;
    uint64_t tick;
};

static float dist3(const float* a, const float* b) {
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

static void nitro_hud_events(bool physics, bool speedo, uint64_t now) {
    static std::vector<FuelSample> path;   // newest last; one minute at 10 samples a second
    static float freeze_pos[3] = {0.0f, 0.0f, 0.0f};
    static bool prev_physics = false;
    static bool prev_speedo = false;
    static uint64_t off_since = 0;
    static bool armed = false;

    float cur[3] = {g_player_pos[0], g_player_pos[1], g_player_pos[2]};
    bool pos_fresh = (now - g_player_pos_tick.load()) < 200;

    if (speedo && physics && pos_fresh && !armed && (path.empty() || now - path.back().tick >= 100)) {
        path.push_back({{cur[0], cur[1], cur[2]}, g_current_fuel.load(), now});
        if (path.size() > 600) path.erase(path.begin());
    }

    if (prev_speedo && !speedo) {   // the speedometer just went off: where the car was
        freeze_pos[0] = cur[0]; freeze_pos[1] = cur[1]; freeze_pos[2] = cur[2];
    }
    if (physics && !speedo) {
        if (off_since == 0) off_since = now;
        if (!prev_physics || now - off_since >= 2000) armed = true;
    } else {
        off_since = 0;
    }

    if (speedo && physics && armed) {
        armed = false;
        int best = -1;
        float best_d = 25.0f;
        if (dist3(cur, freeze_pos) > 40.0f) {
            for (int i = 0; i < (int)path.size(); ++i) {
                float d = dist3(cur, path[i].pos);
                if (d < best_d) { best_d = d; best = i; }
            }
        }
        char msg[128];
        if (best >= 0) {   // a rewind
            float then = path[best].fuel;
            path.resize((size_t)best + 1);   // what came after that point was undone
            if (g_refund_on_rewind.load() && then > g_current_fuel.load() + 0.05f) {
                g_current_fuel.store(then);
                if (then >= 2.0f) g_depleted_lockout.store(false);
                snprintf(msg, sizeof(msg), "Rewind: nitro tank restored to %.1f s (back %.0f m, matched within %.0f m).", then,
                         dist3(cur, freeze_pos), best_d);
                g_api->log(g_mod, msg);
            }
        } else {
            path.clear();   // somewhere new: the old path no longer leads here
            if (g_refill_on_start.load()) refill_nitro("race start / quit / car change / fast travel", now);
        }
    }
    prev_physics = physics;
    prev_speedo = speedo;
}

// Background thread: handles gamepad input polling, state-machine, audio & haptics
static void nitro_worker_thread() {
    if (!g_api || !g_mod) return;

    xin::load();
    g_api->log(g_mod, xin::get_state ? "XInput loaded." : "XInput could not be loaded: the nitro button will not work.");
    g_api->log(g_mod, "Worker thread waiting for game code...");
    void* base = g_api->wait_for_code(0xFFFFFFFF);
    if (!base) {
        g_api->log(g_mod, "Game code not found (timed out).");
        return;
    }

    char log_buf[128];
    snprintf(log_buf, sizeof(log_buf), "Game mapped at base %p. Installing physics boost hook...", base);
    g_api->log(g_mod, log_buf);
    g_game_base = base;

    // FH3: CCarDynamics' per-tick update (vtable slot 16) and CWorldCollObjMan::Update. The other FH2 hooks
    // (backfire, frame step, cameras, speedometer) aren't ported yet.
    g_api->log(g_mod, install_release_hook((char*)base) ? "Car physics tick hooked (nitro push, 360 Hz)."
                                                        : "Could not hook the car physics tick: no nitro push.");

    install_fh3_flames((char*)base);
    {
        FH3Hook* hp = g_api->hook_function(g_mod, "sqlite3LockAndPrepare", (char*)base + 0x440C0,
                                           (void*)detour_prepare, (void**)&g_orig_prepare);
        if (!(hp && g_api->enable_hooks(g_mod, hp))) { g_orig_prepare = nullptr; g_api->log(g_mod, "Could not hook the SQL prepare function."); }
    }
    {
        FH3Hook* hc = g_api->hook_function(g_mod, "HUD controller update", (char*)base + 0xB26690,
                                           (void*)detour_hud_ctrl, (void**)&g_orig_hud_ctrl);
        if (!(hc && g_api->enable_hooks(g_mod, hc))) { g_orig_hud_ctrl = nullptr; g_api->log(g_mod, "Could not hook the HUD controller: the player's car is told apart by position only."); }
    }
    {   // the speedometer's per-frame update (rcx = HUDStatusSpeedometer, xmm1 = rpm): drives the NOS gauge
        FH3Hook* hs = g_api->hook_function(g_mod, "HUDStatusSpeedometer update", (char*)base + 0x10FD450,
                                           (void*)detour_speedo_tick, (void**)&g_orig_speedo_tick);
        g_api->log(g_mod, hs && g_api->enable_hooks(g_mod, hs) ? "Speedometer hooked (NOS gauge)."
                                                              : "Could not hook the speedometer: no NOS gauge.");
        apply_hud_theme();
    }
    {
        struct H { const char* name; uintptr_t rva; void* detour; void** orig; };
        const H hooks[] = {
            {"camera view (mounted)", kFH3ViewMounted, (void*)detour_view_mounted, (void**)&g_orig_view_mounted},
            {"camera view (chase)", kFH3ViewChase, (void*)detour_view_chase, (void**)&g_orig_view_chase},
            {"camera fov (mounted)", kFH3FovMounted, (void*)detour_fov_mounted, (void**)&g_orig_fov_mounted},
            {"camera fov (chase)", kFH3FovChase, (void*)detour_fov_chase, (void**)&g_orig_fov_chase},
        };
        int ok = 0;
        for (const H& h : hooks) {
            FH3Hook* hk = g_api->hook_function(g_mod, h.name, (char*)base + h.rva, h.detour, h.orig);
            if (hk && g_api->enable_hooks(g_mod, hk)) ++ok;
            else *h.orig = nullptr;
        }
        char cm[100];
        snprintf(cm, sizeof cm, "Camera hooks installed: %d of 4 (screen shake and wider field of view while nitro burns).", ok);
        g_api->log(g_mod, cm);
    }

    void* mgr_update = (void*)((uintptr_t)base + kFH3WorldUpdate);
    FH3Hook* hook_mgr = g_api->hook_function(g_mod, "CWorldCollObjMan::Update", mgr_update, (void*)detour_mgr_update, (void**)&g_orig_mgr_update);
    g_api->log(g_mod, hook_mgr && g_api->enable_hooks(g_mod, hook_mgr) ? "CWorldCollObjMan::Update hooked to find the player's car."
                                                                       : "Could not hook CWorldCollObjMan::Update: the player's car can't be found, so no nitro.");

    // FH3 research: what the hooks see, every 5 seconds for the first minutes (the offsets come from reading the
    // code, not from a running game).
    std::thread([] {
        for (int i = 0; i < 300; ++i) {
            Sleep(2000);
            char b[480];
            static uint64_t last_frames = 0;
            const uint64_t frames = g_physics_frames.load();
            int cars = 0;
            const uint64_t now = GetTickCount64();
            for (const auto& c : g_seen)
                if (c.ptr && now - c.tick < 1000) ++cars;
            snprintf(b, sizeof b,
                     "FH3 check: %llu car updates in 2 s (dt %.4f), %d cars, player pos %s (%.0f, %.0f, %.0f), player car %p at %.1f m from it, "
                     "torque scale %.3f at +0x%X, speed %.1f m/s (game's own %.1f, measured %.1f), accel %.1f m/s2 plain / %.1f with nitro (engine %.1f, power scale %.2f), up axis %d, boosted ticks %llu",
                     (unsigned long long)(frames - last_frames), g_last_dt.load(), cars,
                     now - g_player_pos_tick.load() < 1000 ? "seen" : "not seen", g_player_pos[0], g_player_pos[1], g_player_pos[2],
                     g_player_cardynamics, g_diag_player_dist.load(), g_torque_base_seen.load(), g_torque_field.load(), g_player_speed.load(), g_diag_speed_field.load(), g_meas_speed.load(),
                     g_meas_accel_plain.load(), g_meas_accel_boost.load(), g_engine_accel.load(), g_push_scale.load(), g_up_axis.load(),
                     (unsigned long long)g_boosted_frames.load());
            last_frames = frames;
            g_api->log(g_mod, b);
            snprintf(b, sizeof b, "camera: view calls %u (shaken %u), fov calls (widened %u)", g_cam_view_calls.load(), g_cam_view_shaken.load(), g_cam_fov_widened.load());
            g_api->log(g_mod, b);
            snprintf(b, sizeof b, "flames: posts %u (flagged %u, player's %u, thread %lu), routine runs %u (flagged %u, thread %lu), "
                     "player presentation %p at +0x%X, tips known %d, mask %02X (forced %02X), nitro flames posted %u, release thread %lu, queue calls %u (from the player car %u), presentation %p, pres mask %02X",
                     g_fh3_post_calls.load(), g_fh3_post_flagged.load(), g_fh3_post_player.load(), (unsigned long)g_fh3_post_tid.load(),
                     g_fh3_routine_calls.load(), g_fh3_routine_flagged.load(), (unsigned long)g_fh3_routine_tid.load(),
                     g_fh3_player_deferred.load(), g_fh3_deferred_off.load(), (int)g_fh3_tips_ok.load(), g_fh3_mask.load(),
                     g_fh3_mask_forced.load(), g_fh3_driven.load(), (unsigned long)g_phys_tid.load(),
                     g_fh3_queue_calls.load(), g_fh3_queue_stack_hits.load(), g_fh3_pres.load(), g_fh3_mask_pres.load());
            g_api->log(g_mod, b);

        }
    }).detach();

    auto last_time = std::chrono::steady_clock::now();
    float active_duration = 0.0f;

    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60 Hz tick

        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - last_time).count();
        last_time = now;
        if (dt > 0.1f) dt = 0.1f; // Clamp delta time spike

        if (!g_enabled.load()) {
            if (g_nitro_active.load()) {
                g_nitro_active.store(false);
                set_rumble(0, 0);
                stop_nitro_audio();
            }
            g_current_boost.store(1.0f);
            continue;
        }

        // Poll all 4 controller ports for the nitro button (LB by default)
        bool button_pressed = false;
        for (DWORD user_idx = 0; user_idx < 4; ++user_idx) {
            XINPUT_STATE state{};
            if (xin::get_state && xin::get_state(user_idx, &state) == ERROR_SUCCESS) {
                if (nitro_button_down(state.Gamepad)) {
                    button_pressed = true;
                    break;
                }
            }
        }

        // Only run while the player is actually driving: car physics is simulating (not Pause Menu,
        // Map or Loading) and the HUD speedometer is ticking (not Garage or a car meet lobby).
        uint64_t now_ms = GetTickCount64();
        bool physics_active = (now_ms - g_last_physics_tick.load()) < 200;
        // FH3: no speedometer hook yet, so "driving" is the player's car being simulated
        bool driving = physics_active && (now_ms - g_player_pos_tick.load()) < 200;

        if (g_refund_on_rewind.load() || g_refill_on_start.load())
            nitro_hud_events(physics_active, (now_ms - g_last_speedo_tick.load()) < 200, now_ms);

        if (!driving) {
            // Nothing is simulated: no nitro, and the tank neither drains nor recharges.
            if (g_nitro_active.load()) {
                g_nitro_active.store(false);
                g_current_boost.store(1.0f);
                set_rumble(0, 0);
                stop_nitro_audio();
                active_duration = 0.0f;
            }
            continue;
        }

        if (g_up_report.exchange(0) == 1) {
            snprintf(log_buf, sizeof(log_buf), "Vertical axis learned: %c. Nitro now pushes horizontally and only on the ground.",
                     "XYZ"[g_up_axis.load()]);
            g_api->log(g_mod, log_buf);
        }

        int report = g_calib_report.exchange(0);
        if (report == 1) {
            snprintf(log_buf, sizeof(log_buf), "Player car located: position at CCarDynamics+0x%X (%d hits). Nitro now only boosts your car.",
                     g_pos_offset.load(), g_calib_best_hits.load());
            g_api->log(g_mod, log_buf);
        } else if (report == 2) {
            snprintf(log_buf, sizeof(log_buf), "Could not locate the player car yet (best offset only %d hits); retrying.",
                     g_calib_best_hits.load());
            g_api->log(g_mod, log_buf);
        }

        // The nitrous kit decides everything: no kit on the car being driven = no nitro, no gauge; the stage
        // gives the push. A change of stage (another car, a purchase) starts with a full tank.
        const int level = nu_level();
        static int prev_level = 0;
        if (level != prev_level) {
            prev_level = level;
            g_current_fuel.store(g_duration.load());
            g_depleted_lockout.store(false);
            snprintf(log_buf, sizeof(log_buf), "Nitrous kit stage on the driven car: %d", level);
            g_api->log(g_mod, log_buf);
        }
        if (level == 0) {
            button_pressed = false;
            g_current_fuel.store(g_duration.load());
            g_depleted_lockout.store(false);
        }

        float fuel = g_current_fuel.load();
        float max_duration = g_duration.load();
        float target_scale = nu_boost(level);
        float ramp_time = g_buildup_time.load();

        if (g_depleted_lockout.load()) {
            // Require releasing LB AND recharging at least 2.0s before clearing lockout
            if (!button_pressed && fuel > 0.0f) {
                g_depleted_lockout.store(false);
            }
        }

        if (button_pressed && !g_depleted_lockout.load() && fuel > 0.0f) {
            // Activating / Active
            if (!g_nitro_active.load()) {
                g_nitro_active.store(true);
                active_duration = 0.0f;
                start_nitro_audio();
                g_api->log(g_mod, "Fast & Furious Nitro engaged (LB pressed)!");
            }

            active_duration += dt;
            fuel = (std::max)(0.0f, fuel - dt);
            g_current_fuel.store(fuel);

            // Linear ramp-up from 1.0 to target_scale over buildup_time
            float ramp = (ramp_time > 0.001f) ? (std::min)(1.0f, active_duration / ramp_time) : 1.0f;
            float boost = 1.0f + (target_scale - 1.0f) * ramp;
            g_current_boost.store(boost);

            // Haptic rumble
            WORD rumble_strength = static_cast<WORD>(28000 * ramp);
            WORD high_freq = static_cast<WORD>(42000 * ramp);
            set_rumble(rumble_strength, high_freq);

            if (fuel <= 0.0f) {
                g_depleted_lockout.store(true);
                g_nitro_active.store(false);
                g_current_boost.store(1.0f);
                set_rumble(0, 0);
                stop_nitro_audio();
                g_api->log(g_mod, "Nitro tank depleted.");
            }
        } else {
            // Idle / Recharging
            if (g_nitro_active.load()) {
                g_nitro_active.store(false);
                g_current_boost.store(1.0f);
                set_rumble(0, 0);
                stop_nitro_audio();
                active_duration = 0.0f;
            }

            // Recharge over time
            float rate = g_recharge_rate.load();
            if (fuel < max_duration && rate > 0.0f) {
                fuel = (std::min)(max_duration, fuel + dt * rate);
                g_current_fuel.store(fuel);
            }
        }
    }
}

// On-screen debug overlay: a transparent, click-through window laid over the game window, drawn with GDI.
// Switched on from the Mods page. Needs the game in borderless or windowed mode, like ShowFPS.
static char g_overlay_lines[24][180];
static int g_overlay_count = 0;
static HWND g_overlay_hwnd = nullptr;

static void overlay_line(const char* fmt, ...) {
    if (g_overlay_count >= 24) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_overlay_lines[g_overlay_count], sizeof(g_overlay_lines[0]), fmt, ap);
    va_end(ap);
    ++g_overlay_count;
}

static void build_overlay_lines() {
    g_overlay_count = 0;
    uint64_t now = GetTickCount64();
    uint64_t phys_age = now - g_last_physics_tick.load();
    uint64_t speedo_age = now - g_last_speedo_tick.load();
    uint64_t pos_age = now - g_player_pos_tick.load();
    bool driving = phys_age < 200 && speedo_age < 200;
    int up = g_up_axis.load();
    int off = g_pos_offset.load();

    overlay_line("NitroSystem II debug");
    overlay_line("nitro: enabled=%d active=%d lockout=%d fuel=%.1f/%.0f boost=%.2f",
                 g_enabled.load() ? 1 : 0, g_nitro_active.load() ? 1 : 0, g_depleted_lockout.load() ? 1 : 0,
                 g_current_fuel.load(), g_duration.load(), g_current_boost.load());
    overlay_line("nitrous kit: stage=%d boost=%.1fx garage id=%u", nu_level(), nu_boost(nu_level()), (unsigned)nu_active_garage);
    overlay_line("driving=%d  physics age=%llums  speedometer age=%llums", driving ? 1 : 0,
                 (unsigned long long)phys_age, (unsigned long long)speedo_age);
    if (off >= 0) {
        overlay_line("player car: position offset=+0x%X  up axis=%c  position age=%llums", off,
                     up >= 0 ? "XYZ"[up] : '?', (unsigned long long)pos_age);
    } else {
        overlay_line("player car: not located yet (drive for a few seconds)  up axis=%c", up >= 0 ? "XYZ"[up] : '?');
    }
    overlay_line("exhaust tips: %s  (M=flagged by game events, -=not) | events seen=%u mask=%02X (needs one real backfire event)",
                 g_tip_diag[0] ? g_tip_diag : "(none yet)", g_real_events.load(), g_real_mask.load() & 0xFF);
    overlay_line("flames: calls=%u matched=%u tips=%d | routine ran=%u | driven=%u",
                 g_bf_calls.load(), g_bf_matched.load(), g_bf_tips.load(), g_bf_total.load(), g_bf_driven.load());
    DWORD ft = g_frame_tid.load(), bt = g_bf_tid.load();
    overlay_line("threads: game frame=%lu backfire=%lu %s | physics=%lu", (unsigned long)ft, (unsigned long)bt,
                 (ft != 0 && ft == bt) ? "(same: flame driver runs)" : "(different: flame driver blocked)",
                 (unsigned long)g_phys_tid.load());
}

static LRESULT CALLBACK overlay_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);

        HBRUSH key = CreateSolidBrush(RGB(255, 0, 255));      // colour-keyed: becomes see-through
        FillRect(dc, &rc, key);
        DeleteObject(key);

        const int line_h = 17;
        int box_h = g_overlay_count * line_h + 10;
        RECT box = {0, 0, rc.right, box_h};
        HBRUSH bg = CreateSolidBrush(RGB(14, 14, 14));
        FillRect(dc, &box, bg);
        DeleteObject(bg);

        HFONT font = CreateFontA(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
        HGDIOBJ old = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(90, 255, 130));
        for (int i = 0; i < g_overlay_count; ++i) {
            TextOutA(dc, 6, 5 + i * line_h, g_overlay_lines[i], (int)strlen(g_overlay_lines[i]));
        }
        SelectObject(dc, old);
        DeleteObject(font);
        EndPaint(hwnd, &ps);
        return 0;
    }
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

struct FindGame {
    DWORD pid;
    HWND best;
    long best_area;
};

static BOOL CALLBACK find_game_window(HWND hwnd, LPARAM lp) {
    FindGame* f = (FindGame*)lp;
    if (hwnd == g_overlay_hwnd || !IsWindowVisible(hwnd)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != f->pid) return TRUE;
    if (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
    RECT rc;
    GetClientRect(hwnd, &rc);
    long area = (rc.right - rc.left) * (rc.bottom - rc.top);
    if (area > f->best_area) {
        f->best_area = area;
        f->best = hwnd;
    }
    return TRUE;
}

static void debug_overlay_thread() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = overlay_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"NitroSystemIIDebugOverlay";
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    if (!RegisterClassExW(&wc)) return;

    HWND hw = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                              wc.lpszClassName, L"NitroSystem II debug", WS_POPUP, 0, 0, 100, 100,
                              nullptr, nullptr, wc.hInstance, nullptr);
    if (!hw) return;
    g_overlay_hwnd = hw;
    SetLayeredWindowAttributes(hw, RGB(255, 0, 255), 0, LWA_COLORKEY);

    bool shown = false;
    for (;;) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        if (!g_debug.load()) {
            if (shown) { ShowWindow(hw, SW_HIDE); shown = false; }
            continue;
        }

        FindGame f{GetCurrentProcessId(), nullptr, 0};
        EnumWindows(find_game_window, (LPARAM)&f);
        static HWND logged_for = (HWND)1;
        if (f.best != logged_for && g_api && g_mod) {
            logged_for = f.best;
            char b[200];
            if (f.best) {
                RECT r0;
                GetClientRect(f.best, &r0);
                snprintf(b, sizeof(b), "debug overlay: game window found (%ldx%ld client)", r0.right, r0.bottom);
            } else {
                snprintf(b, sizeof(b), "debug overlay: no game window found, nothing to draw on");
            }
            g_api->log(g_mod, b);
        }
        if (!f.best) continue;
        RECT cr;
        GetClientRect(f.best, &cr);
        POINT origin = {0, 0};
        ClientToScreen(f.best, &origin);

        build_overlay_lines();
        int h = g_overlay_count * 17 + 10;
        MoveWindow(hw, origin.x + 12, origin.y + 12, 760, h, TRUE);
        SetWindowPos(hw, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        shown = true;
        InvalidateRect(hw, nullptr, TRUE);
    }
}

// The tank as 0-100. Exported so other mods in the process (VirtualDashboard) can read it.
static int nitro_percent() {
    float duration = g_duration.load();
    float pct = (duration > 0.001f) ? g_current_fuel.load() / duration * 100.0f : 0.0f;
    return static_cast<int>(std::lround((std::min)(100.0f, (std::max)(0.0f, pct))));
}

// the export keeps its old name so anything that already calls it keeps working
extern "C" __declspec(dllexport) int FnFNitro_GetNitroPercent() { return nitro_percent(); }

FH3MOD_EXPORT int fh3mod_init(const FH3ModApi* api) {
    g_api = api;

    FH3ModInfo info{};
    info.size             = sizeof(info);
    info.name             = "NitroSystem II";
    info.version          = "2.0.0-fh3.49";
    info.github_repo      = nullptr;   // the FH3 port has no releases yet
    info.platform_version = FH3MOD_PLATFORM_VERSION;
    info.check_updates    = 0;
    info.description      = "Fast & Furious Nitro, ported from Forza Horizon 2. Hold LB to boost. Early FH3 build: exhaust flames in testing, no garage kit or gauge yet.";
    info.offline_only     = 1;
    
    g_mod = api->register_mod(&info);
    if (!g_mod) return 1;

    api->log(g_mod, "NitroSystem II initializing (nitrous kit upgrade + LB to boost)...");

    // Initialize paths & load persistent settings from NitroSystemII.ini
    init_paths();
    load_config();
    if (api->api_version >= 2 && api->add_sql) {   // the Nitrous Kit rows (Engine > Custom upgrade)
        if (nu_cfg.price[1] == 0) { nu_cfg.price[1] = 15000; nu_cfg.price[2] = 30000; nu_cfg.price[3] = 60000; }
        std::string sql;
        fh3_kit_sql(sql);
        api->add_sql(g_mod, sql.c_str());
    }
    // The garage upgrade (the Nitrous Kit part). Without it the mod has no way to know a car owns nitrous.
    // FH3: the garage upgrade isn't ported yet. Every car gets the stage in [FH3] Stage (1-3, default 2), with the
    // boosts from [Upgrade] as before.
    nu_load_config(g_ini_path, (int)(g_duration.load() + 0.5f));
    {
        WritePrivateProfileStringW(L"FH3", L"Stage", nullptr, g_ini_path);
        int stage = (int)GetPrivateProfileIntW(L"FH3", L"ForceStage", 0, g_ini_path);
        if (stage < 0 || stage > 3) stage = 0;
        wchar_t fv[4] = {(wchar_t)(L'0' + stage), 0};
        WritePrivateProfileStringW(L"FH3", L"ForceStage", fv, g_ini_path);
        g_force_stage = stage;
        if (stage) nu_cur_level = stage;
        else { long lg = (long)GetPrivateProfileIntW(L"FH3", L"LastGarage", 0, g_ini_path); if (lg > 0) nu_activate((unsigned)lg); }
        wchar_t buf[32] = {};
        // [FH3] PowerScale: the extra torque is (boost - 1) x PowerScale, so the default 2 gives +40 / +80 / +120%
        // torque on stages 1 / 2 / 3 (1 would be the bare 1.2 / 1.4 / 1.6x; tested 1 Nov: 1 felt weak). (PushScale was the old, constant-push setting.)
        WritePrivateProfileStringW(L"FH3", L"PushScale", nullptr, g_ini_path);
        GetPrivateProfileStringW(L"FH3", L"PowerScale", L"", buf, 32, g_ini_path);
        float scale = buf[0] ? (float)_wtof(buf) : 2.0f;
        if (!(scale > 0.0f && scale <= 5.0f)) scale = 2.0f;
        if (!buf[0]) WritePrivateProfileStringW(L"FH3", L"PowerScale", L"2.0", g_ini_path);
        g_push_scale.store(scale);
        // which per-car torque scale nitro multiplies: 0x1198 (the engine's output, default) or 0x10CC
        int field = (int)GetPrivateProfileIntW(L"FH3", L"TorqueField", 0x1198, g_ini_path);
        if (field != 0x1198 && field != 0x10CC) field = 0x1198;
        g_torque_field.store(field);
        // [FH3] FlameTips: 0 (default) fires the exhaust tips the game has flagged for the car in its own backfires;
        // a bit mask (1 = first tip, 3 = first two...) forces those tips, for testing.
        g_fh3_mask_forced.store((uint32_t)GetPrivateProfileIntW(L"FH3", L"FlameTips", 0, g_ini_path) & 0xFF);
        g_tipsrc_kind.store((int)GetPrivateProfileIntW(L"FH3", L"TipSrcKind", 0, g_ini_path));
        g_tipsrc_a.store((int)GetPrivateProfileIntW(L"FH3", L"TipSrcA", 0, g_ini_path));
        g_tipsrc_b.store((int)GetPrivateProfileIntW(L"FH3", L"TipSrcB", 0, g_ini_path));
        char msg[96];
        snprintf(msg, sizeof msg, "FH3 build: Nitrous Kit stage %d (ForceStage=%d), boost %.1fx", nu_level(), g_force_stage, nu_boost(nu_level()));
        api->log(g_mod, msg);
    }

    // Register In-Game Settings (Pause > Settings > MODS) with loaded values
    if (api->api_version >= 4) {
        FH3Setting s_enable{};
        s_enable.size        = sizeof(s_enable);
        s_enable.key         = "NitroEnabled";
        s_enable.label       = "Nitro System";
        s_enable.description = "Enable or disable Fast & Furious Nitro. Needs the Nitrous Kit (Garage > Upgrades > Custom upgrade > Engine) on the car.";
        s_enable.value       = g_enabled.load() ? 1 : 0;
        s_enable.on_change   = on_setting_changed;
        api->add_setting(g_mod, &s_enable);

        FH3Setting s_debug{};
        s_debug.size        = sizeof(s_debug);
        s_debug.key         = "DebugOverlay";
        s_debug.label       = "Nitro Debug Overlay";
        s_debug.description = "Draws nitro and flame debug info over the game (needs borderless/windowed mode).";
        s_debug.value       = g_debug.load() ? 1 : 0;
        s_debug.on_change   = on_setting_changed;
        api->add_setting(g_mod, &s_debug);

        FH3Setting s_backfire{};
        s_backfire.size          = sizeof(s_backfire);
        s_backfire.key           = "BackfireOnAllCars";
        s_backfire.label         = "Backfire on All Cars";
        s_backfire.description   = "Lets every car backfire, which the nitro exhaust flames need. Off leaves the game's own backfire settings alone. Takes effect after a restart.";
        s_backfire.value         = g_backfire_all_cars.load() ? 1 : 0;
        s_backfire.needs_restart = 1;
        s_backfire.on_change     = on_setting_changed;
        api->add_setting(g_mod, &s_backfire);

        FH3Setting s_refill{};
        s_refill.size        = sizeof(s_refill);
        s_refill.key         = "RefillOnRaceStart";
        s_refill.label       = "Refill Nitro on Race Start";
        s_refill.description = "Fills the nitro tank when a race starts, you quit one, you change car or you fast travel.";
        s_refill.value       = g_refill_on_start.load() ? 1 : 0;
        s_refill.on_change   = on_setting_changed;
        api->add_setting(g_mod, &s_refill);

        FH3Setting s_rewind{};
        s_rewind.size        = sizeof(s_rewind);
        s_rewind.key         = "RefundOnRewind";
        s_rewind.label       = "Refund Nitro on Rewind";
        s_rewind.description = "Rewinding gives back the nitro you used since the point you rewind to.";
        s_rewind.value       = g_refund_on_rewind.load() ? 1 : 0;
        s_rewind.on_change   = on_setting_changed;
        api->add_setting(g_mod, &s_rewind);

        FH3Setting s_rumble{};
        s_rumble.size        = sizeof(s_rumble);
        s_rumble.key         = "Rumble";
        s_rumble.label       = "Nitro Controller Rumble";
        s_rumble.description = "Rumbles the controller while Nitro is active. The game's own rumble isn't affected.";
        s_rumble.value       = g_rumble.load() ? 1 : 0;
        s_rumble.on_change   = on_setting_changed;
        api->add_setting(g_mod, &s_rumble);

        FH3Setting s_hide{};
        s_hide.size          = sizeof(s_hide);
        s_hide.key           = "HudHide";
        s_hide.label         = "Hide Nitro Gauge Without Kit (after a restart)";
        s_hide.description   = "On (default): the NOS gauge frame is hidden on cars without a Nitrous Kit. Off: an empty frame stays.";
        s_hide.value         = g_hud_hide.load() ? 1 : 0;
        s_hide.on_change     = on_setting_changed;
        api->add_setting(g_mod, &s_hide);

        if (api->api_version >= 5) {
            // Sliders can only show numbers, so the button is picked by number (listed in the description).
            FH3Setting s_button{};
            s_button.size          = sizeof(s_button);
            s_button.key           = "NitroButton";
            s_button.label         = "Nitro Button";
            s_button.description   = "1 LB, 2 RB, 3 A, 4 B, 5 X, 6 Y, 7 LS click, 8 RS click, 9 D-Pad Up, 10 D-Pad Down, "
                                     "11 D-Pad Left, 12 D-Pad Right, 13 Back, 14 Start, 15 LT, 16 RT. Pick one the game "
                                     "doesn't use while driving. Applies right away.";
            s_button.kind          = FH3_SETTING_SLIDER;
            s_button.min           = 1.0f;
            s_button.max           = (float)(sizeof(k_buttons) / sizeof(k_buttons[0]));
            s_button.step          = 1.0f;
            s_button.slider_value  = (float)(button_index() + 1);
            s_button.display_scale = 1.0f;
            s_button.decimals      = 0;
            s_button.on_slide      = on_slider_changed;
            api->add_setting(g_mod, &s_button);

            FH3Setting s_hud{};
            s_hud.size          = sizeof(s_hud);
            s_hud.key           = "HudTheme";
            s_hud.label         = "HUD Theme";
            s_hud.description   = "1 Original (yellow NOS bar), 2 Fast & Furious (cyan NOS bar, gears and assist icons).";
            s_hud.kind          = FH3_SETTING_SLIDER;
            s_hud.min           = 1.0f;
            s_hud.max           = 2.0f;
            s_hud.step          = 1.0f;
            s_hud.slider_value  = g_hud_theme.load() ? 2.0f : 1.0f;
            s_hud.display_scale = 1.0f;
            s_hud.decimals      = 0;
            s_hud.needs_restart = 1;
            s_hud.on_slide      = on_slider_changed;
            api->add_setting(g_mod, &s_hud);

            FH3Setting s_dur{};
            s_dur.size          = sizeof(s_dur);
            s_dur.key           = "Duration";
            s_dur.label         = "Nitro Tank Capacity (sec)";
            s_dur.description   = "Total seconds of continuous boost available in a full tank (5 s by default).";
            s_dur.kind          = FH3_SETTING_SLIDER;
            s_dur.min           = 5.0f;
            s_dur.max           = 60.0f;
            s_dur.step          = 1.0f;
            s_dur.slider_value  = g_duration.load();
            s_dur.display_scale = 1.0f;
            s_dur.decimals      = 0;
            s_dur.on_slide      = on_slider_changed;
            api->add_setting(g_mod, &s_dur);

            FH3Setting s_buildup{};
            s_buildup.size          = sizeof(s_buildup);
            s_buildup.key           = "BuildupTime";
            s_buildup.label         = "Nitro Buildup Time (sec)";
            s_buildup.description   = "Time taken to ramp up to full boost torque (Spire default: 0.5s).";
            s_buildup.kind          = FH3_SETTING_SLIDER;
            s_buildup.min           = 0.1f;
            s_buildup.max           = 2.0f;
            s_buildup.step          = 0.1f;
            s_buildup.slider_value  = g_buildup_time.load();
            s_buildup.display_scale = 1.0f;
            s_buildup.decimals      = 1;
            s_buildup.on_slide      = on_slider_changed;
            api->add_setting(g_mod, &s_buildup);

            FH3Setting s_recharge{};
            s_recharge.size          = sizeof(s_recharge);
            s_recharge.key           = "RechargeRate";
            s_recharge.label         = "Nitro Recharge Rate";
            s_recharge.description   = "Seconds of nitro restored per second while not boosting.";
            s_recharge.kind          = FH3_SETTING_SLIDER;
            s_recharge.min           = 0.0f;
            s_recharge.max           = 1.0f;
            s_recharge.step          = 0.1f;
            s_recharge.slider_value  = g_recharge_rate.load();
            s_recharge.display_scale = 1.0f;
            s_recharge.decimals      = 1;
            s_recharge.on_slide      = on_slider_changed;
            api->add_setting(g_mod, &s_recharge);

            FH3Setting s_volume{};
            s_volume.size          = sizeof(s_volume);
            s_volume.key           = "SoundVolume";
            s_volume.label         = "Nitro Sound Volume";
            s_volume.description   = "Volume of the boost and stop sounds. The game's own volume sliders don't change them.";
            s_volume.kind          = FH3_SETTING_SLIDER;
            s_volume.min           = 0.0f;
            s_volume.max           = 1.0f;
            s_volume.step          = 0.05f;
            s_volume.slider_value  = g_sound_volume.load();
            s_volume.display_scale = 100.0f;
            s_volume.decimals      = 0;
            s_volume.on_slide      = on_slider_changed;
            api->add_setting(g_mod, &s_volume);

            FH3Setting s_shake{};
            s_shake.size          = sizeof(s_shake);
            s_shake.key           = "CameraShake";
            s_shake.label         = "Nitro Camera Shake";
            s_shake.description   = "How much the camera shakes while Nitro is active, in the driver, hood, bumper and chase views. 0 turns it off.";
            s_shake.kind          = FH3_SETTING_SLIDER;
            s_shake.min           = 0.0f;
            s_shake.max           = 1.0f;
            s_shake.step          = 0.05f;
            s_shake.slider_value  = g_camera_shake.load();
            s_shake.display_scale = 100.0f;
            s_shake.decimals      = 0;
            s_shake.on_slide      = on_slider_changed;
            api->add_setting(g_mod, &s_shake);

            FH3Setting s_fov{};
            s_fov.size          = sizeof(s_fov);
            s_fov.key           = "FovBoost";
            s_fov.label         = "Nitro Wider FOV";
            s_fov.description   = "How much wider the field of view gets while Nitro is active, in the driver, hood, bumper and chase views. 0 turns it off.";
            s_fov.kind          = FH3_SETTING_SLIDER;
            s_fov.min           = 0.0f;
            s_fov.max           = 0.3f;
            s_fov.step          = 0.01f;
            s_fov.slider_value  = g_fov_boost.load();
            s_fov.display_scale = 100.0f;
            s_fov.decimals      = 0;
            s_fov.on_slide      = on_slider_changed;
            api->add_setting(g_mod, &s_fov);

            FH3Setting s_vig_on{};
            s_vig_on.size          = sizeof(s_vig_on);
            s_vig_on.key           = "VignetteEnabled";
            s_vig_on.label         = "Nitro Blue Vignette";
            s_vig_on.description   = "A pulsing blue vignette at the screen edges while Nitro is active.";
            s_vig_on.value         = g_vig_on.load() ? 1 : 0;
            s_vig_on.on_change     = on_setting_changed;
            api->add_setting(g_mod, &s_vig_on);

            FH3Setting s_sl_on{};
            s_sl_on.size          = sizeof(s_sl_on);
            s_sl_on.key           = "SpeedLinesEnabled";
            s_sl_on.label         = "Nitro Speed Lines";
            s_sl_on.description   = "Thin blue streaks flying out toward the screen edges while Nitro is active, in step with the car's speed, like in Need for Speed Unbound.";
            s_sl_on.value         = g_sl_on.load() ? 1 : 0;
            s_sl_on.on_change     = on_setting_changed;
            api->add_setting(g_mod, &s_sl_on);

        }
    }

    // Ensure all cars support exhaust backfire flames in gamedbRC (BackfireOnAllCars=0 in the ini turns this off;
    // the nitro flames then only work on cars the game already lets backfire)
    if (g_backfire_all_cars.load() && api->api_version >= 2 && api->add_sql) {
        api->add_sql(g_mod, "UPDATE BackfireLevels SET BackfireLevel = 3;");
    }

    // Launch background worker thread
    std::thread(nitro_worker_thread).detach();
    std::thread(debug_overlay_thread).detach();

    api->log(g_mod, "Fast & Furious NOS initialized successfully.");
    return 0;
}
