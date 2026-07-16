#include "Overlay.h"
#include "AngelfraudAPI.h"
#include "Logger.h"
#include "Config.h"
#include "Globals.h"
#include <thread>
#include <format>
#include <algorithm>
#include <map>
#include <unordered_map>
#include <vector>

void Overlay::run() {
    std::thread serverThread(&Overlay::OverlayLoop, this);
    serverThread.detach();
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

inline float g_ScrollDelta = 0.0f;

void RegisterRawInput(HWND hwnd) {
    RAWINPUTDEVICE rid;
    rid.usUsagePage = 0x01;
    rid.usUsage = 0x02;
    rid.dwFlags = RIDEV_INPUTSINK;
    rid.hwndTarget = hwnd;
    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
        Logger::logWarning("Failed to register raw input!");
    }
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_INPUT: {
        UINT dwSize = 0;
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, nullptr, &dwSize, sizeof(RAWINPUTHEADER)) == -1)
            break;
        std::vector<BYTE> inputData(dwSize);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, inputData.data(), &dwSize, sizeof(RAWINPUTHEADER)) == -1)
            break;
        RAWINPUT* rawInput = reinterpret_cast<RAWINPUT*>(inputData.data());
        if (rawInput->header.dwType == RIM_TYPEMOUSE) {
            if (rawInput->data.mouse.usButtonFlags & RI_MOUSE_WHEEL) {
                g_ScrollDelta = (static_cast<short>(rawInput->data.mouse.usButtonData) > 0) ? 1.0f : -1.0f;
            }
        }
        break;
    }
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

void LimitFPS(int fpsLimit) {
    float fps = 1000000.0f / static_cast<float>(fpsLimit);
    static auto awake_time = std::chrono::steady_clock::now() + std::chrono::microseconds(static_cast<int>(fps));
    awake_time += std::chrono::microseconds(static_cast<int>(fps));
    std::this_thread::sleep_until(awake_time);
}

void Overlay::OverlayLoop() noexcept {
    // Create application window
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, L"GDI+ Hook Window Class", nullptr };
    ::RegisterClassEx(&wc);
    window_handle = ::CreateWindow(wc.lpszClassName, L"GDI+ Window (Lightshot.exe)", WS_POPUP, 0, 0, globals::width, globals::height, nullptr, nullptr, wc.hInstance, nullptr);

    SetWindowLongPtr(window_handle, GWL_EXSTYLE, WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_TOOLWINDOW);
    // TOPMOST is set dynamically in Handler() based on CS2 foreground state
    SetLayeredWindowAttributes(window_handle, 0, 255, LWA_ALPHA);
    MARGINS margins = { -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(window_handle, &margins);

    // Initialize Direct3D
    if (!CreateDeviceD3D()) {
        CleanupDeviceD3D();
        ::UnregisterClass(wc.lpszClassName, wc.hInstance);
        globals::finish = true;
        return;
    }

    SetWindowPos(window_handle, HWND_NOTOPMOST, globals::posX, globals::posY, globals::width, globals::height, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    ::UpdateWindow(window_handle);

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.IniFilename = NULL;
    io.LogFilename = NULL;

    // Glyph range: basic Latin, Cyrillic, general punctuation, misc symbols (stars)
    const ImWchar text_range[] = {
        0x0020, 0x007F,
        0x0400, 0x04FF,
        0x2000, 0x206F,
        0x2600, 0x27BF,   // Misc symbols (⚡★◆ etc.)
        0,
    };
    // Symbol range for font merge (stars + ✕ for delete button)
    const ImWchar symbol_range[] = {
        0x2600, 0x27BF,   // ★ ☆ ✕ ✔ and all misc symbols
        0,
    };

    ImFontConfig cfg_main;
    cfg_main.MergeMode = false; // first font = base
    main_font = io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/segoeui.ttf", 15.0f, &cfg_main, text_range);
    if (main_font == nullptr) {
        main_font = io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/arial.ttf", 15.0f, &cfg_main, text_range);
        if (main_font == nullptr) main_font = io.Fonts->AddFontDefault();
    }
    // Merge Segoe UI Symbol to get visible ★/☆ glyphs
    if (main_font) {
        ImFontConfig cfg_merge;
        cfg_merge.MergeMode = true;
        if (!io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/seguisym.ttf", 15.0f, &cfg_merge, symbol_range)) {
            // Fallback: try merging Segoe UI Emoji
            io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/seguiemj.ttf", 15.0f, &cfg_merge, symbol_range);
        }
    }

    ImFontConfig cfg_title;
    cfg_title.MergeMode = false;
    title_font = io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/segoeuib.ttf", 18.0f, &cfg_title, text_range);
    if (title_font == nullptr) {
        title_font = io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/arial.ttf", 18.0f, &cfg_title, text_range);
        if (title_font == nullptr) title_font = io.Fonts->AddFontDefault();
    }
    if (title_font) {
        ImFontConfig cfg_merge;
        cfg_merge.MergeMode = true;
        if (!io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/seguisym.ttf", 18.0f, &cfg_merge, symbol_range)) {
            io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/seguiemj.ttf", 18.0f, &cfg_merge, symbol_range);
        }
    }

    // ── Monochrome dark theme (black & white) ──
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 10.0f;
    style.WindowBorderSize = 1.0f;
    style.WindowPadding = ImVec2(0, 0);
    style.FrameRounding = 6.0f;
    style.FramePadding = ImVec2(12, 6);
    style.ItemSpacing = ImVec2(10, 6);
    style.ItemInnerSpacing = ImVec2(8, 4);
    style.ScrollbarSize = 5.0f;
    style.ScrollbarRounding = 3.0f;
    style.GrabMinSize = 8.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 6.0f;
    style.TabBorderSize = 0.0f;
    style.ChildRounding = 8.0f;
    style.PopupRounding = 8.0f;

    // Full B&W palette
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.04f, 0.04f, 0.04f, 0.92f);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.06f, 0.06f, 0.06f, 0.85f);
    style.Colors[ImGuiCol_PopupBg] = ImVec4(0.06f, 0.06f, 0.06f, 0.94f);
    style.Colors[ImGuiCol_Border] = ImVec4(1.0f, 1.0f, 1.0f, 0.08f);
    style.Colors[ImGuiCol_Text] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.55f, 0.55f, 1.0f);
    style.Colors[ImGuiCol_Header] = ImVec4(1.0f, 1.0f, 1.0f, 0.08f);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(1.0f, 1.0f, 1.0f, 0.15f);
    style.Colors[ImGuiCol_HeaderActive] = ImVec4(1.0f, 1.0f, 1.0f, 0.20f);
    style.Colors[ImGuiCol_Button] = ImVec4(1.0f, 1.0f, 1.0f, 0.08f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(1.0f, 1.0f, 1.0f, 0.18f);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(1.0f, 1.0f, 1.0f, 0.28f);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.10f, 0.10f, 0.10f, 0.80f);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.14f, 0.14f, 0.14f, 0.80f);
    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.18f, 0.18f, 0.18f, 0.80f);
    style.Colors[ImGuiCol_TitleBg] = ImVec4(0.06f, 0.06f, 0.06f, 1.0f);
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.08f, 0.08f, 0.08f, 1.0f);
    style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.04f, 0.04f, 0.04f, 0.80f);
    style.Colors[ImGuiCol_SliderGrab] = ImVec4(1.0f, 1.0f, 1.0f, 0.20f);
    style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(1.0f, 1.0f, 1.0f, 0.40f);
    style.Colors[ImGuiCol_CheckMark] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_TableRowBg] = ImVec4(1.0f, 1.0f, 1.0f, 0.015f);
    style.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.035f);
    style.Colors[ImGuiCol_TableBorderLight] = ImVec4(1.0f, 1.0f, 1.0f, 0.04f);
    style.Colors[ImGuiCol_TableBorderStrong] = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
    style.Colors[ImGuiCol_Tab] = ImVec4(0.06f, 0.06f, 0.06f, 1.0f);
    style.Colors[ImGuiCol_TabHovered] = ImVec4(1.0f, 1.0f, 1.0f, 0.12f);
    style.Colors[ImGuiCol_TabActive] = ImVec4(1.0f, 1.0f, 1.0f, 0.20f);
    style.Colors[ImGuiCol_TabUnfocused] = ImVec4(0.05f, 0.05f, 0.05f, 1.0f);
    style.Colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.08f, 0.08f, 0.08f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.04f, 0.04f, 0.04f, 0.50f);
    style.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(1.0f, 1.0f, 1.0f, 0.10f);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1.0f, 1.0f, 1.0f, 0.20f);
    style.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(1.0f, 1.0f, 1.0f, 0.35f);
    style.Colors[ImGuiCol_ResizeGrip] = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
    style.Colors[ImGuiCol_ResizeGripHovered] = ImVec4(1.0f, 1.0f, 1.0f, 0.20f);
    style.Colors[ImGuiCol_ResizeGripActive] = ImVec4(1.0f, 1.0f, 1.0f, 0.35f);

    ImGui_ImplWin32_Init(window_handle);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // Get CS2 window size
    HWND cs2Hwnd = FindWindowA(nullptr, "Counter-Strike 2");
    if (cs2Hwnd) {
        RECT rect;
        if (GetClientRect(cs2Hwnd, &rect)) {
            globals::width = rect.right - rect.left;
            globals::height = rect.bottom - rect.top;
            POINT pt = { 0, 0 };
            if (ClientToScreen(cs2Hwnd, &pt)) {
                globals::posX = pt.x;
                globals::posY = pt.y;
            }
        }
    }

    CleanupRenderTarget();
    g_pSwapChain->ResizeBuffers(0, globals::width, globals::height, DXGI_FORMAT_UNKNOWN, 0);
    CreateRenderTarget();

    std::thread(&Overlay::Handler, this).detach();

    const float clear_color[4]{ 0 };

    // Main loop
    while (!globals::finish) {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT) {
                globals::finish = true;
            }
        }

        if (!isFinish) globals::finish = true;
        if (globals::finish) break;

        if (g_SwapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        g_SwapChainOccluded = false;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // Invisible background window (required for draw list)
        ImGui::Begin("##main", 0, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBackground);

        draw_list = ImGui::GetBackgroundDrawList();

        // Render the player list
        Overlay::Render();

        if (cfg->fpsLimiterState && !cfg->vsyncState) {
            LimitFPS(cfg->fpsLimiter);
        }

        ImGui::End();

        ImGui::Render();
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        HRESULT hr = g_pSwapChain->Present(cfg->vsyncState, 0);
        g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
    }

    // Cleanup
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();

    CleanupDeviceD3D();
    ::DestroyWindow(window_handle);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
}

