#ifndef ICONRENDERER_H
#define ICONRENDERER_H

#include <QIcon>
#include <QColor>

// ============================================================================
// IconRenderer — draws small, crisp, single-color vector icons on demand.
//
// Why this exists: the UI previously used emoji characters (🔋 📶 🗺 📍 ⚙ 🔴 …)
// as "icons". On Android devices/builds without a bundled emoji font, glyphs
// that the system font doesn't cover fall back to a generic "missing glyph"
// box that looks like a little keyboard/tofu square instead of the intended
// symbol. Drawing icons ourselves with QPainter sidesteps font/glyph
// availability entirely — same look on every device, no extra asset files,
// no font dependency — and matches the way CompassWidget/MiniMapWidget/
// ElevationLegendWidget already draw themselves.
// ============================================================================
namespace IconRenderer
{
enum class Id
{
    Battery,
    Signal,
    Map,
    Waypoint,
    Scan,
    Settings,
    Cube3D,
    TopView,
    Record,
    RecordStop,
    Expand,
    Restore,
    Close,
    Swap,
    Split,
    Trash,
    Camera,
    ChevronLeft,
    ChevronRight,
    ChevronDown,
    ZoomIn,
    ZoomOut,
    Plus,
    Minus,
    Drone,
    Bolt,
    Waveform,
};

// Renders the icon into a square QPixmap of `pixelSize` x `pixelSize`
// (device-pixel-ratio aware) using `color` as the stroke/fill color.
QIcon icon(Id id, const QColor &color, int pixelSize);

} // namespace IconRenderer

#endif // ICONRENDERER_H
