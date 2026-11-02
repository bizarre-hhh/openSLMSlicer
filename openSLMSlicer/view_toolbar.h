#ifndef VIEW_TOOLBAR_H
#define VIEW_TOOLBAR_H

#include <glm/vec2.hpp>

// A small floating button strip pinned to the top center of the scene view.
// It owns no graphics resources and no scene state: Draw() only reports the
// buttons the user pressed, so the viewport stays the single owner of camera
// and presentation state.
class ViewToolbar
{
public:
    // Buttons left to right. Public so tests and callers can address a button
    // without duplicating the order.
    enum ButtonIndex : int
    {
        kPlaceOnPlateButton = 0,
        kAxisMarkerButton = 1,
        kZoomInButton = 2,
        kZoomOutButton = 3,
    };
    static constexpr int kButtonCount = 4;

    struct Layout
    {
        glm::vec2 minimum{0.0f}; // screen-space top-left corner of the panel
        glm::vec2 size{0.0f};
        glm::vec2 button_size{0.0f};
        float padding = 0.0f;
        float spacing = 0.0f;
        float scale = 0.0f; // 0 disables drawing (viewport too small)
    };

    // View state the buttons need to render their pressed state.
    struct State
    {
        bool axis_markers_visible = true;
        // Grounding a selection that carries generated supports is blocked:
        // the Z move would invalidate them.
        bool can_place_on_plate = false;
        bool selection_has_supports = false;
    };

    // Buttons the user pressed since the previous Draw().
    struct Actions
    {
        bool place_on_plate = false;
        bool toggle_axis_markers = false;
        bool zoom_in = false;
        bool zoom_out = false;
    };

    // One click zooms two wheel notches: a decisive step that still follows
    // the wheel's exponential rate and its distance limits.
    static constexpr float kZoomWheelSteps = 2.0f;

    static Layout GetLayout(const glm::vec2& display_size, float dpi_scale);
    static bool Contains(const glm::vec2& point, const Layout& layout);
    // Screen-space top-left corner of one button, indexed from the left.
    static glm::vec2 ButtonMinimum(const Layout& layout, int index);
    static Actions Draw(const State& state, float dpi_scale);
};

#endif
