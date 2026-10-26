// payload_cam.cpp - camara libre SOLO de camara, para usar tambien en servidor.
//
// No toca al personaje: intercepta las dos escrituras del POV del
// PlayerCameraManager y pone nuestra posicion justo despues. El servidor no ve
// nada porque la posicion del jugador no cambia.
//
// Sitios parcheados (los pone mapper.exe):
//   A: 0x40C13ED  MOVSD [RSI+0x10], XMM1   -> vuelve a 0x40C13F2
//   B: 0x66B9500  MOVSD [R9 +0x10], XMM1   -> vuelve a 0x66B9505
// Solo se escribe si el registro apunta al POV real (g_pov), para no tocar
// otras estructuras.
//
// Teclas: R alterna, flechas mueven, espacio/ctrl suben y bajan.

#include <windows.h>

extern "C" {
__declspec(dllimport) HMODULE WINAPI GetModuleHandleW(LPCWSTR);
__declspec(dllimport) HANDLE WINAPI CreateThread(LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);
__declspec(dllimport) void WINAPI Sleep(DWORD);
__declspec(dllimport) SHORT WINAPI GetAsyncKeyState(int);
__declspec(dllimport) HANDLE WINAPI CreateMutexW(LPSECURITY_ATTRIBUTES, BOOL, LPCWSTR);
__declspec(dllimport) DWORD WINAPI GetLastError(void);
}

// --- estado compartido con el codigo inyectado ------------------------------
extern "C" {
__declspec(dllexport) volatile int g_on = 0;          // 1 = camara libre
__declspec(dllexport) double g_x = 0, g_y = 0, g_z = 0;
__declspec(dllexport) void* g_pov = nullptr;          // direccion del POV real
__declspec(dllexport) void* g_backA = nullptr;        // vuelta desde el sitio A
__declspec(dllexport) void* g_backB = nullptr;        // vuelta desde el sitio B
}

namespace {

constexpr uintptr_t kGWorldRva          = 0xD428FE8;
constexpr uintptr_t kOwningGameInstance = 0x4A8;
constexpr uintptr_t kLocalPlayers       = 0x38;
constexpr uintptr_t kPlayerController   = 0x30;
constexpr uintptr_t kCameraManager      = 0x560;
constexpr uintptr_t kCameraCache        = 0x540;

uintptr_t CameraManager() {
    const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
    if (!base) return 0;
    const uintptr_t world = *(uintptr_t*)(base + kGWorldRva);
    if (world < 0x10000) return 0;
    const uintptr_t gi = *(uintptr_t*)(world + kOwningGameInstance);
    if (!gi) return 0;
    const uintptr_t arr = *(uintptr_t*)(gi + kLocalPlayers);
    if (!arr) return 0;
    const uintptr_t lp = *(uintptr_t*)arr;
    if (!lp) return 0;
    const uintptr_t pc = *(uintptr_t*)(lp + kPlayerController);
    if (!pc) return 0;
    return *(uintptr_t*)(pc + kCameraManager);
}

} // namespace

extern "C" void ApplyPov(double* p) {
    p[0] = g_x;
    p[1] = g_y;
    p[2] = g_z;
}

// Sitio A: RSI apunta al POV.
extern "C" __declspec(dllexport) __attribute__((naked)) void HookA() {
    __asm__ __volatile__(
        "pushfq\n\t"
        "cmpq g_pov(%rip), %rsi\n\t"
        "jne 1f\n\t"
        "cmpq $0, g_on(%rip)\n\t"
        "je 1f\n\t"
        "push %rax\n\tpush %rcx\n\tpush %rdx\n\tpush %r8\n\tpush %r9\n\tpush %r10\n\tpush %r11\n\t"
        "sub $8, %rsp\n\t"
        "mov %rsi, %rcx\n\t"
        "call ApplyPov\n\t"
        "add $8, %rsp\n\t"
        "pop %r11\n\tpop %r10\n\tpop %r9\n\tpop %r8\n\tpop %rdx\n\tpop %rcx\n\tpop %rax\n\t"
        "popfq\n\t"
        "jmp *g_backA(%rip)\n\t"
        "1:\n\t"
        "movsd %xmm1, 0x10(%rsi)\n\t"
        "popfq\n\t"
        "jmp *g_backA(%rip)\n\t"
    );
}

// Sitio B: R9 apunta al POV (la copia que usa el render).
extern "C" __declspec(dllexport) __attribute__((naked)) void HookB() {
    __asm__ __volatile__(
        "pushfq\n\t"
        "cmpq g_pov(%rip), %r9\n\t"
        "jne 1f\n\t"
        "cmpq $0, g_on(%rip)\n\t"
        "je 1f\n\t"
        "push %rax\n\tpush %rcx\n\tpush %rdx\n\tpush %r8\n\tpush %r9\n\tpush %r10\n\tpush %r11\n\t"
        "sub $8, %rsp\n\t"
        "mov %r9, %rcx\n\t"
        "call ApplyPov\n\t"
        "add $8, %rsp\n\t"
        "pop %r11\n\tpop %r10\n\tpop %r9\n\tpop %r8\n\tpop %rdx\n\tpop %rcx\n\tpop %rax\n\t"
        "popfq\n\t"
        "jmp *g_backB(%rip)\n\t"
        "1:\n\t"
        "movsd %xmm1, 0x10(%r9)\n\t"
        "popfq\n\t"
        "jmp *g_backB(%rip)\n\t"
    );
}

namespace {

DWORD WINAPI Loop(LPVOID) {
    bool rDown = false;
    for (;;) {
        const uintptr_t cam = CameraManager();
        if (cam) {
            g_pov = (void*)(cam + kCameraCache);
            const double* pov = (const double*)g_pov;

            const bool rNow = (GetAsyncKeyState('R') & 0x8000) != 0;
            if (rNow && !rDown) {
                g_on = g_on ? 0 : 1;
                if (g_on) {
                    // arranca exactamente donde esta mirando el jugador
                    g_x = pov[0]; g_y = pov[1]; g_z = pov[2];
                }
            }
            rDown = rNow;

            if (g_on) {
                if (GetAsyncKeyState(VK_UP) & 0x8000) g_x += 6.0;
                if (GetAsyncKeyState(VK_DOWN) & 0x8000) g_x -= 6.0;
                if (GetAsyncKeyState(VK_RIGHT) & 0x8000) g_y += 6.0;
                if (GetAsyncKeyState(VK_LEFT) & 0x8000) g_y -= 6.0;
                if (GetAsyncKeyState(VK_SPACE) & 0x8000) g_z += 6.0;
                if (GetAsyncKeyState(VK_CONTROL) & 0x8000) g_z -= 6.0;
            }
        }
        Sleep(8);
    }
    return 0;
}

} // namespace

extern "C" BOOL WINAPI EntryPoint(HINSTANCE, DWORD, LPVOID) {
    CreateMutexW(nullptr, FALSE, L"CoffeeclawCamOnly");
    if (GetLastError() == 183) return 0x22;   // ya hay una instancia
    CreateThread(nullptr, 0, Loop, nullptr, 0, nullptr);
    return 0x11;
}
