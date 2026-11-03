// UntilDawnTweaks (formerly UntilDawnRT): a ReShade add-on for Until Dawn (2024 remake, UE 5,
// DirectX 12). It makes the game's ray tracing work (as shipped most lights opt out of RT
// shadows, and RT reflections only show in a few marked areas on mirror-like surfaces), raises
// detail beyond the menu (mostly the game's own unused Cinematic tier), and replaces the HDR
// brightness sliders that do nothing once the HDR black screen is fixed (r.UCP.HDR=0 in
// Engine.ini). See docs/FINDINGS.md.
//
// 1.2.0 moved it onto the HellbladeIITweaks 1.0.0 framework: everything goes through the game's
// console variables (cvars.hpp), set on the game thread from the console manager's once-per-frame
// CallAllConsoleVariableSinks (GameTick), so values reach the renderer between frames like the
// console's own; letting go puts the game's value and priority back. Options are combos kept in
// ReShade.ini [UntilDawnTweaks]; quality presets (Game / Ultra+ / Cinematic) with in-game cost
// measuring; F8 switches all of it off and on.
//
// The ray tracing fix and HDR brightness are in no preset: they fix the game rather than trade
// frame rate for quality, so a preset (Game included) never turns them off.
//
// Threads: ReShade's present thread draws the overlay, measures frame times and only changes
// options (g_state). The game thread does all setting, the HDR brightness fields and the probe:
// it only tries g_state, to copy what's wanted, and skips a frame if the present thread holds it.
// It never holds g_state while it sets anything. What the overlay shows of the game thread's
// state comes from a copy it publishes under g_viewMutex (also never held across a Set).
//
// The add-on never patches game code. It writes UntilDawnTweaks.log (and -probe.txt) next to the
// game executable.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "cvars.hpp"
#include "version.h"

extern "C" __declspec(dllexport) const char* NAME = DT_PRODUCT;
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Makes " DT_GAME "'s ray tracing work, raises detail beyond its menu and sets its HDR brightness, live from "
    "the overlay.";

namespace {

using namespace reshade::api;

constexpr int kToggleHotkey = VK_F8;

const uint8_t* GameBase() { return reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr)); }

uint32_t PeTimestamp(const uint8_t* base) {
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    return reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew)->FileHeader.TimeDateStamp;
}

bool KnownBuild() { return PeTimestamp(GameBase()) == DT_KNOWN_PE_TIMESTAMP; }

// ---------------------------------------------------------------- logging

std::mutex g_logMutex;
FILE* g_log = nullptr;
ULONGLONG g_loadTick = 0;
std::wstring g_exeDir;

void Log(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02u:%02u:%02u.%03u [+%5.1f s] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
            (GetTickCount64() - g_loadTick) / 1000.0, buf);
    fflush(g_log);
}

// ReShade can load the add-on more than once per process (the game makes test devices at
// start), so the log is started fresh only by the first load in a process (marked in an
// environment variable).
void OpenLog() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    g_exeDir.assign(path, n);
    g_exeDir.resize(g_exeDir.find_last_of(L"\\/") + 1);
    wchar_t pid[32] = {}, mine[32];
    swprintf(mine, 32, L"%lu", GetCurrentProcessId());
    GetEnvironmentVariableW(L"UNTILDAWNTWEAKS_PID", pid, 32);
    const bool again = !wcscmp(pid, mine);
    SetEnvironmentVariableW(L"UNTILDAWNTWEAKS_PID", mine);
    g_log = _wfopen((g_exeDir + DT_PRODUCT_W L".log").c_str(), again ? L"a" : L"w");
    Log(DT_PRODUCT " " DT_VERSION " loaded%s", again ? " (again in this process)" : "");
}

// Cvar names and values are ASCII.
std::string Narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += static_cast<char>(c);
    return s;
}

// ---------------------------------------------------------------- video memory

// The texture streaming pool follows the card's memory, about a quarter of it: ray tracing at
// 4K and the game's other buffers need the rest. (The game uses 950 MB, which at 4K makes
// textures stream at lower resolution.)
struct VramRow {
    int gb;      // the smallest card this row is for
    int poolMb;  // r.Streaming.PoolSize
    const char* card;
};
const VramRow kVram[] = {
    {0, 1024, "6 GB or less"}, {8, 2048, "8 GB"},   {10, 2560, "10 GB"},
    {12, 3072, "12 GB"},       {16, 4096, "16 GB"}, {20, 6144, "20 GB or more"},
};
constexpr int kVramFallback = 1;  // 8 GB, if the card's memory can't be read

std::atomic<uint64_t> g_vramMb{0};  // dedicated memory of the game's adapter, from DXGI (0: unknown)

int AutoVramRow() {
    const uint64_t mb = g_vramMb.load();
    if (!mb) return kVramFallback;
    const int gb = static_cast<int>((mb + 512) / 1024);  // a "16 GB" card reports a little under
    int row = 0;
    for (int i = 0; i < static_cast<int>(std::size(kVram)); ++i)
        if (gb >= kVram[i].gb) row = i;
    return row;
}

// Dedicated memory of the adapter a D3D12 device runs on, in MB (0 if unknown). DXGI is loaded
// at run time: in the game's folder "dxgi.dll" is ReShade, which forwards it.
uint64_t AdapterMemoryMb(ID3D12Device* d3d) {
    using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
    HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
    auto create = dxgi ? reinterpret_cast<CreateFactoryFn>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
    if (!create) return 0;
    uint64_t mb = 0;
    IDXGIFactory4* factory = nullptr;
    if (SUCCEEDED(create(IID_PPV_ARGS(&factory)))) {
        IDXGIAdapter1* adapter = nullptr;
        if (SUCCEEDED(factory->EnumAdapterByLuid(d3d->GetAdapterLuid(), IID_PPV_ARGS(&adapter)))) {
            DXGI_ADAPTER_DESC1 desc = {};
            if (SUCCEEDED(adapter->GetDesc1(&desc))) mb = desc.DedicatedVideoMemory >> 20;
            adapter->Release();
        }
        factory->Release();
    }
    return mb;
}

// ---------------------------------------------------------------- options
//
// Each option is a combo; choice id 0 leaves the game's value. The chosen id is kept in
// ReShade.ini. The Game choice shows the game's current value (the probe writes them all).

using CvarList = std::vector<std::pair<const wchar_t*, const wchar_t*>>;

struct Choice {
    const char* label;
    int id;
    CvarList cvars;
};

struct Option {
    const char* tab;
    const char* section;
    const char* key;       // ReShade.ini
    const char* label;
    const char* tip;
    const wchar_t* shown;  // cvar whose game value the "Game" choice shows (or null)
    std::vector<Choice> choices;
    int value = 0;
};

constexpr const char* kRayTracingTab = "Ray tracing";
constexpr const char* kDetailTab = "Detail";
constexpr const char* kCameraTab = "Camera";  // taste: in no preset, not measured
constexpr const char* kHdrTab = "HDR";
constexpr const char* kHdrKey = "HdrBrightnessOverride";  // the 1.x key, kept
constexpr int kPoolAuto = 1;                               // TextureMemory: from the card's memory

