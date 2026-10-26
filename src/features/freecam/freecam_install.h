// features/freecam/freecam_install.h - instalacion del freecam de camara.
//
// Carga payload\payload_cam.dll dentro de ArkAscended.exe con manual mapping y
// parchea las dos escrituras del POV del PlayerCameraManager. Todo desde
// coffeeclaw.exe, que ya corre como administrador.
//
// Funciona en servidores dedicados: la camara la calcula solo el cliente.
// En single-player el servidor local reescribe la camara, asi que no se espera
// que funcione. Es una limitacion conocida, no un fallo.

#pragma once

#include <windows.h>
#include <psapi.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <tlhelp32.h>

namespace features {
namespace freecam {

// RVA de los dos sitios que escriben el POV y de las instrucciones siguientes.
constexpr uintptr_t kSiteA = 0x40C13ED;  // MOVSD [RSI+0x10], XMM1
constexpr uintptr_t kBackA = 0x40C13F2;
constexpr uintptr_t kSiteB = 0x66B9500;  // MOVSD [R9+0x10], XMM1
constexpr uintptr_t kBackB = 0x66B9505;

inline bool ReadMem(HANDLE h, uintptr_t a, void* out, size_t n) {
    SIZE_T got = 0;
    return ::ReadProcessMemory(h, (LPCVOID)a, out, n, &got) && got == n;
}
inline bool WriteMem(HANDLE h, uintptr_t a, const void* d, size_t n) {
    SIZE_T put = 0;
    return ::WriteProcessMemory(h, (LPVOID)a, d, n, &put) && put == n;
}

inline uintptr_t RemoteModuleBase(HANDLE h, const wchar_t* name) {
    HMODULE mods[1024]{};
    DWORD need = 0;
    if (!::EnumProcessModules(h, mods, sizeof(mods), &need)) return 0;
    const DWORD n = need / sizeof(HMODULE);
    for (DWORD i = 0; i < n && i < 1024; ++i) {
        wchar_t nm[MAX_PATH]{};
        if (::GetModuleBaseNameW(h, mods[i], nm, MAX_PATH) && _wcsicmp(nm, name) == 0)
            return (uintptr_t)mods[i];
    }
    return 0;
}

inline size_t RvaToOff(const std::vector<uint8_t>& f, uint64_t rva) {
    auto* dos = (IMAGE_DOS_HEADER*)f.data();
    auto* nt = (IMAGE_NT_HEADERS64*)(f.data() + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const uint64_t lo = sec[i].VirtualAddress;
        const uint64_t hi = lo + (sec[i].SizeOfRawData ? sec[i].SizeOfRawData : sec[i].Misc.VirtualSize);
        if (rva >= lo && rva < hi) return (size_t)(sec[i].PointerToRawData + (rva - lo));
    }
    return (size_t)rva;
}

inline uint64_t ExportRva(const std::vector<uint8_t>& f, const char* want) {
    auto* dos = (IMAGE_DOS_HEADER*)f.data();
    auto* nt = (IMAGE_NT_HEADERS64*)(f.data() + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress) return 0;
    const size_t off = RvaToOff(f, dir.VirtualAddress);
    if (off + sizeof(IMAGE_EXPORT_DIRECTORY) > f.size()) return 0;
    auto* exp = (IMAGE_EXPORT_DIRECTORY*)(f.data() + off);
    const size_t nOff = RvaToOff(f, exp->AddressOfNames);
    const size_t oOff = RvaToOff(f, exp->AddressOfNameOrdinals);
    const size_t fOff = RvaToOff(f, exp->AddressOfFunctions);
    if (nOff + exp->NumberOfNames * 4 > f.size()) return 0;
    if (oOff + exp->NumberOfNames * 2 > f.size()) return 0;
    if (fOff + exp->NumberOfFunctions * 4 > f.size()) return 0;
    auto* names = (uint32_t*)(f.data() + nOff);
    auto* ords = (uint16_t*)(f.data() + oOff);
    auto* funcs = (uint32_t*)(f.data() + fOff);
    for (uint32_t i = 0; i < exp->NumberOfNames; ++i) {
        const char* nm = (const char*)(f.data() + RvaToOff(f, names[i]));
        if (std::strcmp(nm, want) != 0) continue;
        const uint16_t o = ords[i];
        if (o >= exp->NumberOfFunctions) return 0;
        return funcs[o];
    }
    return 0;
}

inline uintptr_t RemoteGetProc(HANDLE h, uintptr_t base, const char* func, int depth = 0) {
    if (depth > 3) return 0;
    IMAGE_DOS_HEADER dos{};
    if (!ReadMem(h, base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS64 nt{};
    if (!ReadMem(h, base + dos.e_lfanew, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE) return 0;
    const auto& dir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress) return 0;
    IMAGE_EXPORT_DIRECTORY exp{};
    if (!ReadMem(h, base + dir.VirtualAddress, &exp, sizeof(exp))) return 0;
    std::vector<uint32_t> names(exp.NumberOfNames), funcs(exp.NumberOfFunctions);
    std::vector<uint16_t> ords(exp.NumberOfNames);
    if (!ReadMem(h, base + exp.AddressOfNames, names.data(), names.size() * 4)) return 0;
    if (!ReadMem(h, base + exp.AddressOfNameOrdinals, ords.data(), ords.size() * 2)) return 0;
    if (!ReadMem(h, base + exp.AddressOfFunctions, funcs.data(), funcs.size() * 4)) return 0;
    const uint32_t lo = dir.VirtualAddress, hi = dir.VirtualAddress + dir.Size;
    for (uint32_t i = 0; i < exp.NumberOfNames; ++i) {
        char nm[256]{};
        if (!ReadMem(h, base + names[i], nm, sizeof(nm) - 1)) continue;
        if (std::strcmp(nm, func) != 0) continue;
        const uint16_t o = ords[i];
        if (o >= exp.NumberOfFunctions) return 0;
        const uint32_t rva = funcs[o];
        if (rva >= lo && rva < hi) {
            char fwd[256]{};
            if (!ReadMem(h, base + rva, fwd, sizeof(fwd) - 1)) return 0;
            char* dot = std::strchr(fwd, '.');
            if (!dot) return 0;
            std::string dll(fwd, (size_t)(dot - fwd));
            dll += ".dll";
            wchar_t w[128]{};
            for (size_t k = 0; k < dll.size() && k < 127; ++k) w[k] = (wchar_t)dll[k];
            const uintptr_t other = RemoteModuleBase(h, w);
            if (!other) return 0;
            return RemoteGetProc(h, other, dot + 1, depth + 1);
        }
        return base + rva;
    }
    return 0;
}

// Instala el payload y los dos parches. Devuelve true si todo entro.
inline bool Install(DWORD pid, const std::wstring& dllPath, std::wstring& log) {
    HANDLE h = ::OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ |
                             PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h) { log += L"freecam: no pude abrir el proceso\n"; return false; }

    FILE* f = _wfopen(dllPath.c_str(), L"rb");
    if (!f) { log += L"freecam: no encuentro payload_cam.dll\n"; ::CloseHandle(h); return false; }
    std::fseek(f, 0, SEEK_END);
    const long fs = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> file((size_t)fs);
    const bool readOk = std::fread(file.data(), 1, file.size(), f) == file.size();
    std::fclose(f);
    if (!readOk) { log += L"freecam: payload ilegible\n"; ::CloseHandle(h); return false; }

    auto* dos = (IMAGE_DOS_HEADER*)file.data();
    auto* nt = (IMAGE_NT_HEADERS64*)(file.data() + dos->e_lfanew);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || nt->Signature != IMAGE_NT_SIGNATURE) {
        log += L"freecam: payload no es PE\n";
        ::CloseHandle(h);
        return false;
    }
    const uint64_t pref = nt->OptionalHeader.ImageBase;
    const DWORD size = nt->OptionalHeader.SizeOfImage;

    void* remote = ::VirtualAllocEx(h, (LPVOID)pref, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!remote) remote = ::VirtualAllocEx(h, nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!remote) { log += L"freecam: sin memoria en el juego\n"; ::CloseHandle(h); return false; }
    const uint64_t base = (uint64_t)remote;

    WriteMem(h, base, file.data(), nt->OptionalHeader.SizeOfHeaders);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const DWORD sz = sec[i].SizeOfRawData ? sec[i].SizeOfRawData : sec[i].Misc.VirtualSize;
        if (sz && (uint64_t)sec[i].PointerToRawData + sz <= file.size())
            WriteMem(h, base + sec[i].VirtualAddress, file.data() + sec[i].PointerToRawData, sz);
    }

