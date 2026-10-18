// UntilDawnTweaks (formerly UntilDawnRT): a ReShade add-on for Until Dawn (2024 remake, UE 5,
// DirectX 12) that makes its ray tracing show and its HDR usable. See docs/FINDINGS.md.
//
// Saved settings (ReShade.ini, applied at every start, on by default):
//   Ray tracing    RT shadows on all lights (r.RayTracing.Enable.BMEnhancedMode 2) and RT
//                  reflections everywhere (r.RayTracing.Reflections, MaxRoughness), which the
//                  game otherwise limits to designer-marked areas and mirror-like surfaces
//   HDR brightness peak, paper white and UI brightness, replacing the game's HDR sliders,
//                  which do nothing once its Uniform Colour Pipeline is off (r.UCP.HDR=0 in
//                  Engine.ini, the fix for the black screen)
//
// Advanced (collapsed in the overlay): session-only switches for the RT and HDR cvars, the
// renderer's values of the related cvars, the swap chain's format and colour space, and the
// GPU work of the last second (draws, compute dispatches, acceleration structure builds and
// DispatchRays by pipeline).
//
// What it records, next to the game executable:
//   UntilDawnTweaks.log  the census (every cvar matching kPrefixes: type, kind, main and
//                        renderer values, priority), the layout check, every override and
//                        release, later snapshots' changes (e.g. after a menu change), every
//                        ray tracing pipeline the game creates, and one-second work logs

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "cvars.hpp"
#include "version.h"

extern "C" __declspec(dllexport) const char* NAME = UDRT_PRODUCT;
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Makes " UDRT_GAME "'s ray tracing show and its HDR usable: RT shadows and reflections everywhere, HDR peak "
    "and paper white.";

