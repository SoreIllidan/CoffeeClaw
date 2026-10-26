// Coffeeclaw - entry point and control panel
#include "common.h"
#include "proc.h"
#include "config.h"
#include "pattern.h"
#include "ue.h"
#include "features/wallhack/wallhack.h"
#include "features/freecam/freecam_install.h"
#include "w2s.h"
#include "overlay.h"

#include <atomic>
#include <thread>
#include <chrono>
#include <cwchar>
#include <cstdlib>

using namespace claw;

namespace {

constexpr int IDC_ESP      = 1001;
constexpr int IDC_BOXES    = 1002;
constexpr int IDC_NAMES    = 1003;
constexpr int IDC_DIST     = 1004;
constexpr int IDC_LINES    = 1005;
constexpr int IDC_BONES    = 1012;
constexpr int IDC_FREECAM  = 1013;
constexpr int IDC_MAXD     = 1006;
constexpr int IDC_APPLY    = 1007;
constexpr int IDC_RELOAD   = 1008;
constexpr int IDC_DIAG     = 1009;
constexpr int IDC_LOG      = 1010;
constexpr int IDC_STATUS   = 1011;

constexpr UINT WM_APP_REFRESH = WM_APP + 1;

constexpr COLORREF kDino     = RGB(255, 176, 64);
constexpr COLORREF kDinoNear = RGB(255, 96, 96);
constexpr COLORREF kText     = RGB(240, 240, 240);
constexpr COLORREF kDim      = RGB(150, 150, 155);
constexpr COLORREF kAccent   = RGB(120, 220, 160);

struct App {
    Config cfg;
    Process proc;
    UeReader ue;

    Overlay overlay;
    HWND hwnd = nullptr;
    HWND hEsp = nullptr, hBoxes = nullptr, hNames = nullptr, hDist = nullptr, hLines = nullptr;
    HWND hMaxD = nullptr, hLog = nullptr, hStatus = nullptr, hBones = nullptr, hFreecam = nullptr;

    std::atomic<bool> espOn{ false };
    std::atomic<bool> running{ true };
    bool showBoxes = true, showNames = true, showDist = true, showLines = false, showBones = false;

    std::mutex mtx;
    Snapshot snapshot;
    std::wstring logText;
    std::thread worker;
    std::wstring exeDir;
};

App g;

// ---------------------------------------------------------------- helpers ---

std::wstring ExeDir() {
    wchar_t path[MAX_PATH]{};
    ::GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) *slash = 0;
    return path;
}