// Values from the 1.0/1.1 analysis (docs/FINDINGS.md): the game's scalability presets, read
// live, and its unused Cinematic tier (DefaultScalability.ini "@Cine"). Skin (SSS) is already at
// its maximum on the top shading preset; sharpening, film grain, motion blur and chromatic
// aberration are the game's own menu options; so none of those is here.
std::vector<Option> g_options = {
    // ------------------------------------------------ ray tracing (the fix; in no preset)
    {kRayTracingTab, "Shadows", "RtShadowLights", "Ray traced shadows",
     "Which lights cast ray traced shadows. As shipped most lights opt out\n"
     "(r.RayTracing.Enable.BMEnhancedMode 0), so the game's RT shadows barely show.\n"
     "Enhanced lights: also the lights marked for the enhanced mode.\n"
     "All lights: every light. The most visible change, and the most costly.",
     L"r.RayTracing.Enable.BMEnhancedMode",
     {{"Game (few lights)", 0, {}},
      {"Enhanced lights", 1, {{L"r.RayTracing.Enable.BMEnhancedMode", L"1"}}},
      {"All lights", 2, {{L"r.RayTracing.Enable.BMEnhancedMode", L"2"}}}}},
    {kRayTracingTab, "Reflections", "RtReflections", "Ray traced reflections",
     "The game switches RT reflections on every frame only inside areas its designers\n"
     "marked. Everywhere keeps them on. Follows the game's RT Reflections menu option.",
     L"r.RayTracing.Reflections",
     {{"Game (marked areas)", 0, {}}, {"Everywhere", 1, {{L"r.RayTracing.Reflections", L"1"}}}}},
    {kRayTracingTab, "Reflections", "RtRoughness", "Reflection roughness",
     "How rough a surface can be and still get ray traced reflections; rougher ones keep\n"
     "screen space reflections. The game uses 0.01 (mirrors only). Higher costs more and\n"
     "can look noisier.",
     L"r.RayTracing.Reflections.MaxRoughness",
     {{"Game", 0, {}},
      {"0.2", 2, {{L"r.RayTracing.Reflections.MaxRoughness", L"0.2"}}},
      {"0.4", 4, {{L"r.RayTracing.Reflections.MaxRoughness", L"0.4"}}},
      {"0.6", 6, {{L"r.RayTracing.Reflections.MaxRoughness", L"0.6"}}},
      {"All (1.0)", 10, {{L"r.RayTracing.Reflections.MaxRoughness", L"1"}}}}},
    {kRayTracingTab, "Noise", "RtQuality", "Ray tracing quality",
     "Less noise in RT shadows, reflections and ambient occlusion. The game traces 1 ray\n"
     "per pixel and leans on its denoisers. High and Ultra trace 2 and 4 (the cost of those\n"
     "effects goes up about as much) and let the shadow and reflection denoisers take 16\n"
     "samples instead of 8.",
     L"r.RayTracing.Shadows.SamplesPerPixel",
     {{"Game (1 ray)", 0, {}},
      {"High (2 rays)", 1,
       {{L"r.RayTracing.Shadows.SamplesPerPixel", L"2"},
        {L"r.RayTracing.Reflections.SamplesPerPixel", L"2"},
        {L"r.RayTracing.AmbientOcclusion.SamplesPerPixel", L"2"},
        {L"r.Shadow.Denoiser.ReconstructionSamples", L"16"},
        {L"r.Reflections.Denoiser.ReconstructionSamples", L"16"}}},
      {"Ultra (4 rays)", 2,
       {{L"r.RayTracing.Shadows.SamplesPerPixel", L"4"},
        {L"r.RayTracing.Reflections.SamplesPerPixel", L"4"},
        {L"r.RayTracing.AmbientOcclusion.SamplesPerPixel", L"4"},
        {L"r.Shadow.Denoiser.ReconstructionSamples", L"16"},
        {L"r.Reflections.Denoiser.ReconstructionSamples", L"16"}}}}},

    // ------------------------------------------------ detail
    {kDetailTab, "Textures", "TextureMemory", "Texture memory",
     "Video memory for streamed textures. The game uses 950 MB, which at 4K makes textures\n"
     "stream at lower resolution and look soft. Auto gives them about a quarter of your\n"
     "graphics card's memory; or pick your card's size.",
     L"r.Streaming.PoolSize",
     {{"Game", 0, {}},
      {"Auto", kPoolAuto, {}},
      {"1 GB (6 GB card or less)", 1024, {{L"r.Streaming.PoolSize", L"1024"}}},
      {"2 GB (8 GB card)", 2048, {{L"r.Streaming.PoolSize", L"2048"}}},
      {"2.5 GB (10 GB card)", 2560, {{L"r.Streaming.PoolSize", L"2560"}}},
      {"3 GB (12 GB card)", 3072, {{L"r.Streaming.PoolSize", L"3072"}}},
      {"4 GB (16 GB card)", 4096, {{L"r.Streaming.PoolSize", L"4096"}}},
      {"6 GB (20 GB card or more)", 6144, {{L"r.Streaming.PoolSize", L"6144"}}}}},
    {kDetailTab, "Textures", "Anisotropy", "Texture filtering",
     "Sharper floors and walls at shallow angles; the game uses 8x. Nearly free.",
     L"r.MaxAnisotropy",
     {{"Game", 0, {}}, {"16x", 16, {{L"r.MaxAnisotropy", L"16"}}}}},
    {kDetailTab, "Shadows", "ShadowDetail", "Shadow sharpness",
     "Virtual shadow maps of the sun, moon, lamps and flashlights.\n"
     "Sharp: one resolution step higher, with the Cinematic tier's 8192 shadow pages so\n"
     "they don't run out (blur or flicker in patches).\n"
     "Cinematic: the game's own Cinematic shadows (sun -1.5, 16 soft-shadow rays with 8\n"
     "samples): smoother soft edges. Costs the most.",
     L"r.Shadow.Virtual.ResolutionLodBiasDirectional",
     {{"Game", 0, {}},
      {"Sharp", 1,
       {{L"r.Shadow.Virtual.ResolutionLodBiasDirectional", L"-1"},
        {L"r.Shadow.Virtual.ResolutionLodBiasLocal", L"-1"},
        {L"r.Shadow.Virtual.MaxPhysicalPages", L"8192"}}},
      {"Cinematic", 2,
       {{L"r.Shadow.Virtual.ResolutionLodBiasDirectional", L"-1.5"},
        {L"r.Shadow.Virtual.ResolutionLodBiasDirectionalMoving", L"-1.5"},
        {L"r.Shadow.Virtual.ResolutionLodBiasLocal", L"-1"},
        {L"r.Shadow.Virtual.MaxPhysicalPages", L"8192"},
        {L"r.Shadow.Virtual.SMRT.RayCountDirectional", L"16"},
        {L"r.Shadow.Virtual.SMRT.SamplesPerRayDirectional", L"8"},
        {L"r.Shadow.Virtual.SMRT.RayCountLocal", L"16"},
        {L"r.Shadow.Virtual.SMRT.SamplesPerRayLocal", L"8"}}}}},
    {kDetailTab, "Fog", "FogDetail", "Volumetric fog",
     "Detail of the volumetric fog and the light shafts through it (flashlights, lamps):\n"
     "screen pixels per fog cell (game 8 px, 128 depth slices). Smaller cells give smoother\n"
     "fog and beams that shimmer less. Ultra is about 4x the game's fog work.",
     L"r.VolumetricFog.GridPixelSize",
     {{"Game", 0, {}},
      {"High (6 px)", 6, {{L"r.VolumetricFog.GridPixelSize", L"6"}}},
      {"Ultra (4 px)", 4, {{L"r.VolumetricFog.GridPixelSize", L"4"}, {L"r.VolumetricFog.GridSizeZ", L"192"}}}}},
    {kDetailTab, "Distance", "ViewDistance", "View distance",
     "The game's own Cinematic view distance: objects drawn twice as far, detailed meshes\n"
     "kept longer (LOD distance 0.7) and small foliage drawn further. Less pop-in.",
     L"r.ViewDistanceScale",
     {{"Game", 0, {}},
      {"Cinematic", 1,
       {{L"r.ViewDistanceScale", L"2"}, {L"r.StaticMeshLODDistanceScale", L"0.7"},
        {L"foliage.MinimumScreenSize", L"0.0001"}}}}},
    {kDetailTab, "Effects", "CinematicEffects", "Cinematic effects",
     "The game's own Cinematic quality for depth of field (cutscenes), screen space\n"
     "reflections, and the lighting on particles, smoke, glass and hair.",
     L"r.DepthOfFieldQuality",
     {{"Game", 0, {}},
      {"Cinematic", 1,
       {{L"r.DepthOfFieldQuality", L"4"}, {L"r.SSR.Quality", L"4"}, {L"r.TranslucencyLightingVolumeDim", L"128"}}}}},

    // ------------------------------------------------ camera (taste; in no preset, not measured)
    // Chromatic aberration, film grain, motion blur and sharpening are in the game's own menu, and
    // the game already holds r.BloomQuality at 0 (probe 2026-11-02), so they aren't here. Listed
    // after Detail so that depth of field Off wins over Cinematic effects' r.DepthOfFieldQuality.
    {kCameraTab, "Camera effects", "DepthOfField", "Depth of field",
     "Background and foreground blur around the focus point, mostly in cutscenes. Off keeps\n"
     "the whole picture sharp (and overrides Cinematic effects' depth of field).",
     L"r.DepthOfFieldQuality",
     {{"Game", 0, {}}, {"Off", 1, {{L"r.DepthOfFieldQuality", L"0"}}}}},
    {kCameraTab, "Camera effects", "LensFlares", "Lens flares",
     "Ghost reflections of bright lights (flashlights, lamps) across the screen.",
     L"r.LensFlareQuality",
     {{"Game", 0, {}}, {"Off", 1, {{L"r.LensFlareQuality", L"0"}}}}},
    {kCameraTab, "Camera effects", "Vignette", "Vignette",
     "Darkened screen corners. Off drops Unreal's tonemapper to quality 1 (game 5), which\n"
     "also turns off its own film grain and film shadows.",
     L"r.Tonemapper.Quality",
     {{"Game", 0, {}}, {"Off", 1, {{L"r.Tonemapper.Quality", L"1"}}}}},

    // ------------------------------------------------ HDR (in no preset)
    {kHdrTab, "Brightness", kHdrKey, "HDR brightness",
     "Once the HDR black screen is fixed (r.UCP.HDR=0 in Engine.ini), the game's own HDR\n"
     "sliders do nothing and its tone curve is built for 1000 nits. Set here: the sliders\n"
     "below replace them. They start at the game's own values.",
     nullptr,
     {{"Game (fixed 1000 nits)", 0, {}}, {"Set here", 1, {}}}},
};

// HDR brightness (while the HDR option is Set here). With nothing saved the sliders start at the
// game's values, read once on the game thread.
constexpr float kMidGrey = 0.18f;  // paper white sets 18% grey to 0.18 x this
int g_hdrPeak = 1000, g_hdrPaper = 203, g_hdrUi = 300;  // nits
bool g_brightSaved = false;                             // the values came from ReShade.ini
std::atomic<bool> g_brightReady{false};                 // values known (saved, or read from the game)

// Guards the options, the measurement and the requests to the game thread (present thread;
// the game thread only tries it, briefly).
std::recursive_mutex g_state;

std::atomic<bool> g_enabled{true};  // F8

// What the game's menu decides, read by the game thread every second.
std::atomic<int> g_allowRtReflections{-1};  // r.BM.AllowRTReflections (the RT Reflections option)
std::atomic<int> g_rtMaster{-1};            // r.RayTracing.ForceAllRayTracingEffects (0: Ray Tracing off)
std::atomic<int> g_ucpHdr{-1};              // r.UCP.HDR (1: the HDR fix isn't in Engine.ini)
std::atomic<int> g_hdrOutput{-1};           // r.HDR.EnableHDROutput

std::atomic<bool> g_dirty{true};  // options changed: apply at the next game tick

const Choice* Chosen(const Option& o) {
    for (const Choice& c : o.choices)
        if (c.id == o.value) return &c;
    return nullptr;
}

Option* FindOption(const char* key) {
    for (Option& o : g_options)
        if (!strcmp(o.key, key)) return &o;
    return nullptr;
}

int OptionValue(const char* key) {
    const Option* o = FindOption(key);
    return o ? o->value : 0;
}

std::wstring Number(double v) {
    wchar_t buf[32];
    swprintf(buf, 32, L"%g", v);
    return buf;
}

// What we want in force now.
std::map<std::wstring, std::wstring> Wanted() {
    std::map<std::wstring, std::wstring> m;
    if (!g_enabled) return m;
    for (const Option& o : g_options)
        if (const Choice* c = Chosen(o))
            for (auto& [key, value] : c->cvars) m[key] = value;
    // The game's own gate also requires its RT Reflections menu option: keep that working.
    if (OptionValue("RtReflections")) m[L"r.RayTracing.Reflections"] = g_allowRtReflections.load() == 1 ? L"1" : L"0";
    if (OptionValue("TextureMemory") == kPoolAuto) m[L"r.Streaming.PoolSize"] = Number(kVram[AutoVramRow()].poolMb);
    if (OptionValue(kHdrKey) && g_brightReady && KnownBuild()) {
        m[L"r.HDR.Display.MaxLuminance"] = Number(g_hdrPeak);
        m[L"r.HDR.Display.MidLuminance"] = Number(g_hdrPaper * kMidGrey);
        m[L"r.HDR.UI.Luminance"] = Number(g_hdrUi);
    }
    return m;
}

// ---------------------------------------------------------------- presets
//
// A preset sets the quality options (option key -> choice id); every other quality option goes
// to Game. Options in no preset (the ray tracing fix, HDR) are left alone. The preset shown is
// derived from the options, "Custom" when none matches.

struct QualityPreset {
    const char* label;
    const char* cost;  // shown next to the label in the picker ("" = not measured)
    const char* tip;
    std::vector<std::pair<const char*, int>> values;
};