namespace {

using namespace reshade::api;

// ---------------------------------------------------------------- logging

std::mutex g_mutex;  // present (RHI thread) and overlay (same thread, but be safe)
FILE* g_log = nullptr;

void Log(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (g_log) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(g_log, "%02u:%02u:%02u.%03u %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, buf);
        fflush(g_log);
    }
}

void OpenLog() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    std::wstring dir(path, n);
    dir.resize(dir.find_last_of(L"\\/") + 1);
    std::wstring log = dir + L"" UDRT_PRODUCT ".log";
    std::wstring prev = dir + L"" UDRT_PRODUCT ".prev.log";
    // ReShade may load and unload the add-on more than once at startup, so only the
    // first load in a process starts a new log.
    if (GetEnvironmentVariableW(L"UDRT_LOG_OPEN", nullptr, 0) == 0) {
        SetEnvironmentVariableW(L"UDRT_LOG_OPEN", L"1");
        MoveFileExW(log.c_str(), prev.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    g_log = _wfopen(log.c_str(), L"a");
}

std::string Narrow(const wchar_t* s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
    return out;
}

// ---------------------------------------------------------------- executable image

// The build the layouts were read from (Bates-Win64-Shipping.exe with its PDB).
constexpr uint32_t kKnownTimestamp = UDRT_KNOWN_PE_TIMESTAMP;

const uint8_t* g_base = nullptr;
uint32_t g_imageSize = 0;
uint32_t g_timestamp = 0;
cvars::Section g_text, g_rdata, g_data;

void LogGameInfo() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    g_base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
    g_imageSize = nt->OptionalHeader.SizeOfImage;
    g_timestamp = nt->FileHeader.TimeDateStamp;
    cvars::FindSection(g_base, ".text", g_text);
    cvars::FindSection(g_base, ".rdata", g_rdata);
    cvars::FindSection(g_base, ".data", g_data);
    Log("%s %s", UDRT_PRODUCT, UDRT_VERSION);
    Log("exe: %s", Narrow(path).c_str());
    Log("exe: PE timestamp 0x%08x (%s), image size 0x%x, base %p", g_timestamp,
        g_timestamp == kKnownTimestamp ? "the build the layouts come from" : "a different build than the layouts",
        g_imageSize, g_base);
}

bool InSection(const cvars::Section& s, const void* p) {
    auto b = static_cast<const uint8_t*>(p);
    return s.begin && b >= s.begin && b < s.begin + s.size;
}

// "exe+0x1234 .data", "heap" for addresses outside the image.
std::string Where(const void* p) {
    auto b = static_cast<const uint8_t*>(p);
    if (!g_base || b < g_base || b >= g_base + g_imageSize) return "heap";
    const char* sec = InSection(g_text, p) ? " .text" : InSection(g_rdata, p) ? " .rdata" : InSection(g_data, p) ? " .data" : "";
    char buf[48];
    snprintf(buf, sizeof(buf), "exe+0x%llx%s", static_cast<unsigned long long>(b - g_base), sec);
    return buf;
}

// ---------------------------------------------------------------- console manager

void* g_manager = nullptr;
std::string g_status = "waiting for the game";

// The singleton's address once (a scan of .text), then waits for the engine to create the
// manager. Called every frame until it succeeds.
bool FindManager() {
    static void** singleton = nullptr;
    static bool searched = false;
    if (g_manager) return true;
    if (!searched) {
        searched = true;
        int votes = 0, matches = 0;
        singleton = cvars::FindConsoleManagerSingleton(g_base, &votes, &matches);
        if (!singleton) {
            g_status = "IConsoleManager::Singleton not found";
            Log("cvars: %s (no lea of L\"r.ScreenPercentage\" after mov rcx,[rip+x])", g_status.c_str());
        } else {
            Log("cvars: IConsoleManager::Singleton at %s (%d of %d matches agree)", Where(singleton).c_str(), votes,
                matches);
        }
    }
    if (!singleton || !*singleton) return false;
    g_manager = *singleton;
    Log("cvars: manager %p, vtable %s", g_manager, Where(*static_cast<void**>(g_manager)).c_str());
    return true;
}

// ---------------------------------------------------------------- census and layout check

// Names that start with these are looked up; the full names come from the exe's own
// strings, so nothing is hard-coded.
const wchar_t* const kPrefixes[] = {
    L"r.RayTracing", L"r.BM.", L"r.bm.", L"r.ReflectionMethod", L"r.DynamicGlobalIlluminationMethod",
    L"r.Shadow.Virtual.Enable", L"r.Lumen.Reflections.Allow", L"r.Lumen.HardwareRayTracing", L"r.D3D12.DXR",
    L"r.SSR.Quality", L"r.AmbientOcclusion", L"r.Shadow.Denoiser", L"r.Reflections.Denoiser",
    L"r.HDR", L"r.UCP.", L"r.AllowHDR", L"r.Tonemapper",
};

std::vector<std::wstring> CollectNames() {
    std::set<std::wstring> names;
    const uint8_t* p = g_rdata.begin;
    const size_t n = g_rdata.size & ~size_t(1);
    for (size_t i = 2; i + 4 <= n; i += 2) {
        if (p[i] != 'r' || p[i + 1] != 0 || p[i + 2] != '.' || p[i + 3] != 0) continue;
        if (p[i - 2] != 0 || p[i - 1] != 0) continue;  // must start a string
        std::wstring s;
        bool ok = true;
        for (size_t j = i; j + 1 < n; j += 2) {
            wchar_t c = static_cast<wchar_t>(p[j] | (p[j + 1] << 8));
            if (c == 0) break;
            if (!(iswalnum(c) || c == L'.' || c == L'_') || s.size() > 96) {
                ok = false;
                break;
            }
            s.push_back(c);
        }
        if (!ok) continue;
        for (const wchar_t* prefix : kPrefixes)
            if (s.compare(0, wcslen(prefix), prefix) == 0) {
                names.insert(s);
                break;
            }
    }
    return {names.begin(), names.end()};
}

using GetFloatFn = float(__fastcall*)(void* self);

// The engine's own GetFloat (+0x98). Off the game thread it returns the renderer's copy,
// which is what the layout check compares against.
bool EngineFloat(void* var, float& out) {
    __try {
        out = cvars::Virtual<GetFloatFn>(var, cvars::kGetFloat)(var);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct Reading {
    bool ok = false;
    cvars::Storage st;
    double main = 0, render = 0;
    uint32_t priority = 0;
};

Reading Read(void* var) {
    Reading r;
    r.ok = cvars::GetStorage(var, r.st) && cvars::ReadValue(r.st, r.st.main, r.main) &&
           cvars::ReadValue(r.st, r.st.render, r.render);
    r.priority = cvars::Flags(var) >> 24;
    return r;
}

std::string Text(const Reading& r) {
    if (!r.ok) return "unreadable";
    char buf[128];
    snprintf(buf, sizeof(buf), "%s %s | main %g / renderer %g | priority 0x%02x", cvars::TypeName(r.st.type),
             r.st.ref ? "ref" : "data", r.main, r.render, r.priority);
    return buf;
}

struct Snap {
    std::wstring name;
    std::string text;
};
std::vector<Snap> g_last;
int g_snapshots = 0;
int g_layoutOk = 0, g_layoutBad = 0, g_layoutSkipped = 0;

void Snapshot() {
    if (!FindManager()) return;
    std::vector<Snap> now;
    bool first = g_snapshots == 0;
    if (first) Log("census: name | type kind | main / renderer | priority | object, vtable | engine GetFloat");
    for (const std::wstring& name : CollectNames()) {
        void* var = cvars::FindVariable(g_manager, name.c_str());
        if (!var) continue;
        Reading r = Read(var);
        now.push_back({name, Text(r)});
        if (!first) continue;
        float engine = 0;
        const char* check = "";
        if (!r.ok) {
            ++g_layoutSkipped;
        } else if (EngineFloat(var, engine)) {
            bool match = fabs(engine - r.render) < 1e-6 * (1 + fabs(r.render));
            (match ? g_layoutOk : g_layoutBad)++;
            check = match ? "matches" : "MISMATCH";
        }
        void* vtable = nullptr;
        cvars::SafeCopy(&vtable, var, 8);
        Log("  %-58s | %s | %s, %s | %g %s", Narrow(name.c_str()).c_str(), now.back().text.c_str(), Where(var).c_str(),
            Where(vtable).c_str(), engine, check);
    }
    if (first) {
        Log("census: %zu cvars; layout check: renderer value read by us vs the engine's GetFloat: %d match, %d "
            "mismatch, %d not checked (strings)",
            now.size(), g_layoutOk, g_layoutBad, g_layoutSkipped);
    } else {
        std::map<std::wstring, std::string> before;
        for (const Snap& s : g_last) before[s.name] = s.text;
        int changes = 0;
        Log("snapshot %d: changes since the previous one", g_snapshots + 1);
        for (const Snap& s : now) {
            auto it = before.find(s.name);
            if (it != before.end() && it->second == s.text) continue;
            ++changes;
            Log("  %s: %s -> %s", Narrow(s.name.c_str()).c_str(), it == before.end() ? "(new)" : it->second.c_str(),
                s.text.c_str());
        }
        Log("snapshot %d: %d changed", g_snapshots + 1, changes);
    }
    g_last = std::move(now);
    ++g_snapshots;
    char buf[128];
    snprintf(buf, sizeof(buf), "%zu cvars, %d snapshot%s; layout %d ok / %d mismatch", g_last.size(), g_snapshots,
             g_snapshots == 1 ? "" : "s", g_layoutOk, g_layoutBad);
    g_status = buf;
}

// ---------------------------------------------------------------- ray tracing work

std::mutex g_rtMutex;  // pipeline events come from the game's pipeline compile threads
uint32_t g_rtPipelines = 0;
std::map<uint64_t, std::string> g_pipelineNames;  // pipeline handle -> its shaders' export names
std::map<command_list*, uint64_t> g_boundRt;      // ray tracing pipeline bound on each command list
std::set<std::string> g_raygenNames;               // ray generation shader names seen in libraries
struct DispatchStat {
    uint32_t count = 0, width = 0, height = 0;
};
std::map<uint64_t, DispatchStat> g_frameDispatches;  // this frame, by pipeline
std::map<uint64_t, DispatchStat> g_lastDispatches;   // the last complete frame
bool g_logDispatches = false;                        // log the next complete frame

// Printable strings in a DXIL container's RDAT part: function and export names.
std::vector<std::string> RdatNames(const void* code, size_t size) {
    std::vector<std::string> names;
    auto b = static_cast<const uint8_t*>(code);
    if (!b || size < 32 || memcmp(b, "DXBC", 4) != 0) return names;
    uint32_t parts;
    memcpy(&parts, b + 28, 4);
    for (uint32_t i = 0; i < parts && 32 + 4 * (i + 1) <= size; ++i) {
        uint32_t off;
        memcpy(&off, b + 32 + 4 * i, 4);
        if (off + 8 > size || memcmp(b + off, "RDAT", 4) != 0) continue;
        uint32_t partSize;
        memcpy(&partSize, b + off + 4, 4);
        const uint8_t* p = b + off + 8;
        const uint8_t* end = p + (off + 8 + partSize <= size ? partSize : size - off - 8);
        std::string cur;
        for (; p < end; ++p) {
            if (*p >= 0x20 && *p < 0x7f) {
                cur.push_back(static_cast<char>(*p));
                continue;
            }
            if (*p == 0 && cur.size() >= 4 && std::find(names.begin(), names.end(), cur) == names.end())
                names.push_back(cur);
            cur.clear();
        }
    }
    return names;
}

// Adds the plain (unmangled) names to a comma-separated list, skipping repeats.
void AddNames(std::string& list, const std::vector<std::string>& names) {
    for (const std::string& n : names) {
        if (n.find_first_of("?@") != std::string::npos || list.find(n) != std::string::npos) continue;
        if (list.size() > 160) {
            if (list.compare(list.size() - 3, 3, "...") != 0) list += ", ...";
            return;
        }
        list += list.empty() ? n : ", " + n;
    }
}

bool IsShaderSubobject(pipeline_subobject_type t) {
    return t >= pipeline_subobject_type::raygen_shader && t <= pipeline_subobject_type::callable_shader;
}

void OnInitPipeline(device*, pipeline_layout, uint32_t count, const pipeline_subobject* subobjects, pipeline pipe) {
    bool rt = false;
    for (uint32_t i = 0; i < count; ++i)
        rt |= IsShaderSubobject(subobjects[i].type) || subobjects[i].type == pipeline_subobject_type::libraries ||
              subobjects[i].type == pipeline_subobject_type::shader_groups;
    if (!rt) return;
    std::lock_guard lock(g_rtMutex);
    std::string names;
    uint32_t shaders = 0, libraries = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const pipeline_subobject& s = subobjects[i];
        if (IsShaderSubobject(s.type)) {
            auto descs = static_cast<const shader_desc*>(s.data);
            for (uint32_t j = 0; j < s.count; ++j) {
                if (descs[j].entry_point) AddNames(names, {descs[j].entry_point});
                std::vector<std::string> rdat = RdatNames(descs[j].code, descs[j].code_size);
                AddNames(names, rdat);
                for (const std::string& n : rdat)
                    if (n.find("RGS") != std::string::npos && n.find_first_of("?@") == std::string::npos)
                        g_raygenNames.insert(n);
            }
            shaders += s.count;
        } else if (s.type == pipeline_subobject_type::libraries) {
            auto libs = static_cast<const pipeline*>(s.data);
            for (uint32_t j = 0; j < s.count; ++j) {
                auto it = g_pipelineNames.find(libs[j].handle);
                if (it != g_pipelineNames.end()) AddNames(names, {it->second});
            }
            libraries += s.count;
        }
    }
    ++g_rtPipelines;
    g_pipelineNames[pipe.handle] = names;
    Log("rt pipeline %016llx: %u shader libraries, %u collections: %s", static_cast<unsigned long long>(pipe.handle),
        shaders, libraries, names.c_str());
}

void OnBindPipeline(command_list* cmd, pipeline_stage stages, pipeline pipe) {
    if ((static_cast<uint32_t>(stages) & static_cast<uint32_t>(pipeline_stage::ray_tracing_shader)) == 0) return;
    std::lock_guard lock(g_rtMutex);
    g_boundRt[cmd] = pipe.handle;
}

// The pipelines the game dispatches are linked from the libraries above with AddToStateObject,
// which ReShade doesn't report, so they have no name. The first time one is dispatched, ask
// the state object which of the ray generation exports seen in the libraries it can find.
// (If the game renamed the exports when linking, none match and it stays unnamed.)
void NameFromExports(uint64_t handle) {
    if (!handle || g_pipelineNames.count(handle)) return;
    std::string names;
    auto unknown = reinterpret_cast<IUnknown*>(handle);
    ID3D12StateObjectProperties* props = nullptr;
    if (SUCCEEDED(unknown->QueryInterface(IID_PPV_ARGS(&props)))) {
        for (const std::string& n : g_raygenNames) {
            std::wstring wide(n.begin(), n.end());
            if (props->GetShaderIdentifier(wide.c_str())) names += names.empty() ? n : ", " + n;
        }
        props->Release();
    }
    g_pipelineNames[handle] = names;
    Log("rt pipeline %016llx (linked, first dispatch): ray generation %s", static_cast<unsigned long long>(handle),
        names.empty() ? "not found among the library exports" : names.c_str());
}

bool OnDispatchRays(command_list* cmd, resource, uint64_t, uint64_t, resource, uint64_t, uint64_t, uint64_t, resource,
                    uint64_t, uint64_t, uint64_t, resource, uint64_t, uint64_t, uint64_t, uint32_t width, uint32_t height,
                    uint32_t) {
    std::lock_guard lock(g_rtMutex);
    uint64_t bound = g_boundRt[cmd];
    NameFromExports(bound);
    DispatchStat& s = g_frameDispatches[bound];
    ++s.count;
    s.width = width;
    s.height = height;
    return false;
}

// Sanity counters, per frame: if draws and compute dispatches count but rays don't, the
// hooks work and the game isn't tracing; acceleration structure builds show whether it
// keeps a ray tracing scene at all.
struct WorkCounts {
    uint32_t draws = 0, dispatches = 0, blasBuilds = 0, tlasBuilds = 0;
};
std::atomic<uint32_t> g_draws{0}, g_computeDispatches{0}, g_blasBuilds{0}, g_tlasBuilds{0};
WorkCounts g_lastWork;

bool OnDraw(command_list*, uint32_t, uint32_t, uint32_t, uint32_t) {
    g_draws.fetch_add(1, std::memory_order_relaxed);
    return false;
}

bool OnDrawIndexed(command_list*, uint32_t, uint32_t, uint32_t, int32_t, uint32_t) {
    g_draws.fetch_add(1, std::memory_order_relaxed);
    return false;
}

bool OnDispatch(command_list*, uint32_t, uint32_t, uint32_t) {
    g_computeDispatches.fetch_add(1, std::memory_order_relaxed);
    return false;
}

bool OnBuildAccelerationStructure(command_list*, acceleration_structure_type type, acceleration_structure_build_flags,
                                  uint32_t, const acceleration_structure_build_input*, resource, uint64_t,
                                  resource_view, resource_view, acceleration_structure_build_mode) {
    (type == acceleration_structure_type::top_level ? g_tlasBuilds : g_blasBuilds)
        .fetch_add(1, std::memory_order_relaxed);
    return false;
}

std::string PipelineName(uint64_t handle) {
    auto it = g_pipelineNames.find(handle);
    return it == g_pipelineNames.end() || it->second.empty() ? "unnamed" : it->second;
}

void LogRtState();

// The counts are totals over one second, not one present: with frame generation (FSR 3's
// swap chain) every other present is an interpolated frame with no rendering work of its own.
constexpr uint64_t kWindowMs = 1000;
uint64_t g_windowStart = 0;
uint32_t g_windowPresents = 0, g_lastPresents = 0;

// Called at every present: when a second has passed, its counts become the last window's.
void EndFrameDispatches() {
    std::lock_guard lock(g_rtMutex);
    ++g_windowPresents;
    uint64_t now = GetTickCount64();
    if (now - g_windowStart < kWindowMs) return;
    g_windowStart = now;
    g_lastPresents = g_windowPresents;
    g_windowPresents = 0;
    g_lastDispatches.swap(g_frameDispatches);
    g_frameDispatches.clear();
    g_lastWork = {g_draws.exchange(0), g_computeDispatches.exchange(0), g_blasBuilds.exchange(0), g_tlasBuilds.exchange(0)};
    if (!g_logDispatches) return;
    g_logDispatches = false;
    LogRtState();
    Log("work in one second (%u presents): %u draws, %u compute dispatches, %u BLAS builds, %u TLAS builds",
        g_lastPresents, g_lastWork.draws, g_lastWork.dispatches, g_lastWork.blasBuilds, g_lastWork.tlasBuilds);
    Log("ray dispatches in one second: %zu pipelines", g_lastDispatches.size());
    for (auto& [handle, s] : g_lastDispatches)
        Log("  pipeline %016llx: %u dispatches, last %ux%u (%s)", static_cast<unsigned long long>(handle), s.count,
            s.width, s.height, PipelineName(handle).c_str());
}

// ---------------------------------------------------------------- overrides

// A control forces one cvar through IConsoleVariable::Set at ECVF_SetByConsole, which
// outranks the game's menu (GameSetting) and ini defaults, then mirrors the value to the
// renderer. Releasing restores the original priority and value, so the game is in charge
// again.

struct Choice {
    int value;
    const char* label;
};

enum class Group { RayTracing, Hdr, Brightness };  // Brightness: driven by the HDR brightness sliders

struct Override {
    Group group;
    const wchar_t* cvar;
    const char* label;
    const char* help;
    std::vector<Choice> choices;  // empty: a float slider from min to max
    float min = 0, max = 1;

    bool wanted = false;  // the user wants it forced
    float value = 0;      // to this
    bool active = false;  // forced right now
    float applied = 0;
    void* var = nullptr;
    bool missing = false;
    std::wstring original;  // main value before we forced it
    uint32_t originalFlags = 0;
    uint32_t repairs = 0;
    bool driven = false;  // set by the saved Ray tracing settings: locked in the switches table
};

std::vector<Override> g_overrides;

void DefineOverrides() {
    g_overrides = {
        {Group::RayTracing, L"r.RayTracing.ForceAllRayTracingEffects", "RT master switch",
         "What the game's Ray Tracing menu option sets: 0 with it off, which turns every RT effect off whatever "
         "the other switches say.\n-1: each effect's own setting decides (the menu option on)\n0: all RT effects "
         "off\n1: all RT effects on",
         {{-1, "-1 Per effect"}, {0, "0 All off"}, {1, "1 All on"}}},
        {Group::RayTracing, L"r.RayTracing.Enable.BMEnhancedMode", "Enhanced RT mode",
         "Ballistic Moon's gate on which lights may cast ray traced shadows (and on RT in reflections and sky light).\n"
         "0: lights marked Disabled (the project default) and Enhanced-only get none\n"
         "1: Enhanced-only lights get RT shadows too\n"
         "2: every light, whatever its setting (the help text warns: very slow)",
         {{0, "0 Off"}, {1, "1 Enhanced"}, {2, "2 Everywhere"}}},
        {Group::RayTracing, L"r.RayTracing.AmbientOcclusion", "RT ambient occlusion",
         "UE's ray traced ambient occlusion (the menu's RT AO option sets it).\n-1: post-process volume decides\n"
         "0: off\n1: on",
         {{-1, "-1 Volume"}, {0, "0 Off"}, {1, "1 On"}}},
        {Group::RayTracing, L"r.BM.AllowRTReflections", "Allow RT reflections",
         "Ballistic Moon's reflection setting.\n0: always off\n1: on in areas where supported",
         {{0, "0 Off"}, {1, "1 Where supported"}}},
        {Group::RayTracing, L"r.RayTracing.Reflections", "RT reflections",
         "UE's ray traced reflections switch. The game sets it every frame from code: 1 only inside post-process "
         "volumes marked for RT reflections, and only if Allow RT reflections is 1; 0 elsewhere.\n"
         "-1: post-process volume decides\n0: off\n1: on",
         {{-1, "-1 Volume"}, {0, "0 Off"}, {1, "1 On"}}},
        {Group::RayTracing, L"r.RayTracing.Reflections.MaxRoughness", "Reflections max roughness",
         "Surfaces rougher than this skip ray traced reflections. The project ships 0.01 (mirrors only); UE's default "
         "is 0.6.",
         {}, 0.0f, 1.0f},
        {Group::RayTracing, L"r.Shadow.Virtual.Enable", "Virtual shadow maps",
         "UE's virtual shadow maps (the game holds them on from code). Turning them off tests whether they hide "
         "ray traced shadows.",
         {{0, "0 Off"}, {1, "1 On"}}},
        {Group::Hdr, L"r.UCP.HDR", "Uniform Colour Pipeline HDR",
         "With HDR on, the game swaps UE's ACES PQ output (device 3/4) for its own Uniform Colour Pipeline "
         "(device 11/12) when this is 1 (the default). The choice is made when HDR is switched on, so after changing "
         "this, toggle HDR in the game menu, or force the output device below.",
         {{0, "0 Off"}, {1, "1 On"}}},
        {Group::Hdr, L"r.HDR.Display.OutputDevice", "Output device",
         "How the tonemapper encodes the final image.\n"
         "0 sRGB, 1 Rec709 (SDR)\n"
         "3 ACES 1000 nit PQ, 4 ACES 2000 nit PQ (HDR10 swap chain)\n"
         "5 ACES 1000 nit scRGB, 6 ACES 2000 nit scRGB (FP16 swap chain)\n"
         "10 UCP SDR, 11 UCP HDR (PQ), 12 UCP scRGB HDR\n"
         "With HDR on, the game picks 11.",
         {{0, "0 sRGB"}, {3, "3 PQ 1000"}, {4, "4 PQ 2000"}, {5, "5 scRGB"}, {10, "10 UCP SDR"}, {11, "11 UCP HDR"},
          {12, "12 UCP scRGB"}}},
        {Group::Hdr, L"r.HDR.Display.ColorGamut", "Colour gamut",
         "Gamut of the output.\n0 Rec709/sRGB, 1 DCI-P3, 2 Rec2020 (what the game uses with HDR on)",
         {{0, "0 Rec709"}, {1, "1 P3"}, {2, "2 Rec2020"}}},
        {Group::Hdr, L"r.HDR.UI.CompositeMode", "UI composite mode",
         "How the UI is combined with the scene in HDR.\n0: standard compositing\n"
         "1: shader pass to improve HDR blending (UE's default)",
         {{0, "0 Standard"}, {1, "1 Shader pass"}}},
        {Group::Hdr, L"r.HDRCalibrateScreen.ShowGame", "Calibration: show game",
         "The HDR calibration screen's switch for drawing the game behind it.\n0: off\n1: on",
         {{0, "0 Off"}, {1, "1 On"}}},
        {Group::Hdr, L"r.HDRCalibrateScreen.Mode", "Calibration screen",
         "The HDR calibration screen. If it is stuck on, it may hide the game.\n0: off\n1: max luminance\n"
         "2: min luminance",
         {{0, "0 Off"}, {1, "1 Max lum"}, {2, "2 Min lum"}}},
        {Group::Hdr, L"r.UCP.ExposureAdjust", "UCP exposure",
         "The menu's HDR brightness (0-100 %) mapped to 0-10. Default 5.", {}, 0.0f, 10.0f},
        {Group::Hdr, L"r.UCP.HDR.BMMaxLuminanceOverride", "UCP max luminance (PQ)",
         "The menu's HDR max luminance (0-100 %) mapped to PQ 0.5-1.0. Default 0.75 (about 1000 nits).", {}, 0.0f,
         1.0f},
        {Group::Brightness, L"r.HDR.Display.MaxLuminance", "Peak brightness", "", {}, 100.0f, 10000.0f},
        {Group::Brightness, L"r.HDR.Display.MidLuminance", "Mid grey", "", {}, 1.0f, 200.0f},
        {Group::Brightness, L"r.HDR.UI.Luminance", "UI brightness", "", {}, 50.0f, 1000.0f},
    };
}

void* Resolve(Override& o) {
    if (o.var || o.missing) return o.var;
    o.var = cvars::FindVariable(g_manager, o.cvar);
    if (!o.var) {
        o.missing = true;
        Log("override: %ls is not a cvar in this build", o.cvar);
    }
    return o.var;
}

std::wstring MainText(void* var) {
    Reading r = Read(var);
    wchar_t buf[32] = L"0";
    if (r.ok && r.st.type == cvars::Type::Float)
        swprintf(buf, 32, L"%g", r.main);
    else if (r.ok)
        swprintf(buf, 32, L"%d", static_cast<int>(r.main));
    return buf;
}

std::wstring ValueText(const Override& o, float v) {
    wchar_t buf[32];
    if (o.choices.empty())
        swprintf(buf, 32, L"%g", v);
    else
        swprintf(buf, 32, L"%d", static_cast<int>(lroundf(v)));
    return buf;
}

void ApplyOverrides() {
    for (Override& o : g_overrides) {
        if (o.wanted == o.active && (!o.wanted || o.value == o.applied)) continue;
        if (!Resolve(o)) {
            o.wanted = false;
            continue;
        }
        std::string before = Text(Read(o.var));
        if (o.wanted) {
            if (!o.active) {  // first time: remember what the game had
                o.originalFlags = cvars::Flags(o.var);
                o.original = MainText(o.var);
            }
            std::wstring text = ValueText(o, o.value);
            bool ok = cvars::Set(o.var, text.c_str(), cvars::kSetByConsole) && cvars::MirrorToRenderer(o.var);
            Log("override: %ls = %ls%s: %s -> %s", o.cvar, text.c_str(), ok ? "" : " (Set or mirror failed)",
                before.c_str(), Text(Read(o.var)).c_str());
            o.active = ok;
            o.applied = o.value;
            if (!ok) o.wanted = false;
        } else {
            // Back to the game's priority first, or Set would refuse the lower one.
            cvars::Flags(o.var) = o.originalFlags;
            bool ok = cvars::Set(o.var, o.original.c_str(), o.originalFlags & 0xff000000) && cvars::MirrorToRenderer(o.var);
            Log("override: %ls released to %ls%s: %s -> %s", o.cvar, o.original.c_str(),
                ok ? "" : " (Set or mirror failed)", before.c_str(), Text(Read(o.var)).c_str());
            o.active = false;
        }
    }
}

// Puts a forced value back if something changed it (it shouldn't, given the priority).
void CheckOverrides() {
    for (Override& o : g_overrides) {
        if (!o.active) continue;
        Reading r = Read(o.var);
        if (r.ok && fabs(r.render - o.applied) < 1e-5 && fabs(r.main - o.applied) < 1e-5) continue;
        if (++o.repairs <= 5)
            Log("override: %ls changed behind our back: %s; setting it again%s", o.cvar, Text(r).c_str(),
                o.repairs == 5 ? " (not logging further repairs)" : "");
        cvars::Set(o.var, ValueText(o, o.applied).c_str(), cvars::kSetByConsole);
        cvars::MirrorToRenderer(o.var);
    }
}

// ---------------------------------------------------------------- HDR brightness

// With HDR on (and the Uniform Colour Pipeline off), the peak is not a cvar:
// HDRSettingChangedSinkCallback (a cvar sink, on the game thread) rebuilds the tone curve with
// HDRConfigureCVars whenever UGameUserSettings::GetCurrentHDRDisplayNits() or one of the cvars
// it watches changes, then sets r.HDR.Display.MaxLuminance to those nits. That getter returns
// 0 (and the sink then uses 1000) unless UGameUserSettings::bUseHDRDisplayOutput is set, which
// the game leaves off even with HDR on. So the peak is written to
// GEngine->GameUserSettings->HDRDisplayOutputNits, the flag is set while HDR output is on,
// and r.HDR.Display.MaxLuminance
// (a watched cvar) is forced to the same value, which makes the sink run. Paper white is set
// through r.HDR.Display.MidLuminance (nits at 18% grey, also watched), the UI through
// r.HDR.UI.Luminance. The values are kept in ReShade.ini and applied at every start.

constexpr uint32_t kGEngineRva = 0x8994070;    // ?GEngine@@3PEAVUEngine@@EA
constexpr uint32_t kGameUserSettings = 0x2a0;  // UEngine::GameUserSettings
constexpr uint32_t kUseHdr = 0x10c;            // UGameUserSettings::bUseHDRDisplayOutput (bool)
constexpr uint32_t kHdrNits = 0x110;           // UGameUserSettings::HDRDisplayOutputNits (int)
// The nits HDRSettingChangedSinkCallback last built the tone curve for (its cached copy).
constexpr uint32_t kSinkNitsRva = 0x85d3140;
constexpr float kMidGrey = 0.18f;
constexpr const char* kConfigSection = UDRT_PRODUCT;

struct Brightness {
    bool on = true;  // with nothing saved: on, at the game's own values
    int peak = 1000, paper = 203, ui = 300;  // nits
    bool initialised = false;                // from ReShade.ini, or else the game's values
    bool nitsForced = false;
    int originalNits = 0;  // the game's value, put back when the override is turned off
    int written = 0;       // what we last wrote
    uint32_t repairs = 0;
    bool flagForced = false;
    uint8_t originalFlag = 0;
};
Brightness g_bright;

// GEngine->GameUserSettings, or null (other build, not created yet).
uint8_t* UserSettings() {
    if (g_timestamp != kKnownTimestamp) return nullptr;
    uint8_t* engine = nullptr;
    uint8_t* settings = nullptr;
    if (!cvars::SafeCopy(&engine, g_base + kGEngineRva, 8) || !engine) return nullptr;
    if (!cvars::SafeCopy(&settings, engine + kGameUserSettings, 8) || !settings) return nullptr;
    return settings;
}

// The peak's home in the game's user settings, or null.
int* NitsField() {
    uint8_t* settings = UserSettings();
    return settings ? reinterpret_cast<int*>(settings + kHdrNits) : nullptr;
}

Override* FindOverride(const wchar_t* cvar);

// Sets bUseHDRDisplayOutput while the override is on and HDR output is on, so the sink reads
// the nits; puts the game's value back otherwise.
void ApplyUseHdrFlag(bool want) {
    uint8_t* settings = UserSettings();
    uint8_t flag = 0;
    if (!settings || !cvars::SafeCopy(&flag, settings + kUseHdr, 1)) return;
    if (want && flag == 0) {
        if (!g_bright.flagForced) {
            g_bright.flagForced = true;
            g_bright.originalFlag = flag;
            Log("brightness: bUseHDRDisplayOutput was 0; setting it so the tone curve uses the peak");
        }
        uint8_t one = 1;
        cvars::SafeCopy(settings + kUseHdr, &one, 1);
        // The sink may have built the curve while the flag was 0: set the peak cvar again
        // (ApplyOverrides, this frame) so it runs once more.
        if (Override* o = FindOverride(L"r.HDR.Display.MaxLuminance")) o->applied = -1;
    } else if (!want && g_bright.flagForced) {
        g_bright.flagForced = false;
        cvars::SafeCopy(settings + kUseHdr, &g_bright.originalFlag, 1);
        Log("brightness: bUseHDRDisplayOutput back to %u", g_bright.originalFlag);
    }
}

Override* FindOverride(const wchar_t* cvar) {
    for (Override& o : g_overrides)
        if (wcscmp(o.cvar, cvar) == 0) return &o;
    return nullptr;
}

double RendererValue(const wchar_t* cvar, double fallback) {
    void* var = cvars::FindVariable(g_manager, cvar);
    Reading r = var ? Read(var) : Reading{};
    return r.ok ? r.render : fallback;
}

void SaveBrightness() {
    reshade::set_config_value(nullptr, kConfigSection, "HdrBrightnessOverride", g_bright.on);
    reshade::set_config_value(nullptr, kConfigSection, "HdrPeakNits", g_bright.peak);
    reshade::set_config_value(nullptr, kConfigSection, "HdrPaperWhiteNits", g_bright.paper);
    reshade::set_config_value(nullptr, kConfigSection, "HdrUiNits", g_bright.ui);
}

// The ReShade.ini section holding this key: ours, else the one from before the rename
// (UntilDawnRT), else null.
const char* SavedSection(const char* key) {
    size_t size = 0;
    if (reshade::get_config_value(nullptr, kConfigSection, key, nullptr, &size)) return kConfigSection;
    if (reshade::get_config_value(nullptr, UDRT_LEGACY_PRODUCT, key, nullptr, &size)) return UDRT_LEGACY_PRODUCT;
    return nullptr;
}

// Saved values if there are any; otherwise the override is on and the sliders start at the
// game's values, so nothing looks different until one is moved.
void InitBrightness() {
    g_bright.initialised = true;
    const char* section = SavedSection("HdrBrightnessOverride");
    bool saved = section != nullptr;
    if (saved) {
        reshade::get_config_value(nullptr, section, "HdrBrightnessOverride", g_bright.on);
        reshade::get_config_value(nullptr, section, "HdrPeakNits", g_bright.peak);
        reshade::get_config_value(nullptr, section, "HdrPaperWhiteNits", g_bright.paper);
        reshade::get_config_value(nullptr, section, "HdrUiNits", g_bright.ui);
        if (section != kConfigSection) SaveBrightness();  // carry it over to our section
    } else {
        int nits = 0;
        if (int* field = NitsField(); field && cvars::SafeCopy(&nits, field, 4) && nits > 0) g_bright.peak = nits;
        g_bright.paper = static_cast<int>(lround(RendererValue(L"r.HDR.Display.MidLuminance", 15) / kMidGrey));
        g_bright.ui = static_cast<int>(lround(RendererValue(L"r.HDR.UI.Luminance", 300)));
    }
    Log("brightness: %s, override %s: peak %d, paper white %d, UI %d nits", saved ? "from ReShade.ini" : "game values",
        g_bright.on ? "on" : "off", g_bright.peak, g_bright.paper, g_bright.ui);
}

// Every frame, before ApplyOverrides: keeps the nits field at the peak (the game may write
// it back, e.g. when settings are applied) and drives the three cvar overrides.
void ApplyBrightness() {
    if (!g_bright.initialised) InitBrightness();
    ApplyUseHdrFlag(g_bright.on && RendererValue(L"r.HDR.EnableHDROutput", 0) != 0);
    int* field = NitsField();
    int current = 0;
    bool readable = field && cvars::SafeCopy(&current, field, 4);
    if (g_bright.on && readable && current != g_bright.peak) {
        if (!g_bright.nitsForced) {
            g_bright.nitsForced = true;
            g_bright.originalNits = current;
            Log("brightness: peak %d nits (game had %d)", g_bright.peak, current);
        } else if (current == g_bright.written) {
            Log("brightness: peak %d nits", g_bright.peak);
        } else if (++g_bright.repairs <= 5) {
            Log("brightness: the game set HDR nits to %d; setting %d again%s", current, g_bright.peak,
                g_bright.repairs == 5 ? " (not logging further repairs)" : "");
        }
        cvars::SafeCopy(field, &g_bright.peak, 4);
        g_bright.written = g_bright.peak;
    } else if (!g_bright.on && g_bright.nitsForced) {
        g_bright.nitsForced = false;
        if (readable) cvars::SafeCopy(field, &g_bright.originalNits, 4);
        Log("brightness: override off, HDR nits back to %d", g_bright.originalNits);
    }
    struct {
        const wchar_t* cvar;
        float value;
    } const drive[] = {
        {L"r.HDR.Display.MaxLuminance", static_cast<float>(g_bright.peak)},
        {L"r.HDR.Display.MidLuminance", g_bright.paper * kMidGrey},
        {L"r.HDR.UI.Luminance", static_cast<float>(g_bright.ui)},
    };
    for (const auto& d : drive)
        if (Override* o = FindOverride(d.cvar)) {
            o->wanted = g_bright.on;
            o->value = d.value;
        }
}

// ---------------------------------------------------------------- ray tracing settings

// The game's RT barely shows (docs/FINDINGS.md, "Why RT shows so little"): lights opt out of
// RT shadows unless BMEnhancedMode is 2, and RT reflections are switched on per frame only
// inside designer-marked volumes, for mirror-like surfaces. These saved settings force them
// at console priority, which outranks the game's per-frame writes. The game's menu still
// rules: with its Ray Tracing option off, ForceAllRayTracingEffects 0 keeps every effect
// off, and reflections follow its RT reflections option (r.BM.AllowRTReflections).

enum RtShadows : int { kShadowsGame = 0, kShadowsEnhanced = 1, kShadowsAll = 2 };

struct RtSettings {
    bool on = true;  // with nothing saved: on, shadows on every light, reflections everywhere
    int shadows = kShadowsAll;
    bool reflectionsEverywhere = true;
    float maxRoughness = 0.4f;
    bool initialised = false;
};
RtSettings g_rt;

void SaveRt() {
    reshade::set_config_value(nullptr, kConfigSection, "RtOverride", g_rt.on);
    reshade::set_config_value(nullptr, kConfigSection, "RtShadows", g_rt.shadows);
    reshade::set_config_value(nullptr, kConfigSection, "RtReflectionsEverywhere", g_rt.reflectionsEverywhere);
    reshade::set_config_value(nullptr, kConfigSection, "RtReflectionsMaxRoughness", g_rt.maxRoughness);
}

void InitRt() {
    g_rt.initialised = true;
    const char* section = SavedSection("RtOverride");
    bool saved = section != nullptr;
    if (saved) {
        reshade::get_config_value(nullptr, section, "RtOverride", g_rt.on);
        reshade::get_config_value(nullptr, section, "RtShadows", g_rt.shadows);
        reshade::get_config_value(nullptr, section, "RtReflectionsEverywhere", g_rt.reflectionsEverywhere);
        reshade::get_config_value(nullptr, section, "RtReflectionsMaxRoughness", g_rt.maxRoughness);
        if (section != kConfigSection) SaveRt();  // carry it over to our section
    }
    Log("ray tracing: %s, override %s: shadows %d, reflections %s, max roughness %g",
        saved ? "from ReShade.ini" : "defaults", g_rt.on ? "on" : "off", g_rt.shadows,
        g_rt.reflectionsEverywhere ? "everywhere" : "game", g_rt.maxRoughness);
}

// Drives (or releases) one override from the settings.
void Drive(const wchar_t* cvar, bool want, float value) {
    Override* o = FindOverride(cvar);
    if (!o) return;
    if (want) {
        o->driven = true;
        o->wanted = true;
        o->value = value;
    } else if (o->driven) {
        o->driven = false;
        o->wanted = false;  // hand it back to the game
    }
}

// Every frame, before ApplyOverrides.
void ApplyRt() {
    if (!g_rt.initialised) InitRt();
    Drive(L"r.RayTracing.Enable.BMEnhancedMode", g_rt.on && g_rt.shadows != kShadowsGame,
          static_cast<float>(g_rt.shadows));
    bool reflections = g_rt.on && g_rt.reflectionsEverywhere;
    // The game's own gate also requires its menu option: keep that working.
    float allowed = RendererValue(L"r.BM.AllowRTReflections", 0) == 1 ? 1.0f : 0.0f;
    Drive(L"r.RayTracing.Reflections", reflections, allowed);
    Drive(L"r.RayTracing.Reflections.MaxRoughness", reflections, g_rt.maxRoughness);
}

// ---------------------------------------------------------------- events

constexpr uint64_t kCensusFrame = 600;  // ~10 s in: plugins have registered their cvars by then
uint64_t g_frames = 0;
bool g_snapshotRequested = false;

void OnInitDevice(device* dev) {
    std::lock_guard lock(g_mutex);
    Log("device created: %s", dev->get_api() == device_api::d3d12 ? "Direct3D 12" : "not Direct3D 12");
}

// The swap chain's back buffer format and colour space, logged when they change.
format g_backBufferFormat = format::unknown;
color_space g_colorSpace = color_space::unknown;

const char* ColorSpaceName(color_space c) {
    switch (c) {
        case color_space::srgb: return "sRGB";
        case color_space::scrgb: return "scRGB (linear, FP16)";
        case color_space::hdr10_st2084: return "HDR10 (Rec2020, PQ)";
        case color_space::hdr10_hlg: return "HDR10 HLG";
        default: return "unknown";
    }
}

const char* FormatName(format f) {
    switch (f) {
        case format::r8g8b8a8_unorm: return "R8G8B8A8_UNORM";
        case format::b8g8r8a8_unorm: return "B8G8R8A8_UNORM";
        case format::r10g10b10a2_unorm: return "R10G10B10A2_UNORM";
        case format::r16g16b16a16_float: return "R16G16B16A16_FLOAT";
        default: return "other";
    }
}

void TrackSwapchain(swapchain* sc) {
    format f = sc->get_device()->get_resource_desc(sc->get_current_back_buffer()).texture.format;
    color_space c = sc->get_color_space();
    if (f == g_backBufferFormat && c == g_colorSpace) return;
    g_backBufferFormat = f;
    g_colorSpace = c;
    Log("swap chain: back buffer %s (%u), colour space %s", FormatName(f), static_cast<uint32_t>(f), ColorSpaceName(c));
}

// The cvars that decide which RT effects run, logged with each one-second work log so
// every log entry says what state it was taken in.
const wchar_t* const kRtState[] = {
    L"r.RayTracing.Enable",
    L"r.RayTracing.ForceAllRayTracingEffects",
    L"r.RayTracing.Enable.BMEnhancedMode",
    L"r.RayTracing.Shadows",
    L"r.RayTracing.AmbientOcclusion",
    L"r.RayTracing.Reflections",
    L"r.BM.AllowRTReflections",
    L"r.RayTracing.Reflections.MaxRoughness",
    L"r.Shadow.Virtual.Enable",
};

// UBatesGameUserSettings fields the RT menu options write, applied as cvars by
// ApplyNonResolutionSettings (docs/FINDINGS.md, "RT menu options").
constexpr uint32_t kMenuRayTracing = 0x4ae, kMenuRtReflections = 0x4af, kMenuRtAo = 0x4b0, kMenuEnhanced = 0x4b1;
// GRHIGlobals' "supports ray tracing shaders" (full pipelines), which the menu's ray tracing
// option also requires.
constexpr uint32_t kRhiSupportsRtShadersRva = 0x88f1ccc;

uint8_t* UserSettings();

void LogRtState() {
    uint8_t menu[4] = {0xff, 0xff, 0xff, 0xff}, rhi = 0xff;
    if (uint8_t* s = UserSettings()) cvars::SafeCopy(menu, s + kMenuRayTracing, 4);
    if (g_timestamp == kKnownTimestamp) cvars::SafeCopy(&rhi, g_base + kRhiSupportsRtShadersRva, 1);
    Log("rt menu: ray tracing %u, RT reflections %u, RT AO %u, enhanced mode %u; RHI supports RT shaders %u",
        menu[kMenuRayTracing - kMenuRayTracing], menu[kMenuRtReflections - kMenuRayTracing],
        menu[kMenuRtAo - kMenuRayTracing], menu[kMenuEnhanced - kMenuRayTracing], rhi);
    std::string line;
    for (const wchar_t* name : kRtState) {
        void* var = g_manager ? cvars::FindVariable(g_manager, name) : nullptr;
        Reading r = var ? Read(var) : Reading{};
        char buf[96];
        if (r.ok)
            snprintf(buf, sizeof(buf), "%s %g", Narrow(name).c_str(), r.render);
        else
            snprintf(buf, sizeof(buf), "%s ?", Narrow(name).c_str());
        line += line.empty() ? buf : std::string(", ") + buf;
    }
    Log("rt state: %s", line.c_str());
}

void OnPresent(command_queue*, swapchain* sc, const rect*, const rect*, uint32_t, const rect*) {
    std::lock_guard lock(g_mutex);
    ++g_frames;
    if (sc) TrackSwapchain(sc);
    if (g_frames == kCensusFrame || (g_snapshotRequested && g_frames > kCensusFrame)) {
        g_snapshotRequested = false;
        Snapshot();
    }
    if (FindManager()) {
        ApplyBrightness();
        ApplyRt();
        ApplyOverrides();
        if (g_frames % 60 == 0) CheckOverrides();
    }
    EndFrameDispatches();
}

// ---------------------------------------------------------------- overlay

// Related cvars shown read-only, so a menu change can be seen without a snapshot.
const wchar_t* const kWatched[] = {
    L"r.RayTracing.Enable",       L"r.RayTracing.Shadows",           L"r.RayTracing.Reflections",
    L"r.RayTracing.AmbientOcclusion", L"r.ReflectionMethod",         L"r.Shadow.Virtual.Enable",
    L"r.BM.AllowVirtualShadowMapsMode", L"r.RayTracing.ForceAllRayTracingEffects", L"r.RayTracing.Shadows.Lights.Spot",
};

const wchar_t* const kWatchedHdr[] = {
    L"r.AllowHDR",
    L"r.HDR.EnableHDROutput",
    L"r.HDR.Display.OutputDevice",
    L"r.HDR.Display.ColorGamut",
    L"r.HDR.Display.MaxLuminance",
    L"r.HDR.Display.MinLuminanceLog10",
    L"r.HDR.Display.MidLuminance",
    L"r.HDR.Aces.SceneColorMultiplier",
    L"r.HDR.UI.CompositeMode",
    L"r.HDR.UI.Level",
    L"r.HDR.UI.Luminance",
    L"r.UCP.HDR",
    L"r.UCP.SDR",
    L"r.UCP.ExposureAdjust",
    L"r.UCP.HDR.BMMinLuminanceOverride",
    L"r.UCP.HDR.BMMaxLuminanceOverride",
    L"r.UCP.HDRCalibrationMinLumAdjust",
    L"r.UCP.DisplayGrade",
    L"r.UCP.DisplayGrade.ShadowCutoff",
    L"r.UCP.DisplayGrade.ShadowGamma",
    L"r.HDRCalibrateScreen.Mode",
    L"r.HDRCalibrateScreen.ShowGame",
    L"r.HDRCalibrateScreen.SetLum.Min",
    L"r.HDRCalibrateScreen.SetLum.Max",
    L"r.TonemapperGamma",
};

// Width of a radio button or checkbox with this label.
float ToggleWidth(const char* label) {
    return ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize(label, nullptr, true).x;
}

// SameLine, or a new line if an item this wide wouldn't fit in the rest of the column.
void SameLineIfFits(float width) {
    float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    ImGui::SameLine();
    if (ImGui::GetCursorScreenPos().x + width > right) ImGui::NewLine();
}

// Width of the widest of these strings, for a fixed-width label column.
float WidestText(const std::vector<std::string>& texts) {
    float w = 0;
    for (const std::string& t : texts) w = std::max(w, ImGui::CalcTextSize(t.c_str()).x);
    return w;
}

// One row of the switches table: label | controls | the renderer's value.
void DrawOverride(Override& o) {
    ImGui::PushID(o.cvar);
    Reading r = o.var || Resolve(o) ? Read(o.var) : Reading{};
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(o.label);
    ImGui::SetItemTooltip("%s\n\n%s", Narrow(o.cvar).c_str(), o.help);
    ImGui::TableNextColumn();
    if (o.missing) {
        ImGui::TextDisabled("not in this build");
        ImGui::TableNextColumn();
        ImGui::PopID();
        return;
    }
    if (o.driven) {
        ImGui::TextDisabled("set by the Ray tracing settings");
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        if (r.ok) ImGui::TextDisabled("renderer: %g", r.render);
        ImGui::PopID();
        return;
    }
    double game = o.active ? wcstod(o.original.c_str(), nullptr) : r.render;
    char gameLabel[48];
    snprintf(gameLabel, sizeof(gameLabel), "Game (%g)", game);
    if (o.choices.empty()) {
        bool on = o.wanted;
        if (ImGui::Checkbox("##on", &on)) {
            o.wanted = on;
            if (on) o.value = static_cast<float>(game);
        }
        ImGui::SetItemTooltip("Override the game's value");
        ImGui::SameLine();
        if (!o.wanted) ImGui::BeginDisabled();
        float v = o.wanted ? o.value : static_cast<float>(game);
        ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 6));
        if (ImGui::SliderFloat("##v", &v, o.min, o.max, o.wanted ? "%.2f" : "game: %.2f")) o.value = v;
        if (!o.wanted) ImGui::EndDisabled();
    } else {
        if (ImGui::RadioButton(gameLabel, !o.wanted)) o.wanted = false;
        for (const Choice& c : o.choices) {
            SameLineIfFits(ToggleWidth(c.label));
            if (ImGui::RadioButton(c.label, o.wanted && lroundf(o.value) == c.value)) {
                o.wanted = true;
                o.value = static_cast<float>(c.value);
            }
        }
    }
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    if (r.ok) ImGui::TextDisabled("renderer: %g", r.render);
    ImGui::PopID();
}

