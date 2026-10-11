// VimiumNav.cpp — Vimium-like keyboard navigation for Windows
// Compile: cl /EHsc /std:c++17 /O2 VimiumNav.cpp /link user32.lib gdi32.lib ole32.lib oleaut32.lib uiautomationcore.lib

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <ole2.h>        
#include <oleauto.h>
#include <uiautomation.h>
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include <cstdio>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "uiautomationcore.lib")

// ============================================================
//  Global state
// ============================================================
struct HintTarget {
    std::wstring label;
    RECT         rect;       // screen coords of bounding box
    POINT        clickPt;    // exact click point
    int          matchLen;   // how many chars of label have been typed
};

static HHOOK              g_hKeyboardHook = nullptr;
static HWND               g_hOverlay      = nullptr;
static HWND               g_hMessageWnd   = nullptr;
static bool               g_hintMode      = false;
static std::wstring       g_typed;
static std::vector<HintTarget> g_hints;
static IUIAutomation*     g_pAutomation   = nullptr;

static const int HINT_PADDING = 4;
static const int HINT_FONT_SIZE = 12;

// ============================================================
//  Utility: generate hint strings (base-26, fixed length)
// ============================================================
static std::wstring MakeHint(int index, int length) {
    std::wstring s(length, L'a');
    for (int i = length - 1; i >= 0; --i) {
        s[i] = L'a' + (index % 26);
        index /= 26;
    }
    return s;
}

// ============================================================
//  UI Automation: collect clickable elements
// ============================================================
static void CollectHints() {
    g_hints.clear();
    if (!g_pAutomation) return;

    IUIAutomationElement* pRoot = nullptr;
    if (FAILED(g_pAutomation->GetRootElement(&pRoot)) || !pRoot) return;

    // Build condition: OR of interesting control types
    IUIAutomationCondition* pCondition = nullptr;
    IUIAutomationOrCondition* pOr = nullptr;
    g_pAutomation->CreateOrCondition(&pOr);

    const CONTROLTYPEID types[] = {
        UIA_ButtonControlTypeId,
        UIA_HyperlinkControlTypeId,
        UIA_ListItemControlTypeId,
        UIA_MenuItemControlTypeId,
        UIA_TabItemControlTypeId,
        UIA_TreeItemControlTypeId,
        UIA_CheckBoxControlTypeId,
        UIA_RadioButtonControlTypeId,
        UIA_EditControlTypeId,
        UIA_ComboBoxControlTypeId,
        UIA_SplitButtonControlTypeId,
    };

    for (CONTROLTYPEID ct : types) {
        VARIANT var; VariantInit(&var);
        var.vt = VT_I4; var.lVal = ct;
        IUIAutomationCondition* pC = nullptr;
        g_pAutomation->CreatePropertyCondition(UIA_ControlTypePropertyId, var, &pC);
        if (pC) { pOr->AddCondition(pC); pC->Release(); }
        VariantClear(&var);
    }
    pOr->QueryInterface(IID_PPV_ARGS(&pCondition));
    pOr->Release();

    IUIAutomationElementArray* pFound = nullptr;
    if (SUCCEEDED(pRoot->FindAll(TreeScope_Subtree, pCondition, &pFound)) && pFound) {
        int count = 0;
        pFound->get_Length(&count);

        for (int i = 0; i < count; ++i) {
            IUIAutomationElement* pElem = nullptr;
            if (FAILED(pFound->GetElement(i, &pElem)) || !pElem) continue;

            // Bounding rectangle
            VARIANT vRect; VariantInit(&vRect);
            RECT rc = {};
            if (SUCCEEDED(pElem->GetCurrentPropertyValue(UIA_BoundingRectanglePropertyId, &vRect)) && vRect.vt == (VT_ARRAY | VT_R8)) {
                double* pData = nullptr;
                if (SUCCEEDED(SafeArrayAccessData(vRect.parray, (void**)&pData))) {
                    rc.left   = (LONG)pData[0];
                    rc.top    = (LONG)pData[1];
                    rc.right  = rc.left + (LONG)pData[2];
                    rc.bottom = rc.top  + (LONG)pData[3];
                    SafeArrayUnaccessData(vRect.parray);
                }
            }
            VariantClear(&vRect);

            // Clickable point
            POINT pt = {};
            if (SUCCEEDED(pElem->GetClickablePoint(&pt)) && rc.right > rc.left && rc.bottom > rc.top) {
                // Skip our own overlay
                if (g_hOverlay) {
                    HWND hwndAt = WindowFromPoint(pt);
                    if (hwndAt == g_hOverlay) { pElem->Release(); continue; }
                }

                HintTarget t;
                t.rect     = rc;
                t.clickPt  = pt;
                t.matchLen = 0;
                g_hints.push_back(t);
            }
            pElem->Release();
        }
        pFound->Release();
    }
    pCondition->Release();
    pRoot->Release();

    // Assign hint labels
    if (!g_hints.empty()) {
        int n = (int)g_hints.size();
        int len = 1;
        int capacity = 26;
        while (capacity < n) { ++len; capacity *= 26; }
        for (int i = 0; i < n; ++i)
            g_hints[i].label = MakeHint(i, len);
    }
}

