// =============================================================================
//  ScrollBar.cpp
//  Windowless overlay scrollbar: geometry, offset mapping and thumb dragging.
// =============================================================================
#include "Ui/ScrollBar.h"

#include <algorithm>

#include "Ui/Theme.h"

namespace pengpdf {
namespace {

// Shortest thumb that is still comfortable to grab, in DIPs.  Long documents
// would otherwise produce a thumb only a few pixels tall.
constexpr int kMinThumbDip = 28;
// Inset of the painted thumb from the track edges, in DIPs; the track itself is
// never painted, so the inset is what keeps the bar looking slim.
constexpr int kThumbInsetDip = 2;

}  // namespace

void ScrollBar::SetDpi(UINT dpi) {
    // Only affects derived metrics (thickness, minimum thumb); the raw scroll
    // lengths stay in physical pixels supplied by the host.
    m_dpi = dpi;
}

void ScrollBar::Update(ScrollOrientation orientation, int viewportLength,
                       int contentLength, int offset) {
    // Defensive clamps keep hit testing and thumb math valid even if a host
    // passes a negative size during teardown or the first layout pass.
    m_orientation = orientation;
    m_viewportLength = std::max(0, viewportLength);
    m_contentLength = std::max(0, contentLength);
    // Store the clamped offset rather than the raw one so ThumbRect and hit
    // testing always agree with the offset the host is about to paint with.
    m_offset = std::clamp(offset, 0, std::max(0, m_contentLength - m_viewportLength));
}

bool ScrollBar::IsVisible() const {
    // A zero-height viewport has no meaningful track either, so it counts as
    // hidden even when content exists.
    return m_contentLength > m_viewportLength && m_viewportLength > 0;
}

int ScrollBar::VisualThickness() const {
    // Nominal 7 DIP thumb width.  Paint() insets the thumb inside the wider hit
    // strip instead of consuming this value, so the two are intentionally close
    // but not identical.
    return theme::Scale(7, m_dpi);
}

int ScrollBar::HitThickness() const {
    // Wider than the painted bar so the pointer does not have to hit the exact
    // thumb pixels; hosts also use this value to keep row text clear of the
    // overlay strip.
    return theme::Scale(13, m_dpi);
}

int ScrollBar::TrackLength(const RECT& area) const {
    // The track spans the whole area on the scrolling axis; the perpendicular
    // thickness is handled by TrackRect.
    if (m_orientation == ScrollOrientation::Vertical) {
        return std::max(0, static_cast<int>(area.bottom - area.top));
    }
    return std::max(0, static_cast<int>(area.right - area.left));
}

int ScrollBar::ThumbLength(const RECT& area) const {
    const int track = TrackLength(area);
    if (m_contentLength <= 0) {
        return track;
    }
    const int minimum = theme::Scale(kMinThumbDip, m_dpi);
    // Proportional thumb size: viewport/content of the track.  The 64 bit
    // intermediate avoids overflowing when a host presents very long content.
    const int proportional =
        static_cast<int>(static_cast<long long>(track) * m_viewportLength / m_contentLength);
    // Never shorter than the minimum nor longer than the track itself; when
    // the track is shorter than kMinThumbDip the minimum is lowered to fit.
    return std::clamp(proportional, std::min(minimum, track), track);
}

RECT ScrollBar::TrackRect(const RECT& area) const {
    // Overlay model: the track is an edge strip inside the caller's area, never
    // a separate layout column.  The strip uses HitThickness (the wider touch
    // target), while the painted thumb is narrower.
    RECT track = area;
    if (m_orientation == ScrollOrientation::Vertical) {
        track.left = area.right - HitThickness();
    } else {
        track.top = area.bottom - HitThickness();
    }
    return track;
}

RECT ScrollBar::ThumbRect(const RECT& area) const {
    const int trackLength = TrackLength(area);
    const int thumbLength = ThumbLength(area);
    const int maxOffset = std::max(0, m_contentLength - m_viewportLength);
    int position = 0;
    // Linear mapping of the offset onto the free movement span of the track.
    // When the offset is at maxOffset the thumb sits exactly at the far end.
    if (maxOffset > 0 && trackLength > thumbLength) {
        position = static_cast<int>(static_cast<long long>(m_offset) *
                                    (trackLength - thumbLength) / maxOffset);
    }

    const RECT track = TrackRect(area);
    RECT thumb = track;
    if (m_orientation == ScrollOrientation::Vertical) {
        thumb.top = track.top + position;
        thumb.bottom = thumb.top + thumbLength;
    } else {
        thumb.left = track.left + position;
        thumb.right = thumb.left + thumbLength;
    }
    return thumb;
}

void ScrollBar::Paint(HDC dc, const RECT& area, bool hovered) const {
    if (!IsVisible()) {
        return;
    }

    const theme::Palette& colors = theme::Colors();
    const int inset = theme::Scale(kThumbInsetDip, m_dpi);
    RECT thumb = ThumbRect(area);

    // Leave the track invisible: only the thumb is painted, which keeps the
    // page area clean when the user is not scrolling.
    if (m_orientation == ScrollOrientation::Vertical) {
        thumb.left += inset;
        thumb.right = area.right - inset;
    } else {
        thumb.top += inset;
        thumb.bottom = area.bottom - inset;
    }

    // Dragging always uses the hover colour so the grabbed thumb stays visible
    // even if the pointer briefly leaves the window.
    const COLORREF color =
        (hovered || m_dragging) ? colors.scrollbarThumbHover : colors.scrollbarThumb;
    theme::FillRound(dc, thumb, theme::Scale(3, m_dpi), color);
}

bool ScrollBar::HitTestTrack(const RECT& area, POINT point) const {
    if (!IsVisible()) {
        return false;
    }
    const RECT track = TrackRect(area);
    // Half-open bounds match the rest of the UI's hit testing, so adjacent rows
    // never both claim the same pixel.
    return point.x >= track.left && point.x < track.right && point.y >= track.top &&
           point.y < track.bottom;
}

bool ScrollBar::HitTestThumb(const RECT& area, POINT point) const {
    // Track test first: a point outside the edge strip is never a thumb hit,
    // even if the thumb rectangle overlaps it geometrically.
    if (!HitTestTrack(area, point)) {
        return false;
    }
    const RECT thumb = ThumbRect(area);
    if (m_orientation == ScrollOrientation::Vertical) {
        return point.y >= thumb.top && point.y < thumb.bottom;
    }
    return point.x >= thumb.left && point.x < thumb.right;
}

bool ScrollBar::BeginDrag(const RECT& area, POINT point) {
    if (!HitTestThumb(area, point)) {
        return false;
    }
    // Remember where inside the thumb the user grabbed; DragTo subtracts this
    // offset so the thumb follows the pointer without snapping its top edge to
    // the cursor.
    const RECT thumb = ThumbRect(area);
    m_grabOffset = (m_orientation == ScrollOrientation::Vertical)
                       ? point.y - thumb.top
                       : point.x - thumb.left;
    m_dragging = true;
    return true;
}

bool ScrollBar::DragTo(const RECT& area, POINT point, int& newOffset) {
    if (!m_dragging) {
        return false;
    }

    const int trackLength = TrackLength(area);
    const int thumbLength = ThumbLength(area);
    const int maxOffset = std::max(0, m_contentLength - m_viewportLength);
    // Distance the thumb can travel; zero or negative means the whole track is
    // thumb (or the bar should be hidden), handled by the guards below.
    const int span = trackLength - thumbLength;

    const RECT track = TrackRect(area);
    // Desired thumb position in track coordinates, compensating the grab point.
    int thumbPosition =
        ((m_orientation == ScrollOrientation::Vertical) ? point.y - track.top
                                                        : point.x - track.left) -
        m_grabOffset;
    if (span > 0) {
        thumbPosition = std::clamp(thumbPosition, 0, span);
    } else {
        thumbPosition = 0;
    }

    // Inverse of the mapping in ThumbRect.  Integer division can round, but the
    // error is at most one pixel and the next Update() re-clamps the value.
    newOffset = (span > 0 && maxOffset > 0)
                    ? static_cast<int>(static_cast<long long>(thumbPosition) * maxOffset / span)
                    : 0;
    return true;
}

void ScrollBar::EndDrag() {
    // Kept idempotent: WM_CAPTURECHANGED and WM_LBUTTONUP can both call it.
    m_dragging = false;
}

}  // namespace pengpdf