// Measured 2026-11-02 (RTX 5080, 4K, one scene, RT shadows on enhanced lights, reflections
// everywhere), with Ultra+ = 1.1's defaults (High fog): Game 8.44 ms (118 fps), Ultra+ 8.83 ms
// (113, +5%), Cinematic 10.94 ms (91, +30%). "Measure each option" (Game 6.60 ms, +-3% noise):
// RT reflections everywhere +14%, fog Ultra +10%, RT shadows all lights +7%, shadows Cinematic
// +6%, fog High +5%, RT quality Ultra +4%, the rest within noise. Ultra+ should land about
// midway (user's rule), so it now takes Ultra fog, the biggest of Cinematic's extras; Cinematic
// adds Ultra RT quality and Cinematic shadows. Not re-measured as presets yet.
const QualityPreset kPresets[] = {
    {"Game", "", "The game's own quality: everything in the presets at Game. The ray tracing fix\n"
                 "and HDR brightness stay as they are.", {}},
    {"Ultra+", "",
     "High ray tracing quality (2 rays), texture memory from your card, 16x filtering,\n"
     "sharp shadows, Ultra fog (the finest flashlight beams), Cinematic view distance and\n"
     "effects.",
     {{"RtQuality", 1}, {"TextureMemory", kPoolAuto}, {"Anisotropy", 16}, {"ShadowDetail", 1}, {"FogDetail", 4},
      {"ViewDistance", 1}, {"CinematicEffects", 1}}},
    {"Cinematic", "about 30% slower",
     "Ultra+ with Ultra ray tracing quality (4 rays) and the game's Cinematic shadows.\n\n"
     "Measured on an RTX 5080 at 4K in one scene: 118 fps with Game, 91 with Cinematic.",
     {{"RtQuality", 2}, {"TextureMemory", kPoolAuto}, {"Anisotropy", 16}, {"ShadowDetail", 2}, {"FogDetail", 4},
      {"ViewDistance", 1}, {"CinematicEffects", 1}}},
};
constexpr int kPresetCount = static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));
constexpr int kDefaultPreset = 1;  // Ultra+ for a new install, as 1.1 defaulted to these

std::set<std::string> PresetKeys() {
    std::set<std::string> keys;
    for (const QualityPreset& p : kPresets)
        for (auto& [key, id] : p.values) keys.insert(key);
    return keys;
}

int PresetValue(const QualityPreset& p, const std::string& key) {
    for (auto& [k, id] : p.values)
        if (key == k) return id;
    return 0;
}

int CurrentPreset() {
    std::set<std::string> keys = PresetKeys();
    for (int i = 0; i < kPresetCount; ++i) {
        bool match = true;
        for (const std::string& key : keys) {
            const Option* o = FindOption(key.c_str());
            if (o && o->value != PresetValue(kPresets[i], key)) match = false;
        }
        if (match) return i;
    }
    return -1;
}

void ApplyPreset(int index) {
    for (const std::string& key : PresetKeys())
        if (Option* o = FindOption(key.c_str())) o->value = PresetValue(kPresets[index], key);
    g_dirty = true;
}

// ---------------------------------------------------------------- settings

constexpr int kSettingsVersion = 2;  // 1.2.0: one key per option

bool HasKey(const char* section, const char* key) {
    size_t size = 0;
    return reshade::get_config_value(nullptr, section, key, nullptr, &size);
}

void SaveSettings() {
    reshade::set_config_value(nullptr, DT_PRODUCT, "SettingsVersion", kSettingsVersion);
    for (const Option& o : g_options) reshade::set_config_value(nullptr, DT_PRODUCT, o.key, o.value);
    if (g_brightSaved || g_brightReady) {  // not the placeholders before the game's values are read
        reshade::set_config_value(nullptr, DT_PRODUCT, "HdrPeakNits", g_hdrPeak);
        reshade::set_config_value(nullptr, DT_PRODUCT, "HdrPaperWhiteNits", g_hdrPaper);
        reshade::set_config_value(nullptr, DT_PRODUCT, "HdrUiNits", g_hdrUi);
        g_brightSaved = true;
    }
}

void LoadBrightness(const char* section) {
    reshade::get_config_value(nullptr, section, "HdrPeakNits", g_hdrPeak);
    reshade::get_config_value(nullptr, section, "HdrPaperWhiteNits", g_hdrPaper);
    reshade::get_config_value(nullptr, section, "HdrUiNits", g_hdrUi);
    g_brightSaved = true;
}

void SetOption(const char* key, int value) {
    if (Option* o = FindOption(key)) o->value = value;
}

// 1.0/1.1 kept one on/off switch per group plus its settings ([UntilDawnTweaks], or
// [UntilDawnRT] before the rename). Turns them into the options, so nothing changes on update.
// With nothing saved: the ray tracing fix on, Ultra+ (1.1's defaults), HDR brightness Set here.
void MigrateOrDefault() {
    for (Option& o : g_options) o.value = 0;
    ApplyPreset(kDefaultPreset);
    SetOption("RtShadowLights", 2);
    SetOption("RtReflections", 1);
    SetOption("RtRoughness", 4);
    SetOption(kHdrKey, 1);

    const char* rt = HasKey(DT_PRODUCT, "RtOverride")          ? DT_PRODUCT
                     : HasKey(DT_LEGACY_PRODUCT, "RtOverride") ? DT_LEGACY_PRODUCT
                                                               : nullptr;
    if (rt) {
        bool on = true, everywhere = true;
        int shadows = 2, quality = 1;
        float roughness = 0.4f;
        reshade::get_config_value(nullptr, rt, "RtOverride", on);
        reshade::get_config_value(nullptr, rt, "RtShadows", shadows);
        reshade::get_config_value(nullptr, rt, "RtReflectionsEverywhere", everywhere);
        reshade::get_config_value(nullptr, rt, "RtReflectionsMaxRoughness", roughness);
        reshade::get_config_value(nullptr, rt, "RtQuality", quality);
        int rough = 2;  // the nearest choice
        for (int id : {4, 6, 10})
            if (fabs(roughness - id / 10.0f) < fabs(roughness - rough / 10.0f)) rough = id;
        SetOption("RtShadowLights", on ? std::clamp(shadows, 0, 2) : 0);
        SetOption("RtReflections", on && everywhere ? 1 : 0);
        SetOption("RtRoughness", on && everywhere ? rough : 0);
        SetOption("RtQuality", on ? std::clamp(quality, 0, 2) : 0);
    }
    if (HasKey(DT_PRODUCT, "DetailOverride")) {
        bool on = true, aniso = true, effects = true;
        int vram = 0, shadows = 1, fog = 1, view = 1;
        reshade::get_config_value(nullptr, DT_PRODUCT, "DetailOverride", on);
        reshade::get_config_value(nullptr, DT_PRODUCT, "DetailVideoMemory", vram);
        reshade::get_config_value(nullptr, DT_PRODUCT, "DetailAnisotropy16", aniso);
        reshade::get_config_value(nullptr, DT_PRODUCT, "DetailShadows", shadows);
        reshade::get_config_value(nullptr, DT_PRODUCT, "DetailFog", fog);
        reshade::get_config_value(nullptr, DT_PRODUCT, "DetailViewDistance", view);
        reshade::get_config_value(nullptr, DT_PRODUCT, "DetailCinematicEffects", effects);
        vram = std::clamp(vram, 0, static_cast<int>(std::size(kVram)));
        SetOption("TextureMemory", !on ? 0 : vram == 0 ? kPoolAuto : kVram[vram - 1].poolMb);
        SetOption("Anisotropy", on && aniso ? 16 : 0);
        SetOption("ShadowDetail", on ? std::clamp(shadows, 0, 2) : 0);
        SetOption("FogDetail", !on || fog <= 0 ? 0 : fog == 1 ? 6 : 4);
        SetOption("ViewDistance", on && view ? 1 : 0);
        SetOption("CinematicEffects", on && effects ? 1 : 0);
    }
    const char* hdr = HasKey(DT_PRODUCT, kHdrKey)          ? DT_PRODUCT
                      : HasKey(DT_LEGACY_PRODUCT, kHdrKey) ? DT_LEGACY_PRODUCT
                                                           : nullptr;
    if (hdr) {
        bool on = true;
        reshade::get_config_value(nullptr, hdr, kHdrKey, on);
        SetOption(kHdrKey, on ? 1 : 0);
        LoadBrightness(hdr);
    }
    Log("settings: %s", rt || hdr ? "carried over from 1.x" : "none saved: defaults (ray tracing fix, Ultra+)");
}

void LoadSettings() {
    int version = 0;
    reshade::get_config_value(nullptr, DT_PRODUCT, "SettingsVersion", version);
    if (version < kSettingsVersion) {
        MigrateOrDefault();
    } else {
        for (Option& o : g_options) {
            reshade::get_config_value(nullptr, DT_PRODUCT, o.key, o.value);
            if (!Chosen(o)) o.value = 0;
        }
        if (HasKey(DT_PRODUCT, "HdrPeakNits")) LoadBrightness(DT_PRODUCT);
    }
    g_hdrPeak = std::clamp(g_hdrPeak, 400, 10000);
    g_hdrPaper = std::clamp(g_hdrPaper, 80, 500);
    g_hdrUi = std::clamp(g_hdrUi, 80, 1000);
    if (g_brightSaved) g_brightReady = true;
    SaveSettings();
    std::string list;
    for (const Option& o : g_options) list += std::string(list.empty() ? "" : ", ") + o.key + " " + std::to_string(o.value);
    Log("options: %s; HDR %d / %d / %d nits%s", list.c_str(), g_hdrPeak, g_hdrPaper, g_hdrUi,
        g_brightSaved ? "" : " (until the game's are read)");
}

// ---------------------------------------------------------------- device

std::atomic<int> g_api{0};  // device_api of the presenting device (0 = not seen yet)

const char* ApiName() {
    switch (static_cast<device_api>(g_api.load())) {
        case device_api::d3d11: return "DirectX 11";
        case device_api::d3d12: return "DirectX 12";
        case device_api::vulkan: return "Vulkan";
        default: return "?";
    }
}

void OnInitDevice(device* dev) {
    if (dev->get_api() != device_api::d3d12) return;
    // The game creates a few devices at startup; the last one is the one it renders with.
    if (uint64_t mb = AdapterMemoryMb(reinterpret_cast<ID3D12Device*>(dev->get_native()))) {
        g_vramMb = mb;
        Log("device: %llu MB dedicated video memory (Auto texture memory %d MB)", static_cast<unsigned long long>(mb),
            kVram[AutoVramRow()].poolMb);
        g_dirty = true;
    }
}

// ---------------------------------------------------------------- live settings

enum class Live { Idle, Searching, Ready, Failed };
std::atomic<Live> g_live{Live::Idle};
void* g_manager = nullptr;
std::string g_liveError;

