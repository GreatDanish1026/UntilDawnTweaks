// Finds Unreal console variables in the running game and reads, sets and mirrors them.
// Adapted from DarkPicturesRT (UE 4.26) for Until Dawn (UE 5), whose shipped PDB gives
// the layouts below (docs/FINDINGS.md, "Console variables").
//
// IConsoleManager::Singleton is found from the game's own code: cvar lookups load it with
// `mov rcx, [rip+Singleton]` shortly before `lea rdx, [rip+L"r.ScreenPercentage"]`. A few
// unrelated matches exist, so the address most matches agree on wins.
//
// Vtable offsets (FConsoleManager, FConsoleVariable<T>, FConsoleVariableRef<T>):
//   IConsoleManager  +0xb8  FindConsoleVariable(const TCHAR* Name, bool bTrackFrequentCalls)
//                           (UE 4.26: +0xb0)
//   IConsoleObject   +0x30  IsVariableBool    +0x38 IsVariableInt
//                    +0x40  IsVariableFloat   +0x48 IsVariableString
//                    +0x50  AsVariableBool    +0x58 AsVariableInt   +0x60 AsVariableFloat
//                           (TConsoleVariableData<T>*, null for Ref cvars)
//   IConsoleVariable +0x80  Set(const TCHAR* Value, EConsoleVariableFlags SetBy)
//                    +0x88  GetBool  +0x90 GetInt  +0x98 GetFloat
//
// Storage:
//   data cvar  main (game thread) value +0x48, renderer's copy right after it
//              (+0x4c for int/float, +0x49 for bool)
//   Ref cvar   main value +0x50, pointer to the global the renderer reads at +0x48
// Set stores the main value, then OnCVarChange copies it to the renderer's - but only on
// the game thread. Off it (ReShade's present runs on the RHI thread) MirrorToRenderer
// does that copy.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <cwchar>
#include <map>

