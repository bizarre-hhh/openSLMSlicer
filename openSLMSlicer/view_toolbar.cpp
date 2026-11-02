#include "view_toolbar.h"

#include <algorithm>
#include <array>
#include <cstddef>

#include "imgui.h"

namespace
{
// Unscaled design sizes; GetLayout() multiplies them by the DPI/viewport scale.
constexpr float kButtonWidth = 38.0f;
constexpr float kButtonHeight = 36.0f;
constexpr float kPadding = 5.0f;
constexpr float kSpacing = 6.0f;
constexpr float kTopMargin = 12.0f;

enum class ButtonKind
{
    PlaceOnPlate,
    AxisMarkers,
    ZoomIn,
    ZoomOut,
};

// One entry per button, left to right. Adding a view button means adding an
// entry here plus its icon/action branch in the draw loop below.
struct ButtonDefinition
{
    const char* id;
    ButtonKind kind;
};

constexpr std::array<ButtonDefinition, ViewToolbar::kButtonCount> kButtons = {{
    {"place_on_plate", ButtonKind::PlaceOnPlate},
    {"axis_markers", ButtonKind::AxisMarkers},
    {"zoom_in", ButtonKind::ZoomIn},
    {"zoom_out", ButtonKind::ZoomOut},
}};

constexpr ImU32 kIconColor = IM_COL32(42, 52, 66, 255);
constexpr ImU32 kIconHoverColor = IM_COL32(16, 24, 38, 255);
constexpr ImU32 kIconDisabledColor = IM_COL32(150, 158, 168, 255);

ImVec2 ToImVec(const glm::vec2& point)
{
    return ImVec2(point.x, point.y);
}

// A model hovering above the plate with an arrow down to the plate surface,
// mirroring the drop-to-plate icon of the Qt toolbar.
void DrawPlaceOnPlateIcon(ImDrawList* draw_list, const ImVec2& center, float scale,
                          ImU32 color)
{
    const float thickness = 1.75f * scale;
    draw_list->AddRect(ImVec2(center.x - 5.6f * scale, center.y - 7.2f * scale),
                       ImVec2(center.x + 5.6f * scale, center.y - 1.6f * scale),
                       color, 1.2f * scale, 0, thickness);
    const ImVec2 arrow_tip(center.x, center.y + 4.6f * scale);
    draw_list->AddLine(ImVec2(center.x, center.y - 0.6f * scale), arrow_tip,
                       color, thickness);
    draw_list->AddLine(ImVec2(center.x - 2.6f * scale, center.y + 2.0f * scale),
                       arrow_tip, color, thickness);
    draw_list->AddLine(ImVec2(center.x + 2.6f * scale, center.y + 2.0f * scale),
                       arrow_tip, color, thickness);
    draw_list->AddLine(ImVec2(center.x - 7.2f * scale, center.y + 7.2f * scale),
                       ImVec2(center.x + 7.2f * scale, center.y + 7.2f * scale),
                       color, thickness);
}

// A magnifying glass with a plus or minus sign in the lens. The icons are
// drawn with ImDrawList because the embedded ImGui font has no zoom glyphs;
// the proportions follow the 24-unit grid and stroke weight of the toolbar's
// SVG icons.
void DrawZoomIcon(ImDrawList* draw_list, const ImVec2& center, float scale,
                  bool zoom_in, ImU32 color)
{
    const float radius = 7.0f * scale;
    const float thickness = 1.75f * scale;
    const float lens_offset = 1.4f * scale;
    const ImVec2 lens(center.x - lens_offset, center.y - lens_offset);
    draw_list->AddCircle(lens, radius, color, 0, thickness);

    // The handle starts on the ring, at the 45 degree point, and runs outward.
    constexpr float diagonal = 0.70710678f;
    const float handle_start = radius * diagonal;
    const float handle_end = (radius + 3.75f * scale) * diagonal;
    draw_list->AddLine(ImVec2(lens.x + handle_start, lens.y + handle_start),
                       ImVec2(lens.x + handle_end, lens.y + handle_end),
                       color, thickness);

    const float arm = radius * 0.375f;
    draw_list->AddLine(ImVec2(lens.x - arm, lens.y), ImVec2(lens.x + arm, lens.y),
                       color, thickness);
    if (zoom_in)
        draw_list->AddLine(ImVec2(lens.x, lens.y - arm), ImVec2(lens.x, lens.y + arm),
                           color, thickness);
}

// A square marker plate seen head-on: a rim around a smaller face, matching
// the squares at the platform's maximum X, Y and Z coordinates. A diagonal
// strike marks the hidden state so the toggle reads at a glance.
void DrawAxisMarkerIcon(ImDrawList* draw_list, const ImVec2& center, float scale,
                        bool visible, ImU32 color)
{
    const float outer = 14.0f * scale;
    const float inner = 6.5f * scale;
    const float thickness = 1.75f * scale;
    draw_list->AddRect(ImVec2(center.x - outer * 0.5f, center.y - outer * 0.5f),
                       ImVec2(center.x + outer * 0.5f, center.y + outer * 0.5f),
                       color, 1.5f * scale, 0, thickness);
    draw_list->AddRect(ImVec2(center.x - inner * 0.5f, center.y - inner * 0.5f),
                       ImVec2(center.x + inner * 0.5f, center.y + inner * 0.5f),
                       color, 1.0f * scale, 0, thickness);
    if (!visible)
        draw_list->AddLine(ImVec2(center.x - outer * 0.58f, center.y + outer * 0.58f),
                           ImVec2(center.x + outer * 0.58f, center.y - outer * 0.58f),
                           color, thickness);
}
} // namespace

