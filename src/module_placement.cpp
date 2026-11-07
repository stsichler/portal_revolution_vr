#include "module_placement.h"
#include <Windows.h>
#include <psapi.h>
#include <cwchar>
#include <iostream>
#include "MinHook.h"

// Windows picks one address per DLL for each boot. If it is taken in this process, the DLL goes
// somewhere else, and for client.dll (537 MB in memory) other game DLLs can well be there. Then it
// ends up among the game's heaps, without free memory within reach of MinHook's trampolines, and
// none of its hooks can be created: no VR at all, depending only on the boot (measured 2026-11-07).
// So such a DLL gets mapped again in the largest free gap among the DLLs instead.
namespace
{
    constexpr LONG kStatusImageNotAtBase = 0x40000003; // the loader has to relocate it
    constexpr LONG kStatusImageAtDifferentBase = 0x40000036; // relocated by the kernel
    constexpr uintptr_t kDllAreaBegin = 0x7FF000000000;
    constexpr uintptr_t kDllAreaEnd = 0x7FFF00000000;
    constexpr uintptr_t kGranularity = 0x10000;
    constexpr const wchar_t *kModulesToKeepClose[] = { L"\\client.dll", L"\\server.dll", L"\\panorama.dll" };

    using tNtMapViewOfSection = LONG(NTAPI *)(HANDLE section, HANDLE process, void **base, ULONG_PTR zeroBits, SIZE_T commitSize,
        LARGE_INTEGER *offset, SIZE_T *viewSize, DWORD inherit, ULONG allocationType, ULONG protection);
    using tNtMapViewOfSectionEx = LONG(NTAPI *)(HANDLE section, HANDLE process, void **base, LARGE_INTEGER *offset, SIZE_T *viewSize,
        ULONG allocationType, ULONG protection, void *parameters, ULONG parameterCount);
    using tNtUnmapViewOfSection = LONG(NTAPI *)(HANDLE process, void *base);

    tNtMapViewOfSection oNtMapViewOfSection = nullptr;
    tNtMapViewOfSectionEx oNtMapViewOfSectionEx = nullptr;
    tNtUnmapViewOfSection NtUnmapViewOfSection = nullptr;

    const wchar_t *ModuleToKeepClose(void *base)
    {
        wchar_t path[MAX_PATH];
        const DWORD length = GetMappedFileNameW(GetCurrentProcess(), base, path, MAX_PATH);
        for (const wchar_t *name : kModulesToKeepClose)
        {
            const size_t nameLength = wcslen(name);
            if (length >= nameLength && !_wcsicmp(path + length - nameLength, name))
                return name + 1;
        }
        return nullptr;
    }

    // In the middle of the largest gap: free memory on both sides for the trampolines.
    void *FindPlace(SIZE_T size)
    {
        uintptr_t gap = 0;
        SIZE_T gapSize = 0;
        for (uintptr_t address = kDllAreaBegin; address < kDllAreaEnd;)
        {
            MEMORY_BASIC_INFORMATION info;
            if (!VirtualQuery(reinterpret_cast<void *>(address), &info, sizeof(info)))
                break;
            if (info.State == MEM_FREE && info.RegionSize > gapSize)
            {
                gap = reinterpret_cast<uintptr_t>(info.BaseAddress);
                gapSize = info.RegionSize;
            }
            address = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
        }
        if (gapSize < size + 2 * kGranularity)
            return nullptr;
        return reinterpret_cast<void *>((gap + (gapSize - size) / 2) & ~(kGranularity - 1));
    }

    // After a mapping that went somewhere else. map(base, size) maps the same section again.
    template <typename Map>
    LONG MoveIfNeeded(LONG status, HANDLE process, void **base, SIZE_T *size, Map map)
    {
        if ((status != kStatusImageNotAtBase && status != kStatusImageAtDifferentBase) || process != GetCurrentProcess() || !base || !size)
            return status;
        const wchar_t *name = ModuleToKeepClose(*base);
        void *place = name ? FindPlace(*size) : nullptr;
        if (!place)
            return status;

        void *const away = *base;
        NtUnmapViewOfSection(process, away);
        *base = place;
        *size = 0;
        const LONG moved = map(base, size);
        std::cout << "[Modules] ";
        for (const wchar_t *c = name; *c; ++c)
            std::cout << static_cast<char>(*c);
        std::cout << " couldn't take its usual address and went to " << away << ", now at " << *base << ", status 0x" << std::hex << moved << std::dec << "\n";
        if (moved >= 0)
            return moved;

        // As Windows would have done it.
        *base = nullptr;
        *size = 0;
        return map(base, size);
    }

    LONG NTAPI hkNtMapViewOfSection(HANDLE section, HANDLE process, void **base, ULONG_PTR zeroBits, SIZE_T commitSize,
        LARGE_INTEGER *offset, SIZE_T *viewSize, DWORD inherit, ULONG allocationType, ULONG protection)
    {
        const bool anywhere = base && !*base;
        const LONG status = oNtMapViewOfSection(section, process, base, zeroBits, commitSize, offset, viewSize, inherit, allocationType, protection);
        if (!anywhere)
            return status;
        return MoveIfNeeded(status, process, base, viewSize, [&](void **newBase, SIZE_T *newSize)
        {
            return oNtMapViewOfSection(section, process, newBase, zeroBits, commitSize, offset, newSize, inherit, allocationType, protection);
        });
    }

    LONG NTAPI hkNtMapViewOfSectionEx(HANDLE section, HANDLE process, void **base, LARGE_INTEGER *offset, SIZE_T *viewSize,
        ULONG allocationType, ULONG protection, void *parameters, ULONG parameterCount)
    {
        const bool anywhere = base && !*base;
        const LONG status = oNtMapViewOfSectionEx(section, process, base, offset, viewSize, allocationType, protection, parameters, parameterCount);
        if (!anywhere)
            return status;
        return MoveIfNeeded(status, process, base, viewSize, [&](void **newBase, SIZE_T *newSize)
        {
            return oNtMapViewOfSectionEx(section, process, newBase, offset, newSize, allocationType, protection, parameters, parameterCount);
        });
    }

    template <typename F>
    void Hook(HMODULE ntdll, const char *name, F detour, F &original)
    {
        void *target = reinterpret_cast<void *>(GetProcAddress(ntdll, name));
        if (!target)
            return;
        MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void *>(detour), reinterpret_cast<void **>(&original));
        if (status == MH_OK)
            status = MH_EnableHook(target);
        std::cout << "[Modules] hook " << name << ": " << MH_StatusToString(status) << "\n";
    }
}

unsigned long __stdcall ModulePlacement::Install(void *)
{
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    NtUnmapViewOfSection = reinterpret_cast<tNtUnmapViewOfSection>(GetProcAddress(ntdll, "NtUnmapViewOfSection"));
    if (!NtUnmapViewOfSection)
        return 0;

    MH_Initialize(); // MH_ERROR_ALREADY_INITIALIZED is fine
    Hook(ntdll, "NtMapViewOfSection", &hkNtMapViewOfSection, oNtMapViewOfSection);
    Hook(ntdll, "NtMapViewOfSectionEx", &hkNtMapViewOfSectionEx, oNtMapViewOfSectionEx);
    return 0;
}
