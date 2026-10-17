// UntilDawnRT phase 0: a ReShade add-on that checks the console variable layout in Until
// Dawn (UE 5) and lets you force the cvars that gate its ray tracing, live, from an overlay
// tab. See docs/FINDINGS.md for why these:
//   r.RayTracing.Enable.BMEnhancedMode     which lights may cast RT shadows (0/1/2)
//   r.BM.AllowRTReflections                RT reflections everywhere or only where allowed
//   r.RayTracing.Reflections               the game holds it at 0 from code
//   r.RayTracing.Reflections.MaxRoughness  the project ships 0.01: mirrors only
//   r.Shadow.Virtual.Enable                virtual shadow maps, which may hide RT shadows
//
// It also counts the ray tracing work the GPU actually does: every DispatchRays, grouped
// by the ray tracing pipeline bound for it (named by its shaders' export names), shown
// live for the last frame.
//
// What it records, next to the game executable:
//   UntilDawnRT.log  the census (every cvar matching kPrefixes: type, kind, main and
//                    renderer values, priority), the layout check, every override and
//                    release, later snapshots' changes (e.g. after a menu change), every
//                    ray tracing pipeline the game creates, and one-frame dispatch logs
//
// Overrides are session-only in phase 0: mode 2 is documented as very slow, so nothing is
// forced at start.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include <algorithm>
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
    "Ray tracing probe for " UDRT_GAME " (DirectX 12): cvar layout check and live RT switches.";

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
                AddNames(names, RdatNames(descs[j].code, descs[j].code_size));
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

bool OnDispatchRays(command_list* cmd, resource, uint64_t, uint64_t, resource, uint64_t, uint64_t, uint64_t, resource,
                    uint64_t, uint64_t, uint64_t, resource, uint64_t, uint64_t, uint64_t, uint32_t width, uint32_t height,
                    uint32_t) {
    std::lock_guard lock(g_rtMutex);
    DispatchStat& s = g_frameDispatches[g_boundRt[cmd]];
    ++s.count;
    s.width = width;
    s.height = height;
    return false;
}

std::string PipelineName(uint64_t handle) {
    auto it = g_pipelineNames.find(handle);
    return it == g_pipelineNames.end() || it->second.empty() ? "unnamed" : it->second;
}

// Called at present: the frame's dispatches become the last frame's.
void EndFrameDispatches() {
    std::lock_guard lock(g_rtMutex);
    g_lastDispatches.swap(g_frameDispatches);
    g_frameDispatches.clear();
    if (!g_logDispatches) return;
    g_logDispatches = false;
    Log("ray dispatches in one frame: %zu pipelines", g_lastDispatches.size());
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

struct Override {
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
};

std::vector<Override> g_overrides;

void DefineOverrides() {
    g_overrides = {
        {L"r.RayTracing.Enable.BMEnhancedMode", "Enhanced RT mode",
         "Ballistic Moon's gate on which lights may cast ray traced shadows (and on RT in reflections and sky light).\n"
         "0: lights marked Disabled (the project default) and Enhanced-only get none\n"
         "1: Enhanced-only lights get RT shadows too\n"
         "2: every light, whatever its setting (the help text warns: very slow)",
         {{0, "0 Off"}, {1, "1 Enhanced"}, {2, "2 Everywhere"}}},
        {L"r.BM.AllowRTReflections", "Allow RT reflections",
         "Ballistic Moon's reflection setting.\n0: always off\n1: on in areas where supported",
         {{0, "0 Off"}, {1, "1 Where supported"}}},
        {L"r.RayTracing.Reflections", "RT reflections",
         "UE's ray traced reflections switch. The game sets it from code (0 in the first test).\n"
         "-1: post-process volume decides\n0: off\n1: on",
         {{-1, "-1 Volume"}, {0, "0 Off"}, {1, "1 On"}}},
        {L"r.RayTracing.Reflections.MaxRoughness", "Reflections max roughness",
         "Surfaces rougher than this skip ray traced reflections. The project ships 0.01 (mirrors only); UE's default "
         "is 0.6.",
         {}, 0.0f, 1.0f},
        {L"r.Shadow.Virtual.Enable", "Virtual shadow maps",
         "UE's virtual shadow maps (the game holds them on from code). Turning them off tests whether they hide "
         "ray traced shadows.",
         {{0, "0 Off"}, {1, "1 On"}}},
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

// ---------------------------------------------------------------- events

constexpr uint64_t kCensusFrame = 600;  // ~10 s in: plugins have registered their cvars by then
uint64_t g_frames = 0;
bool g_snapshotRequested = false;

void OnInitDevice(device* dev) {
    std::lock_guard lock(g_mutex);
    Log("device created: %s", dev->get_api() == device_api::d3d12 ? "Direct3D 12" : "not Direct3D 12");
}

void OnPresent(command_queue*, swapchain*, const rect*, const rect*, uint32_t, const rect*) {
    std::lock_guard lock(g_mutex);
    ++g_frames;
    if (g_frames == kCensusFrame || (g_snapshotRequested && g_frames > kCensusFrame)) {
        g_snapshotRequested = false;
        Snapshot();
    }
    if (FindManager()) {
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

void DrawOverlay(effect_runtime*) {
    std::lock_guard lock(g_mutex);
    ImGui::TextUnformatted(UDRT_PRODUCT " " UDRT_VERSION " (phase 0 probe)");
    if (!g_manager) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Waiting for the game's console variables...");
        return;
    }
    if (g_timestamp != kKnownTimestamp)
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "A different game build than the one analysed: see the log.");

    ImGui::SeparatorText("Ray tracing switches");
    if (ImGui::BeginTable("##switches", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        std::vector<std::string> labels;
        for (const Override& o : g_overrides) labels.push_back(o.label);
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, WidestText(labels));
        ImGui::TableSetupColumn("controls", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("renderer", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("renderer: -0.000000").x);
        for (Override& o : g_overrides) DrawOverride(o);
        ImGui::EndTable();
    }
    ImGui::TextDisabled("Session only: nothing is forced when the game starts.");

    ImGui::SeparatorText("Related values (renderer's copy)");
    if (ImGui::BeginTable("##watched", 2, ImGuiTableFlags_SizingFixedFit)) {
        std::vector<std::string> names;
        for (const wchar_t* name : kWatched) names.push_back(Narrow(name));
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, WidestText(names));
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
        for (const wchar_t* name : kWatched) {
            void* var = cvars::FindVariable(g_manager, name);
            Reading r = var ? Read(var) : Reading{};
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(Narrow(name).c_str());
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

    ImGui::SeparatorText("Ray tracing work (last frame)");
    {
        std::lock_guard rtLock(g_rtMutex);
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
    if (ImGui::Button("Log ray dispatches (one frame)")) g_logDispatches = true;

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