// Game thread only:
std::map<std::wstring, std::wstring> g_originals;  // the game's value of each cvar we hold
std::map<std::wstring, uint32_t> g_origPriority;   // ...and the priority it was set at
std::map<std::wstring, std::wstring> g_applied;    // what we keep in force
std::set<std::wstring> g_rejected;                 // values the game didn't take (logged once)

// Requests from the present thread (under g_state):
std::vector<std::pair<std::wstring, std::wstring>> g_pendingConsole;  // Advanced tab's "Set once"
std::string g_pendingProbe;                                           // Advanced tab's probe request

std::atomic<int> g_reapplied{0};    // values put back after they changed
std::atomic<bool> g_hooked{false};  // game-thread hook in place
std::atomic<uint64_t> g_ticks{0};   // game ticks seen by the hook
ULONGLONG g_readyTick = 0;

// What the overlay shows of the game thread's state; the game thread publishes it.
struct View {
    std::map<std::wstring, std::wstring> held;  // held cvar -> the game's value
    std::vector<std::string> rejected;
    std::string probeResult;
    int userHdrFlag = -1, userHdrNits = -1, toneCurveNits = -1;  // HDR brightness fields
};
std::mutex g_viewMutex;
View g_view;

void PublishView() {
    std::lock_guard<std::mutex> lock(g_viewMutex);
    g_view.held.clear();
    for (auto& [key, value] : g_applied) {
        auto it = g_originals.find(key);
        g_view.held[key] = it != g_originals.end() ? it->second : L"?";
    }
    g_view.rejected.clear();
    for (const std::wstring& k : g_rejected) g_view.rejected.push_back(Narrow(k));
}

View CopyView() {
    std::lock_guard<std::mutex> lock(g_viewMutex);
    return g_view;
}

struct VarInfo {
    void* var = nullptr;
    cvars::Type type = cvars::Type::Unknown;
};
std::mutex g_varsMutex;
std::map<std::wstring, VarInfo> g_vars;  // lookup cache (cvars live for the whole session)

VarInfo Lookup(const std::wstring& name) {
    std::lock_guard<std::mutex> lock(g_varsMutex);
    auto it = g_vars.find(name);
    if (it != g_vars.end()) return it->second;
    VarInfo info;
    if (!g_manager) return info;
    info.var = cvars::FindVariable(g_manager, name.c_str());
    if (info.var) info.type = cvars::GetType(info.var);
    if (info.var) g_vars[name] = info;  // misses aren't cached: plugin cvars can register late
    return info;
}

bool Read(const std::wstring& name, std::wstring& out) {
    VarInfo v = Lookup(name);
    wchar_t buf[64];
    if (!v.var || !cvars::GetText(v.var, v.type, buf, 64)) return false;
    out = buf;
    return true;
}

int ReadInt(const wchar_t* name, int fallback) {
    std::wstring v;
    return Read(name, v) ? static_cast<int>(lround(_wtof(v.c_str()))) : fallback;
}

bool SameValue(cvars::Type t, const std::wstring& a, const std::wstring& b) {
    if (t == cvars::Type::Float) return fabs(_wtof(a.c_str()) - _wtof(b.c_str())) < 1e-4;
    return _wtoi(a.c_str()) == _wtoi(b.c_str());
}

// How many change callbacks a variable has: OnChangedCallback is a multicast delegate at +0x20
// with its invocation count at +0x28 (FConsoleVariableBase in UE 4.2x/5.x; logged only).
int Callbacks(void* var) {
    int32_t n = -1;
    cvars::SafeCopy(&n, static_cast<uint8_t*>(var) + 0x28, 4);
    return n;
}

void __fastcall HookedSinks(void* self);
cvars::SinksFn g_origSinks = nullptr;
int g_sinkFlag = -1;  // FConsoleManager::bCallAllConsoleVariableSinks offset, read before hooking

void FindManager() {
    const uint8_t* base = GameBase();
    Log("game exe timestamp 0x%08x (%s)", PeTimestamp(base),
        KnownBuild() ? "the tested build" : "NOT the tested build: HDR brightness off");
    int votes = 0, matches = 0;
    void** singleton = cvars::FindConsoleManagerSingleton(base, &votes, &matches);
    void* manager = nullptr;
    if (singleton) cvars::SafeCopy(&manager, singleton, 8);
    Log("console manager: singleton %p (RVA 0x%llx, %d of %d matches), manager %p", singleton,
        singleton ? static_cast<unsigned long long>(reinterpret_cast<const uint8_t*>(singleton) - base) : 0ull, votes,
        matches, manager);
    if (!manager || !cvars::IsConsoleManager(base, manager)) {
        g_liveError = "console manager not found";
        g_live = Live::Failed;
        return;
    }
    // Known variables check the layout: r.MaxAnisotropy is an int (1-16), r.ScreenPercentage a float.
    void* aniso = cvars::FindVariable(manager, L"r.MaxAnisotropy");
    void* screen = cvars::FindVariable(manager, L"r.ScreenPercentage");
    bool vt = aniso && screen && cvars::HasModuleVtable(base, aniso) && cvars::HasModuleVtable(base, screen);
    bool slots = vt && cvars::FindTypeSlots(aniso, screen);
    int32_t a = -1;
    float s = -1;
    // r.ScreenPercentage can read 0 (UE 5's "automatic"), so r.ViewDistanceScale (positive)
    // confirms float reads too.
    void* view = cvars::FindVariable(manager, L"r.ViewDistanceScale");
    float d = -1;
    bool reads = slots && cvars::GetInt(aniso, a) && cvars::GetFloat(screen, s) && view &&
                 cvars::HasModuleVtable(base, view) && cvars::GetFloat(view, d);
    Log("layout check: r.MaxAnisotropy %p, r.ScreenPercentage %p, vtables %d, IsVariableBool at +0x%x, values %d / %g / "
        "view distance %g",
        aniso, screen, vt, cvars::g_isVariableBool, a, s, d);
    if (!reads || a < 0 || a > 16 || !(s == 0 || (s >= 10 && s <= 400)) || !(d > 0.01f && d <= 100)) {
        g_liveError = "the game's settings layout didn't match (see the log)";
        g_live = Live::Failed;
        return;
    }
    for (auto [name, var] : {std::pair<const char*, void*>{"r.MaxAnisotropy", aniso}, {"r.ScreenPercentage", screen}}) {
        cvars::Type t = cvars::GetType(var);
        void* data = cvars::AsData(var, t);
        Log("  %s: %s, priority 0x%02x, data %s (+0x%llx), %d callbacks", name, cvars::TypeName(t), cvars::Priority(var),
            data ? "yes" : "no",
            data ? static_cast<unsigned long long>(static_cast<uint8_t*>(data) - static_cast<uint8_t*>(var)) : 0ull,
            Callbacks(var));
    }
    {
        std::lock_guard<std::mutex> lock(g_varsMutex);
        g_manager = manager;
    }
    g_sinkFlag = cvars::FindSinkFlagOffset(manager);  // from the game's own code, before it's hooked
    Log("console variable sinks flag at +0x%x (now %d)", g_sinkFlag, cvars::ReadSinkFlag(manager, g_sinkFlag));
    if (!cvars::HookSinks(base, manager, &HookedSinks, &g_origSinks)) {
        g_liveError = "couldn't reach the game thread (see the log)";
        Log("game-thread hook: CallAllConsoleVariableSinks slot not found");
        g_live = Live::Failed;
        return;
    }
    g_hooked = true;
    Log("game-thread hook in place (CallAllConsoleVariableSinks was %p, RVA 0x%llx)",
        reinterpret_cast<void*>(g_origSinks),
        static_cast<unsigned long long>(reinterpret_cast<const uint8_t*>(g_origSinks) - base));
    g_readyTick = GetTickCount64();
    g_live = Live::Ready;
}

// Puts <value> in force at console priority. Game thread only.
bool SetVar(const std::wstring& key, const std::wstring& value) {
    VarInfo v = Lookup(key);
    if (!v.var) {
        if (g_rejected.insert(key).second) Log("%ls not found", key.c_str());
        return false;
    }
    if (!cvars::Set(v.var, value.c_str(), cvars::kSetByConsole)) return false;
    std::wstring now;  // on the game thread the getters read the main value, just set
    if (Read(key, now) && !SameValue(v.type, now, value)) {
        if (g_rejected.insert(key).second)
            Log("%ls = %ls not taken (reads %ls, priority 0x%02x)", key.c_str(), value.c_str(), now.c_str(),
                cvars::Priority(v.var));
        return false;
    }
    g_rejected.erase(key);
    return true;
}

// Gives a value back: the game's value, then the game's priority (so its menu can set it again).
bool RestoreVar(const std::wstring& key) {
    auto it = g_originals.find(key);
    if (it == g_originals.end() || !SetVar(key, it->second)) return false;
    auto pr = g_origPriority.find(key);
    VarInfo v = Lookup(key);
    if (pr != g_origPriority.end() && v.var) cvars::RestorePriority(v.var, pr->second);
    return true;
}

// Brings the game in line with Wanted(): our values in force, ones no longer wanted back to the
// game's. Game thread.
void Apply(std::map<std::wstring, std::wstring> want) {
    int set = 0, restored = 0, failed = 0;
    for (auto& [key, value] : g_applied) {
        if (want.count(key)) continue;
        if (RestoreVar(key)) ++restored;
    }
    for (auto& [key, value] : want) {
        if (!g_applied.count(key)) {  // newly held: the game's value is what's there now
            std::wstring old;
            if (Read(key, old)) {
                g_originals[key] = old;
                if (VarInfo v = Lookup(key); v.var) g_origPriority[key] = cvars::GetFlags(v.var) & cvars::kPriorityMask;
            }
        }
        // Already held at this value and still in force: nothing to set (a slider drag re-applies
        // every frame, and every Set makes the engine run its sinks).
        auto held = g_applied.find(key);
        std::wstring cur;
        if (held != g_applied.end() && held->second == value && Read(key, cur) && SameValue(Lookup(key).type, cur, value)) {
            ++set;
            continue;
        }
        if (SetVar(key, value)) ++set;
        else ++failed;
    }
    g_applied = std::move(want);
    Log("applied %d, restored %d, failed %d (tweaks %s)", set, restored, failed, g_enabled ? "on" : "off");
    PublishView();
}