// Saved ray tracing settings.
void DrawRt() {
    bool changed = ImGui::Checkbox("Improve the game's ray tracing", &g_rt.on);
    ImGui::SetItemTooltip("Kept in ReShade.ini and applied every time the game starts. The game's Ray Tracing menu "
                          "option must be on.");
    if (!g_rt.on) ImGui::BeginDisabled();
    const char* shadows[] = {"Game (most lights opt out)", "Lights marked enhanced", "All lights"};
    changed |= ImGui::Combo("RT shadows", &g_rt.shadows, shadows, 3);
    ImGui::SetItemTooltip("r.RayTracing.Enable.BMEnhancedMode. All lights is the most visible and the most costly.");
    const char* reflections[] = {"Game (marked areas, mirrors only)", "Everywhere"};
    int r = g_rt.reflectionsEverywhere ? 1 : 0;
    if (ImGui::Combo("RT reflections", &r, reflections, 2)) {
        g_rt.reflectionsEverywhere = r == 1;
        changed = true;
    }
    ImGui::SetItemTooltip("Everywhere: r.RayTracing.Reflections on outside the game's marked areas too. Follows the "
                          "game's RT reflections menu option.");
    if (!g_rt.reflectionsEverywhere) ImGui::BeginDisabled();
    ImGui::SliderFloat("Reflections max roughness", &g_rt.maxRoughness, 0.01f, 1.0f, "%.2f",
                       ImGuiSliderFlags_AlwaysClamp);
    changed |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SetItemTooltip("Surfaces rougher than this keep screen space and probe reflections. The game ships 0.01 "
                          "(mirrors only); higher costs more and can look noisier.");
    if (!g_rt.reflectionsEverywhere) ImGui::EndDisabled();
    if (!g_rt.on) ImGui::EndDisabled();
    if (changed) SaveRt();
    if (RendererValue(L"r.RayTracing.ForceAllRayTracingEffects", -1) == 0)
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                           "Ray tracing is off in the game's graphics menu: every RT effect is off.");
}

