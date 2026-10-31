// =============================================================================
//  ScrollBar.h
//  Slim, overlay style scrollbar shared by the page canvas and the sidebar.
//
//  The bar is drawn on top of the content instead of reserving layout space,
//  which keeps the reader looking modern and maximises the visible page area.
//
//  The class is deliberately windowless: it owns only the scroll geometry, the
//  drag state and the offset/position mapping.  The hosting control drives it -
//  it queries the hit tests, captures the mouse, calls DragTo and repaints.
// =============================================================================
#pragma once

#include <windows.h>

namespace pengpdf {

// Axis on which the bar is laid out.  A vertical bar occupies a strip along the
// right edge of the supplied area, a horizontal bar a strip along its bottom
// edge (see TrackRect).
enum class ScrollOrientation {
    Vertical,
    Horizontal,
};

class ScrollBar {
public:
    // Updates the DPI used for every scaled thickness and minimum length.
    void SetDpi(UINT dpi);
    // Pushes the current viewport/content sizes and scroll offset.  Lengths are
    // in physical pixels along the orientation axis; the offset is clamped to
    // [0, contentLength - viewportLength] so the thumb can never leave the
    // track.  Hosts call this before painting and before hit testing.
    void Update(ScrollOrientation orientation, int viewportLength, int contentLength,
                int offset);

    // True when the content is longer than the viewport and there is something
    // to scroll.  Nothing is drawn or hit tested while this is false.
    bool IsVisible() const;
    // Nominal thickness of the bar in px.  Painting derives the thumb width from
    // the hit strip below, so this value is informational for host layout.
    int VisualThickness() const;
    // Width of the clickable edge strip in px, slightly wider than the painted
    // thumb for easy grabbing; hosts also reserve it out of their content width.
    int HitThickness() const;

    // Track rectangle inside the client area (right or bottom edge strip).
    // `area` is in physical client coordinates of the hosting control.
    RECT TrackRect(const RECT& area) const;
    // Painted thumb within the track.  Its length is proportional to the
    // viewport/content ratio with a minimum length so the thumb stays usable.
    RECT ThumbRect(const RECT& area) const;

    // Draws only the thumb, inset from the track edges; the track itself is
    // transparent.  `hovered` is combined with the internal drag state to pick
    // the darker thumb colour, so hosts can just pass their pointer-over flag.
    void Paint(HDC dc, const RECT& area, bool hovered) const;

    // Hit testing uses half-open intervals and requires the point to be inside
    // the (invisible) track strip first, so clicks elsewhere fall through to
    // the content underneath.
    bool HitTestTrack(const RECT& area, POINT point) const;
    bool HitTestThumb(const RECT& area, POINT point) const;

    // Begins a thumb drag when the point is on the thumb.  The grab offset is
    // remembered so the thumb does not jump under the cursor.
    // Returns false when the point is on the track but not on the thumb; the
    // caller may then implement page-up/page-down behaviour.
    bool BeginDrag(const RECT& area, POINT point);
    // Maps a pointer position to a new scroll offset while dragging.  Returns
    // false if no drag is active.  The host applies/appends `newOffset` itself.
    bool DragTo(const RECT& area, POINT point, int& newOffset);
    void EndDrag();
    bool IsDragging() const { return m_dragging; }

private:
    // Extent of the track along the orientation axis (height for vertical).
    int TrackLength(const RECT& area) const;
    // Thumb extent along the orientation axis, clamped to the track.
    int ThumbLength(const RECT& area) const;

    ScrollOrientation m_orientation = ScrollOrientation::Vertical;
    UINT m_dpi = 96;
    // Client sizes captured by Update; 0 while no content is available.
    int m_viewportLength = 0;
    int m_contentLength = 0;
    int m_offset = 0;
    bool m_dragging = false;
    // Distance from the top/left of the thumb to the grab point, in pixels.
    int m_grabOffset = 0;
};

}  // namespace pengpdf