// Every second: any value that changed anyway (the game's code can set at console priority too)
// becomes the new game value, and ours goes back in. Game thread.
void Hold() {
    static ULONGLONG last = 0;
    ULONGLONG now = GetTickCount64();
    if (now - last < 1000) return;
    last = now;
    static std::map<std::wstring, int> counts;  // per setting, to keep the log short
    bool any = false;
    for (auto& [key, value] : g_applied) {
        if (g_rejected.count(key)) continue;
        VarInfo v = Lookup(key);
        std::wstring cur;
        if (!Read(key, cur) || SameValue(v.type, cur, value)) continue;
        g_originals[key] = cur;
        if (SetVar(key, value)) ++g_reapplied;
        any = true;
        int n = ++counts[key];
        if (n <= 3 || n == 10 || n % 100 == 0)
            Log("put back %ls = %ls (the game set %ls; %d times so far)", key.c_str(), value.c_str(), cur.c_str(), n);
    }
    if (any) PublishView();
}

// ---------------------------------------------------------------- HDR brightness
//
// With HDR on (and the Uniform Colour Pipeline off), the peak is not just a cvar:
// HDRSettingChangedSinkCallback (a cvar sink, on the game thread) rebuilds the tone curve with
// HDRConfigureCVars whenever UGameUserSettings::GetCurrentHDRDisplayNits() or one of the cvars it
// watches changes, then sets r.HDR.Display.MaxLuminance to those nits. That getter returns 0 (and
// the sink then uses 1000) unless UGameUserSettings::bUseHDRDisplayOutput is set, which the game
// leaves off even with HDR on. So the peak is written to GEngine->GameUserSettings->
// HDRDisplayOutputNits, the flag is set while HDR output is on, and the sinks are asked to run.
// Paper white goes through r.HDR.Display.MidLuminance (nits at 18% grey), the UI through
// r.HDR.UI.Luminance (both in Wanted). Offsets from the PDB of the tested build only.

constexpr uint32_t kGEngineRva = 0x8994070;    // ?GEngine@@3PEAVUEngine@@EA
constexpr uint32_t kGameUserSettings = 0x2a0;  // UEngine::GameUserSettings
constexpr uint32_t kUseHdr = 0x10c;            // UGameUserSettings::bUseHDRDisplayOutput (bool)
constexpr uint32_t kHdrNits = 0x110;           // UGameUserSettings::HDRDisplayOutputNits (int)
constexpr uint32_t kSinkNitsRva = 0x85d3140;   // the nits the HDR sink last built the curve for

// GEngine->GameUserSettings, or null (other build, not created yet).
uint8_t* UserSettings() {
    if (!KnownBuild()) return nullptr;
    uint8_t* engine = nullptr;
    uint8_t* settings = nullptr;
    if (!cvars::SafeCopy(&engine, GameBase() + kGEngineRva, 8) || !engine) return nullptr;
    if (!cvars::SafeCopy(&settings, engine + kGameUserSettings, 8) || !settings) return nullptr;
    return settings;
}

bool g_flagForced = false, g_nitsForced = false;  // game thread
uint8_t g_origFlag = 0;
int g_origNits = 0;

// Game thread, every frame, before the sinks.
void BrightnessOnTick() {
    uint8_t* s = UserSettings();
    if (!s) return;
    if (!g_brightReady) {  // nothing saved: start at the game's own values
        int nits = 0;
        if (cvars::SafeCopy(&nits, s + kHdrNits, 4) && nits > 0) g_hdrPeak = std::clamp(nits, 400, 10000);
        std::wstring v;
        if (Read(L"r.HDR.Display.MidLuminance", v) && _wtof(v.c_str()) > 0)
            g_hdrPaper = std::clamp(static_cast<int>(lround(_wtof(v.c_str()) / kMidGrey)), 80, 500);
        if (Read(L"r.HDR.UI.Luminance", v) && _wtof(v.c_str()) > 0)
            g_hdrUi = std::clamp(static_cast<int>(lround(_wtof(v.c_str()))), 80, 1000);
        g_brightReady = true;
        g_dirty = true;
        Log("HDR brightness: the game's values: peak %d, paper white %d, UI %d nits", g_hdrPeak, g_hdrPaper, g_hdrUi);
    }
    const bool want = g_enabled && OptionValue(kHdrKey) != 0;
    const bool hdrOut = g_hdrOutput.load() > 0;
    bool poke = false;

    uint8_t flag = 0;
    if (cvars::SafeCopy(&flag, s + kUseHdr, 1)) {
        if (want && hdrOut && flag == 0) {
            if (!g_flagForced) g_origFlag = flag, Log("HDR brightness: bUseHDRDisplayOutput was 0; setting it");
            g_flagForced = true;
            uint8_t one = 1;
            cvars::SafeCopy(s + kUseHdr, &one, 1);
            poke = true;
        } else if (!(want && hdrOut) && g_flagForced) {
            g_flagForced = false;
            cvars::SafeCopy(s + kUseHdr, &g_origFlag, 1);
            Log("HDR brightness: bUseHDRDisplayOutput back to %u", g_origFlag);
            poke = true;
        }
    }
    int cur = 0;
    const int peak = g_hdrPeak;
    if (cvars::SafeCopy(&cur, s + kHdrNits, 4)) {
        if (want && cur != peak) {
            if (!g_nitsForced) g_origNits = cur;
            static int n = 0;
            if (++n <= 5 || n % 100 == 0) Log("HDR brightness: peak %d nits (was %d)", peak, cur);
            g_nitsForced = true;
            cvars::SafeCopy(s + kHdrNits, &peak, 4);
            poke = true;
        } else if (!want && g_nitsForced) {
            g_nitsForced = false;
            cvars::SafeCopy(s + kHdrNits, &g_origNits, 4);
            Log("HDR brightness: game's %d nits back", g_origNits);
            poke = true;
        }
    }
    if (poke) cvars::SetSinkFlag(g_manager, g_sinkFlag, 1);  // the HDR sink rebuilds the curve this frame

    static ULONGLONG last = 0;
    if (GetTickCount64() - last > 500) {
        last = GetTickCount64();
        int built = -1;
        cvars::SafeCopy(&built, GameBase() + kSinkNitsRva, 4);
        std::lock_guard<std::mutex> lock(g_viewMutex);
        g_view.userHdrFlag = flag;
        g_view.userHdrNits = cur;
        g_view.toneCurveNits = built;
    }
}

// ---------------------------------------------------------------- probe
//
// Writes the game's value of every console variable under the prefixes below to
// UntilDawnTweaks-probe.txt: automatically 5 s and 60 s after connecting (title screen, then
// after the game has applied its menu), and from the Advanced tab at any time (in a scene). Names
// come from the game's read-only data (UTF-16 strings); nothing is enumerated. Runs on the game
// thread, so it reads the game thread's values.

const char* const kProbePrefixes[] = {
    "r.RayTracing", "r.BM.", "r.bm.", "r.UCP.", "r.HDR", "r.AllowHDR", "r.Tonemapper", "r.Shadow.", "r.Lumen.",
    "r.ReflectionMethod", "r.DynamicGlobalIlluminationMethod", "r.Reflections.Denoiser", "r.AmbientOcclusion",
    "r.TSR.", "r.TemporalAA", "r.AntiAliasing", "r.ScreenPercentage", "r.SecondaryScreenPercentage", "r.DynamicRes.",
    "r.Streaming.", "r.MaxAnisotropy", "r.VT.", "r.SSS.", "r.SubsurfaceScattering", "r.SkinCache.", "r.HairStrands.",
    "r.VolumetricFog", "r.VolumetricCloud", "r.Bloom", "r.LensFlare", "r.DepthOfField", "r.DOF.", "r.MotionBlur",
    "r.SceneColorFringe", "r.FilmGrain", "r.PostProcessAAQuality", "r.SSR.", "r.DistanceField", "r.ContactShadows",
    "r.CapsuleShadows", "r.ViewDistanceScale", "r.StaticMeshLODDistanceScale", "r.SkeletalMeshLODBias",
    "r.Translucency", "r.SSGI", "r.LightMaxDrawDistanceScale", "r.MipMapLODBias", "r.D3D12.DXR", "r.Nanite.",
    "r.FidelityFX", "r.NGX.", "r.Streamline.", "r.XeSS", "r.FSR", "r.Reflex", "foliage.", "grass.", "sg.",
};

std::set<std::wstring> g_probeNames;  // collected once

void CollectProbeNames() {
    std::vector<std::wstring> prefixes;
    for (const char* p : kProbePrefixes) prefixes.emplace_back(p, p + strlen(p));
    for (const cvars::Section& s : cvars::Sections(GameBase(), cvars::SectionKind::ReadOnly)) {
        for (size_t i = 2; i + 8 < s.size; i += 2) {
            if (s.begin[i - 2] || s.begin[i - 1]) continue;  // must start right after a terminator
            const wchar_t* w = reinterpret_cast<const wchar_t*>(s.begin + i);
            wchar_t c0 = w[0];
            if (c0 != L'r' && c0 != L'f' && c0 != L'g' && c0 != L's') continue;
            size_t len = 0;
            while (len < 120 && i + 2 * len + 2 < s.size && w[len] > 32 && w[len] < 127) ++len;
            if (len < 3 || w[len] != 0) continue;
            for (const std::wstring& p : prefixes)
                if (len >= p.size() && !wcsncmp(w, p.c_str(), p.size())) {
                    g_probeNames.emplace(w, len);
                    break;
                }
        }
    }
    Log("probe: %zu candidate names", g_probeNames.size());
}

void WriteProbe(const char* label) {
    if (g_probeNames.empty()) CollectProbeNames();
    FILE* f = _wfopen((g_exeDir + DT_PRODUCT_W L"-probe.txt").c_str(), L"a");
    if (!f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "\n==== %s (%04u-%02u-%02u %02u:%02u:%02u, %.0f s after start, %s, tweaks %s, holding %zu)\n", label,
            t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, (GetTickCount64() - g_loadTick) / 1000.0, ApiName(),
            g_enabled ? "on" : "off", g_applied.size());
    int found = 0, unreadable = 0;
    for (const std::wstring& name : g_probeNames) {
        VarInfo v = Lookup(name);
        if (!v.var) continue;
        std::wstring value;
        if (Read(name, value)) {
            ++found;
            fprintf(f, "%ls (%s, set by 0x%02x) = %ls\n", name.c_str(), cvars::TypeName(v.type), cvars::Priority(v.var),
                    value.c_str());
        } else {
            ++unreadable;
            fprintf(f, "%ls (%s) = ?\n", name.c_str(), cvars::TypeName(v.type));
        }
    }
    fprintf(f, "---- %d values, %d unreadable (strings/commands skipped)\n", found, unreadable);
    fclose(f);
    Log("probe '%s': %d values written", label, found);
    std::lock_guard<std::mutex> lock(g_viewMutex);
    g_view.probeResult = std::to_string(found) + " values written to " DT_PRODUCT "-probe.txt";
}