// Peak, paper white and UI sliders; saved to ReShade.ini when a slider is let go.
void DrawBrightness() {
    if (g_timestamp != kKnownTimestamp) {
        ImGui::TextDisabled("Not available: a different game build than the one analysed.");
        return;
    }
    bool changed = ImGui::Checkbox("Override the game's HDR brightness", &g_bright.on);
    ImGui::SetItemTooltip("Kept in ReShade.ini and applied every time the game starts.");
    if (!g_bright.on) ImGui::BeginDisabled();
    const ImGuiSliderFlags log = ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp;
    ImGui::SliderInt("Peak brightness", &g_bright.peak, 400, 10000, "%d nits", log);
    changed |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SetItemTooltip("The brightest the image gets: set it to your display's peak (e.g. from the Windows HDR "
                          "Calibration app).\nThe game's own HDR max luminance slider has no effect with the Uniform "
                          "Colour Pipeline off.");
    ImGui::SliderInt("Paper white", &g_bright.paper, 80, 500, "%d nits", log);
    changed |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SetItemTooltip("Overall brightness of the scene: sets 18%% grey to 0.18 x this "
                          "(r.HDR.Display.MidLuminance).\nDiffuse white lands near this value, a little lower where "
                          "the curve rolls off. 203 nits is the HDR reference level.");
    ImGui::SliderInt("UI brightness", &g_bright.ui, 80, 1000, "%d nits", log);
    changed |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SetItemTooltip("Brightness of menus and subtitles (r.HDR.UI.Luminance).");
    if (!g_bright.on) ImGui::EndDisabled();
    if (changed) SaveBrightness();

    if (RendererValue(L"r.UCP.HDR", 0) != 0)
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                           "r.UCP.HDR is 1: HDR runs through the Uniform Colour Pipeline (black scene), "
                           "which ignores these.");
    else if (RendererValue(L"r.HDR.EnableHDROutput", 0) == 0)
        ImGui::TextDisabled("HDR is off in the game: these apply once it is on.");
    else if (int built = 0; cvars::SafeCopy(&built, g_base + kSinkNitsRva, 4))
        ImGui::TextDisabled("Tone curve built for %d nits", built);
}

