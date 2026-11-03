// SPDX-License-Identifier: GPL-3.0-or-later
// KookaPictura-SHP fork: the SHP dock (Frames, Playback, Palette; Tiles for a TMP
// terrain template), frame stepping,
// and the save gate. The frame reaches it through three one-line hooks
// (install, retarget, refresh) listed in crates/pictura-shp/HOOKS.md.
#pragma once

#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QString>

class QAction;
class QDockWidget;
class QMenu;
class QMainWindow;
class QShortcut;
class QWidget;

namespace pictura {

class ColorState;
class ImageView;
class PictureView;
class ShpFramesPanel;
class ShpPalettePanel;
class ShpPlaybackPanel;
class ShpUndoFocus;
class ShpUnitPanel;
class TmpTilesPanel;
enum class ToolId;
struct ShpPlan;

// True for a path the fork's writers save (any case): .shp, the theater
// suffixes (an SHP sheet or a TMP template) and .tmp (a TMP template).
bool isShpPath(const QString& path);
// True for a path a TMP template saves under: the theater suffixes and .tmp.
bool isTmpPath(const QString& path);

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
    ShpPlaybackPanel* playback() const { return playback_; }
    ShpUndoFocus* undoFocus() const { return undoFocus_; }
    ShpUnitPanel* unit() const { return unit_; }
    TmpTilesPanel* tiles() const { return tiles_; }

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
    // An image that is not an SHP sheet is first converted (see
    // convertForSave), interactively only.
    bool saveGate(PictureView* view, bool dialogs = true, const QString& path = QString());

    // File > New SHP: a new document of `frames` blank frames of `width` x
    // `height` in the palette at `palette` (empty: built-in grey), after the
    // size limit. False, with the reason in lastGateMessage(), when not.
    bool newShp(int width, int height, int frames, const QString& palette);
    // The New SHP dialog, then newShp().
    void newShpWithDialog();
    QAction* newShpAction() const { return newShpAction_; }
    // Image > Frame Size (enabled for a whole SHP sheet).
    QAction* frameSizeAction() const { return frameSizeAction_; }
    // Every frame onto a `width` x `height` canvas placed by `anchor`
    // (row * 3 + column), size-checked, one recorded edit; false when
    // refused or cancelled (the reason in lastGateMessage()).
    bool frameSize(int width, int height, int anchor);
    bool frameSizeWithDialog();
    // Save As SHP of `source`, an image that is not an SHP sheet: ask how
    // to cut it into frames, make the SHP sheet as a new document (the image
    // stays as it was) and save that through the normal gate. Returns false
    // in any case, so the caller does not save `source`; savedCopy() says
    // whether the new document was saved.
    bool convertForSave(PictureView* source, const QString& path, bool dialogs);
    bool savedCopy() const { return savedCopy_; }

    // Show the window of a paged SHP that holds file frame `frame` (`span`
    // frames; 0 keeps the document's window size), then focus the frame.
    // The window on screen goes through the same gate as a save first (its
    // pixels go into the frame store as palette indices). A page change
    // starts a new undo history; the status bar says so. False, with the
    // reason in lastGateMessage(), when it does not happen.
    bool pageTo(int frame, int span = 0);
    // A frame operation on the active SHP (see shp_frame_op: insert,
    // duplicate, delete, move, reverse), then focus the frame it names. A
    // sheet that would grow past the size limit asks as Frames loaded does;
    // pixels in the gutters are offered the recorded clear first. On a paged
    // SHP the window must also be on the palette (the paging gate) and the
    // operation starts a new undo history, which the status bar says. False,
    // with the reason in lastGateMessage(), when nothing changed.
    bool frameOp(const QString& op, int first, int count, int to = 0);
    // "Snap Frame to Palette" on file frame `frame` (on the sheet): one
    // recorded step; the pixels changed, or -1 (the reason is shown).
    int snapFrame(int frame);
    // --- Unit tools (shp_support_unit.cpp) --------------------------------
    // Generate the shadows of art frames `art` (index 1, moved by `dx`,
    // `dy`, leaning `skew` pixels per row). A sprite without a shadow half
    // gets one first, as many blank frames as it has, through the size
    // check. One recorded step on a whole sheet; on a paged one the window
    // must pass the paging gate and History starts again. The frames
    // written, or -1 with the reason in lastGateMessage().
    int generateShadows(const QList<int>& art, int dx, int dy, double skew);
    // Clear shadow frames `frames` (one recorded step, as above).
    int clearShadows(const QList<int>& frames);