// ─── Forward declarations ───────────────────────────────────────────────────
static void UpdateClickThrough(HWND hwnd);

// ─── Input Handler ──────────────────────────────────────────────────────────

std::map<int, std::pair<char, char>> keyMap = {
    {VK_OEM_1, {';', ':'}},
    {VK_OEM_PLUS, {'=', '+'}},
    {VK_OEM_COMMA, {',', '<'}},
    {VK_OEM_MINUS, {'-', '_'}},
    {VK_OEM_PERIOD, {'.', '>'}},
    {VK_OEM_2, {'/', '?'}},
    {VK_OEM_3, {'`', '~'}},
    {VK_OEM_4, {'[', '{'}},
    {VK_OEM_5, {'\\', '|'}},
    {VK_OEM_6, {']', '}'}},
    {VK_OEM_7, {'\'', '"'}},
    {'1', {'1', '!'}},
    {'2', {'2', '@'}},
    {'3', {'3', '#'}},
    {'4', {'4', '$'}},
    {'5', {'5', '%'}},
    {'6', {'6', '^'}},
    {'7', {'7', '&'}},
    {'8', {'8', '*'}},
    {'9', {'9', '('}},
    {'0', {'0', ')'}}
};

void toggleKey(int vkKey, bool& toggleState, ImGuiIO& io) {
    bool shiftHeld = (GetAsyncKeyState(VK_LSHIFT) & 0x8000) || (GetAsyncKeyState(VK_RSHIFT) & 0x8000);
    char charCode = 0;
    if (vkKey >= 'A' && vkKey <= 'Z') {
        charCode = shiftHeld ? (char)vkKey : (char)(vkKey + 32);
    }
    else if (vkKey >= '0' && vkKey <= '9') {
        charCode = shiftHeld ? keyMap[(char)vkKey].second : keyMap[(char)vkKey].first;
    }
    else if (keyMap.find(vkKey) != keyMap.end()) {
        charCode = shiftHeld ? keyMap[vkKey].second : keyMap[vkKey].first;
    }
    else if (vkKey == VK_SPACE) {
        charCode = (char)VK_SPACE;
    }
    if (toggleState && (GetAsyncKeyState(vkKey) & 0x8000)) {
        if (charCode != 0) io.AddInputCharacter(charCode);
        toggleState = false;
    }
    else if (!toggleState && !(GetAsyncKeyState(vkKey) & 0x8000)) {
        toggleState = true;
    }
}

