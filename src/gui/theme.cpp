#include "peersync/theme.h"
#include <imgui.h>

namespace peersync {
namespace theme {

static void SetCommonRounding(ImGuiStyle& style) {
    style.WindowPadding = ImVec2(16.0f, 16.0f);
    style.FramePadding = ImVec2(12.0f, 8.0f);
    style.CellPadding = ImVec2(12.0f, 8.0f);
    style.ItemSpacing = ImVec2(12.0f, 10.0f);
    style.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
    style.ScrollbarSize = 14.0f;
    style.GrabMinSize = 12.0f;
    
    style.WindowRounding = 8.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 6.0f;
    style.PopupRounding = 6.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 6.0f;
    
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.PopupBorderSize = 1.0f;
}

void ApplyDarkTheme() {
    ImGuiStyle& style = ImGui::GetStyle();
    SetCommonRounding(style);
    
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text]                   = ImVec4(0.95f, 0.96f, 0.98f, 1.00f);
    colors[ImGuiCol_TextDisabled]           = ImVec4(0.50f, 0.55f, 0.60f, 1.00f);
    colors[ImGuiCol_WindowBg]               = ImVec4(0.08f, 0.09f, 0.11f, 1.00f);
    colors[ImGuiCol_ChildBg]                = ImVec4(0.18f, 0.20f, 0.24f, 1.00f);
    colors[ImGuiCol_PopupBg]                = ImVec4(0.15f, 0.16f, 0.19f, 0.98f);
    colors[ImGuiCol_Border]                 = ImVec4(0.24f, 0.26f, 0.30f, 1.00f);
    colors[ImGuiCol_BorderShadow]           = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg]                = ImVec4(0.20f, 0.22f, 0.26f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]         = ImVec4(0.26f, 0.29f, 0.34f, 1.00f);
    colors[ImGuiCol_FrameBgActive]          = ImVec4(0.30f, 0.34f, 0.40f, 1.00f);
    colors[ImGuiCol_TitleBg]                = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);
    colors[ImGuiCol_TitleBgActive]          = ImVec4(0.14f, 0.16f, 0.19f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed]       = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);
    colors[ImGuiCol_MenuBarBg]              = ImVec4(0.14f, 0.16f, 0.19f, 1.00f);
    colors[ImGuiCol_ScrollbarBg]            = ImVec4(0.10f, 0.11f, 0.13f, 0.60f);
    colors[ImGuiCol_ScrollbarGrab]          = ImVec4(0.26f, 0.29f, 0.34f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered]   = ImVec4(0.32f, 0.36f, 0.42f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]    = ImVec4(0.38f, 0.42f, 0.50f, 1.00f);
    colors[ImGuiCol_CheckMark]              = ImVec4(0.24f, 0.64f, 0.78f, 1.00f);
    colors[ImGuiCol_SliderGrab]             = ImVec4(0.24f, 0.64f, 0.78f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]       = ImVec4(0.31f, 0.73f, 0.88f, 1.00f);
    colors[ImGuiCol_Button]                 = ImVec4(0.20f, 0.50f, 0.62f, 1.00f);
    colors[ImGuiCol_ButtonHovered]          = ImVec4(0.25f, 0.60f, 0.74f, 1.00f);
    colors[ImGuiCol_ButtonActive]           = ImVec4(0.16f, 0.42f, 0.52f, 1.00f);
    colors[ImGuiCol_Header]                 = ImVec4(0.20f, 0.22f, 0.26f, 1.00f);
    colors[ImGuiCol_HeaderHovered]          = ImVec4(0.26f, 0.29f, 0.34f, 1.00f);
    colors[ImGuiCol_HeaderActive]           = ImVec4(0.30f, 0.34f, 0.40f, 1.00f);
    colors[ImGuiCol_Separator]              = ImVec4(0.24f, 0.26f, 0.30f, 1.00f);
    colors[ImGuiCol_SeparatorHovered]       = ImVec4(0.24f, 0.64f, 0.78f, 1.00f);
    colors[ImGuiCol_SeparatorActive]        = ImVec4(0.31f, 0.73f, 0.88f, 1.00f);
    colors[ImGuiCol_ResizeGrip]             = ImVec4(0.24f, 0.64f, 0.78f, 0.25f);
    colors[ImGuiCol_ResizeGripHovered]      = ImVec4(0.24f, 0.64f, 0.78f, 0.67f);
    colors[ImGuiCol_ResizeGripActive]       = ImVec4(0.24f, 0.64f, 0.78f, 0.95f);
    colors[ImGuiCol_Tab]                    = ImVec4(0.15f, 0.16f, 0.19f, 1.00f);
    colors[ImGuiCol_TabHovered]             = ImVec4(0.24f, 0.64f, 0.78f, 0.80f);
    colors[ImGuiCol_TabActive]              = ImVec4(0.20f, 0.50f, 0.62f, 1.00f);
    colors[ImGuiCol_TabUnfocused]           = ImVec4(0.15f, 0.16f, 0.19f, 1.00f);
    colors[ImGuiCol_TabUnfocusedActive]     = ImVec4(0.20f, 0.22f, 0.26f, 1.00f);
    colors[ImGuiCol_TableBorderStrong]      = ImVec4(0.24f, 0.26f, 0.30f, 1.00f);
    colors[ImGuiCol_TableBorderLight]       = ImVec4(0.20f, 0.22f, 0.25f, 1.00f);
    colors[ImGuiCol_TableRowBg]             = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_TableRowBgAlt]          = ImVec4(1.00f, 1.00f, 1.00f, 0.03f);
    colors[ImGuiCol_TextSelectedBg]         = ImVec4(0.24f, 0.64f, 0.78f, 0.35f);
    colors[ImGuiCol_DragDropTarget]         = ImVec4(0.95f, 0.80f, 0.00f, 0.90f);
    colors[ImGuiCol_NavHighlight]           = ImVec4(0.24f, 0.64f, 0.78f, 1.00f);
    colors[ImGuiCol_NavWindowingHighlight]  = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    colors[ImGuiCol_NavWindowingDimBg]      = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
    colors[ImGuiCol_ModalWindowDimBg]       = ImVec4(0.00f, 0.00f, 0.00f, 0.60f);
}