ViewToolbar::Layout ViewToolbar::GetLayout(const glm::vec2& display_size, float dpi_scale)
{
    Layout layout;
    // Shrink the strip with the viewport so it never covers a small scene.
    layout.scale = std::min({dpi_scale, display_size.x / 400.0f,
                             display_size.y / 160.0f});
    if (layout.scale <= 0.0f)
        return layout;
    layout.button_size = glm::vec2(kButtonWidth, kButtonHeight) * layout.scale;
    layout.padding = kPadding * layout.scale;
    layout.spacing = kSpacing * layout.scale;
    layout.size = glm::vec2(kButtonWidth * kButtonCount +
                                kSpacing * (kButtonCount - 1) + kPadding * 2.0f,
                            kButtonHeight + kPadding * 2.0f) * layout.scale;
    // Center the strip horizontally on the top edge of the viewport.
    layout.minimum = glm::vec2((display_size.x - layout.size.x) * 0.5f,
                               kTopMargin * layout.scale);
    return layout;
}

bool ViewToolbar::Contains(const glm::vec2& point, const Layout& layout)
{
    return layout.scale > 0.0f && point.x >= layout.minimum.x && point.y >= layout.minimum.y &&
           point.x < layout.minimum.x + layout.size.x && point.y < layout.minimum.y + layout.size.y;
}

glm::vec2 ViewToolbar::ButtonMinimum(const Layout& layout, int index)
{
    return layout.minimum +
        glm::vec2(layout.padding + index * (layout.button_size.x + layout.spacing),
                  layout.padding);
}