void toggleKey(int vkKey, bool& toggleState, ImGuiMouseButton_ imguiKey, ImGuiIO& io) {
    if (toggleState && (GetAsyncKeyState(vkKey) & 0x8000)) {
        io.AddMouseButtonEvent(imguiKey, true);
        toggleState = false;
    }
    else if (!toggleState && !(GetAsyncKeyState(vkKey) & 0x8000)) {
        io.AddMouseButtonEvent(imguiKey, false);
        toggleState = true;
    }
}

void toggleKey(int vkKey, bool& toggleState, ImGuiKey imguiKey, ImGuiKey imguiModKey, ImGuiIO& io) {
    if (toggleState && (GetAsyncKeyState(vkKey) & 0x8000)) {
        io.AddKeyEvent(imguiKey, true);
        if (imguiModKey) {
            io.AddKeyEvent(imguiModKey, true);
        }
        toggleState = false;
    }
    else if (!toggleState && !(GetAsyncKeyState(vkKey) & 0x8000)) {
        io.AddKeyEvent(imguiKey, false);
        if (imguiModKey) {
            io.AddKeyEvent(imguiModKey, false);
        }
        toggleState = true;
    }
}

void Overlay::Handler() noexcept {
    bool prevMenuState = false;
    int width_old, height_old, posX_old, posY_old;

    // Track CS2 window
    HWND cs2Hwnd = FindWindowA(nullptr, "Counter-Strike 2");
    if (cs2Hwnd) {
        RECT rect;
        if (GetClientRect(cs2Hwnd, &rect)) {
            width_old = rect.right - rect.left;
            height_old = rect.bottom - rect.top;
            POINT pt = { 0, 0 };
            if (ClientToScreen(cs2Hwnd, &pt)) {
                posX_old = pt.x;
                posY_old = pt.y;
            }
        }
    }

    CleanupRenderTarget();
    SetWindowPos(window_handle, (HWND)-1, posX_old, posY_old, globals::width, globals::height, 0);
    g_pSwapChain->ResizeBuffers(0, globals::width, globals::height, DXGI_FORMAT_UNKNOWN, 0);
    CreateRenderTarget();

    RegisterRawInput(window_handle);

    bool curMenuState = false;
    int prevMenuBind = cfg->overlayMenuBind;

    while (!globals::finish) {
        curMenuState = (GetAsyncKeyState(cfg->overlayMenuBind) & 0x8000) != 0;
        if (curMenuState && !prevMenuState) {
            globals::menuState = !globals::menuState;
            UpdateClickThrough(window_handle);
            // When closing the menu: keep ImGui visible (just non-interactive),
            // remove only the dark overlay, and focus the game for immediate play.
            if (!globals::menuState) {
                HWND cs2w = FindWindowA(nullptr, "Counter-Strike 2");
                if (cs2w) SetForegroundWindow(cs2w);
            }
        }
        prevMenuState = curMenuState;

        // Detect appMode change to update click-through
        static int prevAppMode = globals::appMode;
        if (prevAppMode != globals::appMode) {
            prevAppMode = globals::appMode;
            UpdateClickThrough(window_handle);

            // When switching to overlay mode, ensure window is fullscreen over CS2
            if (globals::appMode == 1) {
                HWND cs2 = FindWindowA(nullptr, "Counter-Strike 2");
                if (cs2) {
                    RECT rect;
                    if (GetClientRect(cs2, &rect)) {
                        int w = rect.right - rect.left;
                        int h = rect.bottom - rect.top;
                        POINT pt = { 0, 0 };
                        if (ClientToScreen(cs2, &pt)) {
                            globals::width = w; globals::height = h;
                            globals::posX = pt.x; globals::posY = pt.y;
                            CleanupRenderTarget();
                            SetWindowPos(window_handle, HWND_TOPMOST, pt.x, pt.y, w, h, SWP_SHOWWINDOW);
                            g_pSwapChain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
                            CreateRenderTarget();
                        }
                    }
                }
            }
        }

        // Track CS2 window position changes
        HWND cs2 = FindWindowA(nullptr, "Counter-Strike 2");
        if (cs2) {
            RECT rect;
            if (GetClientRect(cs2, &rect)) {
                int w = rect.right - rect.left;
                int h = rect.bottom - rect.top;
                POINT pt = { 0, 0 };
                if (ClientToScreen(cs2, &pt)) {
                    if (w != width_old || h != height_old || pt.x != posX_old || pt.y != posY_old) {
                        width_old = globals::width = w;
                        height_old = globals::height = h;
                        posX_old = globals::posX = pt.x;
                        posY_old = globals::posY = pt.y;
                        CleanupRenderTarget();
                        SetWindowPos(window_handle, (HWND)-1, pt.x, pt.y, w, h, 0);
                        g_pSwapChain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
                        CreateRenderTarget();
                    }
                }
            }
        }

        // ── Foreground window check: only keep overlay visible when CS2 is active ──
        static bool overlayHidden = false;
        bool cs2Foreground = (cs2 && GetForegroundWindow() == cs2);
        bool shouldBeHidden = !cs2Foreground && (globals::appMode == 1) && !globals::menuState;

        if (shouldBeHidden && !overlayHidden) {
            // CS2 lost focus — hide overlay but keep it positioned
            ShowWindow(window_handle, SW_HIDE);
            overlayHidden = true;
        } else if (!shouldBeHidden && overlayHidden) {
            // CS2 regained focus — show overlay again and bring to top
            ShowWindow(window_handle, SW_SHOW);
            SetWindowPos(window_handle, HWND_TOPMOST,
                globals::posX, globals::posY, globals::width, globals::height,
                SWP_NOACTIVATE);
            overlayHidden = false;
        } else if (cs2Foreground) {
            // CS2 is active — ensure overlay is TOPMOST (z-order safety)
            SetWindowPos(window_handle, HWND_TOPMOST,
                globals::posX, globals::posY, globals::width, globals::height,
                SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
        }

        // Input
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = NULL;
        io.LogFilename = NULL;

        POINT cursorPos;
        if (GetCursorPos(&cursorPos)) {
            ScreenToClient(window_handle, &cursorPos);
            io.AddMousePosEvent((float)cursorPos.x, (float)cursorPos.y);
        }

        if (g_ScrollDelta) {
            io.AddMouseWheelEvent(0.0f, g_ScrollDelta);
            g_ScrollDelta = 0.0f;
        }

        static bool keyState[256] = { true };
        for (int vkKey = 'A'; vkKey <= 'Z'; ++vkKey)
            toggleKey(vkKey, keyState[vkKey], io);
        for (int vkKey = '0'; vkKey <= '9'; ++vkKey)
            toggleKey(vkKey, keyState[vkKey], io);

        static bool LMouseState = true, RMouseState = true, MMouseState = true;
        static bool LeftCtrl = true, LeftShift = true, Backspace = true, Enter = true, Tab = true, Delete = true;
        static bool ArrowUp = true, ArrowDown = true, ArrowLeft = true, ArrowRight = true;

        toggleKey(VK_LBUTTON, LMouseState, ImGuiMouseButton_Left, io);
        toggleKey(VK_RBUTTON, RMouseState, ImGuiMouseButton_Right, io);
        toggleKey(VK_MBUTTON, MMouseState, ImGuiMouseButton_Middle, io);
        toggleKey(VK_LCONTROL, LeftCtrl, ImGuiKey_LeftCtrl, ImGuiMod_Ctrl, io);
        toggleKey(VK_LSHIFT, LeftShift, ImGuiKey_LeftShift, ImGuiMod_Shift, io);
        toggleKey(VK_BACK, Backspace, ImGuiKey_Backspace, ImGuiMod_None, io);
        toggleKey(VK_RETURN, Enter, ImGuiKey_Enter, ImGuiMod_None, io);
        toggleKey(VK_TAB, Tab, ImGuiKey_Tab, ImGuiMod_None, io);
        toggleKey(VK_DELETE, Delete, ImGuiKey_Delete, ImGuiMod_None, io);
        toggleKey(VK_UP, ArrowUp, ImGuiKey_UpArrow, ImGuiMod_None, io);
        toggleKey(VK_DOWN, ArrowDown, ImGuiKey_DownArrow, ImGuiMod_None, io);
        toggleKey(VK_LEFT, ArrowLeft, ImGuiKey_LeftArrow, ImGuiMod_None, io);
        toggleKey(VK_RIGHT, ArrowRight, ImGuiKey_RightArrow, ImGuiMod_None, io);

        toggleKey(VK_OEM_1, keyState[VK_OEM_1], io);
        toggleKey(VK_OEM_PLUS, keyState[VK_OEM_PLUS], io);
        toggleKey(VK_OEM_COMMA, keyState[VK_OEM_COMMA], io);
        toggleKey(VK_OEM_MINUS, keyState[VK_OEM_MINUS], io);
        toggleKey(VK_OEM_PERIOD, keyState[VK_OEM_PERIOD], io);
        toggleKey(VK_SPACE, keyState[VK_SPACE], io);
        toggleKey(VK_OEM_2, keyState[VK_OEM_2], io);
        toggleKey(VK_OEM_3, keyState[VK_OEM_3], io);
        toggleKey(VK_OEM_4, keyState[VK_OEM_4], io);
        toggleKey(VK_OEM_5, keyState[VK_OEM_5], io);
        toggleKey(VK_OEM_6, keyState[VK_OEM_6], io);
        toggleKey(VK_OEM_7, keyState[VK_OEM_7], io);

        std::this_thread::sleep_for(std::chrono::microseconds(15625));
    }
}

// ─── Rendering ──────────────────────────────────────────────────────────────

// Syncs matchPlayers.isFavorite from the server favorites cache.
// Called every frame from Render() so stars stay correct even when the
// Favorites window is closed.
static void SyncFavoriteFlags() {
    static auto lastSync = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (now - lastSync < std::chrono::seconds(3)) return;
    lastSync = now;

    auto* api = g_api;
    if (!api) return;

    auto favs = api->GetFavorites();
    if (favs.empty()) {
        std::vector<FavoriteEntry> tmp;
        if (api->RefreshFavorites(tmp)) favs = tmp;
    }

    std::lock_guard<std::mutex> lock(globals::playersMutex);
    for (auto& mp : globals::matchPlayers) {
        for (const auto& f : favs) {
            if (f.steamid == mp.steamid) {
                mp.isFavorite = true;
                break;
            }
        }
    }
}

void Overlay::Render() noexcept {
    if (globals::appMode == 0) {
        RenderLauncher();
        return;
    }

    // Sync isFavorite from server cache every 3s (runs regardless of Favorites window)
    SyncFavoriteFlags();

    // ── Overlay running mode ──
    // Dark overlay only when menu is open (INSERT toggle)
    if (globals::menuState) {
        ImGui::GetForegroundDrawList()->AddRectFilled(
            ImVec2(0, 0), ImVec2((float)globals::width, (float)globals::height),
            IM_COL32(0, 0, 0, 120), 10.0f);
    }

    RenderPlayerList();

    // Favorites window (shown on top of player list when toggled)
    if (showFavorites) {
        RenderFavoritesWindow();
    }
}

// ─── B&W colour helpers ──────────────────────────────────────────────────────
static ImVec4 cWhite(float a = 1.0f)     { return ImVec4(1.00f, 1.00f, 1.00f, a); }
static ImVec4 cGray3(float a = 1.0f)    { return ImVec4(0.80f, 0.80f, 0.80f, a); }
static ImVec4 cGray2(float a = 1.0f)    { return ImVec4(0.55f, 0.55f, 0.55f, a); }
static ImVec4 cGray1(float a = 1.0f)    { return ImVec4(0.35f, 0.35f, 0.35f, a); }
static ImVec4 cGray0(float a = 1.0f)    { return ImVec4(0.18f, 0.18f, 0.18f, a); }
static ImVec4 cBlack1(float a = 1.0f)   { return ImVec4(0.06f, 0.06f, 0.06f, a); }
static ImVec4 cBlack2(float a = 1.0f)   { return ImVec4(0.04f, 0.04f, 0.04f, a); }
static ImU32  c32(ImVec4 v)             { return ImGui::ColorConvertFloat4ToU32(v); }

// ─── Overlay click-through management ───────────────────────────────────────
static void UpdateClickThrough(HWND hwnd) {
    if (!hwnd) return;
    LONG_PTR style = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    bool needInput = (globals::appMode == 0) || globals::menuState;
    if (needInput) {
        style &= ~WS_EX_TRANSPARENT;
    } else {
        style |= WS_EX_TRANSPARENT;
    }
    SetWindowLongPtr(hwnd, GWL_EXSTYLE, style);
    // Force window to refresh its hit-test region
    SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
}

// ─── Launcher ─────────────────────────────────────────────────────────────────

void Overlay::RenderLauncher() noexcept {
    const float W = 380.0f;
    const float H = 340.0f;
    const ImVec2 ctr((float)globals::width * 0.5f, (float)globals::height * 0.5f);
    ImGui::SetNextWindowPos(ImVec2(ctr.x - W * 0.5f, ctr.y - H * 0.5f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(W, H), ImGuiCond_Always);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1, 1, 1, 0.10f));

    ImGui::Begin("##launcher", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoSavedSettings);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetWindowPos();
    ImVec2 size = ImGui::GetWindowSize();

    // ── Top accent line ──
    dl->AddRectFilledMultiColor(pos, ImVec2(pos.x + size.x, pos.y + 3),
        IM_COL32(180, 180, 180, 80), IM_COL32(120, 120, 120, 80),
        IM_COL32(60, 60, 60, 80), IM_COL32(100, 100, 100, 80));

    // ── Logo / Title ──
    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + 44));
    ImGui::PushFont(title_font);
    const char* title = "ANGELFRAUD SCRAPER";
    float tw = ImGui::CalcTextSize(title).x;
    ImGui::SetCursorPosX((W - tw) * 0.5f);
    ImGui::TextColored(cWhite(0.95f), "%s", title);
    ImGui::PopFont();

    // Subtitle
    ImGui::PushFont(main_font);
    const char* sub = "CS2 Inventory Overlay";
    float sw = ImGui::CalcTextSize(sub).x;
    ImGui::SetCursorPosX((W - sw) * 0.5f);
    ImGui::TextColored(cGray2(0.6f), "%s", sub);
    ImGui::PopFont();

    ImGui::Dummy(ImVec2(0, 10));

    // Divider line
    dl->AddLine(ImVec2(pos.x + 40, ImGui::GetCursorScreenPos().y),
        ImVec2(pos.x + size.x - 40, ImGui::GetCursorScreenPos().y),
        IM_COL32(255, 255, 255, 15));

    ImGui::Dummy(ImVec2(0, 20));

    // ── START button ──
    float btnW = 220.0f;
    float btnH = 52.0f;
    ImGui::SetCursorPosX((W - btnW) * 0.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1, 1, 1, 0.12f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.22f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.32f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));

    if (ImGui::Button("START", ImVec2(btnW, btnH))) {
        globals::appMode = 1;
    }

    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(1);

    // ── Status ──
    ImGui::Dummy(ImVec2(0, 18));
    std::string status;
    if (phpsessid.empty()) {
        status = "PHPSESSID not set";
        ImGui::PushStyleColor(ImGuiCol_Text, cGray1(0.55f));
    } else {
        status = "Session active";
        ImGui::PushStyleColor(ImGuiCol_Text, cGray2(0.55f));
    }
    float stw = ImGui::CalcTextSize(status.c_str()).x;
    ImGui::SetCursorPosX((W - stw) * 0.5f);
    ImGui::Text("%s", status.c_str());
    ImGui::PopStyleColor();

    // ── Footer ──
    float fy = size.y - 26.0f;
    dl->AddLine(ImVec2(pos.x + 30, pos.y + fy),
        ImVec2(pos.x + size.x - 30, pos.y + fy),
        IM_COL32(255, 255, 255, 10));
    const char* footer = "Angelfraud-Scraper | CS2 Overlay by try0raw";
    float fw = ImGui::CalcTextSize(footer).x;
    ImGui::SetCursorPosX((W - fw) * 0.5f);
    ImGui::SetCursorPosY(fy + 6.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, cGray1(0.35f));
    ImGui::Text("%s", footer);
    ImGui::PopStyleColor();

    ImGui::End();
    ImGui::PopStyleColor(1); // border
    ImGui::PopStyleVar(3);
}