// The user settings fields the peak goes through (Advanced).
void DrawHdrSettingsFields() {
    if (uint8_t* settings = UserSettings()) {
        uint8_t flag = 0;
        int nits = 0;
        cvars::SafeCopy(&flag, settings + kUseHdr, 1);
        cvars::SafeCopy(&nits, settings + kHdrNits, 4);
        ImGui::Text("Game user settings: bUseHDRDisplayOutput %u, HDRDisplayOutputNits %d", flag, nits);
    }
}

// The switches of one group: label | controls | the renderer's value.
void DrawSwitches(Group group) {
    ImGui::PushID(static_cast<int>(group));
    if (ImGui::BeginTable("##switches", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        std::vector<std::string> labels;
        for (const Override& o : g_overrides) labels.push_back(o.label);  // all groups: columns line up
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, WidestText(labels));
        ImGui::TableSetupColumn("controls", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("renderer", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("renderer: -0.000000").x);
        for (Override& o : g_overrides)
            if (o.group == group) DrawOverride(o);
        ImGui::EndTable();
    }
    ImGui::PopID();
}

// Read-only cvars: name | renderer value (priority).
void DrawWatched(const char* id, const wchar_t* const* names, size_t count) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingFixedFit)) return;
    std::vector<std::string> narrow;
    for (size_t i = 0; i < count; ++i) narrow.push_back(Narrow(names[i]));
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, WidestText(narrow));
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
    for (size_t i = 0; i < count; ++i) {
        void* var = cvars::FindVariable(g_manager, names[i]);
        Reading r = var ? Read(var) : Reading{};
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(narrow[i].c_str());
        ImGui::TableNextColumn();
        if (!var)
            ImGui::TextDisabled("not a cvar");
        else if (!r.ok)
            ImGui::TextDisabled("unreadable");
        else
            ImGui::Text("%g  (priority 0x%02x)", r.render, r.priority);
    }
    ImGui::EndTable();
}