// ============================================================
//  Overlay: render hints
// ============================================================
static void PaintOverlay(HDC hdc) {
    if (!g_hintMode || g_hints.empty()) return;

    // Clear to fully transparent (layered window alpha is handled by SetLayeredWindowAttributes)
    RECT rcClient; GetClientRect(g_hOverlay, &rcClient);
    HBRUSH hClear = CreateSolidBrush(RGB(0,0,0));
    FillRect(hdc, &rcClient, hClear);
    DeleteObject(hClear);

    // Set up font
    HFONT hFont = CreateFontW(
        HINT_FONT_SIZE, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Consolas");
    HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(0, 0, 0));

    for (auto& h : g_hints) {
        if (h.matchLen == 0 && !g_typed.empty()) continue; // filtered out

        // Background
        HBRUSH hBrush = CreateSolidBrush(RGB(255, 255, 0));
        HBRUSH hOldBrush = (HBRUSH)SelectObject(hdc, hBrush);

        RECT rc = h.rect;
        // Clamp to screen
        int sw = GetSystemMetrics(SM_CXSCREEN);
        int sh = GetSystemMetrics(SM_CYSCREEN);
        rc.left   = max(0, rc.left);
        rc.top    = max(0, rc.top);
        rc.right  = min(sw, rc.right);
        rc.bottom = min(sh, rc.bottom);

        // Draw rounded rect (approximated with Rectangle)
        HPEN hPen = CreatePen(PS_SOLID, 1, RGB(0,0,0));
        HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
        Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);

        // Draw label at top-left of the rect
        RECT rcText = rc;
        rcText.left  += HINT_PADDING;
        rcText.top   += HINT_PADDING;

        // If some chars typed, draw typed prefix in blue, rest in black
        if (h.matchLen > 0) {
            // Draw typed prefix
            std::wstring prefix = h.label.substr(0, h.matchLen);
            SetTextColor(hdc, RGB(0, 0, 200));
            DrawTextW(hdc, prefix.c_str(), (int)prefix.size(), &rcText,
                      DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOCLIP);
            // Measure prefix width
            SIZE sz; GetTextExtentPoint32W(hdc, prefix.c_str(), (int)prefix.size(), &sz);
            rcText.left += sz.cx;

            SetTextColor(hdc, RGB(0, 0, 0));
            std::wstring rest = h.label.substr(h.matchLen);
            DrawTextW(hdc, rest.c_str(), (int)rest.size(), &rcText,
                      DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOCLIP);
        } else {
            DrawTextW(hdc, h.label.c_str(), (int)h.label.size(), &rcText,
                      DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOCLIP);
        }

        SelectObject(hdc, hOldPen);
        DeleteObject(hPen);
        SelectObject(hdc, hOldBrush);
        DeleteObject(hBrush);
    }

    SelectObject(hdc, hOldFont);
    DeleteObject(hFont);
}

// ============================================================
//  Click simulation
// ============================================================
static void SimulateClick(POINT pt) {
    SetCursorPos(pt.x, pt.y);
    INPUT inputs[2] = {};
    inputs[0].type = INPUT_MOUSE;
    inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    inputs[1].type = INPUT_MOUSE;
    inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(2, inputs, sizeof(INPUT));
}

// ============================================================
//  Hint mode control
// ============================================================
static void EnterHintMode() {
    if (g_hintMode) return;

    CollectHints();
    if (g_hints.empty()) return;

    g_hintMode = true;
    g_typed.clear();

    // Show overlay
    SetWindowPos(g_hOverlay, HWND_TOPMOST, 0, 0,
                 GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_hOverlay, nullptr, TRUE);
    UpdateWindow(g_hOverlay);
}