void Overlay::RenderPlayerList() noexcept {
    static bool windowInit = false;
    static ImVec2 windowPos(cfg->overlayPos[0], cfg->overlayPos[1]);

    const float HEADER_H = 34.0f;
    const float CONTENT_PAD = 12.0f;
    const float footH = 24.0f;
    const float ROW_H = 28.0f;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(280, 150));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse
        | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar;

    if (!globals::menuState) {
        flags |= ImGuiWindowFlags_NoInputs;
    }

    // Focus the window the moment it opens — no click needed to start dragging
    static bool prevMenuFocus = false;
    if (globals::menuState && !prevMenuFocus) {
        ImGui::SetNextWindowFocus();
    }
    prevMenuFocus = globals::menuState;

    // ── Calculate target size ──
    int numPlayers = 0;
    {
        std::lock_guard<std::mutex> lock(globals::playersMutex);
        numPlayers = (int)globals::matchPlayers.size();
    }

    float tableH = numPlayers > 0 ? (float)numPlayers * ROW_H + 28.0f : 50.0f;
    float profileH = 0.0f;
    {
        std::lock_guard<std::mutex> lock(globals::profileMutex);
        if (globals::profile.loaded) profileH = 20.0f;
    }
    float baseH = HEADER_H + 6.0f + profileH + 20.0f + 8.0f + footH;
    float targetH = baseH + tableH;

    // Smooth animation (lerp)
    static float animH = targetH;
    animH += (targetH - animH) * 0.14f;
    if (fabs(animH - targetH) < 0.5f) animH = targetH;
    if (animH < 150.0f) animH = 150.0f;
    if (animH > 620.0f) animH = 620.0f;

    float targetW = std::max(300.0f, windowPos.x > 0 ? cfg->overlaySize[0] : 340.0f);

    if (!windowInit) {
        ImGui::SetNextWindowPos(windowPos, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(targetW, animH), ImGuiCond_FirstUseEver);
        windowInit = true;
    }
    ImGui::SetNextWindowSize(ImVec2(targetW, animH), ImGuiCond_Always);

    ImGui::Begin("##playerlist", nullptr, flags);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetWindowPos();
    ImVec2 size = ImGui::GetWindowSize();

    // ── Update stored position/size ──
    windowPos = pos;
    cfg->overlayPos[0] = pos.x;
    cfg->overlayPos[1] = pos.y;
    cfg->overlaySize[0] = size.x;
    cfg->overlaySize[1] = size.y;

    // ── Dark gradient header ──
    {
        const ImRect hr(pos, ImVec2(pos.x + size.x, pos.y + HEADER_H));
        dl->AddRectFilledMultiColor(hr.Min, hr.Max,
            c32(cGray0(0.85f)), c32(cBlack1(0.85f)),
            c32(cBlack2(0.85f)), c32(cBlack1(0.85f)));

        ImGui::PushFont(title_font);
        const char* t = "INVENTORY PRICES";
        float ty = pos.y + (HEADER_H - ImGui::CalcTextSize(t).y) * 0.5f;
        dl->AddText(ImVec2(pos.x + 12, ty), IM_COL32(255, 255, 255, 235), t);

        if (!globals::currentMap.empty()) {
            std::string ms = "[" + globals::currentMap + "]";
            ImVec2 msz = ImGui::CalcTextSize(ms.c_str());
            dl->AddText(ImVec2(pos.x + size.x - msz.x - 12, ty),
                IM_COL32(255, 255, 255, 120), ms.c_str());
        }
        ImGui::PopFont();

        dl->AddLine(ImVec2(pos.x + 6, pos.y + HEADER_H),
            ImVec2(pos.x + size.x - 6, pos.y + HEADER_H),
            IM_COL32(255, 255, 255, 18));

        // Drag handle — auto-activates on hover+mousedown (no click needed)
        ImGui::SetCursorScreenPos(hr.Min);
        ImGui::InvisibleButton("##drag", hr.GetSize());
        if (ImGui::IsItemHovered() && ImGui::IsMouseDown(0) && !ImGui::IsAnyItemActive()) {
            ImGui::SetActiveID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
        }
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0)) {
            ImVec2 d = ImGui::GetIO().MouseDelta;
            ImGui::SetWindowPos(ImVec2(pos.x + d.x, pos.y + d.y));
        }
    }

    // ── Content ──
    float cy = pos.y + HEADER_H + 6.0f;
    ImGui::SetCursorScreenPos(ImVec2(pos.x + CONTENT_PAD, cy));

    // ── Profile info bar ──
    {
        std::lock_guard<std::mutex> lock(globals::profileMutex);
        if (globals::profile.loaded) {
            auto& p = globals::profile;
            std::string info = std::format("{} [ID {}] | {}% | +${:.2f} | -${:.2f}",
                p.nickname, p.workerId, p.currentSharePercent, p.yourShare, p.debt);
            ImGui::TextColored(cGray3(0.8f), "%s", info.c_str());
            ImGui::SetCursorScreenPos(ImVec2(pos.x + CONTENT_PAD, cy + 20.0f));
        } else {
            ImGui::SetCursorScreenPos(ImVec2(pos.x + CONTENT_PAD, cy));
        }
    }

    // Player count & favorites button
    float pcY = ImGui::GetCursorScreenPos().y;
    {
        std::lock_guard<std::mutex> lock(globals::playersMutex);
        if (!globals::matchPlayers.empty()) {
            int loaded = 0;
            for (auto& p : globals::matchPlayers) if (p.valueLoaded) loaded++;
            ImGui::TextColored(cGray2(0.6f), "%d players | %d loaded",
                (int)globals::matchPlayers.size(), loaded);
        }
    }

    // [*] Favorites button at the right of the info line
    {
        ImVec2 btnPos = ImVec2(pos.x + size.x - CONTENT_PAD - 85, pcY - 2.0f);
        ImGui::SetCursorScreenPos(btnPos);
        std::string favLabel = showFavorites ? "\xe2\x98\x85 Favorites" : "\xe2\x98\x86 Favorites";
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.55f, 0.55f, 0.8f));
        if (ImGui::Button(favLabel.c_str(), ImVec2(90, 0))) {
            showFavorites = !showFavorites;
            if (showFavorites && g_api) {
                std::vector<FavoriteEntry> tmp;
                g_api->RefreshFavorites(tmp);
            }
        }
        ImGui::PopStyleColor(2);

        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::Text("Open favorites window");
            ImGui::EndTooltip();
        }
    }

    ImGui::SetCursorScreenPos(ImVec2(pos.x + CONTENT_PAD, pcY + 26.0f));

    // ── Table (child with left+right padding) ──
    {
        float tableWrapW = size.x - CONTENT_PAD * 2;
        ImGui::BeginChild("##tableWrap", ImVec2(tableWrapW, 0), false,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        std::lock_guard<std::mutex> lock(globals::playersMutex);
        if (globals::matchPlayers.empty()) {
            ImGui::SetCursorScreenPos(ImVec2(pos.x + CONTENT_PAD + 4, ImGui::GetCursorScreenPos().y + 24.0f));
            if (globals::serverConnected && !globals::allplayersEverSeen) {
                ImGui::TextColored(cGray2(0.45f), "Connected — no player list (retake/DM mode)");
                ImGui::SetCursorScreenPos(ImVec2(pos.x + CONTENT_PAD + 4, ImGui::GetCursorScreenPos().y + 4));
                if (!globals::steamid.empty())
                    ImGui::TextColored(cGray1(0.35f), "Local: %s", globals::nickname.c_str());
            } else {
                ImGui::TextColored(cGray2(0.45f), "Waiting for match data...");
            }
        } else {
            std::vector<MatchPlayer> sorted = globals::matchPlayers;
            std::sort(sorted.begin(), sorted.end(), [](const MatchPlayer& a, const MatchPlayer& b) {
                if (a.isLocalPlayer != b.isLocalPlayer) return a.isLocalPlayer;
                return a.observerSlot < b.observerSlot;
            });
            RenderPlayerTable(sorted);
        }

        ImGui::EndChild();
    }

    // ── Footer ──
    {
        float fy = ImGui::GetWindowHeight() - footH;
        dl->AddLine(ImVec2(pos.x + 6, pos.y + fy),
            ImVec2(pos.x + size.x - 6, pos.y + fy),
            IM_COL32(255, 255, 255, 12));
        ImGui::SetCursorScreenPos(ImVec2(pos.x + CONTENT_PAD, pos.y + fy + 4.0f));

        std::string ft;
        if (phpsessid.empty()) ft = "PHPSESSID not set";
        else if (globals::matchPlayers.empty()) {
            auto elapsed = std::chrono::steady_clock::now() - globals::gsiLastUpdate;
            auto sec = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();

            if (!globals::serverConnected) {
                // Not on any server — waiting for CS2 match
                if (sec < 10) ft = std::format("Connecting to server... (GSI active, {}s ago)", sec);
                else ft = "Waiting for CS2 match...";
            }
            else if (!globals::allplayersEverSeen) {
                // Connected to a server, but allplayers never arrived
                // This happens in retake, deathmatch, and community server modes
                ft = std::format("[{}] Connected — no player data (retake/DM?)", globals::currentMap);
            }
            else if (sec < 10) {
                // allplayers was seen but is temporarily absent (e.g. between rounds)
                if (globals::allplayersActive)
                    ft = std::format("In match... waiting for players (GSI active, {}s ago)", sec);
                else
                    ft = std::format("In match (allplayers inactive)... waiting for players");
            }
            else ft = "Waiting for CS2 match...";
        }
        else ft = "Angelfraud-Scraper | CS2 Overlay by try0raw";
        ImGui::TextColored(cGray1(0.45f), "%s", ft.c_str());
    }

    ImGui::End();
    ImGui::PopStyleVar(3);
}

