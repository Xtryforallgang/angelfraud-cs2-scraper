#pragma once

#pragma comment(lib,"d3d11.lib")
#pragma comment(lib,"dwmapi.lib")

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imgui/imgui_impl_win32.h"
#include "imgui/imgui_impl_dx11.h"

#include "Globals.h"

#include <dwmapi.h>
#include <d3d11.h>
#include <string>
#include <vector>

class Overlay {
public:
    void run();
    void SetPhpsessid(const std::string& sid) { phpsessid = sid; }

private:
    HWND window_handle = nullptr;
    ID3D11Device* g_pd3dDevice = nullptr;
    ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
    IDXGISwapChain* g_pSwapChain = nullptr;
    bool g_SwapChainOccluded = false;
    ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

    ImDrawList* draw_list = nullptr;
    ImFont* main_font = new ImFont();
    ImFont* title_font = new ImFont();

    bool isFinish = true;

    std::string phpsessid;

    bool showFavorites = false;

    template <typename T>
    inline VOID SafeRelease(T*& p) noexcept;

    bool CreateDeviceD3D() noexcept;
    void CleanupDeviceD3D() noexcept;
    void CreateRenderTarget() noexcept;
    void CleanupRenderTarget() noexcept;

    void Handler() noexcept;
    void Render() noexcept;
    void RenderPlayerList() noexcept;
    void RenderPlayerTable(const std::vector<MatchPlayer>& players) noexcept;
    void RenderFavoritesWindow() noexcept;
    void RenderLauncher() noexcept;

    FLOAT RenderText(ImFont* font, const std::string& text, const ImVec2& position,
        float size, const ImColor& color, bool centerX = false, bool centerY = false,
        bool outline = false, bool background = false) noexcept;

    void OverlayLoop() noexcept;
};