void Log(const std::wstring& line) {
    g.logText += line;
    g.logText += L"\r\n";
    if (g.logText.size() > 20000) g.logText = g.logText.substr(g.logText.size() - 20000);
    if (g.hLog) {
        std::wstring text = g.logText;
        ::SetWindowTextW(g.hLog, text.c_str());
        ::SendMessageW(g.hLog, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
        ::SendMessageW(g.hLog, EM_SCROLLCARET, 0, 0);
    }
    // Copia en disco para poder diagnosticar sin depender de la ventana.
    static std::wstring path;
    if (path.empty()) path = g.exeDir + L"\\coffeeclaw.log";
    HANDLE f = ::CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        const std::string utf8 = Narrow(line) + "\r\n";
        DWORD written = 0;
        ::WriteFile(f, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
        ::CloseHandle(f);
    }
}

void SetStatus(const std::wstring& text) {
    if (g.hStatus) ::SetWindowTextW(g.hStatus, text.c_str());
}

void Publish(const Snapshot& s) {
    {
        std::lock_guard<std::mutex> lock(g.mtx);
        g.snapshot = s;
    }
    if (g.hwnd) ::PostMessageW(g.hwnd, WM_APP_REFRESH, 0, 0);
}

Snapshot Copy() {
    std::lock_guard<std::mutex> lock(g.mtx);
    return g.snapshot;
}

// ----------------------------------------------------------------- worker ---

void ReaderLoop() {
    std::wstring lastStatus;
    std::vector<uintptr_t> actors;
    auto lastActorRefresh = std::chrono::steady_clock::now() - std::chrono::hours(1);
    const int sleepMs = 1000 / (g.cfg.readerHz > 0 ? g.cfg.readerHz : 25);

    while (g.running.load()) {
        Snapshot snap;

        if (!g.proc.Alive()) {
            g.proc.Detach();
            g.ue.Reset();
            if (!g.proc.Attach(g.cfg.processName)) {
                snap.status = L"sin proceso: abre " + g.cfg.processName;
                if (snap.status != lastStatus) { lastStatus = snap.status; SetStatus(snap.status); }
                Publish(snap);
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                continue;
            }
            g.ue.Bind(&g.proc, &g.cfg);
            g.ue.Reset();
            Log(L"conectado a " + g.cfg.processName + L" (pid " + std::to_wstring(g.proc.pid()) + L")");
        }

        snap.attached = true;

        if (!g.ue.gworld()) {
            std::wstring why;
            if (!g.ue.ResolveRoots(why)) {
                snap.status = L"GWorld sin resolver";
                snap.worldResolved = false;
                if (snap.status != lastStatus) {
                    lastStatus = snap.status;
                    SetStatus(snap.status);
                    Log(L"resolucion fallida:\r\n" + why);
                }
                Publish(snap);
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                continue;
            }
            Log(L"GWorld 0x" + std::to_wstring((unsigned long long)g.ue.gworld()) + L" resuelto");
            lastActorRefresh = std::chrono::steady_clock::now() - std::chrono::hours(1);
        }

        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastActorRefresh).count() >= g.cfg.actorRefreshMs) {
            std::vector<uintptr_t> fresh;
            if (g.ue.EnumerateActors(fresh)) actors.swap(fresh);
            lastActorRefresh = now;
        }

        Vec3 cam;
        Rotator rot;
        double fov = 90.0;
        if (!g.ue.GetCamera(cam, rot, fov)) {
            snap.attached = true;
            snap.worldResolved = true;
            snap.status = L"camara no legible (revisa los offsets de PlayerController)";
            if (snap.status != lastStatus) { lastStatus = snap.status; SetStatus(snap.status); Log(snap.status); }
            Publish(snap);
            std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
            continue;
        }

        snap.camera = cam;
        snap.cameraRot = rot;
        snap.fov = fov;

        int matched = 0;
        for (uintptr_t actor : actors) {
            std::wstring cls;
            if (!g.ue.IsDinoClass(actor, cls)) continue;
            ++matched;

            Vec3 pos;
            if (!g.ue.ActorLocation(actor, pos)) continue;

            const double distM = (pos - cam).Length() / 100.0;
            if (distM > g.cfg.maxDistanceM) continue;

            Entity e;
            e.name = cls;
            // [DESACTIVADO] esqueleto: no se leen huesos.
            e.world = pos;
            e.distanceM = distM;
            e.heightCm = features::wallhack::DinoHeight(cls);
            Vec3 boxC, boxE;
            if (g.ue.ActorBox(actor, boxC, boxE)) {
                e.world = boxC;
                e.heightCm = boxE.z * 2.0;
                e.widthRatio = (boxE.z > 1.0) ? (float)(boxE.x / boxE.z) : 0.62f;
                if (e.widthRatio < 0.40f) e.widthRatio = 0.40f;
                if (e.widthRatio > 1.60f) e.widthRatio = 1.60f;
            }
            snap.entities.push_back(std::move(e));
        }

        snap.worldResolved = true;
        snap.status = L"activo - " + std::to_wstring(snap.entities.size()) + L" en rango / " +
                      std::to_wstring(matched) + L" dinos / " +
                      std::to_wstring(actors.size()) + L" actores";
        if (snap.status != lastStatus) { lastStatus = snap.status; SetStatus(snap.status); }

        // Traza periodica: sirve para comprobar si las posiciones se mueven.
        static int watchTick = 0;
        if (++watchTick % 50 == 0) {
            wchar_t buf[320];
            if (!snap.entities.empty()) {
                const Entity& e = snap.entities[0];
                std::swprintf(buf, 320, L"[watch] cam %.1f %.1f %.1f | %ls %.0fm @ %.1f %.1f %.1f",
                              cam.x, cam.y, cam.z, e.name.c_str(), e.distanceM,
                              e.world.x, e.world.y, e.world.z);
            } else {
                std::swprintf(buf, 320, L"[watch] cam %.1f %.1f %.1f | actores=%zu matched=%d sin dinos", cam.x, cam.y, cam.z, actors.size(), matched);
            }
            Log(buf);
        }

        Publish(snap);
        std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
    }
}

// ---------------------------------------------------------------- overlay ---

// Altura aproximada por especie (cm). Sin leer bounds: tabla por nombre.

HWND g_gameWnd = nullptr;

