// Coffeeclaw - process access (read only)
#pragma once

#include "common.h"
#include <tlhelp32.h>
#include <psapi.h>

#pragma comment(lib, "psapi.lib")

namespace claw {

class Process {
public:
    Process() = default;
    ~Process() { Detach(); }

    bool Attach(const std::wstring& exeName) {
        Detach();
        DWORD pid = FindPid(exeName);
        if (pid == 0) return false;

        HANDLE h = ::OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION, FALSE, pid);
        if (!h) return false;

        handle_ = h;
        pid_ = pid;
        name_ = exeName;
        base_ = ModuleBase(exeName);
        imageSize_ = ModuleSize(exeName);
        return base_ != 0;
    }

    void Detach() {
        if (handle_) { ::CloseHandle(handle_); handle_ = nullptr; }
        pid_ = 0;
        base_ = 0;
        imageSize_ = 0;
    }

    bool Alive() const {
        if (!handle_) return false;
        DWORD code = 0;
        return ::GetExitCodeProcess(handle_, &code) && code == STILL_ACTIVE;
    }

    HANDLE handle() const { return handle_; }
    DWORD pid() const { return pid_; }
    uintptr_t base() const { return base_; }
    size_t imageSize() const { return imageSize_; }

    bool Read(uintptr_t addr, void* out, size_t size) const {
        if (!handle_ || !addr || !size) return false;
        // NtReadVirtualMemory en vez de ReadProcessMemory: misma lectura por
        // la puerta de ntdll, que deja menos rastro en hooks de kernel32.
        typedef LONG (NTAPI* PFN)(HANDLE, PVOID, PVOID, SIZE_T, PSIZE_T);
        static PFN pNt = (PFN)::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "NtReadVirtualMemory");
        SIZE_T got = 0;
        if (pNt) {
            const LONG st = pNt(handle_, (PVOID)addr, out, size, &got);
            if (st == 0 && got == size) return true;
        }
        return ::ReadProcessMemory(handle_, (LPCVOID)addr, out, size, &got) && got == size;
    }

    // Escritura: solo la usa el freecam, sobre la camara del juego.
    bool Write(uintptr_t addr, const void* data, size_t size) const {
        if (!handle_ || !addr || !data || !size) return false;
        DWORD old = 0;
        const bool prot = ::VirtualProtectEx(handle_, (LPVOID)addr, size, PAGE_READWRITE, &old) != 0;
        SIZE_T done = 0;
        const bool ok = ::WriteProcessMemory(handle_, (LPVOID)addr, data, size, &done) && done == size;
        if (prot) { DWORD tmp = 0; ::VirtualProtectEx(handle_, (LPVOID)addr, size, old, &tmp); }
        return ok;
    }

    bool ReadPtr(uintptr_t addr, uintptr_t& out) const {
        uint64_t v = 0;
        if (!Read(addr, &v, sizeof(v))) return false;
        out = (uintptr_t)v;
        return out != 0;
    }

    template <typename T>
    bool ReadT(uintptr_t addr, T& out) const { return Read(addr, &out, sizeof(T)); }

    std::vector<uint8_t> ReadImage(size_t cap) const {
        std::vector<uint8_t> buf;
        size_t size = imageSize_ ? imageSize_ : cap;
        if (size > cap) size = cap;
        if (!size) return buf;
        buf.resize(size);
        SIZE_T got = 0;
        if (!::ReadProcessMemory(handle_, (LPCVOID)base_, buf.data(), size, &got)) buf.clear();
        return buf;
    }

private:
    static DWORD FindPid(const std::wstring& exeName) {
        HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return 0;
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        DWORD pid = 0;
        if (::Process32FirstW(snap, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, exeName.c_str()) == 0) { pid = pe.th32ProcessID; break; }
            } while (::Process32NextW(snap, &pe));
        }
        ::CloseHandle(snap);
        return pid;
    }

    uintptr_t ModuleBase(const std::wstring& modName) const {
        HMODULE mods[1024];
        DWORD need = 0;
        if (!::EnumProcessModulesEx(handle_, mods, sizeof(mods), &need, LIST_MODULES_ALL)) return 0;
        size_t count = need / sizeof(HMODULE);
        if (count > 1024) count = 1024;
        wchar_t path[MAX_PATH]{};
        for (size_t i = 0; i < count; ++i) {
            if (!::GetModuleFileNameExW(handle_, mods[i], path, MAX_PATH)) continue;
            const wchar_t* file = wcsrchr(path, L'\\');
            file = file ? file + 1 : path;
            if (_wcsicmp(file, modName.c_str()) == 0) return (uintptr_t)mods[i];
        }
        return 0;
    }

    size_t ModuleSize(const std::wstring& modName) const {
        MODULEINFO info{};
        HMODULE mods[1024];
        DWORD need = 0;
        if (!::EnumProcessModulesEx(handle_, mods, sizeof(mods), &need, LIST_MODULES_ALL)) return 0;
        size_t count = need / sizeof(HMODULE);
        if (count > 1024) count = 1024;
        wchar_t path[MAX_PATH]{};
        for (size_t i = 0; i < count; ++i) {
            if (!::GetModuleFileNameExW(handle_, mods[i], path, MAX_PATH)) continue;
            const wchar_t* file = wcsrchr(path, L'\\');
            file = file ? file + 1 : path;
            if (_wcsicmp(file, modName.c_str()) != 0) continue;
            if (::GetModuleInformation(handle_, mods[i], &info, sizeof(info))) return (size_t)info.SizeOfImage;
        }
        return 0;
    }

    HANDLE handle_ = nullptr;
    DWORD pid_ = 0;
    uintptr_t base_ = 0;
    size_t imageSize_ = 0;
    std::wstring name_;
};

} // namespace claw