void DrawOverlay(effect_runtime*) {
    std::lock_guard lock(g_mutex);
    ImGui::TextUnformatted(UDRT_PRODUCT " " UDRT_VERSION);
    if (!g_manager) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Waiting for the game's console variables...");
        return;
    }
    if (g_timestamp != kKnownTimestamp)
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "A different game build than the one analysed: see the log.");

    ImGui::SeparatorText("Ray tracing");
    DrawRt();

    ImGui::SeparatorText("HDR brightness");
    DrawBrightness();

    ImGui::Spacing();
    if (!ImGui::CollapsingHeader("Advanced")) return;  // closed by default: testing and diagnostics
    ImGui::TextDisabled("Session-only switches and readouts for testing. Nothing here is saved.");

    ImGui::SeparatorText("HDR");
    ImGui::Text("Swap chain: %s, %s", FormatName(g_backBufferFormat), ColorSpaceName(g_colorSpace));
    DrawHdrSettingsFields();
    DrawSwitches(Group::Hdr);
    if (ImGui::TreeNode("HDR values (renderer's copy)")) {
        DrawWatched("##watchedHdr", kWatchedHdr, std::size(kWatchedHdr));
        ImGui::TreePop();
    }

    ImGui::SeparatorText("Ray tracing");
    DrawSwitches(Group::RayTracing);

    ImGui::SeparatorText("Ray tracing values (renderer's copy)");
    DrawWatched("##watched", kWatched, std::size(kWatched));

    ImGui::SeparatorText("Ray tracing work (last second)");
    {
        std::lock_guard rtLock(g_rtMutex);
        ImGui::TextDisabled("%u presents: %u draws, %u compute dispatches, %u BLAS / %u TLAS builds", g_lastPresents,
                            g_lastWork.draws, g_lastWork.dispatches, g_lastWork.blasBuilds, g_lastWork.tlasBuilds);
        if (g_lastDispatches.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "No rays dispatched (%u RT pipelines created so far)",
                               g_rtPipelines);
        } else if (ImGui::BeginTable("##rays", 3, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("count", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("00000 x").x);
            ImGui::TableSetupColumn("size", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("0000x0000").x);
            ImGui::TableSetupColumn("names", ImGuiTableColumnFlags_WidthStretch);
            for (auto& [handle, st] : g_lastDispatches) {
                ImGui::TableNextColumn();
                ImGui::Text("%u x", st.count);
                ImGui::TableNextColumn();
                ImGui::Text("%ux%u", st.width, st.height);
                ImGui::TableNextColumn();
                ImGui::TextWrapped("%s", PipelineName(handle).c_str());
            }
            ImGui::EndTable();
        }
    }
    if (ImGui::Button("Log ray tracing work (one second)")) g_logDispatches = true;

    ImGui::SeparatorText("Diagnostics");
    if (ImGui::Button("Snapshot cvars")) g_snapshotRequested = true;
    ImGui::SetItemTooltip("Logs what changed since the last snapshot, e.g. after changing a game menu option.");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", g_frames < kCensusFrame ? "census after 10 s" : g_status.c_str());
    if (g_layoutBad) ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "Layout check found mismatches: see the log.");
}