    const auto& rel = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (rel.VirtualAddress && base != pref) {
        uint64_t off = 0;
        while (off < rel.Size) {
            IMAGE_BASE_RELOCATION blk{};
            if (!ReadMem(h, base + rel.VirtualAddress + off, &blk, sizeof(blk))) break;
            if (!blk.SizeOfBlock || !blk.VirtualAddress) break;
            const DWORD n = (blk.SizeOfBlock - sizeof(blk)) / 2;
            for (DWORD i = 0; i < n; ++i) {
                uint16_t e = 0;
                if (!ReadMem(h, base + rel.VirtualAddress + off + sizeof(blk) + i * 2, &e, 2)) break;
                if ((e >> 12) != IMAGE_REL_BASED_DIR64) continue;
                const uint64_t a = base + blk.VirtualAddress + (e & 0xFFF);
                uint64_t v = 0;
                if (!ReadMem(h, a, &v, 8)) continue;
                v += (base - pref);
                WriteMem(h, a, &v, 8);
            }
            off += blk.SizeOfBlock;
        }
    }

    const auto& imp = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (imp.VirtualAddress) {
        uint64_t off = 0;
        for (;;) {
            IMAGE_IMPORT_DESCRIPTOR d{};
            if (!ReadMem(h, base + imp.VirtualAddress + off, &d, sizeof(d))) break;
            if (!d.Name) break;
            char nm[256]{};
            ReadMem(h, base + d.Name, nm, sizeof(nm) - 1);
            wchar_t w[256]{};
            for (int i = 0; i < 255 && nm[i]; ++i) w[i] = (wchar_t)nm[i];
            const uintptr_t mod = RemoteModuleBase(h, w);
            if (mod) {
                uint64_t thunk = base + (d.FirstThunk ? d.FirstThunk : d.OriginalFirstThunk);
                uint64_t orig = base + d.OriginalFirstThunk;
                for (;;) {
                    uint64_t e = 0;
                    if (!ReadMem(h, orig, &e, 8) || !e) break;
                    if (!(e & 0x8000000000000000ULL)) {
                        char fn[256]{};
                        ReadMem(h, base + (e & 0x7FFFFFFF) + 2, fn, sizeof(fn) - 1);
                        const uintptr_t a = RemoteGetProc(h, mod, fn);
                        if (a) WriteMem(h, thunk, &a, 8);
                    }
                    thunk += 8; orig += 8;
                }
            }
            off += sizeof(d);
        }
    }