void ProbeOnTick(const std::string& request) {
    static int done = 0;
    ULONGLONG since = GetTickCount64() - g_readyTick;
    if (done == 0 && since > 5000) WriteProbe("auto: 5 s after connecting"), done = 1;
    else if (done == 1 && since > 60000) WriteProbe("auto: 60 s after connecting"), done = 2;
    if (!request.empty()) WriteProbe(request.c_str());
}

// ---------------------------------------------------------------- game thread

// Every second: the menu options the add-on follows or warns about.
void ReadMenuState() {
    static ULONGLONG last = 0;
    if (GetTickCount64() - last < 1000) return;
    last = GetTickCount64();
    const int allow = ReadInt(L"r.BM.AllowRTReflections", 0);
    if (g_allowRtReflections.exchange(allow) != allow) {
        Log("menu: RT reflections allowed %d", allow);
        g_dirty = true;  // "Everywhere" follows it
    }
    const int master = ReadInt(L"r.RayTracing.ForceAllRayTracingEffects", -1);
    if (g_rtMaster.exchange(master) != master) Log("menu: r.RayTracing.ForceAllRayTracingEffects %d", master);
    const int ucp = ReadInt(L"r.UCP.HDR", -1);
    if (g_ucpHdr.exchange(ucp) != ucp) Log("r.UCP.HDR %d%s", ucp, ucp == 1 ? " (the HDR fix isn't in Engine.ini)" : "");
    const int hdr = ReadInt(L"r.HDR.EnableHDROutput", 0);
    if (g_hdrOutput.exchange(hdr) != hdr) Log("HDR output %s", hdr ? "on" : "off");
}

// Once per frame, before the game's sinks run: all setting happens here.
void GameTick() {
    ++g_ticks;
    if (g_live.load() != Live::Ready) return;
    ReadMenuState();
    BrightnessOnTick();
    // Copy the requests; g_state is let go before anything is set (see the header).
    bool dirty = false;
    std::map<std::wstring, std::wstring> want;
    std::vector<std::pair<std::wstring, std::wstring>> console;
    std::string probe;
    {
        std::unique_lock<std::recursive_mutex> lock(g_state, std::try_to_lock);
        if (lock.owns_lock()) {
            dirty = g_dirty.exchange(false);
            if (dirty) want = Wanted();
            console.swap(g_pendingConsole);
            probe.swap(g_pendingProbe);
        }
    }
    for (auto& [key, value] : console) {
        bool ok = SetVar(key, value);
        Log("console: %ls = %ls %s", key.c_str(), value.c_str(), ok ? "set" : "NOT taken");
    }
    if (!console.empty()) PublishView();
    static bool first = true;
    if (first) {  // the game's values of everything the options touch, for reference
        first = false;
        std::set<std::wstring> keys = {L"r.RayTracing", L"r.RayTracing.Enable", L"r.RayTracing.ForceAllRayTracingEffects",
                                       L"r.BM.AllowRTReflections", L"r.RayTracing.AmbientOcclusion",
                                       L"r.HDR.EnableHDROutput", L"r.UCP.HDR", L"r.HDR.Display.OutputDevice",
                                       L"r.HDR.Display.MaxLuminance", L"r.HDR.Display.MidLuminance",
                                       L"r.HDR.UI.Luminance", L"r.Streaming.PoolSize"};
        for (const Option& o : g_options)
            for (const Choice& c : o.choices)
                for (auto& [key, value] : c.cvars) keys.insert(key);
        for (const std::wstring& key : keys) {
            std::wstring v;
            VarInfo info = Lookup(key);
            Log("game value: %ls (%s, priority 0x%02x, %d callbacks) = %ls", key.c_str(), cvars::TypeName(info.type),
                info.var ? cvars::Priority(info.var) : -1, info.var ? Callbacks(info.var) : 0,
                Read(key, v) ? v.c_str() : L"(not found)");
        }
    }
    if (dirty) Apply(std::move(want));
    Hold();
    ProbeOnTick(probe);
}

// The engine's HDR sink can re-apply the game's own peak to r.HDR.Display.MaxLuminance whenever
// any console variable changed. So right after the sinks the held peak goes back in, before the
// frame is rendered, and the "run the sinks" flag that Set raised is cleared again (otherwise the
// sinks would run every frame). Game thread.
void AfterSinks() {
    static const std::wstring kPeak = L"r.HDR.Display.MaxLuminance";
    auto it = g_applied.find(kPeak);
    if (it == g_applied.end()) return;
    VarInfo v = Lookup(kPeak);
    std::wstring cur;
    if (!v.var || !Read(kPeak, cur) || SameValue(v.type, cur, it->second)) return;
    const int before = cvars::ReadSinkFlag(g_manager, g_sinkFlag);
    cvars::Set(v.var, it->second.c_str(), cvars::kSetByConsole);
    if (before == 0) cvars::SetSinkFlag(g_manager, g_sinkFlag, 0);  // only the flag our Set raised
    static int n = 0;
    if (++n <= 3 || n % 100 == 0)
        Log("HDR peak: the engine's HDR sink set %ls; %ls back in after the sinks (%d times)", cur.c_str(),
            it->second.c_str(), n);
}

void __fastcall HookedSinks(void* self) {
    GameTick();
    g_origSinks(self);  // then the game's sinks, which see what GameTick set
    AfterSinks();
}

// ---------------------------------------------------------------- frame time

int64_t g_qpcFreq = 1;
std::vector<float> g_frameMs;  // recent frames, a ring of kFrameRing
size_t g_framePos = 0;
constexpr size_t kFrameRing = 240;

// The average, not the median: with frame generation the present intervals alternate.
float Mean(const std::vector<float>& v) {
    if (v.empty()) return 0;
    double sum = 0;
    for (float x : v) sum += x;
    return static_cast<float>(sum / v.size());
}

// Ray tracing activity: DispatchRays calls and acceleration structure builds per second.
std::atomic<uint64_t> g_asBuilds{0}, g_rayDispatches{0};
uint64_t g_asPerSec = 0, g_raysPerSec = 0;

// A measurement runs stages one after another: each sets the measured options (key -> choice
// id, every other measured option at Game), settles, then averages the frame time. "Presets"
// runs Game and each preset; "Each option" runs Game, then every option but HDR (Ray tracing only
// while rays are traced) at each of its choices, one at a time, then Game again (to show drift).
struct Stage {
    std::string label;
    std::vector<std::pair<std::string, int>> values;
};

struct Measure {
    bool running = false;
    bool perOption = false;
    ULONGLONG settleMs = 6000, sampleMs = 8000;
    std::vector<Stage> stages;
    std::set<std::string> keys;  // the options the stages control
    size_t stage = 0;
    ULONGLONG stageStart = 0;
    bool sampling = false;
    std::vector<float> samples;
    std::vector<std::pair<std::string, int>> saved;
    std::vector<float> resultMs;
    bool done = false;
    std::string note;
    std::string summary;
} g_measure;

void ApplyStage(const Stage& st) {
    for (const std::string& key : g_measure.keys)
        if (Option* o = FindOption(key.c_str())) o->value = 0;
    for (auto& [key, id] : st.values)
        if (Option* o = FindOption(key.c_str())) o->value = id;
    g_dirty = true;
}

bool Measurable(const Option& o) {
    if (!strcmp(o.tab, kHdrTab) || !strcmp(o.tab, kCameraTab)) return false;
    if (!strcmp(o.tab, kRayTracingTab)) return g_raysPerSec > 0;  // only while ray tracing runs
    return true;
}

void StartMeasure(bool perOption) {
    g_measure = {};
    g_measure.running = true;
    g_measure.perOption = perOption;
    if (perOption) g_measure.settleMs = 5000, g_measure.sampleMs = 6000;
    for (const Option& o : g_options) g_measure.saved.push_back({o.key, o.value});
    g_measure.stages.push_back({"Game", {}});
    if (perOption) {
        for (const Option& o : g_options) {
            if (!Measurable(o)) continue;
            g_measure.keys.insert(o.key);
            for (const Choice& c : o.choices)
                if (c.id) g_measure.stages.push_back({std::string(o.key) + " " + c.label, {{o.key, c.id}}});
        }
        g_measure.stages.push_back({"Game (again)", {}});
    } else {
        g_measure.keys = PresetKeys();
        for (int i = 1; i < kPresetCount; ++i) {
            Stage st{kPresets[i].label, {}};
            for (auto& [key, id] : kPresets[i].values) st.values.push_back({key, id});
            g_measure.stages.push_back(st);
        }
    }
    g_measure.resultMs.assign(g_measure.stages.size(), 0);
    g_measure.stageStart = GetTickCount64();
    ApplyStage(g_measure.stages[0]);
    Log("measure: started (%s, %zu stages, ray tracing %s)", perOption ? "each option" : "presets",
        g_measure.stages.size(), g_raysPerSec ? "running" : "not running");
}

void FinishMeasure(const char* why) {
    for (auto& [key, value] : g_measure.saved)
        if (Option* o = FindOption(key.c_str())) o->value = value;
    g_dirty = true;
    g_measure.running = false;
    g_measure.note = why;
    Log("measure: %s", why);
}

void SummarizeMeasure() {
    const std::vector<float>& r = g_measure.resultMs;
    const float base = g_measure.perOption ? (r.front() + r.back()) / 2 : r.front();
    char line[160];
    if (!g_measure.perOption) {
        std::string sum = "Game " + std::to_string(static_cast<int>(1000.0f / r[0] + 0.5f)) + " fps";
        std::string log = "measure: result";
        for (size_t i = 1; i < r.size(); ++i) {
            snprintf(line, sizeof(line), ", %s %.0f fps (%+.0f%% frame time)", g_measure.stages[i].label.c_str(),
                     1000.0f / r[i], (r[i] / base - 1) * 100);
            sum += line;
            snprintf(line, sizeof(line), ", %s %.2f ms %+.0f%% frame time", g_measure.stages[i].label.c_str(), r[i],
                     (r[i] / base - 1) * 100);
            log += line;
        }
        g_measure.summary = sum;
        Log("%s (Game %.2f ms)", log.c_str(), r[0]);
        return;
    }
    // Each option, costliest first.
    std::vector<size_t> order;
    for (size_t i = 1; i + 1 < r.size(); ++i) order.push_back(i);
    std::sort(order.begin(), order.end(), [&](size_t x, size_t y) { return r[x] > r[y]; });
    Log("measure: each option vs Game %.2f ms (start %.2f, end %.2f), costliest first:", base, r.front(), r.back());
    std::string sum;
    for (size_t k = 0; k < order.size(); ++k) {
        size_t i = order[k];
        Log("measure:   %+6.1f%%  %+6.2f ms  %s", (r[i] / base - 1) * 100, r[i] - base, g_measure.stages[i].label.c_str());
        if (k < 4) {
            snprintf(line, sizeof(line), "%s%s %+.0f%%", k ? ", " : "Costliest: ", g_measure.stages[i].label.c_str(),
                     (r[i] / base - 1) * 100);
            sum += line;
        }
    }
    g_measure.summary = sum + " (all in " DT_PRODUCT ".log)";
}