static void ExitHintMode() {
    if (!g_hintMode) return;
    g_hintMode = false;
    g_typed.clear();
    ShowWindow(g_hOverlay, SW_HIDE);
}

static void ProcessHintChar(wchar_t ch) {
    if (!g_hintMode) return;

    ch = towlower(ch);
    if (ch < L'a' || ch > L'z') {
        if (ch == VK_ESCAPE) ExitHintMode();
        return;
    }

    g_typed.push_back(ch);

    // Filter hints
    int exactIdx = -1;
    int remaining = 0;
    for (int i = 0; i < (int)g_hints.size(); ++i) {
        auto& h = g_hints[i];
        if (h.matchLen >= (int)g_typed.size()) continue;

        if (h.label.compare(0, g_typed.size(), g_typed) == 0) {
            h.matchLen = (int)g_typed.size();
            ++remaining;
            if (h.label.size() == g_typed.size()) exactIdx = i;
        }
    }

    if (exactIdx >= 0) {
        POINT pt = g_hints[exactIdx].clickPt;
        ExitHintMode();
        SimulateClick(pt);
        return;
    }

    if (remaining == 0) { ExitHintMode(); return; }

    InvalidateRect(g_hOverlay, nullptr, TRUE);
    UpdateWindow(g_hOverlay);
}

// ============================================================
//  Low-level keyboard hook
// ============================================================
static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT* p = (KBDLLHOOKSTRUCT*)lParam;
        if (!(p->flags & LLKHF_INJECTED)) {
            bool isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);

            if (g_hintMode) {
                if (isDown) {
                    if (p->vkCode == VK_ESCAPE) { ExitHintMode(); return 1; }
                    wchar_t ch = (wchar_t)p->vkCode;
                    if (ch >= L'A' && ch <= L'Z') ch = towlower(ch);
                    if (ch >= L'a' && ch <= L'z') {
                        ProcessHintChar(ch);
                        return 1; // swallow
                    }
                }
                // Swallow all keys while in hint mode
                return 1;
            } else {
                // Toggle hint mode with 'f'
                if (isDown && p->vkCode == 'F') {
                    EnterHintMode();
                    return 1;
                }
            }
        }
    }
    return CallNextHookEx(g_hKeyboardHook, nCode, wParam, lParam);
}

// ============================================================
//  Overlay window procedure
// ============================================================
static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        PaintOverlay(hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1; // we handle background in WM_PAINT
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// ============================================================
//  Hidden message window (for hook callback delivery)
// ============================================================
static LRESULT CALLBACK MsgWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// ============================================================
//  WinMain
// ============================================================
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    // COM init for UI Automation
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
                     IID_PPV_ARGS(&g_pAutomation));

    // Register overlay window class
    WNDCLASSEXW wcOverlay = {};
    wcOverlay.cbSize        = sizeof(wcOverlay);
    wcOverlay.lpfnWndProc   = OverlayWndProc;
    wcOverlay.hInstance     = hInstance;
    wcOverlay.lpszClassName = L"VimiumOverlayClass";
    wcOverlay.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wcOverlay);

    // Create overlay (initially hidden)
    g_hOverlay = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"VimiumOverlayClass", L"",
        WS_POPUP,
        0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
        nullptr, nullptr, hInstance, nullptr);

    SetLayeredWindowAttributes(g_hOverlay, 0, 255, LWA_ALPHA);
    ShowWindow(g_hOverlay, SW_HIDE);

    // Register hidden message window class
    WNDCLASSEXW wcMsg = {};
    wcMsg.cbSize        = sizeof(wcMsg);
    wcMsg.lpfnWndProc   = MsgWndProc;
    wcMsg.hInstance     = hInstance;
    wcMsg.lpszClassName = L"VimiumMsgClass";
    RegisterClassExW(&wcMsg);

    g_hMessageWnd = CreateWindowExW(0, L"VimiumMsgClass", L"",
                                    0, 0, 0, 0, 0,
                                    HWND_MESSAGE, nullptr, hInstance, nullptr);

    // Install global low-level keyboard hook
    g_hKeyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc,
                                        nullptr, 0);
    if (!g_hKeyboardHook) {
        MessageBoxW(nullptr, L"Failed to install keyboard hook.", L"Error", MB_ICONERROR);
        return 1;
    }

    // Message loop
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // Cleanup
    UnhookWindowsHookEx(g_hKeyboardHook);
    if (g_pAutomation) g_pAutomation->Release();
    CoUninitialize();
    return 0;
}
