// SPDX-License-Identifier: GPL-3.0-or-later
// KookaPictura-SHP fork: the SHP dock (Frames + Palette), frame stepping,
// and the save gate. The frame reaches it through three one-line hooks
// (install, retarget, refresh) listed in crates/pictura-shp/HOOKS.md.
#pragma once

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QString>

class QDockWidget;
class QMainWindow;
class QShortcut;

namespace pictura {

class ColorState;
class ImageView;
class PictureView;
class ShpFramesPanel;
class ShpPalettePanel;

// True for a path with the .shp suffix (any case).
bool isShpPath(const QString& path);

class ShpSupport : public QObject {
    Q_OBJECT

public:
    // Create the support object (a child of `frame`, found again by `of`).
    // The returned dock still has to be registered with the frame.
    static ShpSupport* install(QMainWindow* frame, ColorState* colors);
    static ShpSupport* of(const QObject* frame);

    QDockWidget* dock() const { return dock_; }
    ShpFramesPanel* frames() const { return frames_; }
    ShpPalettePanel* palette() const { return palette_; }

    // The active document changed: follow it, and show the dock when an
    // SHP becomes active.
    void setView(PictureView* view, ImageView* canvas);
    void refresh();

    // Before saving `view` as an SHP: report anything Save would refuse and
    // offer the fix. Off-palette pixels: [Snap to Palette and Save] /
    // [Show Me] / [Cancel]; gutter pixels: [Clear and Save] / [Cancel]. The
    // fixes are recorded edits, so what is saved is what is on screen.
    // False when the save must not go ahead. Without an interactive
    // platform (offscreen, control server) nothing is fixed silently: the
    // save is refused and the reason is in `lastGateMessage()`.
    bool saveGate(PictureView* view);
    QString lastGateMessage() const { return gateMessage_; }

    // Test seam: force the gate's dialogs on (or off) whatever the platform.
    static void setGateInteractive(int interactive);

private:
    ShpSupport(QMainWindow* frame, ColorState* colors);
    void message(const QString& text);
    bool interactive() const;
    void showOffPalette(PictureView* view);

    QMainWindow* frame_ = nullptr;
    QDockWidget* dock_ = nullptr;
    ShpFramesPanel* frames_ = nullptr;
    ShpPalettePanel* palette_ = nullptr;
    QShortcut* prevKey_ = nullptr;
    QShortcut* nextKey_ = nullptr;
    QPointer<PictureView> view_;
    bool wasShp_ = false;
    QString gateMessage_;
};

} // namespace pictura
