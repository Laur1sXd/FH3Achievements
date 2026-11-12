// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <windows.h>
#include <string>

namespace ach {
class Overlay {
public:
    explicit Overlay(HINSTANCE module);
    ~Overlay();
    void toast(std::wstring title, std::wstring detail);
    void update(bool popups);
    void reset();
private:
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM w, LPARAM l);
    void paint(HDC dc, RECT bounds);

    HINSTANCE module_;
    HWND window_ = nullptr;
    bool dirty_ = true;
    ULONGLONG expires_ = 0;
    std::wstring toastTitle_, toastDetail_;
};
}
