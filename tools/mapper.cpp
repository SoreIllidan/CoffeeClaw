// mapper - manual mapping de una DLL en ArkAscended.exe.
// Sin LoadLibrary: escribe la imagen, aplica relocalizaciones, resuelve imports
// contra los modulos ya cargados y llama al punto de entrada.
//
// Uso: mapper.exe <ruta.dll>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <cstdio>
#include <cwchar>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

static FILE* g_log = nullptr;
static void Log(const char* fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::printf("%s\n", buf);
    if (g_log) { std::fprintf(g_log, "%s\n", buf); std::fflush(g_log); }
}

static DWORD FindPid(const wchar_t* name) {
    HANDLE s = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (::Process32FirstW(s, &pe)) {
        do { if (_wcsicmp(pe.szExeFile, name) == 0) { pid = pe.th32ProcessID; break; } } while (::Process32NextW(s, &pe));
    }
    ::CloseHandle(s);
    return pid;
}

struct T {
    HANDLE h = nullptr;
    bool Read(uintptr_t a, void* out, size_t n) {
        SIZE_T got = 0;
        return ::ReadProcessMemory(h, (LPCVOID)a, out, n, &got) && got == n;
    }
    bool Write(uintptr_t a, const void* d, size_t n) {
        SIZE_T put = 0;
        return ::WriteProcessMemory(h, (LPVOID)a, d, n, &put) && put == n;
    }
};

static uintptr_t ModuleBase(HANDLE h, const wchar_t* name) {
    HMODULE mods[1024]{}; DWORD need = 0;
    if (!::EnumProcessModules(h, mods, sizeof(mods), &need)) return 0;
    const DWORD n = need / sizeof(HMODULE);
    for (DWORD i = 0; i < n && i < 1024; ++i) {
        wchar_t nm[MAX_PATH]{};
        if (::GetModuleBaseNameW(h, mods[i], nm, MAX_PATH) && _wcsicmp(nm, name) == 0) return (uintptr_t)mods[i];
    }
    return 0;
}

