// Finding code in the game's DLLs: vtables by RTTI name, strings, calls.
#include "strata_hooks.h"
#include <Windows.h>
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace StrataHooks
{
    struct Section
    {
        uint8_t *begin;
        uint8_t *end;
    };

    // The module's sections that hold code or data (RTTI lives in .rdata).
    std::vector<Section> Sections(uint8_t *base, bool code)
    {
        std::vector<Section> sections;
        auto dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
        auto nt = reinterpret_cast<IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
        IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(nt);
        for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
        {
            if (((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0) != code)
                continue;
            uint8_t *begin = base + section->VirtualAddress;
            sections.push_back({ begin, begin + section->Misc.VirtualSize });
        }
        return sections;
    }

    // The instruction `lea <register>, [rip + text]`; modrm selects the register (0x0D rcx, 0x15 rdx).
    uint8_t *FindStringLea(uint8_t *base, const char *text, uint8_t modrm)
    {
        // With the 0 before and after it, so it isn't the end of a longer string.
        const std::string needle = std::string(1, '\0') + text + std::string(1, '\0');
        uint8_t *string = nullptr;
        for (const Section &section : Sections(base, false))
        {
            uint8_t *found = std::search(section.begin, section.end, needle.begin(), needle.end());
            if (found != section.end)
            {
                string = found + 1;
                break;
            }
        }
        if (!string)
            return nullptr;

        for (const Section &section : Sections(base, true))
        {
            for (uint8_t *p = section.begin; p + 7 <= section.end; ++p)
            {
                if (p[0] == 0x48 && p[1] == 0x8D && p[2] == modrm && p + 7 + *reinterpret_cast<int32_t *>(p + 3) == string)
                    return p;
            }
        }
        return nullptr;
    }

    // The target of the `call rel32` at this address, nullptr if there is none.
    uint8_t *CallTarget(uint8_t *call)
    {
        return *call == 0xE8 ? call + 5 + *reinterpret_cast<int32_t *>(call + 1) : nullptr;
    }
}

void **StrataHooks::FindVTable(const char *module, const char *className, uint32_t offset)
{
    auto base = reinterpret_cast<uint8_t *>(GetModuleHandleA(module));
    if (!base)
        return nullptr;
    const std::vector<Section> sections = Sections(base, false);

    // RTTI type descriptor: vftable pointer, spare pointer, then the decorated name.
    const std::string name = std::string(".?AV") + className + "@@";
    const char *nameBegin = name.c_str(), *nameEnd = nameBegin + name.size() + 1; // with the terminating 0
    uint8_t *typeDescriptor = nullptr;
    for (const Section &section : sections)
    {
        uint8_t *found = std::search(section.begin, section.end, nameBegin, nameEnd);
        if (found != section.end)
        {
            typeDescriptor = found - 0x10;
            break;
        }
    }
    if (!typeDescriptor)
        return nullptr;
    const uint32_t typeDescriptorRva = (uint32_t)(typeDescriptor - base);

    // Complete object locator of the vtable: signature 1, offset of the subobject, type descriptor,
    // and (x64) its own RVA.
    uint8_t *locator = nullptr;
    for (const Section &section : sections)
    {
        for (uint8_t *p = section.begin; p + 24 <= section.end && !locator; p += 4)
        {
            auto fields = reinterpret_cast<const uint32_t *>(p);
            if (fields[0] == 1 && fields[1] == offset && fields[3] == typeDescriptorRva && fields[5] == (uint32_t)(p - base))
                locator = p;
        }
        if (locator)
            break;
    }
    if (!locator)
        return nullptr;

    // The vtable follows the pointer to the locator.
    for (const Section &section : sections)
    {
        for (uint8_t *p = section.begin; p + 16 <= section.end; p += 8)
        {
            if (*reinterpret_cast<uint8_t **>(p) == locator)
                return reinterpret_cast<void **>(p + 8);
        }
    }
    return nullptr;
}

int StrataHooks::VTableLength(void *const *vtable, const char *module)
{
    auto base = reinterpret_cast<uint8_t *>(GetModuleHandleA(module));
    if (!base || !vtable)
        return 0;
    const std::vector<Section> code = Sections(base, true);
    int length = 0;
    for (;; ++length)
    {
        const auto entry = static_cast<const uint8_t *>(vtable[length]);
        if (std::none_of(code.begin(), code.end(), [entry](const Section &section) { return entry >= section.begin && entry < section.end; }))
            return length;
    }
}

void **StrataHooks::FindCheckedVTable(const char *module, const char *className, int length, uint32_t offset)
{
    void **vtable = FindVTable(module, className, offset);
    if (!vtable)
        return nullptr;
    const int found = VTableLength(vtable, module);
    if (found == length)
        return vtable;
    std::cout << "[Hooks] " << className << " in " << module << " has " << found << " vtable entries instead of " << length
              << ": the game has changed, what uses them stays off\n";
    return nullptr;
}
