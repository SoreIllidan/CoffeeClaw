// Coffeeclaw - click-through layered overlay (GDI, color key)
#pragma once

#include "common.h"
#include <cstring>

namespace claw {

class Overlay {
public:
    ~Overlay() { Destroy(); }

    bool Create(HINSTANCE inst) {
        inst_ = inst;
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &Overlay::WndProc;
        wc.hInstance = inst;
        wc.lpszClassName = L"CoffeeclawOverlay";
        wc.hCursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        if (!::RegisterClassExW(&wc) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

        screenW_ = ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
        screenH_ = ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
        screenX_ = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
        screenY_ = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
        if (screenW_ <= 0 || screenH_ <= 0) return false;

        hwnd_ = ::CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            wc.lpszClassName, L"Coffeeclaw", WS_POPUP,
            screenX_, screenY_, screenW_, screenH_, nullptr, nullptr, inst, this);
        if (!hwnd_) return false;

        ::SetLayeredWindowAttributes(hwnd_, kColorKey, 0, LWA_COLORKEY);
        ::ShowWindow(hwnd_, SW_HIDE);

        hdcScreen_ = ::GetDC(nullptr);
        hdcMem_ = ::CreateCompatibleDC(hdcScreen_);
        if (!CreateSurface()) { Destroy(); return false; }
        return true;
    }

    void Destroy() {
        if (bmp_) { ::DeleteObject(bmp_); bmp_ = nullptr; }
        if (hdcMem_) { ::DeleteDC(hdcMem_); hdcMem_ = nullptr; }
        if (hdcScreen_) { ::ReleaseDC(nullptr, hdcScreen_); hdcScreen_ = nullptr; }
        ReleaseFonts();
        if (hwnd_) { ::DestroyWindow(hwnd_); hwnd_ = nullptr; }
    }

    HWND hwnd() const { return hwnd_; }
    int width() const { return screenW_; }
    int height() const { return screenH_; }

    // Ajusta superficie y posicion al rectangulo del juego, no a todo el
    // escritorio virtual: si el juego vive en un monitor, el overlay va ahi.
    void SetGeometry(int x, int y, int w, int h) {
        if (!hwnd_ || w <= 0 || h <= 0) return;
        if (w != screenW_ || h != screenH_) {
            if (bmp_) { ::DeleteObject(bmp_); bmp_ = nullptr; }
            screenW_ = w;
            screenH_ = h;
            if (!CreateSurface()) return;
        }
        ::SetWindowPos(hwnd_, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        if (visible_) Render();
    }

    void SetVisible(bool on) {
        if (!hwnd_) return;
        visible_ = on;
        ::ShowWindow(hwnd_, on ? SW_SHOWNOACTIVATE : SW_HIDE);
        if (on) { ::SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); Render(); }
    }
    bool visible() const { return visible_; }

    void BeginFrame() {
        if (!hdcMem_ || !pixels_) return;
        std::memset(pixels_, 0, (size_t)screenW_ * screenH_ * 4);
        ::SelectObject(hdcMem_, bmp_);
        ::SetBkMode(hdcMem_, TRANSPARENT);
    }

    void EndFrame() {
        if (!hwnd_ || !visible_) return;
        HDC dc = ::GetDC(hwnd_);
        if (dc) {
            ::BitBlt(dc, 0, 0, screenW_, screenH_, hdcMem_, 0, 0, SRCCOPY);
            ::ReleaseDC(hwnd_, dc);
        }
    }

    void Render() {
        if (!hwnd_ || !visible_) return;
        HDC dc = ::GetDC(hwnd_);
        if (!dc) return;
        ::BitBlt(dc, 0, 0, screenW_, screenH_, hdcMem_, 0, 0, SRCCOPY);
        ::ReleaseDC(hwnd_, dc);
    }

    void Box(int x0, int y0, int x1, int y1, COLORREF color, int thickness = 1) {
        HPEN pen = ::CreatePen(PS_SOLID, thickness, color);
        HGDIOBJ oldPen = ::SelectObject(hdcMem_, pen);
        HGDIOBJ oldBrush = ::SelectObject(hdcMem_, ::GetStockObject(NULL_BRUSH));
        ::Rectangle(hdcMem_, x0, y0, x1, y1);
        ::SelectObject(hdcMem_, oldBrush);
        ::SelectObject(hdcMem_, oldPen);
        ::DeleteObject(pen);
    }

    void Line(int x0, int y0, int x1, int y1, COLORREF color, int thickness = 1) {
        HPEN pen = ::CreatePen(PS_SOLID, thickness, color);
        HGDIOBJ oldPen = ::SelectObject(hdcMem_, pen);
        ::MoveToEx(hdcMem_, x0, y0, nullptr);
        ::LineTo(hdcMem_, x1, y1);
        ::SelectObject(hdcMem_, oldPen);
        ::DeleteObject(pen);
    }

    void Text(int x, int y, const std::wstring& text, COLORREF color, int px = 14,
              bool centered = true, bool bold = false) {
        HFONT font = FontFor(px, bold);
        HGDIOBJ oldFont = font ? ::SelectObject(hdcMem_, font) : nullptr;
        ::SetTextColor(hdcMem_, color);
        ::SetTextAlign(hdcMem_, centered ? (TA_CENTER | TA_TOP) : (TA_LEFT | TA_TOP));
        ::TextOutW(hdcMem_, x, y, text.c_str(), (int)text.size());
        if (oldFont) ::SelectObject(hdcMem_, oldFont);
    }

private:
    static constexpr COLORREF kColorKey = RGB(0, 0, 0);

    static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCHITTEST) return HTTRANSPARENT;
        if (msg == WM_ERASEBKGND) return 1;
        if (msg == WM_PAINT) {
            PAINTSTRUCT ps{};
            ::BeginPaint(h, &ps);
            if (Overlay* self = reinterpret_cast<Overlay*>(::GetWindowLongPtrW(h, GWLP_USERDATA))) self->Render();
            ::EndPaint(h, &ps);
            return 0;
        }
        if (msg == WM_CREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            ::SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
            return 0;
        }
        if (msg == WM_DESTROY) return 0;
        return ::DefWindowProcW(h, msg, wp, lp);
    }

    bool CreateSurface() {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = screenW_;
        bi.bmiHeader.biHeight = -screenH_;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        bmp_ = ::CreateDIBSection(hdcScreen_, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp_ || !bits) return false;
        pixels_ = (uint8_t*)bits;
        return true;
    }

    HFONT FontFor(int px, bool bold) {
        auto& slot = bold ? boldFont_ : normalFont_;
        if (slot && slotPx_ == px) return slot;
        if (slot) { ::DeleteObject(slot); slot = nullptr; }
        slot = ::CreateFontW(-px, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                             ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        slotPx_ = px;
        return slot;
    }

    void ReleaseFonts() {
        if (normalFont_) { ::DeleteObject(normalFont_); normalFont_ = nullptr; }
        if (boldFont_) { ::DeleteObject(boldFont_); boldFont_ = nullptr; }
    }

    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr;
    HDC hdcScreen_ = nullptr;
    HDC hdcMem_ = nullptr;
    HBITMAP bmp_ = nullptr;
    uint8_t* pixels_ = nullptr;
    HFONT normalFont_ = nullptr;
    HFONT boldFont_ = nullptr;
    int slotPx_ = 0;
    int screenW_ = 0, screenH_ = 0, screenX_ = 0, screenY_ = 0;
    bool visible_ = false;
};

} // namespace claw