static uintptr_t GetProc(T& t, uintptr_t base, const char* func, int depth = 0) {
    if (depth > 3) return 0;
    IMAGE_DOS_HEADER dos{};
    if (!t.Read(base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS64 nt{};
    if (!t.Read(base + dos.e_lfanew, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE) return 0;
    const auto& dir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress) return 0;

    IMAGE_EXPORT_DIRECTORY exp{};
    if (!t.Read(base + dir.VirtualAddress, &exp, sizeof(exp))) return 0;
    std::vector<uint32_t> names(exp.NumberOfNames), funcs(exp.NumberOfFunctions);
    std::vector<uint16_t> ords(exp.NumberOfNames);
    if (!t.Read(base + exp.AddressOfNames, names.data(), names.size() * 4)) return 0;
    if (!t.Read(base + exp.AddressOfNameOrdinals, ords.data(), ords.size() * 2)) return 0;
    if (!t.Read(base + exp.AddressOfFunctions, funcs.data(), funcs.size() * 4)) return 0;

    const uint32_t lo = dir.VirtualAddress, hi = dir.VirtualAddress + dir.Size;
    for (uint32_t i = 0; i < exp.NumberOfNames; ++i) {
        char nm[256]{};
        if (!t.Read(base + names[i], nm, sizeof(nm) - 1)) continue;
        if (std::strcmp(nm, func) != 0) continue;
        const uint16_t o = ords[i];
        if (o >= exp.NumberOfFunctions) return 0;
        const uint32_t rva = funcs[o];
        if (rva >= lo && rva < hi) {
            char fwd[256]{};
            if (!t.Read(base + rva, fwd, sizeof(fwd) - 1)) return 0;
            char* dot = std::strchr(fwd, '.');
            if (!dot) return 0;
            std::string dll(fwd, (size_t)(dot - fwd));
            dll += ".dll";
            wchar_t w[128]{};
            for (size_t k = 0; k < dll.size() && k < 127; ++k) w[k] = (wchar_t)dll[k];
            const uintptr_t other = ModuleBase(t.h, w);
            if (!other) return 0;
            return GetProc(t, other, dot + 1, depth + 1);
        }
        return base + rva;
    }
    return 0;
}

// RVA de un export del payload, leido del archivo en disco.
// RVA -> offset dentro del archivo (no son lo mismo).
static size_t RvaToOff(const std::vector<uint8_t>& file, uint64_t rva) {
    auto* dos = (IMAGE_DOS_HEADER*)file.data();
    auto* nt = (IMAGE_NT_HEADERS64*)(file.data() + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const uint64_t lo = sec[i].VirtualAddress;
        const uint64_t hi = lo + (sec[i].SizeOfRawData ? sec[i].SizeOfRawData : sec[i].Misc.VirtualSize);
        if (rva >= lo && rva < hi) return (size_t)(sec[i].PointerToRawData + (rva - lo));
    }
    return (size_t)rva;
}

static uint64_t ExportRva(const std::vector<uint8_t>& file, const char* want) {
    auto* dos = (IMAGE_DOS_HEADER*)file.data();
    auto* nt = (IMAGE_NT_HEADERS64*)(file.data() + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress) return 0;
    const size_t expOff = RvaToOff(file, dir.VirtualAddress);
    if (expOff + sizeof(IMAGE_EXPORT_DIRECTORY) > file.size()) return 0;
    auto* exp = (IMAGE_EXPORT_DIRECTORY*)(file.data() + expOff);
    const size_t namesOff = RvaToOff(file, exp->AddressOfNames);
    const size_t ordsOff = RvaToOff(file, exp->AddressOfNameOrdinals);
    const size_t funcsOff = RvaToOff(file, exp->AddressOfFunctions);
    if (namesOff + exp->NumberOfNames * 4 > file.size()) return 0;
    if (ordsOff + exp->NumberOfNames * 2 > file.size()) return 0;
    if (funcsOff + exp->NumberOfFunctions * 4 > file.size()) return 0;
    auto* names = (uint32_t*)(file.data() + namesOff);
    auto* ords = (uint16_t*)(file.data() + ordsOff);
    auto* funcs = (uint32_t*)(file.data() + funcsOff);
    for (uint32_t i = 0; i < exp->NumberOfNames; ++i) {
        const char* nm = (const char*)(file.data() + RvaToOff(file, names[i]));
        if (std::strcmp(nm, want) != 0) continue;
        const uint16_t o = ords[i];
        if (o >= exp->NumberOfFunctions) return 0;
        return funcs[o];
    }
    return 0;
}
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 2) { std::wprintf(L"uso: mapper.exe <dll>\n"); return 1; }

    wchar_t self[MAX_PATH]{};
    ::GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring s(self);
    const size_t sl = s.find_last_of(L"\\/");
    std::wstring dir = (sl == std::wstring::npos) ? L"" : s.substr(0, sl + 1);
    std::wstring logPath = dir + L"mapper.log";
    std::string lp(logPath.begin(), logPath.end());
    g_log = std::fopen(lp.c_str(), "w");

    const std::wstring dll = argv[1];
    Log("[mapper] dll: %ls", dll.c_str());

    FILE* f = _wfopen(dll.c_str(), L"rb");
    if (!f) { Log("[-] no puedo leer la DLL"); return 1; }
    std::fseek(f, 0, SEEK_END);
    const long fs = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> file((size_t)fs);
    if (std::fread(file.data(), 1, file.size(), f) != file.size()) { std::fclose(f); return 1; }
    std::fclose(f);

    auto* dos = (IMAGE_DOS_HEADER*)file.data();
    auto* nt = (IMAGE_NT_HEADERS64*)(file.data() + dos->e_lfanew);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || nt->Signature != IMAGE_NT_SIGNATURE) {
        Log("[-] no es un PE valido"); return 1;
    }
    const uint64_t pref = nt->OptionalHeader.ImageBase;
    const DWORD size = nt->OptionalHeader.SizeOfImage;

    const DWORD pid = FindPid(L"ArkAscended.exe");
    if (!pid) { Log("[-] ArkAscended.exe no esta abierto"); return 1; }

    T t;
    t.h = ::OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ |
                        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!t.h) { Log("[-] no pude abrir el proceso (%lu)", ::GetLastError()); return 1; }
    Log("[+] proceso %lu", pid);

    void* remote = ::VirtualAllocEx(t.h, (LPVOID)pref, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!remote) remote = ::VirtualAllocEx(t.h, nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!remote) { Log("[-] VirtualAllocEx fallo"); return 1; }
    const uint64_t base = (uint64_t)remote;
    Log("[+] imagen en 0x%llX (delta %lld)", (unsigned long long)base, (long long)(base - pref));

    t.Write(base, file.data(), nt->OptionalHeader.SizeOfHeaders);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const DWORD sz = sec[i].SizeOfRawData ? sec[i].SizeOfRawData : sec[i].Misc.VirtualSize;
        if (sz && (uint64_t)sec[i].PointerToRawData + sz <= file.size()) {
            t.Write(base + sec[i].VirtualAddress, file.data() + sec[i].PointerToRawData, sz);
        }
    }

    const auto& rel = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    int relocs = 0;
    if (rel.VirtualAddress && base != pref) {
        uint64_t off = 0;
        while (off < rel.Size) {
            IMAGE_BASE_RELOCATION blk{};
            if (!t.Read(base + rel.VirtualAddress + off, &blk, sizeof(blk))) break;
            if (!blk.SizeOfBlock || !blk.VirtualAddress) break;
            const DWORD n = (blk.SizeOfBlock - sizeof(blk)) / 2;
            for (DWORD i = 0; i < n; ++i) {
                uint16_t e = 0;
                if (!t.Read(base + rel.VirtualAddress + off + sizeof(blk) + i * 2, &e, 2)) break;
                if ((e >> 12) != IMAGE_REL_BASED_DIR64) continue;
                const uint64_t addr = base + blk.VirtualAddress + (e & 0xFFF);
                uint64_t v = 0;
                if (!t.Read(addr, &v, 8)) continue;
                v += (base - pref);
                t.Write(addr, &v, 8);
                ++relocs;
            }
            off += blk.SizeOfBlock;
        }
    }
    Log("[mapper] relocalizaciones: %d", relocs);

    const auto& imp = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    int imports = 0;
    if (imp.VirtualAddress) {
        uint64_t off = 0;
        for (;;) {
            IMAGE_IMPORT_DESCRIPTOR d{};
            if (!t.Read(base + imp.VirtualAddress + off, &d, sizeof(d))) break;
            if (!d.Name) break;
            char nm[256]{};
            t.Read(base + d.Name, nm, sizeof(nm) - 1);
            wchar_t w[256]{};
            for (int i = 0; i < 255 && nm[i]; ++i) w[i] = (wchar_t)nm[i];
            const uintptr_t mod = ModuleBase(t.h, w);
            Log("[mapper] import %s -> 0x%llX", nm, (unsigned long long)mod);
            if (mod) {
                uint64_t thunk = base + (d.FirstThunk ? d.FirstThunk : d.OriginalFirstThunk);
                uint64_t orig = base + d.OriginalFirstThunk;
                for (;;) {
                    uint64_t e = 0;
                    if (!t.Read(orig, &e, 8) || !e) break;
                    if (!(e & 0x8000000000000000ULL)) {
                        char fn[256]{};
                        t.Read(base + (e & 0x7FFFFFFF) + 2, fn, sizeof(fn) - 1);
                        const uintptr_t a = GetProc(t, mod, fn);
                        if (a) { t.Write(thunk, &a, 8); ++imports; }
                        else Log("[mapper]   [!] sin resolver: %s", fn);
                    }
                    thunk += 8; orig += 8;
                }
            }
            off += sizeof(d);
        }
    }
    Log("[mapper] imports resueltos: %d", imports);

    const uint64_t entry = base + nt->OptionalHeader.AddressOfEntryPoint;
    Log("[mapper] entry 0x%llX", (unsigned long long)entry);
    HANDLE th = ::CreateRemoteThread(t.h, nullptr, 0, (LPTHREAD_START_ROUTINE)entry, (LPVOID)base, 0, nullptr);
    if (!th) { Log("[-] CreateRemoteThread fallo (%lu)", ::GetLastError()); return 1; }
    ::WaitForSingleObject(th, 5000);
    DWORD code = 0;
    ::GetExitCodeThread(th, &code);
    ::CloseHandle(th);
    Log("[mapper] el hilo termino con 0x%lX", code);
    // --- hook de la camara, si el payload lo exporta -----------------------
    {
        const uint64_t hookRva = ExportRva(file, "HookA");
        const uint64_t backRva = ExportRva(file, "g_backA");
        if (hookRva && backRva) {
            const uint64_t hookVA = base + hookRva;
            const uintptr_t mod = ModuleBase(t.h, L"ArkAscended.exe");
            Log("[hook] modulo 0x%llX  HookZ 0x%llX", (unsigned long long)mod, (unsigned long long)hookVA);
            if (mod) {
                // stub cerca del modulo: un salto rel32 de 5 bytes no llega lejos.
                void* stub = nullptr;
                for (int k = 1; k < 4000 && !stub; ++k) {
                    const uintptr_t hi = mod + (uintptr_t)k * 0x10000;
                    stub = ::VirtualAllocEx(t.h, (LPVOID)hi, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
                }
                for (int k = 1; k < 4000 && !stub; ++k) {
                    const uintptr_t lo = mod - (uintptr_t)k * 0x10000;
                    stub = ::VirtualAllocEx(t.h, (LPVOID)lo, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
                }
                if (!stub) { Log("[hook] sin hueco cerca del modulo"); }
                else {
                    uint8_t code[14] = { 0xFF, 0x25, 0, 0, 0, 0 };
                    std::memcpy(code + 6, &hookVA, 8);
                    t.Write((uintptr_t)stub, code, sizeof(code));

                    const uintptr_t target = mod + 0x40C13ED;   // MOVSD [RSI+0x10], XMM1  (Z del POV)
                    const int64_t delta = (int64_t)((uintptr_t)stub - (target + 5));
                    if (delta > 0x7FFFFFFFLL || delta < -0x7FFFFFFFLL) {
                        Log("[hook] stub fuera de rango");
                    } else {
                        uint8_t patch[5];
                        patch[0] = 0xE9;
                        const int32_t d32 = (int32_t)delta;
                        std::memcpy(patch + 1, &d32, 4);
                        DWORD old = 0;
                        ::VirtualProtectEx(t.h, (LPVOID)target, 5, PAGE_EXECUTE_READWRITE, &old);
                        const bool ok = t.Write(target, patch, 5);
                        DWORD tmp = 0;
                        ::VirtualProtectEx(t.h, (LPVOID)target, 5, old, &tmp);
                        const uint64_t back = mod + 0x40C13F2;
                        t.Write(base + backRva, &back, 8);

                        // Segundo punto: la copia que usa el render.
                        const uint64_t hook2Rva = ExportRva(file, "HookB");
                        const uint64_t back2Rva = ExportRva(file, "g_backB");
                        if (hook2Rva && back2Rva) {
                            const uint64_t hook2VA = base + hook2Rva;
                            uint8_t code2[14] = { 0xFF, 0x25, 0, 0, 0, 0 };
                            std::memcpy(code2 + 6, &hook2VA, 8);
                            void* stub2 = ::VirtualAllocEx(t.h, (LPVOID)((uintptr_t)stub + 0x1000), 0x1000,
                                                         MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
                            if (stub2) {
                                t.Write((uintptr_t)stub2, code2, sizeof(code2));
                                const uintptr_t t2 = mod + 0x66B9500;   // MOVSD [R9+0x10], XMM1
                                const int64_t d2 = (int64_t)((uintptr_t)stub2 - (t2 + 5));
                                if (d2 <= 0x7FFFFFFFLL && d2 >= -0x7FFFFFFFLL) {
                                    uint8_t p2[5]; p2[0] = 0xE9;
                                    const int32_t dd = (int32_t)d2; std::memcpy(p2 + 1, &dd, 4);
                                    DWORD o2 = 0;
                                    ::VirtualProtectEx(t.h, (LPVOID)t2, 5, PAGE_EXECUTE_READWRITE, &o2);
                                    const bool ok2 = t.Write(t2, p2, 5);
                                    DWORD q2 = 0; ::VirtualProtectEx(t.h, (LPVOID)t2, 5, o2, &q2);
                                    const uint64_t b2 = mod + 0x66B9505;
                                    t.Write(base + back2Rva, &b2, 8);
                                    Log("[hook] segundo parche en 0x%llX ok=%d", (unsigned long long)t2, ok2 ? 1 : 0);
                                }
                            }
                        }
                        Log("[hook] parche en 0x%llX -> stub 0x%llX  ok=%d",
                            (unsigned long long)target, (unsigned long long)(uintptr_t)stub, ok ? 1 : 0);
                    }
                }
            }
        }
    }

    return 0;
}