BOOL CALLBACK FindGameWindowProc(HWND h, LPARAM) {
    DWORD pid = 0;
    ::GetWindowThreadProcessId(h, &pid);
    if (!g.proc.Alive() || pid != g.proc.pid()) return TRUE;
    if (!::IsWindowVisible(h)) return TRUE;
    wchar_t cls[128]{};
    ::GetClassNameW(h, cls, 128);
    if (wcsstr(cls, L"Unreal") || wcsstr(cls, L"ARK")) {
        g_gameWnd = h;
        return FALSE;
    }
    return TRUE;
}

// El overlay se pega al area del juego. Sin esto, dibuja sobre todo el
// escritorio virtual y las cajas caen en el otro monitor.
void UpdateGameGeometry() {
    if (!g.proc.Alive()) return;
    g_gameWnd = nullptr;
    ::EnumWindows(&FindGameWindowProc, 0);
    if (!g_gameWnd) return;
    RECT rc{};
    if (!::GetClientRect(g_gameWnd, &rc)) return;
    POINT pt{ 0, 0 };
    if (!::ClientToScreen(g_gameWnd, &pt)) return;
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    g.overlay.SetGeometry(pt.x, pt.y, w, h);
}

void PaintOverlay() {
    if (!g.overlay.hwnd()) return;
    UpdateGameGeometry();
    Snapshot s = Copy();
    Overlay& o = g.overlay;

    o.BeginFrame();

    // Calibracion visible: geometria del overlay y valores de camara.
    {
        wchar_t info[256];
        RECT grc{};
        if (g_gameWnd) ::GetClientRect(g_gameWnd, &grc);
        std::swprintf(info, 256, L"overlay %dx%d | juego %ldx%ld | cam %.0f %.0f %.0f | yaw %.1f pitch %.1f fov %.1f",
                      o.width(), o.height(), grc.right - grc.left, grc.bottom - grc.top,
                      s.camera.x, s.camera.y, s.camera.z, s.cameraRot.yaw, s.cameraRot.pitch, s.fov);
        o.Text(10, 8, info, RGB(120, 200, 255), 13, false, false);
    }
    o.Line(o.width() / 2 - 30, o.height() / 2, o.width() / 2 + 30, o.height() / 2, RGB(255, 60, 60), 1);
    o.Line(o.width() / 2, o.height() / 2 - 30, o.width() / 2, o.height() / 2 + 30, RGB(255, 60, 60), 1);

    // Marcas de orientacion: si "TOP" aparece abajo, el lienzo esta invertido.
    o.Line(24, 30, 24, 230, RGB(255, 40, 40), 4);
    o.Text(42, 30, L"TOP", RGB(255, 60, 60), 16, false, true);
    o.Line(24, o.height() - 230, 24, o.height() - 30, RGB(40, 255, 40), 4);
    o.Text(42, o.height() - 62, L"BOT", RGB(60, 255, 60), 16, false, true);
    o.Box(o.width() / 2 - 8, o.height() - 12, o.width() / 2 + 8, o.height() - 2, RGB(255, 40, 220), 2);

    if (!s.attached || !s.worldResolved) {
        o.Text(24, 40, L"Coffeeclaw: " + s.status, RGB(255, 120, 120), 16, false, true);
        o.EndFrame();
        return;
    }
    if (s.entities.empty()) {
        o.Text(o.width() / 2, o.height() / 2 - 20, L"Coffeeclaw: sin dinos dentro del rango", kDim, 15, true, true);
        o.EndFrame();
        return;
    }

    const Basis basis = MakeBasis(s.cameraRot);
    const int cx = o.width() / 2;
    const int cy = o.height() / 2;
    o.Box(cx - 1, cy - 1, cx + 2, cy + 2, RGB(110, 110, 110), 1);

    for (const Entity& e : s.entities) {
        const Vec3 head = Vec3(e.world.x, e.world.y, e.world.z + e.heightCm);
        float fx = 0, fy = 0, hx = 0, hy = 0;
        if (!WorldToScreen(e.world, s.camera, basis, s.fov, o.width(), o.height(), fx, fy)) continue;
        if (!WorldToScreen(head, s.camera, basis, s.fov, o.width(), o.height(), hx, hy)) continue;

        const float height = fy - hy;
        if (height < 4.0f || height > o.height() * 1.5f) continue;
        const float width = height * e.widthRatio;

        const COLORREF color = (e.distanceM < 60.0) ? kDinoNear : kDino;

        // [DESACTIVADO] Cajas: la funcion queda comentada de momento.
        // if (g.showBoxes) {
        //     const int x0 = (int)(hx - width * 0.5f);
        //     const int x1 = (int)(hx + width * 0.5f);
        //     const int y0 = (int)hy;
        //     const int y1 = (int)fy;
        //     o.Box(x0, y0, x1, y1, color, 1);
        //     o.Box(x0 - 1, y0 - 1, x1 + 1, y1 + 1, RGB(20, 20, 20), 1);
        //     o.Box(x0, y0, x1, y1, color, 1);
        // }
        (void)hx; (void)hy; (void)fy; (void)width; (void)color;

        // [DESACTIVADO] Esqueleto: queda comentado hasta cerrar el layout.
        // if (g.showBones && !e.bones.empty()) {
        //     std::vector<std::pair<float, float>> sp(e.bones.size(), { 0.f, 0.f });
        //     std::vector<char> vis(e.bones.size(), 0);
        //     for (size_t bi = 0; bi < e.bones.size(); ++bi) {
        //         float bx = 0, by = 0;
        //         if (!WorldToScreen(e.bones[bi], s.camera, basis, s.fov, o.width(), o.height(), bx, by)) continue;
        //         sp[bi] = { bx, by };
        //         vis[bi] = 1;
        //     }
        //     for (size_t bi = 0; bi < e.bones.size(); ++bi) {
        //         if (!vis[bi]) continue;
        //         const int32_t par = (bi < e.boneParents.size()) ? e.boneParents[bi] : -1;
        //         if (par >= 0 && (size_t)par < e.bones.size() && vis[par])
        //             o.Line((int)sp[par].first, (int)sp[par].second, (int)sp[bi].first, (int)sp[bi].second,
        //                    RGB(255, 40, 220), 1);
        //         o.Box((int)sp[bi].first - 1, (int)sp[bi].second - 1, (int)sp[bi].first + 2, (int)sp[bi].second + 2,
        //               RGB(255, 90, 240), 1);
        //     }
        // }
        if (g.showLines) o.Line(cx, o.height() - 4, (int)hx, (int)fy, RGB(90, 90, 90), 1);

        int textY = (int)hy - 18;
        if (textY < 2) textY = (int)fy + 2;

        if (g.showNames) {
            const std::wstring label = features::wallhack::CleanDinoName(e.name);

            o.Text((int)hx, textY, label, kText, 12, true, false);
            textY += 14;
        }
        if (g.showDist) {
            wchar_t buf[32];
            std::swprintf(buf, 32, L"%.0f m", e.distanceM);
            o.Text((int)hx, textY, buf, color, 12, true, true);
        }
    }

    o.EndFrame();
}