    const uint64_t entry = base + nt->OptionalHeader.AddressOfEntryPoint;
    HANDLE th = ::CreateRemoteThread(h, nullptr, 0, (LPTHREAD_START_ROUTINE)entry, (LPVOID)base, 0, nullptr);
    if (!th) { log += L"freecam: no pude crear el hilo\n"; ::CloseHandle(h); return false; }
    ::WaitForSingleObject(th, 5000);
    DWORD code = 0;
    ::GetExitCodeThread(th, &code);
    ::CloseHandle(th);
    if (code != 0x11) {
        log += (code == 0x22) ? L"freecam: ya estaba instalado\n" : L"freecam: el payload no arranco\n";
        ::CloseHandle(h);
        return false;
    }

    const uintptr_t mod = RemoteModuleBase(h, L"ArkAscended.exe");
    if (!mod) { log += L"freecam: sin modulo del juego\n"; ::CloseHandle(h); return false; }

    auto doPatch = [&](uintptr_t site, uintptr_t back, const char* hookName, const char* backName) -> bool {
        const uint64_t hookRva = ExportRva(file, hookName);
        const uint64_t backRva = ExportRva(file, backName);
        if (!hookRva || !backRva) return false;
        const uint64_t hookVA = base + hookRva;
        uint8_t stub[14] = { 0xFF, 0x25, 0, 0, 0, 0 };
        std::memcpy(stub + 6, &hookVA, 8);
        void* s = nullptr;
        for (int k = 1; k < 4000 && !s; ++k)
            s = ::VirtualAllocEx(h, (LPVOID)(mod + (uintptr_t)k * 0x10000), 0x1000,
                                 MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        for (int k = 1; k < 4000 && !s; ++k)
            s = ::VirtualAllocEx(h, (LPVOID)(mod - (uintptr_t)k * 0x10000), 0x1000,
                                 MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!s) return false;
        WriteMem(h, (uintptr_t)s, stub, sizeof(stub));

        const uintptr_t target = mod + site;
        const int64_t delta = (int64_t)((uintptr_t)s - (target + 5));
        if (delta > 0x7FFFFFFFLL || delta < -0x7FFFFFFFLL) return false;

        uint8_t patch[5];
        patch[0] = 0xE9;
        const int32_t d32 = (int32_t)delta;
        std::memcpy(patch + 1, &d32, 4);
        DWORD old = 0;
        ::VirtualProtectEx(h, (LPVOID)target, 5, PAGE_EXECUTE_READWRITE, &old);
        const bool ok = WriteMem(h, target, patch, 5);
        DWORD tmp = 0;
        ::VirtualProtectEx(h, (LPVOID)target, 5, old, &tmp);

        const uint64_t backVA = mod + back;
        WriteMem(h, base + backRva, &backVA, 8);
        return ok;
    };

    const bool a = doPatch(kSiteA, kBackA, "HookA", "g_backA");
    const bool b = doPatch(kSiteB, kBackB, "HookB", "g_backB");

    ::CloseHandle(h);
    if (!a || !b) {
        log += L"freecam: parches incompletos (";
        log += a ? L"A ok" : L"A fallo";
        log += L", ";
        log += b ? L"B ok" : L"B fallo";
        log += L")\n";
        return false;
    }
    log += L"freecam instalado: R activa, flechas mueven, R vuelve\n";
    return true;
}

} // namespace freecam
} // namespace features