void MeasureOnFrame(float ms, bool focused) {
    if (!g_measure.running) return;
    if (!focused) return FinishMeasure("stopped: the game lost focus");
    if (!g_enabled) return FinishMeasure("stopped: tweaks switched off (F8)");
    ULONGLONG now = GetTickCount64();
    if (!g_measure.sampling) {
        if (now - g_measure.stageStart >= g_measure.settleMs) g_measure.sampling = true, g_measure.stageStart = now;
        return;
    }
    g_measure.samples.push_back(ms);
    if (now - g_measure.stageStart < g_measure.sampleMs) return;
    float avg = Mean(g_measure.samples);
    g_measure.resultMs[g_measure.stage] = avg;
    Log("measure: %s %.2f ms average (%.1f fps) over %zu frames", g_measure.stages[g_measure.stage].label.c_str(), avg,
        1000.0f / avg, g_measure.samples.size());
    g_measure.samples.clear();
    g_measure.sampling = false;
    g_measure.stageStart = now;
    if (++g_measure.stage < g_measure.stages.size()) {
        ApplyStage(g_measure.stages[g_measure.stage]);
        return;
    }
    g_measure.done = true;
    SummarizeMeasure();
    FinishMeasure("done");
}

void FrameOnPresent(bool focused) {
    static int64_t last = 0;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    float ms = last ? static_cast<float>((t.QuadPart - last) * 1000.0 / g_qpcFreq) : 0;
    last = t.QuadPart;
    if (!ms || !focused || ms > 500) {
        if (g_measure.running && !focused) MeasureOnFrame(ms, false);
        return;
    }
    if (g_frameMs.size() < kFrameRing) g_frameMs.push_back(ms);
    else g_frameMs[g_framePos] = ms;
    g_framePos = (g_framePos + 1) % kFrameRing;
    MeasureOnFrame(ms, true);
}

// ---------------------------------------------------------------- ray tracing activity

bool OnBuildAs(command_list*, acceleration_structure_type, acceleration_structure_build_flags, uint32_t,
               const acceleration_structure_build_input*, resource, uint64_t, resource_view, resource_view,
               acceleration_structure_build_mode) {
    ++g_asBuilds;
    return false;
}

bool OnDispatchRays(command_list*, resource, uint64_t, uint64_t, resource, uint64_t, uint64_t, uint64_t, resource,
                    uint64_t, uint64_t, uint64_t, resource, uint64_t, uint64_t, uint64_t, uint32_t, uint32_t, uint32_t) {
    ++g_rayDispatches;
    return false;
}

void RayTracingOnPresent() {
    static ULONGLONG last = 0, lastLog = 0;
    static uint64_t prevAs = 0, prevRays = 0, loggedRays = ~0ull;
    ULONGLONG now = GetTickCount64();
    if (now - last < 1000) return;
    uint64_t as = g_asBuilds.load(), rays = g_rayDispatches.load();
    g_asPerSec = as - prevAs, g_raysPerSec = rays - prevRays;
    prevAs = as, prevRays = rays, last = now;
    if (now - lastLog >= 10000 && now - g_loadTick < 600000 && rays != loggedRays) {
        lastLog = now, loggedRays = rays;
        Log("ray tracing: %llu acceleration structure builds, %llu DispatchRays so far (%llu / %llu per second)", as,
            rays, g_asPerSec, g_raysPerSec);
    }
}

// ---------------------------------------------------------------- present

color_space g_colorSpace = color_space::unknown;
bool g_keyWasDown = false;

void LiveOnPresent() {
    if (g_live.load() != Live::Idle) return;
    g_live = Live::Searching;
    std::thread(FindManager).detach();
}

void OnPresent(command_queue*, swapchain* sc, const rect*, const rect*, uint32_t, const rect*) {
    const int api = static_cast<int>(sc->get_device()->get_api());
    if (g_api.exchange(api) != api) Log("presenting on %s", ApiName());
    if (sc->get_color_space() != g_colorSpace) {
        g_colorSpace = sc->get_color_space();
        Log("swap chain colour space %s", g_colorSpace == color_space::hdr10_st2084 ? "HDR10 PQ"
                                          : g_colorSpace == color_space::srgb       ? "sRGB"
                                          : g_colorSpace == color_space::scrgb      ? "scRGB"
                                                                                    : "other");
    }
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    const bool focused = pid == GetCurrentProcessId();
    std::lock_guard<std::recursive_mutex> lock(g_state);
    bool down = focused && (GetAsyncKeyState(kToggleHotkey) & 0x8000);
    if (down && !g_keyWasDown) {
        g_enabled = !g_enabled;
        g_dirty = true;
        Log("tweaks %s (F8)", g_enabled ? "on" : "off");
    }
    g_keyWasDown = down;
    FrameOnPresent(focused);
    RayTracingOnPresent();
    LiveOnPresent();
}

// ---------------------------------------------------------------- overlay

constexpr ImVec4 kGood(0.45f, 0.85f, 0.45f, 1.0f);
constexpr ImVec4 kWarn(1.0f, 0.8f, 0.3f, 1.0f);
constexpr ImVec4 kBad(1.0f, 0.45f, 0.35f, 1.0f);
constexpr ImVec4 kAccent(0.4f, 0.75f, 1.0f, 1.0f);

float ControlWidth() { return ImGui::GetFontSize() * 14.0f; }

void Help(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

void ChangedMark(bool changed) {
    if (changed)
        ImGui::TextColored(kAccent, "*");
    else
        ImGui::TextUnformatted(" ");
    ImGui::SameLine();
}

void Note(const ImVec4* color, const char* text) {
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34.0f);
    if (color)
        ImGui::TextColored(*color, "%s", text);
    else
        ImGui::TextDisabled("%s", text);
    ImGui::PopTextWrapPos();
}

// The game's value of <name> as text: the one we saved if we're holding it, else the current
// (the renderer's copy, read from this thread).
std::string GameValue(const View& view, const wchar_t* name) {
    if (!name || g_live.load() != Live::Ready) return {};
    auto it = view.held.find(name);
    if (it != view.held.end()) return Narrow(it->second);
    std::wstring cur;
    return Read(name, cur) ? Narrow(cur) : std::string();
}

std::string ChoiceLabel(const View& view, const Option& o, const Choice& c) {
    if (!strcmp(o.key, "TextureMemory") && c.id == kPoolAuto) {
        const VramRow& row = kVram[AutoVramRow()];
        char buf[96];
        snprintf(buf, sizeof(buf), "Auto (%.3g GB: %s card%s)", row.poolMb / 1024.0, row.card,
                 g_vramMb.load() ? "" : ", not detected");
        return buf;
    }
    if (c.id != 0 || !o.shown) return c.label;
    std::string v = GameValue(view, o.shown);
    return v.empty() ? c.label : std::string(c.label) + " (" + v + ")";
}

bool DrawOption(const View& view, Option& o) {
    bool changed = false;
    const Choice* cur = Chosen(o);
    ChangedMark(o.value != 0);
    ImGui::SetNextItemWidth(ControlWidth());
    std::string preview = cur ? ChoiceLabel(view, o, *cur) : "?";
    if (ImGui::BeginCombo(o.label, preview.c_str())) {
        for (const Choice& c : o.choices) {
            std::string label = ChoiceLabel(view, o, c);
            if (ImGui::Selectable(label.c_str(), c.id == o.value) && c.id != o.value) {
                o.value = c.id;
                changed = true;
                Log("option %s: %s", o.key, c.label);
            }
        }
        ImGui::EndCombo();
    }
    Help(o.tip);
    return changed;
}

// Peak, paper white and UI sliders for the HDR tab; true if one changed.
bool DrawHdrSliders() {
    if (!OptionValue(kHdrKey) || !g_brightReady) return false;
    bool changed = false;
    const ImGuiSliderFlags log = ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp;
    ChangedMark(true);
    ImGui::SetNextItemWidth(ControlWidth());
    changed |= ImGui::SliderInt("Peak brightness", &g_hdrPeak, 400, 10000, "%d nits", log);
    Help("The brightest the image gets: set it to your display's peak (for example from the\n"
         "Windows HDR Calibration app). The game otherwise uses 1000 nits.");
    ChangedMark(true);
    ImGui::SetNextItemWidth(ControlWidth());
    changed |= ImGui::SliderInt("Paper white", &g_hdrPaper, 80, 500, "%d nits", log);
    Help("Overall brightness of the scene. 203 nits is the HDR reference level; raise it in a\n"
         "bright room.");
    ChangedMark(true);
    ImGui::SetNextItemWidth(ControlWidth());
    changed |= ImGui::SliderInt("UI brightness", &g_hdrUi, 80, 1000, "%d nits", log);
    Help("Brightness of menus and subtitles.");
    return changed;
}

bool DrawTab(const View& view, const char* tab) {
    bool changed = false;
    if (!strcmp(tab, kRayTracingTab)) {
        Note(nullptr, "As shipped, the game's ray tracing barely shows: most lights opt out of RT shadows, and RT "
                      "reflections only appear in a few marked areas on mirror-like surfaces. These settings make it "
                      "work. Quality presets don't change them.");
        if (g_rtMaster.load() == 0)
            Note(&kWarn, "Ray Tracing is off in the game's graphics menu, so every RT effect is off. Turn it on there.");
        ImGui::TextDisabled("Ray tracing on the GPU now: %s (%llu ray dispatches per second)",
                            g_raysPerSec ? "yes" : "no", static_cast<unsigned long long>(g_raysPerSec));
    }
    if (!strcmp(tab, kDetailTab))
        Note(nullptr, "Mostly the game's own Cinematic tier, which its menu doesn't offer.");
    if (!strcmp(tab, kCameraTab))
        Note(nullptr, "Film grain, motion blur, chromatic aberration and sharpening are in the game's own graphics "
                      "menu. Quality presets don't change these.");
    if (!strcmp(tab, kHdrTab)) {
        if (!KnownBuild())
            Note(&kWarn, "Not available: a different game version than the one " DT_PRODUCT " was made for.");
        else if (g_ucpHdr.load() == 1)
            Note(&kWarn, "The HDR fix isn't applied: add r.UCP.HDR=0 to Engine.ini (see the README). Until then HDR "
                         "shows a black screen and these do nothing.");
        else if (g_hdrOutput.load() == 0)
            Note(nullptr, "HDR is off in the game: these apply once it is on.");
        ImGui::TextDisabled("Swap chain: %s", g_colorSpace == color_space::hdr10_st2084 ? "HDR10" : "SDR (sRGB)");
    }
    const char* section = nullptr;
    for (Option& o : g_options) {
        if (strcmp(o.tab, tab)) continue;
        if (!section || strcmp(section, o.section)) {
            section = o.section;
            ImGui::SeparatorText(section);
        }
        ImGui::BeginDisabled(!strcmp(tab, kHdrTab) && !KnownBuild());
        changed |= DrawOption(view, o);
        ImGui::EndDisabled();
    }
    if (!strcmp(tab, kHdrTab)) changed |= DrawHdrSliders();
    ImGui::Spacing();
    bool any = false;
    for (const Option& o : g_options) any |= !strcmp(o.tab, tab) && o.value != 0;
    ImGui::BeginDisabled(!any);
    if (ImGui::Button("Set this tab to Game")) {
        for (Option& o : g_options)
            if (!strcmp(o.tab, tab)) o.value = 0;
        changed = true;
    }
    ImGui::EndDisabled();
    return changed;
}