// ─── Helper: toggle favorite for a player ────────────────────────────────
static void TryToggleFavorite(const MatchPlayer& player) {
    std::thread([steamid = player.steamid, name = player.name,
                 avatar = player.avatar, totalValue = player.totalValue,
                 valueStatus = player.valueStatus]() {
        try {
            auto* api = g_api;
            if (!api) { Logger::logWarning("ToggleFav: g_api null"); return; }
            if (api->GetSessionId().empty()) { Logger::logWarning("ToggleFav: no PHPSESSID"); return; }

            std::string valStr = std::format("${:.2f}", totalValue);
            if (totalValue <= 0) {
                if (valueStatus == "private") valStr = "PRIVATE";
                else if (valueStatus == "empty") valStr = "EMPTY";
                else valStr = "$0.00";
            }
            Logger::logInfo(std::format("ToggleFav: toggling {} ({})", steamid, name));
            bool ok = api->ToggleFavorite(steamid, name, avatar, valStr, "");
            Logger::logInfo(std::format("ToggleFav: result = {}", ok ? "success" : "failed"));

            if (ok) {
                std::lock_guard<std::mutex> lock(globals::playersMutex);
                for (auto& p : globals::matchPlayers) {
                    if (p.steamid == steamid) {
                        p.isFavorite = !p.isFavorite;
                        break;
                    }
                }
                globals::favNeedRefresh = true;
            }
        } catch (const std::exception& e) {
            Logger::logWarning(std::format("ToggleFav: exception: {}", e.what()));
        } catch (...) {
            Logger::logWarning("ToggleFav: unknown exception");
        }
    }).detach();
}

