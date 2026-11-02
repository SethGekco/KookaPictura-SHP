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
class QWidget;

namespace pictura {

class ColorState;
class ImageView;
class PictureView;
class ShpFramesPanel;
class ShpPalettePanel;
struct ShpPlan;

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
    // save is refused and the reason is in `lastGateMessage()`; so it is with
    // `dialogs` false (the control server).
    bool saveGate(PictureView* view, bool dialogs = true);

    // Show the window of a paged SHP that holds file frame `frame` (`span`
    // frames; 0 keeps the document's window size), then focus the frame.
    // The window on screen goes through the same gate as a save first (its
    // pixels go into the frame store as palette indices). A page change
    // starts a new undo history; the status bar says so. False, with the
    // reason in lastGateMessage(), when it does not happen.
    bool pageTo(int frame, int span = 0);
    // The bytes of sheet past the limit the user agreed `view` may hold.
    static quint64 allowanceOf(const PictureView* view);
    QString lastGateMessage() const { return gateMessage_; }
    // Why the last SHP write failed after the gate let it through.
    QString lastSaveError() const;

    // Test seam: force the gate's dialogs on (or off) whatever the platform.
    static void setGateInteractive(int interactive);

    // --- The SHP size limit (Edit > Preferences > Advanced) ---------------
    // A sheet over the limit opens as a window of frames. Holding more than
    // the limit (a whole sheet, a window of one frame that is itself over
    // it, a bigger window, a resize) needs "Allow SHP files over the size
    // limit", and past 10% over also "Remove the 10% ceiling", and each time
    // a confirmation with a checkbox (a second one past 10%). Neither
    // setting is saved: both are off whenever Kooka starts. No setting
    // passes the floor: the estimated footprint must fit the memory
    // available now. Without dialogs (offscreen, the control server) a step
    // that needs confirming is refused with the reason, never confirmed.
    bool allowOverLimit() const { return allowOverLimit_; }
    void setAllowOverLimit(bool on);
    // Effective only while allowOverLimit() is on.
    bool removeCeiling() const { return removeCeiling_; }
    void setRemoveCeiling(bool on);
    // The Preferences page holding both settings (owned by `parent`).
    QWidget* advancedPage(QWidget* parent);

    // Before `view` opens the SHP at `path` (header only): confirm or refuse
    // what is over the limit, and grant `view` the agreed allowance. False
    // when the open must not go ahead; the reason in lastLimitMessage().
    bool confirmOpen(PictureView* view, const QString& path);
    // Before `view` (an SHP or not) is resized to `width` x `height` (Image
    // Size, Canvas Size). False when it must not be.
    bool allowSize(PictureView* view, int width, int height);
    // Hold windows of `count` frames of the active paged SHP.
    bool setWindowFrames(int count);
    QString lastLimitMessage() const { return limitMessage_; }

    // Spans a control request: no size-limit dialog is shown, and a step
    // the limit refused is reported (`refusal()`), so the request can
    // answer `refused` with the reason.
    class Quiet {
    public:
        Quiet();
        ~Quiet();
        Quiet(const Quiet&) = delete;
        Quiet& operator=(const Quiet&) = delete;
        QString refusal() const;
    };

signals:
    void limitSettingsChanged();

private:
    ShpSupport(QMainWindow* frame, ColorState* colors);
    void message(const QString& text);
    bool interactive() const;
    void showOffPalette(PictureView* view);
    // The checks before the window's pixels become indices (a save, or a
    // page change when `paging`): off-palette pixels and gutter bleed, with
    // the recorded fixes offered when `dialogs` and interactive.
    bool cleanGate(PictureView* view, bool dialogs, bool paging);
    void refreshFrame();
    enum class Verdict { Go, Alternative, Refused, Cancelled };
    // Measure `plan` against the limit and the settings; ask (stages the
    // view has not confirmed yet) when dialogs are allowed. `what` names
    // the step ("Opening x.shp"); `go`/`alternative` label the buttons.
    Verdict confirmLimit(const ShpPlan& plan, int confirmedStage, const QString& what,
                         const QString& go, const QString& alternative);
    Verdict limitDialog(const ShpPlan& plan, int stage, const QString& what, const QString& go,
                        const QString& alternative);
    // A refusal the user should see: status bar, a warning when dialogs are
    // allowed, and the control request's answer.
    bool refuseLimit();
    bool asksNow() const;
    static void grant(PictureView* view, quint64 bytes, int stage);

    QMainWindow* frame_ = nullptr;
    QDockWidget* dock_ = nullptr;
    ShpFramesPanel* frames_ = nullptr;
    ShpPalettePanel* palette_ = nullptr;
    QShortcut* prevKey_ = nullptr;
    QShortcut* nextKey_ = nullptr;
    QPointer<PictureView> view_;
    bool wasShp_ = false;
    QString gateMessage_;
    QString limitMessage_;
    bool allowOverLimit_ = false;
    bool removeCeiling_ = false;
};

} // namespace pictura