char g_cvarName[128] = "r.RayTracing.Enable.BMEnhancedMode";
char g_cvarValue[64] = "";
std::string g_cvarResult;

void DrawAdvanced(const View& view) {
    const Live live = g_live.load();
    ImGui::SeparatorText("Status");
    ImGui::TextDisabled("Game settings: %s", live == Live::Ready    ? "connected"
                                             : live == Live::Failed ? g_liveError.c_str()
                                                                    : "connecting...");
    ImGui::TextDisabled("Game version: %s", KnownBuild() ? "the tested one" : "different from the tested one");
    ImGui::TextDisabled("Game thread: %s, %llu frames", g_hooked ? "reached" : "not reached",
                        static_cast<unsigned long long>(g_ticks.load()));
    ImGui::TextDisabled("Holding %zu settings; put back %d times after they changed", view.held.size(),
                        g_reapplied.load());
    if (!view.rejected.empty()) {
        std::string list;
        for (const std::string& k : view.rejected) list += (list.empty() ? "" : ", ") + k;
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34.0f);
        ImGui::TextColored(kWarn, "Not taken: %s", list.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::TextDisabled("Graphics API: %s; video memory %llu MB", ApiName(),
                        static_cast<unsigned long long>(g_vramMb.load()));
    ImGui::TextDisabled("Ray tracing: %llu structure builds, %llu ray dispatches per second; menu master switch %d, "
                        "RT reflections allowed %d",
                        static_cast<unsigned long long>(g_asPerSec), static_cast<unsigned long long>(g_raysPerSec),
                        g_rtMaster.load(), g_allowRtReflections.load());
    ImGui::TextDisabled("HDR: output %d, r.UCP.HDR %d, swap chain %s", g_hdrOutput.load(), g_ucpHdr.load(),
                        g_colorSpace == color_space::hdr10_st2084 ? "HDR10" : g_colorSpace == color_space::srgb ? "sRGB" : "other");
    if (KnownBuild())
        ImGui::TextDisabled("HDR brightness: bUseHDRDisplayOutput %d, HDRDisplayOutputNits %d, tone curve built for %d "
                            "nits",
                            view.userHdrFlag, view.userHdrNits, view.toneCurveNits);
    ImGui::TextDisabled("Log: " DT_PRODUCT ".log next to the game's exe");
    if (live != Live::Ready) return;

    ImGui::SeparatorText("Probe");
    if (ImGui::Button("Write all settings to file")) g_pendingProbe = "manual";
    Help("Writes the game's value of every ray tracing, HDR, shadow, detail and\n"
         "post-processing setting to " DT_PRODUCT "-probe.txt next to the game's exe.\n"
         "Do it in a scene (not a menu) for the values the game really plays with.");
    if (!view.probeResult.empty()) ImGui::TextDisabled("%s", view.probeResult.c_str());

    ImGui::SeparatorText("Console variable");
    ImGui::SetNextItemWidth(ControlWidth() * 1.6f);
    ImGui::InputText("Name", g_cvarName, sizeof(g_cvarName));
    std::wstring name(g_cvarName, g_cvarName + strlen(g_cvarName));
    std::wstring value;
    VarInfo v = Lookup(name);
    if (!v.var)
        ImGui::TextDisabled("Not found");
    else
        ImGui::TextDisabled("%s, now %s (set by 0x%02x)%s", cvars::TypeName(v.type),
                            Read(name, value) ? Narrow(value).c_str() : "?", cvars::Priority(v.var),
                            view.held.count(name) ? " (held by an option)" : "");
    ImGui::SetNextItemWidth(ControlWidth() * 0.6f);
    ImGui::InputText("##value", g_cvarValue, sizeof(g_cvarValue));
    ImGui::SameLine();
    ImGui::BeginDisabled(!v.var || !g_cvarValue[0]);
    if (ImGui::Button("Set once")) {
        std::wstring val(g_cvarValue, g_cvarValue + strlen(g_cvarValue));
        g_pendingConsole.push_back({name, val});  // set by the game thread at its next frame
        g_cvarResult = "Sent (see the log; lasts until an option changes it or the game restarts)";
    }
    ImGui::EndDisabled();
    Help("For experiments: sets the value at the game's next frame, at console priority.\n"
         "It isn't saved.");
    if (!g_cvarResult.empty()) ImGui::TextDisabled("%s", g_cvarResult.c_str());
}

void DrawOverlay(effect_runtime*) {
    std::lock_guard<std::recursive_mutex> lock(g_state);  // the game thread skips a frame meanwhile
    const View view = CopyView();
    ImGui::TextDisabled(DT_PRODUCT " " DT_VERSION);
    ImGui::SameLine();
    const Live live = g_live.load();
    if (live == Live::Failed)
        ImGui::TextColored(kBad, "  Can't reach the game's settings (Advanced tab)");
    else if (live != Live::Ready)
        ImGui::TextDisabled("  Connecting to the game's settings...");
    else if (!g_enabled)
        ImGui::TextColored(kWarn, "  Tweaks off (F8)");
    else
        ImGui::TextColored(kGood, "  Tweaks on (F8 compares)");
    ImGui::TextDisabled("* = changed from the game's value. Hover (?) for details. Changes apply at once.");

    bool changed = false;
    const bool measuring = g_measure.running;

    ImGui::Spacing();
    ImGui::BeginDisabled(measuring || live != Live::Ready);
    const int current = CurrentPreset();
    ChangedMark(current != 0);
    ImGui::SetNextItemWidth(ControlWidth());
    if (ImGui::BeginCombo("Quality preset", current >= 0 ? kPresets[current].label : "Custom")) {
        for (int i = 0; i < kPresetCount; ++i) {
            std::string item = kPresets[i].label;
            if (*kPresets[i].cost) item += std::string(" (") + kPresets[i].cost + ")";
            if (ImGui::Selectable(item.c_str(), i == current) && i != current) {
                ApplyPreset(i);
                changed = true;
                Log("preset: %s", kPresets[i].label);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kPresets[i].tip);
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    Help("Sets the quality options together: ray tracing quality and the Detail tab. The\n"
         "ray tracing fix and HDR brightness aren't part of it. Measure preset costs checks\n"
         "them on your PC. Changing an option afterwards shows Custom.");
    ImGui::SameLine();
    float ms = Mean(g_frameMs);
    if (ms > 0) ImGui::TextDisabled("   %.1f ms (%.0f fps)", ms, 1000.0f / ms);

    if (measuring) {
        ImGui::TextColored(kAccent, "Measuring %s (%zu of %zu)... keep the camera still.",
                           g_measure.stages[g_measure.stage].label.c_str(), g_measure.stage + 1, g_measure.stages.size());
        ImGui::SameLine();
        if (ImGui::SmallButton("Stop")) FinishMeasure("stopped");
    } else {
        ImGui::BeginDisabled(live != Live::Ready || !g_enabled);
        if (ImGui::SmallButton("Measure preset costs")) StartMeasure(false);
        ImGui::EndDisabled();
        Help("Runs each preset for 14 s (6 s to settle, 8 s measured) and shows how much longer\n"
             "each frame takes than with Game. Stand still in a typical scene and leave the mouse\n"
             "alone. Your options come back afterwards.");
        ImGui::SameLine();
        ImGui::BeginDisabled(live != Live::Ready || !g_enabled);
        if (ImGui::SmallButton("Measure each option")) StartMeasure(true);
        ImGui::EndDisabled();
        Help("Runs every option on its own at each of its choices (11 s each, several minutes in\n"
             "all; Camera and HDR aren't measured, Ray tracing only while the game traces rays) and writes\n"
             "how much each one costs to " DT_PRODUCT ".log, costliest first.");
        if (g_measure.done) {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34.0f);
            ImGui::TextDisabled("%s", g_measure.summary.c_str());
            ImGui::PopTextWrapPos();
        } else if (!g_measure.note.empty()) {
            ImGui::TextColored(kWarn, "Measurement %s.", g_measure.note.c_str());
        }
    }
    ImGui::Spacing();

    ImGui::BeginDisabled(measuring);
    if (ImGui::BeginTabBar("tabs")) {
        std::vector<const char*> tabs;  // in the order the options list them
        for (const Option& o : g_options)
            if (std::none_of(tabs.begin(), tabs.end(), [&](const char* t) { return !strcmp(t, o.tab); }))
                tabs.push_back(o.tab);
        for (const char* tab : tabs)
            if (ImGui::BeginTabItem(tab)) {
                changed |= DrawTab(view, tab);
                ImGui::EndTabItem();
            }
        if (ImGui::BeginTabItem("Advanced")) {
            DrawAdvanced(view);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndDisabled();
    if (changed) {
        SaveSettings();
        g_dirty = true;
    }
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            if (!reshade::register_addon(module)) return FALSE;
            g_loadTick = GetTickCount64();
            {
                LARGE_INTEGER f;
                QueryPerformanceFrequency(&f);
                g_qpcFreq = f.QuadPart;
            }
            OpenLog();
            LoadSettings();
            reshade::register_event<reshade::addon_event::init_device>(&OnInitDevice);
            reshade::register_event<reshade::addon_event::present>(&OnPresent);
            reshade::register_event<reshade::addon_event::build_acceleration_structure>(&OnBuildAs);
            reshade::register_event<reshade::addon_event::dispatch_rays>(&OnDispatchRays);
            reshade::register_overlay(DT_PRODUCT, &DrawOverlay);
            break;
        case DLL_PROCESS_DETACH:
            if (g_hooked) cvars::UnhookSinks(g_manager);
            reshade::unregister_addon(module);
            if (g_log) fclose(g_log);
            break;
    }
    return TRUE;
}
