#include "Overlay.h"

FLOAT Overlay::RenderText(ImFont* font, const std::string& text, const ImVec2& position,
    const float size, const ImColor& color, const bool centerX, const bool centerY,
    const bool outline, const bool background) noexcept {
    ImGui::PushFont(font);
    ImGui::SetWindowFontScale(size / ImGui::GetFontSize());

    ImVec2 textSize = ImGui::CalcTextSize(text.c_str());
    ImVec2 renderPos = position;
    if (centerX) renderPos.x -= textSize.x / 2.0f;
    if (centerY) renderPos.y -= textSize.y / 2.0f;

    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    if (background) {
        const ImVec4 padding = ImVec4(2.f, .25f, 1.25f, -0.5f);
        ImVec2 bgMin = ImVec2(renderPos.x - padding.x, renderPos.y - padding.y);
        ImVec2 bgMax = ImVec2(renderPos.x + textSize.x + padding.z, renderPos.y + textSize.y + padding.w);
        draw_list->AddRectFilled(bgMin, bgMax, ImColor(0.f, 0.f, 0.f, color.Value.w * 0.5f));
    }

    if (outline) {
        draw_list->AddText({ renderPos.x + 1.f, renderPos.y }, ImColor(0.f, 0.f, 0.f, color.Value.w), text.c_str());
        draw_list->AddText({ renderPos.x - 1.f, renderPos.y }, ImColor(0.f, 0.f, 0.f, color.Value.w), text.c_str());
        draw_list->AddText({ renderPos.x, renderPos.y + 1.f }, ImColor(0.f, 0.f, 0.f, color.Value.w), text.c_str());
        draw_list->AddText({ renderPos.x, renderPos.y - 1.f }, ImColor(0.f, 0.f, 0.f, color.Value.w), text.c_str());
    }

    draw_list->AddText(renderPos, color, text.c_str());

    ImGui::SetWindowFontScale(1.f);
    ImGui::PopFont();
    return textSize.x;
}
