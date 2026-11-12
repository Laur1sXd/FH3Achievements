// SPDX-License-Identifier: GPL-3.0-only
#include "overlay.hpp"

#include <algorithm>
#include <iterator>

namespace ach {
namespace {
constexpr wchar_t ClassName[] = L"AzimuthFH3AchievementPopups";
constexpr COLORREF Green = RGB(16, 124, 16), Bright = RGB(111, 210, 86),
                   White = RGB(244, 244, 244), Surface = RGB(27, 27, 27);

void fill(HDC dc, int x, int y, int w, int h, COLORREF color) {
    RECT r{x, y, x + w, y + h};
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &r, brush);
    DeleteObject(brush);
}

void label(HDC dc, int x, int y, int w, int h, const std::wstring& text, int size,
           COLORREF color, bool bold = false) {
    RECT r{x, y, x + w, y + h};
    HFONT font = CreateFontW(-size, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ old = font ? SelectObject(dc, font) : nullptr;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), -1, &r, DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX | DT_VCENTER | DT_SINGLELINE);
    if (old) SelectObject(dc, old);
    if (font) DeleteObject(font);
}

void trophy(HDC dc, int x, int y, COLORREF color) {
    POINT cup[] = {{x+5,y+2},{x+19,y+2},{x+17,y+12},{x+13,y+15},
                   {x+13,y+19},{x+18,y+19},{x+18,y+22},{x+6,y+22},
                   {x+6,y+19},{x+11,y+19},{x+11,y+15},{x+7,y+12}};
    HBRUSH brush = CreateSolidBrush(color);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    Polygon(dc, cup, static_cast<int>(std::size(cup)));
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(brush);
    HPEN pen = CreatePen(PS_SOLID, 2, color);
    oldPen = SelectObject(dc, pen);
    POINT left[] = {{x+6,y+5},{x+1,y+5},{x+2,y+10},{x+8,y+13}};
    POINT right[] = {{x+18,y+5},{x+23,y+5},{x+22,y+10},{x+16,y+13}};
    Polyline(dc, left, 4);
    Polyline(dc, right, 4);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

}

Overlay::Overlay(HINSTANCE module) : module_(module) {
    WNDCLASSW cls{};
    cls.lpfnWndProc = windowProc;
    cls.hInstance = module_;
    cls.lpszClassName = ClassName;
    if (!RegisterClassW(&cls)) return;
    window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                              ClassName, L"FH3 Achievements", WS_POPUP,
                              0, 0, 0, 0, nullptr, nullptr, module_, this);
}
Overlay::~Overlay() {
    if (window_) DestroyWindow(window_);
    UnregisterClassW(ClassName, module_);
}

void Overlay::reset() {
    expires_ = 0;
    dirty_ = true;
}
void Overlay::toast(std::wstring title, std::wstring detail) {
    toastTitle_ = std::move(title);
    toastDetail_ = std::move(detail);
    expires_ = GetTickCount64() + 6500;
    dirty_ = true;
}

void Overlay::update(bool popups) {
    if (!window_) return;
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
    if (!popups) expires_ = 0;
    HWND owner = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(owner, &pid);
    RECT r{};
    POINT origin{};
    if (pid != GetCurrentProcessId() || IsIconic(owner) || !GetClientRect(owner, &r) ||
        !ClientToScreen(owner, &origin) || r.right < 480 || r.bottom < 320 ||
        GetTickCount64() >= expires_) {
        if (IsWindowVisible(window_)) ShowWindow(window_, SW_HIDE);
        return;
    }
    const int width = (std::min)(470L, r.right - 28);
    const int height = 68;
    const int x = origin.x + (r.right - width) / 2;
    const int y = origin.y + r.bottom - height - 42;
    RECT old{};
    GetWindowRect(window_, &old);
    if (!IsWindowVisible(window_) || old.left != x || old.top != y ||
        old.right - old.left != width || old.bottom - old.top != height) {
        SetWindowPos(window_, HWND_TOPMOST, x, y, width, height,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
        dirty_ = true;
    }
    if (dirty_) {
        InvalidateRect(window_, nullptr, FALSE);
        dirty_ = false;
    }
}

LRESULT CALLBACK Overlay::windowProc(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_NCCREATE) {
        const auto* data = reinterpret_cast<const CREATESTRUCTW*>(l);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(data->lpCreateParams));
    }
    auto* self = reinterpret_cast<Overlay*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT && self) {
        PAINTSTRUCT ps{};
        HDC screen = BeginPaint(window, &ps);
        RECT bounds{};
        GetClientRect(window, &bounds);
        HDC buffer = CreateCompatibleDC(screen);
        HBITMAP bitmap = buffer ? CreateCompatibleBitmap(screen, bounds.right, bounds.bottom) : nullptr;
        if (bitmap) {
            auto old = SelectObject(buffer, bitmap);
            self->paint(buffer, bounds);
            BitBlt(screen, 0, 0, bounds.right, bounds.bottom, buffer, 0, 0, SRCCOPY);
            SelectObject(buffer, old);
            DeleteObject(bitmap);
        } else {
            self->paint(screen, bounds);
        }
        if (buffer) DeleteDC(buffer);
        EndPaint(window, &ps);
        return 0;
    }
    return DefWindowProcW(window, message, w, l);
}

void Overlay::paint(HDC dc, RECT bounds) {
    fill(dc, 0, 0, bounds.right, bounds.bottom, Surface);
    fill(dc, 0, 0, 68, bounds.bottom, Green);
    trophy(dc, 22, 23, White);
    label(dc, 85, 8, bounds.right - 101, 19, L"Achievement unlocked", 14, Bright, true);
    label(dc, 85, 28, bounds.right - 101, 30, toastDetail_ + L"  -  " + toastTitle_, 18, White);
}
}