ViewToolbar::Actions ViewToolbar::Draw(const State& state, float dpi_scale)
{
    Actions actions;
    const ImGuiIO& io = ImGui::GetIO();
    const Layout layout = GetLayout(glm::vec2(io.DisplaySize.x, io.DisplaySize.y), dpi_scale);
    if (layout.scale <= 0.0f)
        return actions;

    // Same light panel style as the orientation cube in the opposite corner.
    ImGui::SetNextWindowPos(ToImVec(layout.minimum), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ToImVec(layout.size), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 9.0f * layout.scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.97f, 0.98f, 0.99f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.70f, 0.75f, 0.82f, 0.85f));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoFocusOnAppearing;
    ImGui::Begin("View Controls##view_toolbar", nullptr, flags);

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    for (int index = 0; index < kButtonCount; ++index)
    {
        const ButtonDefinition& button = kButtons[static_cast<std::size_t>(index)];
        ImGui::SetCursorScreenPos(ToImVec(ButtonMinimum(layout, index)));
        ImGui::PushID(button.id);
        const bool pressed = ImGui::InvisibleButton("##button", ToImVec(layout.button_size));
        // Grounding the selection is blocked while supports exist and would be
        // invalidated by the Z move.
        const bool enabled = button.kind != ButtonKind::PlaceOnPlate ||
                             state.can_place_on_plate;
        if (pressed && enabled)
        {
            switch (button.kind)
            {
            case ButtonKind::PlaceOnPlate: actions.place_on_plate = true; break;
            case ButtonKind::AxisMarkers: actions.toggle_axis_markers = true; break;
            case ButtonKind::ZoomIn: actions.zoom_in = true; break;
            case ButtonKind::ZoomOut: actions.zoom_out = true; break;
            }
        }

        const bool hovered = ImGui::IsItemHovered();
        const bool active = ImGui::IsItemActive() && enabled;
        // Toggle buttons keep a pressed-looking fill while their state is on.
        const bool toggled_on =
            button.kind == ButtonKind::AxisMarkers && state.axis_markers_visible;
        const ImVec2 minimum = ImGui::GetItemRectMin();
        const ImVec2 maximum = ImGui::GetItemRectMax();
        if ((hovered && enabled) || active || toggled_on)
        {
            const ImU32 background = ImGui::GetColorU32(
                active ? ImVec4(0.61f, 0.74f, 0.92f, 1.0f)
                       : hovered ? ImVec4(0.72f, 0.82f, 0.96f, 1.0f)
                                 : ImVec4(0.84f, 0.89f, 0.96f, 1.0f));
            const ImVec2 inset(layout.scale, layout.scale);
            draw_list->AddRectFilled(ImVec2(minimum.x + inset.x, minimum.y + inset.y),
                                     ImVec2(maximum.x - inset.x, maximum.y - inset.y),
                                     background, 6.0f * layout.scale);
        }
        const bool muted = !enabled ||
            (button.kind == ButtonKind::AxisMarkers && !state.axis_markers_visible);
        const ImU32 icon_color = muted ? kIconDisabledColor
                                      : (hovered || active) ? kIconHoverColor
                                                            : kIconColor;
        const ImVec2 center((minimum.x + maximum.x) * 0.5f,
                            (minimum.y + maximum.y) * 0.5f);
        switch (button.kind)
        {
        case ButtonKind::PlaceOnPlate:
            DrawPlaceOnPlateIcon(draw_list, center, layout.scale, icon_color);
            break;
        case ButtonKind::AxisMarkers:
            DrawAxisMarkerIcon(draw_list, center, layout.scale,
                               state.axis_markers_visible, icon_color);
            break;
        case ButtonKind::ZoomIn:
        case ButtonKind::ZoomOut:
            DrawZoomIcon(draw_list, center, layout.scale,
                         button.kind == ButtonKind::ZoomIn, icon_color);
            break;
        }
        if (hovered)
        {
            if (enabled)
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            switch (button.kind)
            {
            case ButtonKind::PlaceOnPlate:
                ImGui::SetTooltip(state.can_place_on_plate ? "Place on plate"
                    : state.selection_has_supports
                        ? "Place on plate - selected model has supports"
                        : "Place on plate - select a model");
                break;
            case ButtonKind::AxisMarkers:
                ImGui::SetTooltip(state.axis_markers_visible ? "Hide axis markers"
                                                             : "Show axis markers");
                break;
            case ButtonKind::ZoomIn:
                ImGui::SetTooltip("Zoom in");
                break;
            case ButtonKind::ZoomOut:
                ImGui::SetTooltip("Zoom out");
                break;
            }
        }
        ImGui::PopID();
    }

    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
    return actions;
}