// ---------------------------------------------------------------- window ----

HBRUSH PanelBrush() {
    static HBRUSH brush = ::CreateSolidBrush(RGB(28, 28, 30));
    return brush;
}

HWND MakeControl(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h,
                 int id, DWORD exStyle = 0) {    HWND hw = ::CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h,
                                g.hwnd, (HMENU)(INT_PTR)id, ::GetModuleHandleW(nullptr), nullptr);
    ::SendMessageW(hw, WM_SETFONT, (WPARAM)::GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return hw;
}

void ApplyDistance() {
    wchar_t buf[64]{};
    ::GetWindowTextW(g.hMaxD, buf, 64);
    double v = _wtof(buf);
    if (v < 5.0) v = 5.0;
    if (v > 20000.0) v = 20000.0;
    g.cfg.maxDistanceM = v;
    std::swprintf(buf, 64, L"%.0f", v);
    ::SetWindowTextW(g.hMaxD, buf);
    Log(L"distancia maxima: " + std::wstring(buf) + L" m");
}

void ReloadConfig() {
    std::wstring path = g.exeDir + L"\\offsets.ini";
    g.cfg.processName = L"ArkAscended.exe";
    g.cfg.Load(path);
    g.ue.Reset();
    g.proc.Detach();
    Log(L"offsets.ini recargado");
}

void RunDiagnostics() {
    Snapshot s = Copy();
    Log(L"--- diagnostico ---");
    if (!g.proc.Alive()) {
        Log(L"sin proceso. abre ARK y vuelve a intentar.");
        return;
    }
    Log(g.ue.Diagnose());
    Log(L"--- fin ---");
    (void)s;
}