void Overlay::RenderPlayerTable(const std::vector<MatchPlayer>& players) noexcept {
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6, 5));
    ImGui::PushStyleColor(ImGuiCol_TableBorderLight, ImVec4(1, 1, 1, 0.03f));
    ImGui::PushStyleColor(ImGuiCol_TableBorderStrong, ImVec4(1, 1, 1, 0.05f));

    if (ImGui::BeginTable("players", 3,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoHostExtendX)) {

        ImGui::TableSetupColumn("Player", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("##fav", ImGuiTableColumnFlags_WidthFixed, 30.0f);

        // ── Table header ──
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        for (int col = 0; col < 3; col++) {
            ImGui::TableSetColumnIndex(col);
            if (col == 0) ImGui::TextColored(cGray2(0.55f), "Player");
            else if (col == 1) ImGui::TextColored(cGray2(0.55f), "Value");
        }

        // ── Player rows ──
        for (const auto& player : players) {
            ImGui::TableNextRow();

            // Name column
            ImGui::TableSetColumnIndex(0);
            std::string nm = player.name.empty() ? "Unknown" : player.name;
            if (nm.length() > 20) nm = nm.substr(0, 18) + "..";

            if (player.isLocalPlayer) {
                ImGui::TextColored(cWhite(1.0f), "> %s", nm.c_str());
            } else {
                ImGui::TextColored(cGray3(0.85f), "%s", nm.c_str());
            }

            // Value column
            ImGui::TableSetColumnIndex(1);
            if (player.valueLoaded && !player.valueError) {
                if (player.totalValue > 0) {
                    ImGui::TextColored(cWhite(0.95f), "$%.2f", player.totalValue);
                } else {
                    const char* dt = (player.valueStatus == "ok") ? "-"
                        : player.valueStatus.c_str();
                    ImGui::TextColored(cGray2(0.6f), "%s", dt);
                }
            } else if (player.valueError) {
                std::string s = player.valueStatus;
                if (s == "private") {
                    ImGui::TextColored(cGray1(0.7f), "PRIVATE");
                } else if (s == "empty") {
                    ImGui::TextColored(cGray2(0.55f), "EMPTY");
                } else if (s == "banned") {
                    ImGui::TextColored(cGray1(0.8f), "BANNED");
                } else {
                    ImGui::TextColored(cGray1(0.7f), "%s", s.c_str());
                }
            } else if (player.steamid.empty()) {
                ImGui::TextColored(cGray0(0.7f), "--");
            } else {
                ImGui::TextColored(cGray2(0.6f), "...");
            }

            // ── Favorite star column ──
            ImGui::TableSetColumnIndex(2);
            if (!player.steamid.empty()) {
                ImGui::PushID(player.steamid.c_str());

                // Use the table's font (main_font/Segoe UI has ★/☆)
                if (player.isFavorite) {
                    // Filled star — gold colored button
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.1f, 0.1f, 0.1f, 0.3f));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.0f, 0.9f));
                    if (ImGui::SmallButton("\xe2\x98\x85")) {  // ★
                        TryToggleFavorite(player);
                    }
                    ImGui::PopStyleColor(3);
                } else {
                    // Empty star — dim gray button
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.1f, 0.1f, 0.1f, 0.3f));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 0.5f));
                    if (ImGui::SmallButton("\xe2\x98\x86")) {  // ☆
                        TryToggleFavorite(player);
                    }
                    ImGui::PopStyleColor(3);
                }

                // Tooltip
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    if (player.isFavorite) {
                        ImGui::Text("Remove from favorites");
                    } else {
                        ImGui::Text("Add to favorites");
                    }
                    ImGui::EndTooltip();
                }

                ImGui::PopID();
            }
        }

        ImGui::EndTable();
    }

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(1);
}