namespace cvars {

constexpr uint32_t kFindConsoleVariable = 0xb8;
constexpr uint32_t kIsVariableBool = 0x30;
constexpr uint32_t kIsVariableInt = 0x38;
constexpr uint32_t kIsVariableFloat = 0x40;
constexpr uint32_t kIsVariableString = 0x48;
constexpr uint32_t kAsVariableBool = 0x50;
constexpr uint32_t kAsVariableInt = 0x58;
constexpr uint32_t kAsVariableFloat = 0x60;
constexpr uint32_t kSet = 0x80;
constexpr uint32_t kGetInt = 0x90;
constexpr uint32_t kGetFloat = 0x98;
constexpr uint32_t kFlags = 0x18;  // EConsoleVariableFlags; top byte = SetBy priority
// What FConsoleManager::ProcessUserConsoleInput passes to Set. UE 5 renumbered the
// priorities (4.26: 0x09000000); the game sets some cvars from code at 0x0a000000.
constexpr uint32_t kSetByConsole = 0x0b000000;

struct Section {
    const uint8_t* begin = nullptr;
    size_t size = 0;
};

inline bool FindSection(const uint8_t* base, const char* name, Section& out) {
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        if (strncmp(reinterpret_cast<const char*>(sec[i].Name), name, 8) == 0) {
            out = {base + sec[i].VirtualAddress, sec[i].Misc.VirtualSize};
            return true;
        }
    return false;
}

// A NUL-terminated UTF-16 string that starts right after another string's terminator.
inline const uint8_t* FindWideString(const Section& s, const wchar_t* text) {
    const size_t n = (wcslen(text) + 1) * 2;
    for (size_t i = 2; i + n <= s.size; i += 2)
        if (s.begin[i - 2] == 0 && s.begin[i - 1] == 0 && memcmp(s.begin + i, text, n) == 0) return s.begin + i;
    return nullptr;
}

// IConsoleManager::Singleton (the variable holding the pointer), or null. *votes gets how
// many of the matches agreed, *matches how many there were.
inline void** FindConsoleManagerSingleton(const uint8_t* base, int* votes = nullptr, int* matches = nullptr) {
    Section text, rdata;
    if (!FindSection(base, ".text", text) || !FindSection(base, ".rdata", rdata)) return nullptr;
    const uint8_t* name = FindWideString(rdata, L"r.ScreenPercentage");
    if (!name) return nullptr;
    std::map<void**, int> found;
    int total = 0;
    for (size_t i = 0x40; i + 7 <= text.size; ++i) {
        const uint8_t* p = text.begin + i;
        // lea r64, [rip+disp32] -> the name
        if ((p[0] != 0x48 && p[0] != 0x4c) || p[1] != 0x8d || (p[2] & 0xc7) != 0x05) continue;
        int32_t disp;
        memcpy(&disp, p + 3, 4);
        if (p + 7 + disp != name) continue;
        // mov rcx, [rip+disp32] within the 0x40 bytes before it
        for (const uint8_t* q = p - 7; q >= p - 0x40; --q) {
            if (q[0] != 0x48 || q[1] != 0x8b || q[2] != 0x0d) continue;
            memcpy(&disp, q + 3, 4);
            ++found[reinterpret_cast<void**>(const_cast<uint8_t*>(q + 7 + disp))];
            ++total;
            break;
        }
    }
    void** best = nullptr;
    int bestVotes = 0;
    for (auto& [addr, n] : found)
        if (n > bestVotes) best = addr, bestVotes = n;
    if (votes) *votes = bestVotes;
    if (matches) *matches = total;
    return best;
}

using FindFn = void*(__fastcall*)(void* self, const wchar_t* name, bool track);
using PtrFn = void*(__fastcall*)(void* self);
using BoolFn = bool(__fastcall*)(void* self);

template <typename Fn>
inline Fn Virtual(void* object, uint32_t offset) {
    return *reinterpret_cast<Fn*>(*reinterpret_cast<uint8_t**>(object) + offset);
}

// memcpy that reports a fault instead of raising it.
inline bool SafeCopy(void* dst, const void* src, size_t n) {
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The IConsoleVariable object for <name> (null if absent or a command). Guarded: a wrong
// vtable layout faults here instead of taking the game down.
inline void* FindVariable(void* manager, const wchar_t* name) {
    __try {
        return Virtual<FindFn>(manager, kFindConsoleVariable)(manager, name, true);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

inline void* CallPtr(void* var, uint32_t offset) {
    __try {
        return Virtual<PtrFn>(var, offset)(var);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

inline bool CallBool(void* var, uint32_t offset, bool& out) {
    __try {
        out = Virtual<BoolFn>(var, offset)(var);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

enum class Type { Unknown, Bool, Int, Float, String };

inline const char* TypeName(Type t) {
    switch (t) {
        case Type::Bool: return "bool";
        case Type::Int: return "int";
        case Type::Float: return "float";
        case Type::String: return "string";
        default: return "?";
    }
}

// The value type, from the IsVariable* slots.
inline Type GetType(void* var) {
    bool is = false;
    if (CallBool(var, kIsVariableFloat, is) && is) return Type::Float;
    if (CallBool(var, kIsVariableInt, is) && is) return Type::Int;
    if (CallBool(var, kIsVariableBool, is) && is) return Type::Bool;
    if (CallBool(var, kIsVariableString, is) && is) return Type::String;
    return Type::Unknown;
}

// TConsoleVariableData<T>* for data cvars, null for Ref cvars.
inline void* AsData(void* var, Type t) {
    switch (t) {
        case Type::Bool: return CallPtr(var, kAsVariableBool);
        case Type::Int: return CallPtr(var, kAsVariableInt);
        case Type::Float: return CallPtr(var, kAsVariableFloat);
        default: return nullptr;
    }
}

using SetFn = void(__fastcall*)(void* self, const wchar_t* value, uint32_t setBy);

// IConsoleVariable::Set: refuses a SetBy priority lower than the last one (CanChange),
// stores the main value, records the priority and runs the change delegates. Follow it
// with MirrorToRenderer off the game thread. False if it faulted.
inline bool Set(void* var, const wchar_t* value, uint32_t setBy) {
    __try {
        Virtual<SetFn>(var, kSet)(var, value, setBy);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The flags word; the top byte is the priority of the last Set.
inline uint32_t& Flags(void* var) { return *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(var) + kFlags); }

// Where Set keeps the main value and where the renderer reads it.
struct Storage {
    Type type = Type::Unknown;
    bool ref = false;
    uint8_t* render = nullptr;
    const uint8_t* main = nullptr;
    size_t size = 0;
};

inline bool GetStorage(void* var, Storage& out) {
    out = {};
    out.type = GetType(var);
    if (out.type != Type::Bool && out.type != Type::Int && out.type != Type::Float) return false;
    out.size = out.type == Type::Bool ? 1 : 4;
    auto obj = static_cast<uint8_t*>(var);
    if (void* data = AsData(var, out.type)) {
        out.main = static_cast<uint8_t*>(data);
        out.render = static_cast<uint8_t*>(data) + out.size;
        return true;
    }
    uint8_t* global = nullptr;
    if (!SafeCopy(&global, obj + 0x48, 8) || !global) return false;
    out.ref = true;
    out.main = obj + 0x50;
    out.render = global;
    return true;
}

// Copies the main value to where the renderer reads it (what OnCVarChange does on the
// game thread). An aligned store, so the render thread sees old or new, not a mix.
inline bool MirrorToRenderer(void* var) {
    Storage s;
    if (!GetStorage(var, s)) return false;
    uint8_t value[4];
    return SafeCopy(value, s.main, s.size) && SafeCopy(s.render, value, s.size);
}

// Reads 1 or 4 bytes as a double in the storage's type.
inline bool ReadValue(const Storage& s, const uint8_t* where, double& out) {
    uint32_t raw = 0;
    if (!SafeCopy(&raw, where, s.size)) return false;
    if (s.type == Type::Float) {
        float f;
        memcpy(&f, &raw, 4);
        out = f;
    } else if (s.type == Type::Bool) {
        out = (raw & 0xff) ? 1 : 0;
    } else {
        out = static_cast<int32_t>(raw);
    }
    return true;
}

}  // namespace cvars