void ToggleEsp(bool on) {
    g.espOn.store(on);
    ::SendMessageW(g.hEsp, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
    g.overlay.SetVisible(on);
    if (on) {
        ::SetForegroundWindow(g.hwnd);
        PaintOverlay();
        Log(L"wallhack ON");
    } else {
        Log(L"wallhack OFF");
    }
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g.hwnd = h;
        const DWORD cb = WS_TABSTOP | BS_AUTOCHECKBOX;
        ::CreateWindowExW(0, L"STATIC", L"Coffeeclaw  -  ARK: Survival Ascended",
                          WS_CHILD | WS_VISIBLE, 14, 10, 380, 20, h, nullptr, nullptr, nullptr);
        g.hEsp = MakeControl(L"BUTTON", L"Wallhack (dinosaurios)   [F1]", cb, 14, 38, 300, 24, IDC_ESP);
        // [DESACTIVADO] Cajas y Esqueleto: se ocultan del panel mientras esten
        // comentadas. Se recrean quitando el comentario de las dos lineas.
        // g.hBoxes = MakeControl(L"BUTTON", L"Cajas", cb, 30, 66, 110, 22, IDC_BOXES);
        g.hNames = MakeControl(L"BUTTON", L"Nombres", cb, 150, 66, 110, 22, IDC_NAMES);
        g.hDist = MakeControl(L"BUTTON", L"Distancia", cb, 270, 66, 120, 22, IDC_DIST);
        g.hLines = MakeControl(L"BUTTON", L"Lineas al centro", cb, 30, 92, 150, 22, IDC_LINES);
        g.hFreecam = MakeControl(L"BUTTON", L"Camara libre (alma)", cb, 190, 92, 190, 22, IDC_FREECAM);
        // g.hBones = MakeControl(L"BUTTON", L"Esqueleto (exp.)", cb, 190, 92, 150, 22, IDC_BONES);

        ::CreateWindowExW(0, L"STATIC", L"Rango maximo (m):", WS_CHILD | WS_VISIBLE,
                          30, 124, 130, 20, h, nullptr, nullptr, nullptr);
        g.hMaxD = MakeControl(L"EDIT", L"400", WS_BORDER | ES_NUMBER, 160, 122, 70, 22, IDC_MAXD, WS_EX_CLIENTEDGE);
        MakeControl(L"BUTTON", L"Aplicar", BS_PUSHBUTTON, 238, 121, 80, 24, IDC_APPLY);

        MakeControl(L"BUTTON", L"Recargar offsets.ini  [F2]", BS_PUSHBUTTON, 14, 156, 190, 26, IDC_RELOAD);
        MakeControl(L"BUTTON", L"Diagnostico  [F3]", BS_PUSHBUTTON, 214, 156, 180, 26, IDC_DIAG);

        g.hStatus = MakeControl(L"STATIC", L"inactivo", SS_LEFTNOWORDWRAP, 14, 190, 380, 20, IDC_STATUS);

        g.hLog = MakeControl(L"EDIT", L"", WS_BORDER | ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL,
                             14, 214, 380, 190, IDC_LOG, WS_EX_CLIENTEDGE);

        ::SendMessageW(g.hBoxes, BM_SETCHECK, BST_CHECKED, 0);
        ::SendMessageW(g.hNames, BM_SETCHECK, BST_CHECKED, 0);
        ::SendMessageW(g.hDist, BM_SETCHECK, BST_CHECKED, 0);

        ::RegisterHotKey(h, 1, 0, VK_F1);
        ::RegisterHotKey(h, 2, 0, VK_F2);
        ::RegisterHotKey(h, 3, 0, VK_F3);

        g.worker = std::thread(ReaderLoop);
        return 0;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wp);
        const int code = HIWORD(wp);
        if (id == IDC_ESP && code == BN_CLICKED) {
            ToggleEsp(::SendMessageW(g.hEsp, BM_GETCHECK, 0, 0) == BST_CHECKED);
        } else if (id == IDC_BOXES) {
            g.showBoxes = ::SendMessageW(g.hBoxes, BM_GETCHECK, 0, 0) == BST_CHECKED;
            PaintOverlay();
        } else if (id == IDC_NAMES) {
            g.showNames = ::SendMessageW(g.hNames, BM_GETCHECK, 0, 0) == BST_CHECKED;
            PaintOverlay();
        } else if (id == IDC_DIST) {
            g.showDist = ::SendMessageW(g.hDist, BM_GETCHECK, 0, 0) == BST_CHECKED;
            PaintOverlay();
        } else if (id == IDC_BONES) {
            g.showBones = ::SendMessageW(g.hBones, BM_GETCHECK, 0, 0) == BST_CHECKED;
            PaintOverlay();
        } else if (id == IDC_FREECAM) {
            if (::SendMessageW(g.hFreecam, BM_GETCHECK, 0, 0) == BST_CHECKED) {
                std::wstring flog;
                const std::wstring dll = ExeDir() + L"..\\payload\\payload_cam.dll";
                features::freecam::Install(g.proc.pid(), dll, flog);
                Log(flog);
            }
        } else if (id == IDC_LINES) {
            g.showLines = ::SendMessageW(g.hLines, BM_GETCHECK, 0, 0) == BST_CHECKED;
            PaintOverlay();
        } else if (id == IDC_APPLY) {
            ApplyDistance();
            PaintOverlay();
        } else if (id == IDC_RELOAD) {
            ReloadConfig();
        } else if (id == IDC_DIAG) {
            RunDiagnostics();
        }
        return 0;
    }
    case WM_HOTKEY: {
        if (wp == 1) {
            ToggleEsp(!g.espOn.load());
            ::SendMessageW(g.hEsp, BM_SETCHECK, g.espOn.load() ? BST_CHECKED : BST_UNCHECKED, 0);
        } else if (wp == 2) {
            ReloadConfig();
        } else if (wp == 3) {
            RunDiagnostics();
        }
        return 0;
    }
    case WM_APP_REFRESH: {
        if (!g.espOn.load()) {
            Snapshot s = Copy();
            if (s.attached && !s.worldResolved) SetStatus(s.status);
            return 0;
        }
        // Solo dibujar cuando el juego esta en primer plano: asi el overlay no
        // se queda pegado al cambiar de pantalla o al alt-tab.
        UpdateGameGeometry();
        const bool fg = g_gameWnd && ::GetForegroundWindow() &&
                        ::GetAncestor(::GetForegroundWindow(), GA_ROOT) == g_gameWnd;
        if (!fg) {
            if (g.overlay.visible()) g.overlay.SetVisible(false);
            return 0;
        }
        if (!g.overlay.visible()) g.overlay.SetVisible(true);
        PaintOverlay();
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        ::SetBkColor(dc, RGB(28, 28, 30));
        ::SetTextColor(dc, RGB(220, 220, 220));
        return (LRESULT)PanelBrush();
    }
    case WM_CLOSE:
        g.running.store(false);
        g.overlay.SetVisible(false);
        if (g.worker.joinable()) g.worker.join();
        ::DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        ::UnregisterHotKey(h, 1);
        ::UnregisterHotKey(h, 2);
        ::UnregisterHotKey(h, 3);
        g.running.store(false);
        if (g.worker.joinable()) g.worker.join();
        g.overlay.Destroy();
        ::PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return ::DefWindowProcW(h, msg, wp, lp);
}

} // namespace

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int) {
    ::SetProcessDPIAware();
    g.exeDir = ExeDir();
    g.cfg.Load(g.exeDir + L"\\offsets.ini");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"CoffeeclawPanel";
    wc.hCursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = PanelBrush();
    if (!::RegisterClassExW(&wc)) return 1;

    g.hwnd = ::CreateWindowExW(0, wc.lpszClassName, L"Coffeeclaw", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                               120, 120, 430, 480, nullptr, nullptr, inst, nullptr);
    if (!g.hwnd) return 1;

    ::ShowWindow(g.hwnd, SW_SHOW);
    ::UpdateWindow(g.hwnd);

    g.overlay.Create(inst);

    wchar_t buf[64];
    std::swprintf(buf, 64, L"%.0f", g.cfg.maxDistanceM);
    ::SetWindowTextW(g.hMaxD, buf);

    Log(L"coffeeclaw iniciado");
    Log(L"process = " + g.cfg.processName);
    Log(g.cfg.FirstMissing().empty() ? L"offsets.ini: completo"
                                     : L"offsets.ini: falta " + g.cfg.FirstMissing());

    MSG msg{};
    while (::GetMessageW(&msg, nullptr, 0, 0)) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    g.running.store(false);
    if (g.worker.joinable()) g.worker.join();
    return 0;
}




