// ─── Favorites Window ───────────────────────────────────────────────────────

void Overlay::RenderFavoritesWindow() noexcept {
    auto* api = g_api;
    if (!api) return;

    const float W = 420.0f;
    const float H = 440.0f;
    const ImVec2 ctr((float)globals::width * 0.5f, (float)globals::height * 0.5f);
    ImGui::SetNextWindowPos(ImVec2(ctr.x - W * 0.5f, ctr.y - H * 0.5f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(W, H), ImGuiCond_FirstUseEver);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 12));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings
        | ImGuiWindowFlags_NoScrollbar;

    if (ImGui::Begin("\xe2\x98\x85 Favorites", &showFavorites, flags)) {
        ImVec2 pos = ImGui::GetWindowPos();
        ImVec2 size = ImGui::GetWindowSize();

        // ── Per-favorite note edit buffers ──
        static std::unordered_map<int, std::string> noteBufs;

        // ── Fetch/refresh favorites ──
        static std::vector<FavoriteEntry> favs;
        static std::chrono::steady_clock::time_point lastRefresh;
        auto now = std::chrono::steady_clock::now();

        // Refresh on timer OR when favNeedRefresh is set (from add/remove)
        bool needRefresh = favs.empty()
            || now - lastRefresh > std::chrono::seconds(5)
            || globals::favNeedRefresh.load();
        if (needRefresh) {
            globals::favNeedRefresh = false;
            favs = api->GetFavorites();
            if (favs.empty()) {
                std::vector<FavoriteEntry> tmp;
                if (api->RefreshFavorites(tmp)) favs = tmp;
            }
            lastRefresh = now;

            // Sync isFavorite: only add true from server, never reset to false.
            // TryToggleFavorite and the Delete button handle the OFF state locally.
            {
                std::lock_guard<std::mutex> lock(globals::playersMutex);
                for (auto& mp : globals::matchPlayers) {
                    for (const auto& f : favs)
                        if (f.steamid == mp.steamid) { mp.isFavorite = true; break; }
                }
            }
            // Clean stale note buffers
            for (auto it = noteBufs.begin(); it != noteBufs.end(); ) {
                bool found = false;
                for (const auto& f : favs)
                    if (f.favId == it->first) { found = true; break; }
                if (found) ++it; else it = noteBufs.erase(it);
            }
        }

        // ── Header bar (title + refresh button) ──
        ImGui::SetCursorScreenPos(ImVec2(pos.x + 12, pos.y + 12));
        ImGui::TextColored(cGray3(0.7f), "Favorite players");

        ImGui::SameLine(pos.x + size.x - 105);
        if (ImGui::Button("Refresh", ImVec2(90, 22))) {
            favs.clear();
            noteBufs.clear();
            lastRefresh = {};
            std::thread([api]() { std::vector<FavoriteEntry> tmp; api->RefreshFavorites(tmp); }).detach();
        }

        // ── Scrollable list ──
        float childW = size.x - 24.0f;
        float childX = pos.x + (size.x - childW) * 0.5f;
        ImGui::SetCursorScreenPos(ImVec2(childX, pos.y + 40));
        ImGui::BeginChild("##favList", ImVec2(childW, size.y - 52), false,
            ImGuiWindowFlags_AlwaysVerticalScrollbar);

        if (favs.empty()) {
            ImGui::SetCursorPosY(60);
            ImGui::SetCursorPosX(10);
            ImGui::TextColored(cGray2(0.55f), "No favorites yet.");
            ImGui::SetCursorPosX(10);
            ImGui::TextColored(cGray2(0.45f), "Click \xe2\x98\x85 next to a player to add them.");
        } else {
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 4));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 8));
            for (int favIdx = 0; favIdx < (int)favs.size(); favIdx++) {
                auto& fav = favs[favIdx];
                ImGui::PushID(fav.steamid.c_str());

                float cardH = 96.0f;
                ImGui::BeginChild("##card", ImVec2(childW - 8, cardH), true,
                    ImGuiWindowFlags_NoScrollbar);
                {
                    // Card content with centered padding
                    ImGui::SetCursorPos(ImVec2(10, 8));

                    // Row 1: Name + Value
                    std::string dispName = fav.name.empty() ? "Unknown" : fav.name;
                    if (dispName.size() > 28) dispName = dispName.substr(0, 26) + "â¦";
                    ImGui::TextColored(cWhite(0.95f), "%s", dispName.c_str());
                    if (!fav.value.empty()) {
                        float valW = ImGui::CalcTextSize(fav.value.c_str()).x;
                        ImGui::SameLine(ImGui::GetContentRegionAvail().x - valW);
                        ImVec4 valColor = (fav.value.find("PRIVATE") != std::string::npos)
                            ? cGray1(0.7f) : ImVec4(0.6f, 1.0f, 0.6f, 0.9f);
                        ImGui::TextColored(valColor, "%s", fav.value.c_str());
                    }

                    // Row 2: SteamID
                    ImGui::SetCursorPos(ImVec2(10, 34));
                    ImGui::TextColored(cGray2(0.5f), "ID: %s", fav.steamid.c_str());

                    // Delete X button — transparent, bottom-right corner
                    float bx = ImGui::GetContentRegionMax().x - 26.0f;
                    float by = ImGui::GetContentRegionMax().y - 26.0f;
                    ImGui::SetCursorPos(ImVec2(bx, by));
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.4f, 0.4f, 0.4f, 0.25f));
                    if (ImGui::Button("X", ImVec2(22, 22))) {
                        int deletedFavId = fav.favId;
                        std::string removedSteamid = fav.steamid;
                        // Remove from local list immediately — no wait for server
                        favs.erase(favs.begin() + favIdx);
                        std::thread([api, deletedFavId]() {
                            api->RemoveFavorite(deletedFavId);
                            std::vector<FavoriteEntry> tmp;
                            api->RefreshFavorites(tmp);
                            globals::favNeedRefresh = true;
                        }).detach();
                        noteBufs.erase(deletedFavId);
                        {
                            std::lock_guard<std::mutex> lock(globals::playersMutex);
                            for (auto& p : globals::matchPlayers)
                                if (p.steamid == removedSteamid) { p.isFavorite = false; break; }
                        }
                        ImGui::PopStyleColor(2);
                        ImGui::EndChild();
                        ImGui::PopID();
                        break;
                    }
                    ImGui::PopStyleColor(2);
                }
                ImGui::EndChild();
                ImGui::PopID();
            }
            ImGui::PopStyleVar(2); // FramePadding + ItemSpacing
        }

        ImGui::EndChild();
    }
    ImGui::End();

    ImGui::PopStyleVar(2);
}

// ─── DX11 Device ────────────────────────────────────────────────────────────

bool Overlay::CreateDeviceD3D() noexcept {
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = window_handle;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
        &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED) {
        Logger::logWarning("Render Type Changed to WARP");
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
            &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    }
    if (res != S_OK) return false;

    CreateRenderTarget();
    return true;
}

void Overlay::CleanupDeviceD3D() noexcept {
    CleanupRenderTarget();
    SafeRelease(g_pSwapChain);
    SafeRelease(g_pd3dDeviceContext);
    SafeRelease(g_pd3dDevice);
    delete main_font;
    delete title_font;
}

void Overlay::CreateRenderTarget() noexcept {
    ID3D11Texture2D* pBackBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    if (pBackBuffer) {
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    }
    if (pBackBuffer) pBackBuffer->Release();
}

void Overlay::CleanupRenderTarget() noexcept {
    SafeRelease(g_mainRenderTargetView);
}

template <typename T>
inline VOID Overlay::SafeRelease(T*& p) noexcept {
    if (nullptr != p) {
        p->Release();
        p = nullptr;
    }
}