    // --- TMP templates from images (shp_support_tmp.cpp) ------------------
    // File > New TMP from Image (enabled while a document is open).
    QAction* newTmpAction() const { return newTmpAction_; }
    // A new TMP template document made of `source`'s composite (the
    // options as tmp_from_image takes them), after the size limit. False,
    // with the reason in lastGateMessage(), when nothing was made; the
    // status bar says what was (pixels left outside the tiles, which Save
    // offers to clip, and pixels off the canvas).
    bool newTmpFromImage(PictureView* source, const QList<int>& params, const QList<int>& cells,
                         const QString& palette, const QString& name);
    // The options dialog (TmpFromImageDialog), then newTmpFromImage().
    // `path`: a Save As target (its suffix is the theater; the new document
    // is then saved there through the TMP gate, and this returns false with
    // savedCopy() telling whether it was), or empty (File > New TMP from
    // Image). Interactive only: without dialogs it is refused with the
    // reason.
    bool tmpFromImageWithDialog(PictureView* source, const QString& path, bool dialogs);

    // Save As SHP of a TMP template (shp_support_tmp.cpp): a new SHP
    // document of its pixels (see tmp_to_shp), after the size limit; the
    // template is not changed. False, with the reason in
    // lastGateMessage(), when nothing was made.
    bool tmpToShp(PictureView* source, bool perTile, bool extras, const QString& name);
    // The options dialog (TmpToShpDialog), then tmpToShp() and the save of
    // the new document to `path` through the SHP gate. Returns false (the
    // template itself is not saved); savedCopy() says whether the SHP was.
    // Interactive only: without dialogs it is refused with the reason.
    bool tmpToShpWithDialog(PictureView* source, const QString& path, bool dialogs);

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
    // Image Size, Canvas Size, Crop, Trim, Reveal All, rotations, flips and
    // mode changes: commands that would reshape an SHP sheet. blocks() is
    // true for them while the active document is an SHP sheet (the command
    // registry disables them; the control server refuses them).
    static bool isSheetCommand(const QString& id);
    static bool blocks(const QObject* frame, const QString& id);
    static QString blockedReason();
    // The Crop and Perspective Crop tools reshape the sheet too: while an
    // SHP sheet is active allowsTool() refuses them (with a status message)
    // and their toolbox entries are disabled; an active one is put down.
    static bool isSheetTool(ToolId id);
    static bool allowsTool(const QObject* frame, ToolId id);
    static QString cropToolReason();
    // Why a recorded Actions step running `commandId` must not run on the
    // active document, or empty (the Actions panel's step gate).
    static QString stepRefusal(const QObject* frame, const QString& commandId);
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
    // Menu entries the command table does not hold: File > New SHP.
    void installMenus();
    // File > New TMP from Image, after New SHP in `menu`
    // (shp_support_tmp.cpp).
    void installTmpMenus(QMenu* menu);
    void message(const QString& text);
    bool interactive() const;
    void showOffPalette(PictureView* view);
    // The save gate of a TMP template (shp_support_tmp.cpp): off-palette
    // and off-scale pixels ([Snap and Save] / [Show Me] / [Cancel]), then
    // pixels outside the tiles ([Clip and Save] / [Show Me] / [Cancel]);
    // without dialogs it refuses with the reason.
    bool tmpGate(PictureView* view, bool dialogs, const QString& path);
    // The checks before the window's pixels become indices (a save, or a
    // page change when `paging`): off-palette pixels (unless not `palette`)
    // and gutter bleed, with the recorded fixes offered when `dialogs` and
    // interactive.
    bool cleanGate(PictureView* view, bool dialogs, bool paging, bool palette = true);
    // Save warnings for unit sprites (shp_support_unit.cpp): shadow frames
    // holding colors other than index 0 and 1. Never refuses without
    // dialogs; false when the user cancels.
    bool unitSaveCheck(PictureView* view, bool asks);
    // Report a refused unit-tool step (status bar, a warning when asking);
    // returns -1.
    int unitRefused(const QString& text);
    void refreshFrame();
    // The dock's panels for an SHP sheet, or for a TMP template.
    void showPanelsFor(bool tmp);
    // The active document is an SHP sheet or a TMP template.
    bool westwoodView() const;
    void refreshCommands(bool shp);
    void refreshCropTools(bool shp);
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
    ShpPlaybackPanel* playback_ = nullptr;
    ShpUndoFocus* undoFocus_ = nullptr;
    ShpUnitPanel* unit_ = nullptr;
    TmpTilesPanel* tiles_ = nullptr;
    bool commandsForShp_ = false;
    QMetaObject::Connection hoverConnection_;
    QAction* newShpAction_ = nullptr;
    QAction* newTmpAction_ = nullptr;
    QAction* frameSizeAction_ = nullptr;
    bool savedCopy_ = false;
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