void ApplyLightTheme() {
    ImGuiStyle& style = ImGui::GetStyle();
    SetCommonRounding(style);
    
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text]                   = ImVec4(0.12f, 0.13f, 0.15f, 1.00f);
    colors[ImGuiCol_TextDisabled]           = ImVec4(0.55f, 0.60f, 0.65f, 1.00f);
    colors[ImGuiCol_WindowBg]               = ImVec4(0.93f, 0.94f, 0.96f, 1.00f); // Light neutral off-white
    colors[ImGuiCol_ChildBg]                = ImVec4(1.00f, 1.00f, 1.00f, 1.00f); // Pure white for distinct cards
    colors[ImGuiCol_PopupBg]                = ImVec4(0.98f, 0.98f, 1.00f, 0.98f);
    colors[ImGuiCol_Border]                 = ImVec4(0.80f, 0.82f, 0.86f, 1.00f);
    colors[ImGuiCol_BorderShadow]           = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg]                = ImVec4(0.88f, 0.90f, 0.93f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]         = ImVec4(0.83f, 0.86f, 0.90f, 1.00f);
    colors[ImGuiCol_FrameBgActive]          = ImVec4(0.78f, 0.82f, 0.86f, 1.00f);
    colors[ImGuiCol_TitleBg]                = ImVec4(0.88f, 0.90f, 0.93f, 1.00f);
    colors[ImGuiCol_TitleBgActive]          = ImVec4(0.82f, 0.85f, 0.88f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed]       = ImVec4(0.90f, 0.92f, 0.95f, 1.00f);
    colors[ImGuiCol_MenuBarBg]              = ImVec4(0.93f, 0.94f, 0.96f, 1.00f);
    colors[ImGuiCol_ScrollbarBg]            = ImVec4(0.90f, 0.92f, 0.95f, 0.60f);
    colors[ImGuiCol_ScrollbarGrab]          = ImVec4(0.70f, 0.74f, 0.80f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered]   = ImVec4(0.60f, 0.65f, 0.72f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]    = ImVec4(0.50f, 0.56f, 0.64f, 1.00f);
    
    // Darker, more saturated teal accent for good contrast on white
    ImVec4 accentColor                      = ImVec4(0.08f, 0.45f, 0.55f, 1.00f); 
    ImVec4 accentHovered                    = ImVec4(0.12f, 0.52f, 0.62f, 1.00f);
    ImVec4 accentActive                     = ImVec4(0.06f, 0.38f, 0.46f, 1.00f);
    
    colors[ImGuiCol_CheckMark]              = accentColor;
    colors[ImGuiCol_SliderGrab]             = accentColor;
    colors[ImGuiCol_SliderGrabActive]       = accentActive;
    colors[ImGuiCol_Button]                 = ImVec4(0.82f, 0.85f, 0.89f, 1.00f);
    colors[ImGuiCol_ButtonHovered]          = ImVec4(0.75f, 0.79f, 0.84f, 1.00f);
    colors[ImGuiCol_ButtonActive]           = ImVec4(0.68f, 0.73f, 0.79f, 1.00f);
    
    // Main CTA buttons often use accent color in custom widgets, but default buttons are grey in this theme. 
    // We can also colorize headers (tables/lists):
    colors[ImGuiCol_Header]                 = ImVec4(0.85f, 0.88f, 0.92f, 1.00f);
    colors[ImGuiCol_HeaderHovered]          = ImVec4(0.78f, 0.82f, 0.87f, 1.00f);
    colors[ImGuiCol_HeaderActive]           = ImVec4(0.70f, 0.75f, 0.81f, 1.00f);
    colors[ImGuiCol_Separator]              = ImVec4(0.80f, 0.82f, 0.86f, 1.00f);
    colors[ImGuiCol_SeparatorHovered]       = accentColor;
    colors[ImGuiCol_SeparatorActive]        = accentActive;
    colors[ImGuiCol_ResizeGrip]             = ImVec4(0.08f, 0.45f, 0.55f, 0.25f);
    colors[ImGuiCol_ResizeGripHovered]      = ImVec4(0.08f, 0.45f, 0.55f, 0.67f);
    colors[ImGuiCol_ResizeGripActive]       = ImVec4(0.08f, 0.45f, 0.55f, 0.95f);
    
    colors[ImGuiCol_Tab]                    = ImVec4(0.88f, 0.90f, 0.93f, 1.00f);
    colors[ImGuiCol_TabHovered]             = ImVec4(0.80f, 0.83f, 0.87f, 1.00f);
    colors[ImGuiCol_TabActive]              = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    colors[ImGuiCol_TabUnfocused]           = ImVec4(0.90f, 0.92f, 0.95f, 1.00f);
    colors[ImGuiCol_TabUnfocusedActive]     = ImVec4(0.95f, 0.96f, 0.98f, 1.00f);
    colors[ImGuiCol_TableBorderStrong]      = ImVec4(0.75f, 0.78f, 0.83f, 1.00f);
    colors[ImGuiCol_TableBorderLight]       = ImVec4(0.82f, 0.85f, 0.89f, 1.00f);
    colors[ImGuiCol_TableRowBg]             = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_TableRowBgAlt]          = ImVec4(0.00f, 0.00f, 0.00f, 0.03f);
    colors[ImGuiCol_TextSelectedBg]         = ImVec4(0.08f, 0.45f, 0.55f, 0.35f);
    colors[ImGuiCol_DragDropTarget]         = ImVec4(0.95f, 0.60f, 0.00f, 0.90f);
    colors[ImGuiCol_NavHighlight]           = accentColor;
    colors[ImGuiCol_NavWindowingHighlight]  = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    colors[ImGuiCol_NavWindowingDimBg]      = ImVec4(0.20f, 0.20f, 0.20f, 0.20f);
    colors[ImGuiCol_ModalWindowDimBg]       = ImVec4(0.20f, 0.20f, 0.20f, 0.35f);
}

} // namespace theme
} // namespace peersync