void Install() {
    OpenLog();
    LogGameInfo();
    DefineOverrides();
    reshade::register_event<reshade::addon_event::init_device>(&OnInitDevice);
    reshade::register_event<reshade::addon_event::present>(&OnPresent);
    reshade::register_event<reshade::addon_event::init_pipeline>(&OnInitPipeline);
    reshade::register_event<reshade::addon_event::bind_pipeline>(&OnBindPipeline);
    reshade::register_event<reshade::addon_event::dispatch_rays>(&OnDispatchRays);
    reshade::register_event<reshade::addon_event::draw>(&OnDraw);
    reshade::register_event<reshade::addon_event::draw_indexed>(&OnDrawIndexed);
    reshade::register_event<reshade::addon_event::dispatch>(&OnDispatch);
    reshade::register_event<reshade::addon_event::build_acceleration_structure>(&OnBuildAccelerationStructure);
    reshade::register_overlay(UDRT_PRODUCT, &DrawOverlay);
}

void Uninstall() {
    reshade::unregister_overlay(UDRT_PRODUCT, &DrawOverlay);
    if (g_log) {
        Log("unloading");
        fclose(g_log);
        g_log = nullptr;
    }
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            if (!reshade::register_addon(module)) return FALSE;
            Install();
            break;
        case DLL_PROCESS_DETACH:
            Uninstall();
            reshade::unregister_addon(module);
            break;
    }
    return TRUE;
}
