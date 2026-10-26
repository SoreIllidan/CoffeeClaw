// bp_count - cuenta cuantas veces se ejecuta cada instruccion que escribe el
// POV de la camara. Sustituye a x64dbg para este caso concreto.
//
// Se adjunta como depurador, arma un breakpoint de escritura de hardware sobre
// CameraManager+0x540 en el hilo de la ventana del juego, corre unos segundos y
// desarma verificando que el registro quedo limpio.

#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

#include "../src/config.h"
#include "../src/proc.h"
#include "../src/ue.h"

static FILE* g_out = nullptr;
static void Say(const char* fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::printf("%s", buf);
    std::fflush(stdout);
    if (g_out) { std::fprintf(g_out, "%s", buf); std::fflush(g_out); }
}

struct Find { DWORD pid; HWND hwnd; };
static BOOL CALLBACK OnWindow(HWND h, LPARAM lp) {
    Find* f = (Find*)lp;
    DWORD pid = 0;
    ::GetWindowThreadProcessId(h, &pid);
    if (pid == f->pid && ::IsWindowVisible(h)) { f->hwnd = h; return FALSE; }
    return TRUE;
}

static bool Arm(DWORD tid, uintptr_t addr, bool enable, bool& verified) {
    verified = false;
    HANDLE h = ::OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, tid);
    if (!h) return false;
    ::SuspendThread(h);
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    bool ok = false;
    if (::GetThreadContext(h, &ctx)) {
        if (enable) {
            ctx.Dr0 = (DWORD64)addr;
            ctx.Dr7 = (ctx.Dr7 & ~0xF0003ULL) | 0x90001ULL;
        } else {
            ctx.Dr0 = 0;
            ctx.Dr7 &= ~0x3ULL;
        }
        ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        ok = ::SetThreadContext(h, &ctx) != 0;
        CONTEXT chk{};
        chk.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (::GetThreadContext(h, &chk)) verified = (chk.Dr0 == (enable ? (DWORD64)addr : 0ULL));
    }
    ::ResumeThread(h);
    ::CloseHandle(h);
    return ok;
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    g_out = std::fopen("bp_count.txt", "w");
    Say("# bp_count build %s %s\n", __DATE__, __TIME__);

    claw::Process proc;
    if (!proc.Attach(L"ArkAscended.exe")) { Say("[-] no pude abrir el juego\n"); return 1; }
    const DWORD pid = proc.pid();
    const uintptr_t base = proc.base();

    claw::Config cfg;
    cfg.Load(L"offsets.ini");
    cfg.gworldSlot = 0xD428FE8;
    cfg.gnamesSlot = 0xD472550;
    cfg.gnamesIndirection = 0;
    cfg.gworldPattern.clear();
    cfg.gnamesPattern.clear();

    claw::UeReader ue;
    ue.Bind(&proc, &cfg);
    std::wstring log;
    if (!ue.ResolveRoots(log)) { Say("[-] raices\n"); return 2; }

    const uintptr_t pov = ue.CameraPovAddr();
    if (!pov) { Say("[-] sin camara\n"); return 3; }
    Say("[+] CameraCache 0x%llX\n", (unsigned long long)pov);

    Find f{ pid, nullptr };
    ::EnumWindows(OnWindow, (LPARAM)&f);
    if (!f.hwnd) { Say("[-] sin ventana\n"); return 4; }
    const DWORD tid = ::GetWindowThreadProcessId(f.hwnd, nullptr);
    Say("[+] hilo de ventana %lu\n", (unsigned long)tid);

    if (!::DebugActiveProcess(pid)) { Say("[-] no pude adjuntarme (%lu)\n", ::GetLastError()); return 5; }
    ::DebugSetProcessKillOnExit(FALSE);

    bool verified = false;
    Arm(tid, pov, true, verified);
    Say("[+] breakpoint armado (verificado=%d). 3 segundos...\n", verified ? 1 : 0);

    std::map<uintptr_t, int> hits;
    const ULONGLONG end = ::GetTickCount64() + 3000;
    while (::GetTickCount64() < end) {
        DEBUG_EVENT ev{};
        if (!::WaitForDebugEvent(&ev, 200)) continue;
        DWORD cont = DBG_CONTINUE;
        if (ev.dwDebugEventCode == EXCEPTION_DEBUG_EVENT) {
            const auto& rec = ev.u.Exception.ExceptionRecord;
            if (rec.ExceptionCode == EXCEPTION_SINGLE_STEP) {
                HANDLE th = ::OpenThread(THREAD_GET_CONTEXT, FALSE, ev.dwThreadId);
                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_CONTROL;
                if (th) { ::GetThreadContext(th, &ctx); ::CloseHandle(th); }
                hits[(uintptr_t)ctx.Rip - base]++;
            } else if (rec.ExceptionCode != EXCEPTION_BREAKPOINT) {
                cont = DBG_EXCEPTION_NOT_HANDLED;
            }
        }
        ::ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, cont);
    }

    bool clean = false;
    Arm(tid, pov, false, clean);
    if (!clean) { Say("[!] NO desengancho: cierra el juego.\n"); return 6; }
    ::DebugActiveProcessStop(pid);

    std::vector<std::pair<int, uintptr_t>> sorted;
    for (auto& kv : hits) sorted.push_back({ kv.second, kv.first });
    std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.first > b.first; });

    Say("[+] instrucciones que escriben el POV (RVA, veces):\n");
    for (size_t i = 0; i < sorted.size() && i < 10; ++i)
        Say("   0x%llX   %d\n", (unsigned long long)sorted[i].second, sorted[i].first);
    Say("[+] total de escrituras capturadas: %zu\n", hits.size());
    if (g_out) std::fclose(g_out);
    return 0;
}
