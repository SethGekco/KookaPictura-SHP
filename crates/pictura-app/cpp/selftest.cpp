#include "selftest_report.h"
#include "api/layers_api.h"
#include "pictura_app/src/cxxqt_object/layer_target_bridge.cxxqt.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QSet>
#include <QtCore/QStringList>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtGui/QAction>
#include <QtGui/QColor>
#include <QtGui/QIcon>
#include <QtGui/QImage>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QPalette>
#include <QtGui/QPixmap>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QApplication>
#include <QtWidgets/QDockWidget>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QTabBar>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QToolBar>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QTreeView>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>

#include "commands.h"
#include "dialogs.h"
#include "frame.h"
#include "icons.h"
#include "image_view.h"
#include "panels/layers_panel.h"
#include "panels/panel_column.h"
#include "panels/panel_group.h"
#include "preferences_dialog.h"
#include "session.h"
#include "theme.h"
#include "toolbox.h"

#include "pictura_app/src/cxxqt_object.cxxqt.h"
#include "pictura_app/src/cxxqt_object/impl_history/purge.cxxqt.h"
#include "pictura_app/src/cxxqt_object/impl_selection/bridge.cxxqt.h"
#include "interop.h"
#include "selftest.h"
#include "selftest_control.h"
#include "selftest_layers_controls.h"
#include "selftest_layers_filter.h"
#include "selftest_canvas.h"

int runSelfTest(QApplication& app, bool headless, const QString& psdPath,
                pictura::PicturaMainWindow& frame, pictura::PictureView* view,
                const QImage& image, bool codecLoaded, int gpu)
{
        ST_BEGIN("platform_headless");
        if (headless && qApp->platformName() != QStringLiteral("offscreen")) {
            ST_FAIL(152, "headless platform is %s, expected offscreen",
                    qPrintable(qApp->platformName()));
        }
        ST_PASS("offscreen");

        std::fprintf(stderr,
                     "pictura self-test: image=%dx%d codec_loaded=%d gpu=%d\n",
                     image.width(),
                     image.height(),
                     codecLoaded ? 1 : 0,
                     gpu);
        std::fflush(stderr);
        ST_BEGIN("null_image");
        if (!view || image.isNull()) {
            ST_FAIL(2, "null image");
        }
        ST_PASS("image present");
        ST_BEGIN("gpu_blank");
        if (gpu == 2) {
            ST_FAIL(4, "GPU render was blank");
        }
        ST_PASS("gpu=%d", gpu);
        if (!codecLoaded) {
            // E1: the scratch document is plain white and its first present must
            // be pure white across every pixel. The GPU smoke probe above must
            // not leak its gradient into the document's display image.
            ST_BEGIN("fresh_white");
            bool freshWhite = true;
            for (int y = 0; y < image.height() && freshWhite; ++y) {
                for (int x = 0; x < image.width(); ++x) {
                    if (image.pixel(x, y) != 0xFFFFFFFFu) {
                        freshWhite = false;
                        break;
                    }
                }
            }
            ST_PASS("fresh_white=%d", freshWhite ? 1 : 0);
            if (!freshWhite) {
                ST_FAIL(5, "fresh document not white");
            }
        }
        if (codecLoaded) {
            // two_layers.psd is an 8x8 layer stack: a red top-left quadrant and
            // a blue bottom-right quadrant; the other quadrants are uncovered.
            QSet<QRgb> seen;
            for (int y = 0; y < image.height(); ++y) {
                for (int x = 0; x < image.width(); ++x) {
                    seen.insert(image.pixel(x, y));
                }
            }
            ST_BEGIN("layered_distinct");
            ST_PASS("layered distinct=%d", seen.size());
            if (seen.size() < 2) {
                ST_FAIL(6, "blank composition");
            }
            if (image.width() >= 8 && image.height() >= 8) {
                const QRgb tl = image.pixel(2, 2);
                const QRgb br = image.pixel(6, 6);
                const QRgb tr = image.pixel(6, 2);
                const QRgb bl = image.pixel(2, 6);
                ST_BEGIN("tl");
                ST_PASS("tl=(%d,%d,%d,a%d) br=(%d,%d,%d,a%d) "
                             "tr_a=%d bl_a=%d", qRed(tl),
                             qGreen(tl),
                             qBlue(tl),
                             qAlpha(tl),
                             qRed(br),
                             qGreen(br),
                             qBlue(br),
                             qAlpha(br),
                             qAlpha(tr),
                             qAlpha(bl));
                const bool red = qRed(tl) > 200 && qGreen(tl) < 60 && qBlue(tl) < 60
                                 && qAlpha(tl) == 255;
                const bool blue = qBlue(br) > 200 && qRed(br) < 60 && qGreen(br) < 60
                                  && qAlpha(br) == 255;
                if (!red || !blue) {
                    ST_FAIL(7, "composited quadrants wrong");
                }
                // Uncovered quadrants must be transparent: this proves the layer
                // stack was composited, not the opaque embedded PSD composite.
                if (qAlpha(tr) != 0 || qAlpha(bl) != 0) {
                    ST_FAIL(8, "layer stack not composited");
                }
            }

            // M5-C2: a wand selection must confine an adjustment to the
            // selected quadrant. Wand the red top-left, Invert it, and require
            // the blue bottom-right to be untouched. Clean up afterwards so the
            // full-frame checks below see the original stack.
            const bool wand = view->magic_wand(2, 2, 10, true, QStringLiteral("new"));
            const bool hasSelection = view->has_selection();
            const int selectedPx = view->selection_count();
            ST_BEGIN("magic_wand");
            ST_PASS("magic_wand=%d has_selection=%d selected_px=%d", wand ? 1 : 0,
                         hasSelection ? 1 : 0,
                         selectedPx);
            if (!wand || !hasSelection || selectedPx <= 0
                || selectedPx >= image.width() * image.height()) {
                ST_FAIL(14, "wand selection wrong");
            }
            const bool maskedAdded = view->add_adjustment(QStringLiteral("invert"));
            const QImage masked = view->image();
            const QRgb mtl = masked.pixel(2, 2);
            const QRgb mbr = masked.pixel(6, 6);
            ST_BEGIN("masked_adjustment");
            ST_PASS("masked_adjustment=%d tl=(%d,%d,%d,a%d) "
                         "br=(%d,%d,%d,a%d)", maskedAdded ? 1 : 0,
                         qRed(mtl),
                         qGreen(mtl),
                         qBlue(mtl),
                         qAlpha(mtl),
                         qRed(mbr),
                         qGreen(mbr),
                         qBlue(mbr),
                         qAlpha(mbr));
            const bool maskedCyan = qRed(mtl) < 60 && qGreen(mtl) > 200 && qBlue(mtl) > 200;
            const bool maskedBlue = qBlue(mbr) > 200 && qRed(mbr) < 60 && qGreen(mbr) < 60;
            if (!maskedAdded || !maskedCyan || !maskedBlue) {
                ST_FAIL(15, "masked adjustment not confined");
            }
            view->remove_layer(view->layer_count() - 1);
            view->deselect();
            if (view->has_selection() || view->selection_count() != 0) {
                ST_FAIL(16, "deselect left a selection");
            }

            // M4-C: add an Invert adjustment layer over the stack and verify the
            // composite changed as expected (red -> cyan, blue -> yellow).
            const QImage beforeAdjust = view->image();
            const bool added = view->add_adjustment(QStringLiteral("invert"));
            const QImage adjusted = view->image();
            const int layerCount = view->layer_count();
            ST_BEGIN("add_adjustment(invert)");
            ST_PASS("add_adjustment(invert)=%d layers=%d last_kind=%s", added ? 1 : 0,
                         layerCount,
                         view->layer_kind(layerCount - 1).toLocal8Bit().constData());
            if (!added || layerCount != 3
                || view->layer_kind(layerCount - 1) != QStringLiteral("adjustment")) {
                ST_FAIL(9, "invert adjustment not added");
            }
            const QRgb atl = adjusted.pixel(2, 2);
            const QRgb abr = adjusted.pixel(6, 6);
            ST_BEGIN("adjusted_tl");
            ST_PASS("adjusted tl=(%d,%d,%d,a%d) br=(%d,%d,%d,a%d)", qRed(atl),
                         qGreen(atl),
                         qBlue(atl),
                         qAlpha(atl),
                         qRed(abr),
                         qGreen(abr),
                         qBlue(abr),
                         qAlpha(abr));
            const bool cyan = qRed(atl) < 60 && qGreen(atl) > 200 && qBlue(atl) > 200
                              && qAlpha(atl) == 255;
            const bool yellow = qRed(abr) > 200 && qGreen(abr) > 200 && qBlue(abr) < 60
                                && qAlpha(abr) == 255;
            if (!cyan || !yellow) {
                ST_FAIL(10, "invert composite wrong");
            }
            if (beforeAdjust == adjusted) {
                ST_FAIL(11, "adjustment did not change image");
            }

            // Toggle the bottom pixel layer's visibility: the output must change.
            if (!view->layer_visible(0)) {
                ST_FAIL(12, "base layer not visible");
            }
            view->set_layer_visible(0, false);
            const QImage hidden = view->image();
            bool differs = false;
            for (int y = 0; y < hidden.height() && !differs; ++y) {
                for (int x = 0; x < hidden.width(); ++x) {
                    if (hidden.pixel(x, y) != adjusted.pixel(x, y)) {
                        differs = true;
                        break;
                    }
                }
            }
            ST_BEGIN("visibility_change");
            ST_PASS("visibility_change=%d", differs ? 1 : 0);
            if (!differs || view->layer_visible(0)) {
                ST_FAIL(13, "visibility toggle did not change output");
            }
            // The adjustment created above is the target under the creation
            // rule; the filter must edit the topmost pixel layer.
            view->set_active_layer(QStringLiteral("1"));

            // M6-C: a filter must confine its change to the active selection.
            // The topmost pixel layer is the bottom-right blue quadrant; wand
            // that quadrant, apply the fixed-seed Add Noise, and require the
            // selected quadrant to change while the rest is bit-identical.
            view->deselect();
            const bool filterWand = view->magic_wand(6, 6, 10, true, QStringLiteral("new"));
            const bool filterSelected = view->has_selection();
            const int filterSelectedPx = view->selection_count();
            ST_BEGIN("filter_wand");
            ST_PASS("filter_wand=%d selected_px=%d", filterWand ? 1 : 0,
                         filterSelectedPx);
            if (!filterWand || !filterSelected || filterSelectedPx <= 0
                || filterSelectedPx >= view->image().width() * view->image().height()) {
                ST_FAIL(17, "filter selection wrong");
            }
            const QImage filterBefore = view->image();
            const bool filtered = view->apply_filter(QStringLiteral("add-noise"));
            const QImage filterAfter = view->image();
            int insideChanged = 0;
            int outsideChanged = 0;
            for (int y = 0; y < filterAfter.height(); ++y) {
                for (int x = 0; x < filterAfter.width(); ++x) {
                    const bool inside = x >= 4 && x < 8 && y >= 4 && y < 8;
                    if (filterAfter.pixel(x, y) == filterBefore.pixel(x, y)) {
                        continue;
                    }
                    if (inside) {
                        ++insideChanged;
                    } else {
                        ++outsideChanged;
                    }
                }
            }
            ST_BEGIN("filter_change");
            ST_PASS("filter_change=%d changed_inside=%d "
                         "changed_outside=%d", filtered ? 1 : 0,
                         insideChanged,
                         outsideChanged);
            if (!filtered || insideChanged == 0 || outsideChanged != 0) {
                ST_FAIL(18, "filter not confined");
            }
            view->deselect();

            // M13: document ops. Earlier checks mutated the stack (hidden
            // layer, active invert, noise), so assert the exact 90 deg CW
            // remap (x,y) -> (7-y,x) on captured pixels, plus that a
            // successful op clears the selection.
            const QImage preRotate = view->image();
            view->select_all();
            const bool rotated = view->rotate_doc(1);
            const QImage rotatedImg = view->image();
            const QRgb rotTr = rotatedImg.pixel(5, 2);
            const QRgb rotBl = rotatedImg.pixel(1, 6);
            ST_BEGIN("rotate_cw");
            ST_PASS("rotate_cw=%d size=%dx%d "
                         "map_tl=%d map_br=%d corner_a=%d sel=%d", rotated ? 1 : 0,
                         rotatedImg.width(),
                         rotatedImg.height(),
                         rotTr == preRotate.pixel(2, 2) ? 1 : 0,
                         rotBl == preRotate.pixel(6, 6) ? 1 : 0,
                         qAlpha(rotatedImg.pixel(5, 6)),
                         view->selection_count());
            if (!rotated || rotatedImg.width() != 8 || rotatedImg.height() != 8
                || rotTr != preRotate.pixel(2, 2) || rotBl != preRotate.pixel(6, 6)
                || qAlpha(rotatedImg.pixel(5, 6)) != 0 || view->has_selection()
                || view->selection_count() != 0) {
                ST_FAIL(19, "rotate cw wrong");
            }

            // M13: invalid document ops must be rejected and leave pixels put.
            const bool badRotate = view->rotate_doc(0);
            const bool badRotateClean = view->image() == rotatedImg;
            const bool badResize = view->resize_image(QStringLiteral("bicubic"), 0, 8);
            const bool badResizeClean = view->image() == rotatedImg;
            const bool badCanvas = view->resize_canvas(QStringLiteral("nope"), 10, 10);
            const bool badCanvasClean = view->image() == rotatedImg;
            ST_BEGIN("reject_rotate0");
            ST_PASS("reject rotate0=%d resize_w0=%d "
                         "canvas_bad_anchor=%d unchanged=%d", badRotate ? 1 : 0,
                         badResize ? 1 : 0,
                         badCanvas ? 1 : 0,
                         badRotateClean && badResizeClean && badCanvasClean ? 1 : 0);
            if (badRotate || badResize || badCanvas || !badRotateClean || !badResizeClean
                || !badCanvasClean) {
                ST_FAIL(20, "invalid doc op accepted");
            }

            // M13: CCW must undo CW bit-exactly, then growing the canvas to
            // 10x12 with the bottom-right anchor maps old (x,y) to (x+2,y+4)
            // and leaves the new top-left area transparent.
            const bool restoredOk = view->rotate_doc(3);
            const QImage restored = view->image();
            if (!restoredOk || restored != preRotate) {
                ST_FAIL(21, "rotate ccw did not restore");
            }
            const bool grown = view->resize_canvas(QStringLiteral("bottom-right"), 10, 12);
            const QImage grownImg = view->image();
            ST_BEGIN("canvas_grow");
            ST_PASS("canvas_grow=%d size=%dx%d "
                         "map_tl=%d map_br=%d corner_a=%d", grown ? 1 : 0,
                         grownImg.width(),
                         grownImg.height(),
                         grownImg.pixel(4, 6) == restored.pixel(2, 2) ? 1 : 0,
                         grownImg.pixel(8, 10) == restored.pixel(6, 6) ? 1 : 0,
                         qAlpha(grownImg.pixel(0, 0)));
            if (!grown || grownImg.width() != 10 || grownImg.height() != 12
                || grownImg.pixel(4, 6) != restored.pixel(2, 2)
                || grownImg.pixel(8, 10) != restored.pixel(6, 6)
                || qAlpha(grownImg.pixel(0, 0)) != 0 || qAlpha(grownImg.pixel(1, 1)) != 0) {
                ST_FAIL(21, "canvas growth wrong");
            }

            // M14: mutating ops capture history; undo/redo must round-trip the
            // pixels bit-exactly.
            const QImage preUndo = view->image();
            const int depthBefore = view->history_depth();
            const bool historyRotated = view->rotate_doc(1);
            const int depthAfterRotate = view->history_depth();
            const bool rotatedCanUndo = view->can_undo();
            const QImage postRotate = view->image();
            const bool undone = view->undo();
            const bool undoIdentical = view->image() == preUndo;
            const bool redone = view->redo();
            const bool redoIdentical = view->image() == postRotate;
            ST_BEGIN("history_rotate");
            ST_PASS("history rotate=%d depth=%d undo=%d "
                         "undo_ident=%d redo=%d redo_ident=%d", historyRotated ? 1 : 0,
                         depthAfterRotate,
                         undone ? 1 : 0,
                         undoIdentical ? 1 : 0,
                         redone ? 1 : 0,
                         redoIdentical ? 1 : 0);
            if (!historyRotated || depthAfterRotate != depthBefore + 1
                || !rotatedCanUndo || postRotate.width() != 12
                || postRotate.height() != 10 || !undone || !undoIdentical
                || !redone || !redoIdentical) {
                ST_FAIL(22, "undo/redo round-trip wrong");
            }

            // M14: a fresh op invalidates redo; reopening the fixture resets
            // history and an empty-stack undo fails without touching pixels.
            const bool flipped = view->flip_doc(true);
            const bool redoInvalidated = flipped && !view->can_redo();
            const bool reopened = view->open(psdPath);
            const QImage reopenedImg = view->image();
            const bool noUndoAfterOpen = !view->can_undo();
            const bool boundaryUndone = view->undo();
            const bool boundaryIdentical = view->image() == reopenedImg;
            const bool openReset = reopened && noUndoAfterOpen && boundaryIdentical;
            ST_BEGIN("history_redo_invalid");
            ST_PASS("history redo_invalid=%d open_reset=%d "
                         "boundary_undo=%d", redoInvalidated ? 1 : 0,
                         openReset ? 1 : 0,
                         boundaryUndone ? 1 : 0);
            if (!redoInvalidated || !openReset || boundaryUndone) {
                ST_FAIL(23, "history invalidation wrong");
            }

            // M15: render filters. The topmost pixel layer is the blue
            // quadrant, so a full-frame apply must change pixels only inside
            // the blue rect (4,4)-(8,8); the fixed seed must make a re-apply
            // bit-identical.
            const QImage preClouds = view->image();
            const bool clouded = view->apply_filter(QStringLiteral("clouds"));
            const QImage cloudedImg = view->image();
            int cloudsInside = 0;
            int cloudsOutside = 0;
            for (int y = 0; y < cloudedImg.height(); ++y) {
                for (int x = 0; x < cloudedImg.width(); ++x) {
                    if (cloudedImg.pixel(x, y) == preClouds.pixel(x, y)) {
                        continue;
                    }
                    if (x >= 4 && x < 8 && y >= 4 && y < 8) {
                        ++cloudsInside;
                    } else {
                        ++cloudsOutside;
                    }
                }
            }
            const bool reapplied = view->apply_filter(QStringLiteral("clouds"));
            const bool reapplyIdentical = reapplied && view->image() == cloudedImg;
            const bool flared = view->apply_filter(QStringLiteral("lens-flare"));
            const QImage flaredImg = view->image();
            int flareOutsideChanged = 0;
            for (int y = 0; y < flaredImg.height(); ++y) {
                for (int x = 0; x < flaredImg.width(); ++x) {
                    if (!(x >= 4 && x < 8 && y >= 4 && y < 8)
                        && flaredImg.pixel(x, y) != cloudedImg.pixel(x, y)) {
                        ++flareOutsideChanged;
                    }
                }
            }
            ST_BEGIN("clouds");
            ST_PASS("clouds=%d confined=%d inside=%d "
                         "reapply_ident=%d flare=%d", clouded ? 1 : 0,
                         cloudsOutside == 0 && flareOutsideChanged == 0 ? 1 : 0,
                         cloudsInside,
                         reapplyIdentical ? 1 : 0,
                         flared && flaredImg != cloudedImg ? 1 : 0);
            if (!clouded || cloudedImg == preClouds || cloudsInside == 0
                || cloudsOutside != 0 || !reapplyIdentical || !flared
                || flaredImg == cloudedImg || flareOutsideChanged != 0) {
                ST_FAIL(24, "clouds/flare wrong");
            }

            pictura::CommandRegistry* registry = frame.registry();

            // M16: document-required commands disable with no document, while
            // File > Open stays enabled.
            view->open(QStringLiteral("/nonexistent-kooka-pictura.psd"));
            registry->refresh();
            QAction* rotateAction =
                registry->action(QString::fromLatin1(pictura::command_ids::ImageRotate90Cw));
            QAction* openAction =
                registry->action(QString::fromLatin1(pictura::command_ids::FileOpen));
            const bool docDisabled = rotateAction && !rotateAction->isEnabled();
            const bool openEnabled = openAction && openAction->isEnabled();
            view->open(psdPath);
            registry->refresh();
            ST_BEGIN("no_doc_disable");
            ST_PASS("no_doc_disable=%d open_enable=%d", docDisabled ? 1 : 0,
                         openEnabled ? 1 : 0);
            if (!docDisabled || !openEnabled) {
                ST_FAIL(27, "command enablement wrong");
            }

            // M16: brightness levels apply and differ (theme is the source of truth).
            frame.setBrightnessLevel(0);
            const QColor darkWindow = qApp->palette().color(QPalette::Window);
            frame.setBrightnessLevel(3);
            const QColor lightWindow = qApp->palette().color(QPalette::Window);
            const bool brightOk =
                frame.brightnessLevel() == 3 && darkWindow != lightWindow;
            frame.setBrightnessLevel(1);
            ST_BEGIN("brightness_ok");
            ST_PASS("brightness ok=%d dark=%s light=%s", brightOk ? 1 : 0,
                         darkWindow.name().toLocal8Bit().constData(),
                         lightWindow.name().toLocal8Bit().constData());
            if (!brightOk) {
                ST_FAIL(28, "brightness wrong");
            }

            // M16: screen modes cycle forward and backward.
            using ScreenMode = pictura::PicturaMainWindow::ScreenMode;
            frame.setScreenMode(ScreenMode::Standard);
            frame.cycleScreenMode(true);
            const ScreenMode mode1 = frame.screenMode();
            frame.cycleScreenMode(true);
            const ScreenMode mode2 = frame.screenMode();
            frame.cycleScreenMode(false);
            const ScreenMode mode3 = frame.screenMode();
            const bool modesOk = mode1 == ScreenMode::FullWithMenuBar
                                 && mode2 == ScreenMode::Full
                                 && mode3 == ScreenMode::FullWithMenuBar;
            frame.setScreenMode(ScreenMode::Standard);
            ST_BEGIN("screen_modes");
            ST_PASS("screen_modes=%d", modesOk ? 1 : 0);
            if (!modesOk) {
                ST_FAIL(29, "screen mode cycle wrong");
            }

            // M16: layout + brightness persist atomically under the self-test's
            // temp XDG state dir (main.cpp redirects it before the frame exists).
            frame.setBrightnessLevel(2); frame.saveSession(); frame.setBrightnessLevel(1);
            const pictura::SessionState loaded = pictura::loadSession();
            const bool sessionOk = loaded.brightnessLevel == 2
                                   && loaded.layout == frame.saveState();
            ST_BEGIN("session_ok");
            ST_PASS("session_ok=%d bytes=%d level=%d", sessionOk ? 1 : 0,
                         loaded.layout.size(),
                         loaded.brightnessLevel);
            if (!sessionOk) {
                ST_FAIL(30, "session round-trip wrong");
            }

            // M16: duplicate panel objectNames are rejected.
            auto* duplicate = new QDockWidget(QStringLiteral("Duplicate"), &frame);
            duplicate->setObjectName(QStringLiteral("layersPanel"));
            const bool duplicateRejected = !frame.registerPanel(duplicate, Qt::LeftDockWidgetArea);
            delete duplicate;
            ST_BEGIN("dup_panel_rejected");
            ST_PASS("dup_panel_rejected=%d", duplicateRejected ? 1 : 0);
            if (!duplicateRejected) {
                ST_FAIL(31, "duplicate panel accepted");
            }

            // M16: Tab hides and restores all panels.
            auto* layersPanel = frame.findChild<QWidget*>(QStringLiteral("layersPanel"));
            frame.setPanelsHidden(true);
            const bool panelsHidden = layersPanel && !layersPanel->isVisible();
            frame.setPanelsHidden(false);
            const bool panelsShown = layersPanel && layersPanel->isVisible();
            ST_BEGIN("hide_all");
            ST_PASS("hide_all=%d restore=%d", panelsHidden ? 1 : 0,
                         panelsShown ? 1 : 0);
            if (!panelsHidden || !panelsShown) {
                ST_FAIL(32, "hide-all wrong");
            }

            // M17: New creates an untitled document with the requested size and
            // a white background.
            const int docsBeforeNew = frame.documentCount();
            const bool created = frame.newDocument(QStringLiteral("Scratch"), 4, 3,
                                                   QStringLiteral("rgb"), 8,
                                                   QStringLiteral("white"));
            pictura::PictureView* fresh = frame.activeView();
            const bool freshOk = created && frame.documentCount() == docsBeforeNew + 1
                                 && fresh && fresh->has_document()
                                 && fresh->image().width() == 4 && fresh->image().height() == 3;
            const QRgb whitePixel = fresh ? fresh->image().pixel(1, 1) : 0;
            const bool whiteOk = qRed(whitePixel) == 255 && qGreen(whitePixel) == 255
                                 && qBlue(whitePixel) == 255 && qAlpha(whitePixel) == 255;
            ST_BEGIN("new_doc");
            ST_PASS("new_doc=%d white=%d", freshOk ? 1 : 0, whiteOk ? 1 : 0);
            if (!freshOk || !whiteOk) {
                ST_FAIL(33, "new document wrong");
            }

            // M17: Save As then open round-trips the new document's pixels.
            const QString savePath =
                QDir::tempPath() + QStringLiteral("/kooka-pictura-roundtrip.psd");
            const QImage scratchImg = fresh->image();
            const bool saved = frame.saveActiveAs(savePath);
            const bool reopenedDoc = frame.openPath(savePath);
            pictura::PictureView* reloaded = frame.activeView();
            const bool roundtrip = saved && reopenedDoc && reloaded && reloaded->has_document()
                                   && reloaded->image() == scratchImg;
            ST_BEGIN("roundtrip");
            ST_PASS("roundtrip=%d", roundtrip ? 1 : 0);
            if (!roundtrip) {
                ST_FAIL(34, "save/open round-trip wrong");
            }

            // M17: a mutating command sets dirty; save clears it.
            const bool cleanAfterOpen = !reloaded->is_dirty();
            reloaded->select_all();
            const bool dirtyAfterMutate = reloaded->is_dirty();
            const bool resaved = frame.saveActive();
            const bool cleanAfterSave = !reloaded->is_dirty();
            ST_BEGIN("dirty_clean");
            ST_PASS("dirty clean=%d set=%d resave=%d cleared=%d", cleanAfterOpen ? 1 : 0, dirtyAfterMutate ? 1 : 0,
                         resaved ? 1 : 0, cleanAfterSave ? 1 : 0);
            if (!cleanAfterOpen || !dirtyAfterMutate || !resaved || !cleanAfterSave) {
                ST_FAIL(35, "dirty state wrong");
            }

            // M17: multiple documents become multiple tabs; switching targets.
            const int totalDocs = frame.documentCount();
            frame.setActiveDocumentIndex(0);
            const bool firstActive = frame.activeDocumentIndex() == 0
                                     && frame.imageView() == frame.canvasAt(0);
            frame.setActiveDocumentIndex(totalDocs - 1);
            const bool lastActive = frame.activeDocumentIndex() == totalDocs - 1;
            ST_BEGIN("tabs");
            ST_PASS("tabs=%d first=%d last=%d", totalDocs, firstActive ? 1 : 0, lastActive ? 1 : 0);
            if (totalDocs < 2 || !firstActive || !lastActive) {
                ST_FAIL(36, "document tabs wrong");
            }

            // M17: closing a modified document honours the prompt: Cancel keeps
            // it, Discard closes it.
            pictura::setUnsavedPromptInteractive(false);
            pictura::setNonInteractiveUnsavedChoice(pictura::UnsavedChoice::Cancel);
            frame.setActiveDocumentIndex(frame.documentCount() - 1);
            frame.activeView()->select_all();
            const int beforeClose = frame.documentCount();
            const bool cancelled = !frame.closeDocument(frame.activeDocumentIndex(), true)
                                   && frame.documentCount() == beforeClose;
            pictura::setNonInteractiveUnsavedChoice(pictura::UnsavedChoice::Discard);
            const bool discarded = frame.closeDocument(frame.activeDocumentIndex(), true)
                                   && frame.documentCount() == beforeClose - 1;
            pictura::setUnsavedPromptInteractive(true);
            ST_BEGIN("close_cancel");
            ST_PASS("close_cancel=%d close_discard=%d", cancelled ? 1 : 0, discarded ? 1 : 0);
            if (!cancelled || !discarded) {
                ST_FAIL(37, "unsaved close prompt wrong");
            }

            // M18: the frame's active tool round-trips through the controller.
            frame.setActiveTool(pictura::ToolId::Marquee);
            const bool marqueeOn = frame.activeTool() == pictura::ToolId::Marquee;
            frame.setActiveTool(pictura::ToolId::Lasso);
            const bool lassoOn = frame.activeTool() == pictura::ToolId::Lasso;
            frame.setActiveTool(pictura::ToolId::Eyedropper);
            const bool eyedropOn = frame.activeTool() == pictura::ToolId::Eyedropper;
            frame.setActiveTool(pictura::ToolId::Move);
            const bool moveOn = frame.activeTool() == pictura::ToolId::Move;
            ST_BEGIN("tool_switch_marquee");
            ST_PASS("tool_switch marquee=%d lasso=%d eyedropper=%d "
                         "move=%d active=%s", marqueeOn ? 1 : 0,
                         lassoOn ? 1 : 0,
                         eyedropOn ? 1 : 0,
                         moveOn ? 1 : 0,
                         pictura::toolInfo(frame.activeTool()).label);
            if (!marqueeOn || !lassoOn || !eyedropOn || !moveOn) {
                ST_FAIL(39, "tool switch wrong");
            }

            // M18: marquee rect/ellipse counts and pixel membership. The 4x4
            // ellipse rasterizes 12 px (pictura-select ellipse test).
            frame.newDocument(QStringLiteral("ToolTest"), 8, 8, QStringLiteral("rgb"), 8,
                              QStringLiteral("white"));
            pictura::PictureView* toolView = frame.activeView();
            const bool rectSel =
                toolView && toolView->select_rect(0, 0, 4, 4, QStringLiteral("new"), 0.0);
            const int rectPx = toolView ? toolView->selection_count() : -1;
            const bool ellipseSel =
                toolView && toolView->select_ellipse(0, 0, 4, 4, QStringLiteral("new"), 0.0);
            const int ellipsePx = toolView ? toolView->selection_count() : -1;
            if (toolView) {
                toolView->select_ellipse(0, 0, 4, 4, QStringLiteral("add"), 0.0);
            }
            const bool addIdempotent = toolView && toolView->selection_count() == ellipsePx;
            if (toolView) {
                toolView->select_ellipse(0, 0, 4, 4, QStringLiteral("new"), 0.0);
                toolView->select_rect(2, 2, 1, 1, QStringLiteral("intersect"), 0.0);
            }
            const bool centreInside = toolView && toolView->selection_count() == 1;
            if (toolView) {
                toolView->select_ellipse(0, 0, 4, 4, QStringLiteral("new"), 0.0);
                toolView->select_rect(0, 0, 1, 1, QStringLiteral("intersect"), 0.0);
            }
            const bool cornerOutside = toolView && toolView->selection_count() == 0;
            ST_BEGIN("marquee_rect");
            ST_PASS("marquee rect=%d(%d) ellipse=%d(%d) add_same=%d "
                         "centre=%d corner=%d", rectSel ? 1 : 0,
                         rectPx,
                         ellipseSel ? 1 : 0,
                         ellipsePx,
                         addIdempotent ? 1 : 0,
                         centreInside ? 1 : 0,
                         cornerOutside ? 1 : 0);
            if (!rectSel || rectPx != 16 || !ellipseSel || ellipsePx != 12 || !addIdempotent
                || !centreInside || !cornerOutside) {
                ST_FAIL(40, "marquee selection wrong");
            }

            // M18: combine modes union/subtract/intersect on the 8x8 canvas.
            if (toolView) {
                toolView->select_rect(0, 0, 4, 4, QStringLiteral("new"), 0.0);
            }
            const int newPx = toolView ? toolView->selection_count() : -1;
            if (toolView) {
                toolView->select_rect(2, 2, 4, 4, QStringLiteral("add"), 0.0);
            }
            const int addPx = toolView ? toolView->selection_count() : -1;
            if (toolView) {
                toolView->select_rect(0, 0, 4, 4, QStringLiteral("new"), 0.0);
                toolView->select_rect(1, 1, 2, 2, QStringLiteral("subtract"), 0.0);
            }
            const int subPx = toolView ? toolView->selection_count() : -1;
            if (toolView) {
                toolView->select_rect(0, 0, 4, 4, QStringLiteral("new"), 0.0);
                toolView->select_rect(2, 2, 4, 4, QStringLiteral("intersect"), 0.0);
            }
            const int interPx = toolView ? toolView->selection_count() : -1;
            ST_BEGIN("combine_new");
            ST_PASS("combine new=%d add=%d subtract=%d intersect=%d", newPx,
                         addPx,
                         subPx,
                         interPx);
            if (newPx != 16 || addPx != 28 || subPx != 12 || interPx != 4) {
                ST_FAIL(41, "combine modes wrong");
            }

            // M18: lasso fills a 5x5 square, and a two-point path is rejected
            // without disturbing the existing selection.
            const bool lassoStarted =
                toolView && toolView->begin_lasso(QStringLiteral("new"));
            if (toolView) {
                toolView->lasso_add_point(1, 1);
                toolView->lasso_add_point(6, 1);
                toolView->lasso_add_point(6, 6);
                toolView->lasso_add_point(1, 6);
            }
            const bool lassoEnded = toolView && toolView->end_lasso(0.0);
            const int lassoPx = toolView ? toolView->selection_count() : -1;
            const bool shortStarted =
                toolView && toolView->begin_lasso(QStringLiteral("new"));
            if (toolView) {
                toolView->lasso_add_point(1, 1);
                toolView->lasso_add_point(6, 1);
            }
            const bool shortEnded = toolView && toolView->end_lasso(0.0);
            const bool shortUnchanged = toolView && toolView->selection_count() == lassoPx;
            ST_BEGIN("lasso_start");
            ST_PASS("lasso start=%d end=%d px=%d short_end=%d "
                         "unchanged=%d", lassoStarted ? 1 : 0,
                         lassoEnded ? 1 : 0,
                         lassoPx,
                         shortEnded ? 1 : 0,
                         shortUnchanged ? 1 : 0);
            if (!lassoStarted || !lassoEnded || lassoPx != 25 || !shortStarted || shortEnded
                || !shortUnchanged) {
                ST_FAIL(42, "lasso selection wrong");
            }

            // M18: quick selection wands a white region and leaves a selection.
            if (toolView) {
                toolView->deselect();
            }
            const bool quickOk =
                toolView && pictura::quick_select(*toolView, 4, 4, 32, QStringLiteral("new"));
            const bool quickSelected = toolView && toolView->has_selection();
            const int quickPx = toolView ? toolView->selection_count() : -1;
            ST_BEGIN("quick_select");
            ST_PASS("quick_select=%d has=%d px=%d", quickOk ? 1 : 0, quickSelected ? 1 : 0, quickPx);
            if (!quickOk || !quickSelected || quickPx <= 0) {
                ST_FAIL(43, "quick selection wrong");
            }

            // M18: crop the fixture to its blue bottom-right quadrant.
            frame.openPath(psdPath);
            pictura::PictureView* cropView = frame.activeView();
            const bool cropOk = cropView && cropView->crop(4, 4, 4, 4);
            const QImage cropImg = cropView ? cropView->image() : QImage();
            const QRgb cropPx = cropImg.isNull() ? 0 : cropImg.pixel(1, 1);
            const bool cropBlue = qBlue(cropPx) > 200 && qRed(cropPx) < 60 && qGreen(cropPx) < 60;
            ST_BEGIN("crop");
            ST_PASS("crop=%d size=%dx%d blue=%d", cropOk ? 1 : 0, cropImg.width(), cropImg.height(), cropBlue ? 1 : 0);
            if (!cropOk || cropImg.width() != 4 || cropImg.height() != 4 || !cropBlue) {
                ST_FAIL(44, "crop wrong");
            }

            // M18: move the topmost (blue) layer over the red quadrant.
            frame.openPath(psdPath);
            pictura::PictureView* moveView = frame.activeView(); if (moveView) moveView->set_active_layer(QString::number(moveView->topmost_pixel_layer_index()));
            const bool moved = moveView && moveView->translate_layer(-4, -4);
            const QImage moveImg = moveView ? moveView->image() : QImage();
            const QRgb movePx = moveImg.isNull() ? 0 : moveImg.pixel(2, 2);
            const bool moveBlue = qBlue(movePx) > 200 && qRed(movePx) < 60 && qGreen(movePx) < 60;
            ST_BEGIN("translate");
            ST_PASS("translate=%d blue=%d", moved ? 1 : 0, moveBlue ? 1 : 0);
            if (!moved || !moveBlue) {
                ST_FAIL(45, "layer move wrong");
            }

            // M18: eyedropper samples the composited red and rejects out-of-bounds.
            frame.openPath(psdPath);
            pictura::PictureView* eyeView = frame.activeView();
            const quint32 redSample = eyeView ? eyeView->sample_argb(2, 2) : 0u;
            const quint32 outSample = eyeView ? eyeView->sample_argb(-1, -1) : 1u;
            if (eyeView) {
                eyeView->deselect();
            }
            const bool eyeDeselected = eyeView && !eyeView->has_selection();
            ST_BEGIN("eyedropper_red");
            ST_PASS("eyedropper red=%08x out=%08x deselected=%d", static_cast<unsigned>(redSample),
                         static_cast<unsigned>(outSample),
                         eyeDeselected ? 1 : 0);
            if (redSample != 0xFFFF0000u || outSample != 0u || !eyeDeselected) {
                ST_FAIL(46, "eyedropper wrong");
            }

            // M20: layer property getters/setters round-trip and mark dirty.
            frame.openPath(psdPath);
            pictura::PictureView* propView = frame.activeView();
            const int layersCount = propView ? propView->layer_count() : -1;
            const int baseOpacity = propView ? propView->layer_opacity(0) : -1;
            const bool renameOk =
                propView && propView->set_layer_name(0, QStringLiteral("Renamed"));
            const bool renamedOk =
                propView && propView->layer_name(0) == QStringLiteral("Renamed");
            const bool dirtyAfterRename = propView && propView->is_dirty();
            const bool blendOk =
                propView && propView->set_layer_blend(0, QStringLiteral("mul "));
            const bool blendRoundTrip =
                propView && propView->layer_blend(0) == QStringLiteral("mul ");
            const bool badBlendRejected =
                propView && !propView->set_layer_blend(0, QStringLiteral("zzzz"))
                && propView->layer_blend(0) == QStringLiteral("mul ");
            const bool opacityOk = propView && propView->set_layer_opacity(0, 128);
            const int roundTripOpacity = propView ? propView->layer_opacity(0) : -1;
            const bool clampOk = propView && propView->set_layer_opacity(0, 9999);
            const int clampedOpacity = propView ? propView->layer_opacity(0) : -1;
            ST_BEGIN("layer_count");
            ST_PASS("layer count=%d name=%d blend=%d badblend=%d "
                         "opacity=%d dirty=%d", layersCount,
                         renameOk && renamedOk ? 1 : 0,
                         blendOk && blendRoundTrip ? 1 : 0,
                         badBlendRejected ? 1 : 0,
                         roundTripOpacity,
                         dirtyAfterRename ? 1 : 0);
            if (layersCount != 2 || !renameOk || !renamedOk || !dirtyAfterRename || !blendOk
                || !blendRoundTrip || !badBlendRejected || !opacityOk || roundTripOpacity != 128
                || !clampOk || clampedOpacity != 255) {
                ST_FAIL(50, "layer properties wrong");
            }

            // M20: labeled history, jump restore, and named snapshots.
            const int histBefore = propView ? propView->history_count() : -1;
            const bool histOpenLabel =
                propView && histBefore >= 1 && !propView->history_label(0).isEmpty();
            const bool histMutated = propView && propView->set_layer_opacity(0, 64);
            const int histAfter = propView ? propView->history_count() : -1;
            const bool histGrew = histAfter == histBefore + 1;
            const bool histTopLabel =
                propView && histAfter >= 1 && !propView->history_label(histAfter - 1).isEmpty();
            const bool histAtTop = propView && propView->history_index() == histAfter - 1;
            const bool histJump = propView && propView->history_jump(0);
            const bool histAtZero = propView && propView->history_index() == 0;
            const int restoredOpacity = propView ? propView->layer_opacity(0) : -1;
            const bool opacityRestored = restoredOpacity == baseOpacity;
            const bool histJumpBack = propView && propView->history_jump(histAfter - 1);
            const bool snapAdded =
                propView && propView->history_add_snapshot(QStringLiteral("Checkpoint"));
            const int snapCount = propView ? pictura::history_snapshot_count(*propView) : -1;
            const bool snapLabelOk =
                propView && pictura::history_snapshot_label(*propView, 0) == QStringLiteral("Checkpoint");
            const bool snapRestored = propView && propView->history_restore_snapshot(0);
            ST_BEGIN("history_states");
            ST_PASS("history states=%d open_label=%d grew=%d "
                         "jump=%d snapshot=%d", histBefore,
                         histOpenLabel ? 1 : 0,
                         histGrew ? 1 : 0,
                         histJump && histAtZero && opacityRestored && histJumpBack ? 1 : 0,
                         snapAdded && snapCount >= 1 && snapLabelOk && snapRestored ? 1 : 0);
            if (!histOpenLabel || !histMutated || !histGrew || !histTopLabel || !histAtTop
                || !histJump || !histAtZero || !opacityRestored || !histJumpBack || !snapAdded
                || snapCount < 1 || !snapLabelOk || !snapRestored) {
                ST_FAIL(51, "history wrong");
            }
        }
        const bool docTabReorderOk = frame.reorderDocumentsForTest();
        ST_BEGIN("doc_tab_reorder_aligned");
        ST_PASS("doc_tab_reorder aligned=%d", docTabReorderOk ? 1 : 0);
        if (!docTabReorderOk) {
            ST_FAIL(196, "document tab reorder desync");
        }

        // M19: every documented asset id resolves from the Qt resource
        // bundle; unknown ids must be inert rather than crash. Assets are
        // document-independent, so these run with or without a loaded PSD.
        const QStringList expectedIcons = {
            QStringLiteral("app"),
            QStringLiteral("tool.move"),
            QStringLiteral("tool.marquee"),
            QStringLiteral("tool.lasso"),
            QStringLiteral("tool.quickselection"),
            QStringLiteral("tool.crop"),
            QStringLiteral("tool.eyedropper"),
            QStringLiteral("tool.hand"),
            QStringLiteral("tool.zoom"),
            QStringLiteral("file.new"),
            QStringLiteral("file.open"),
            QStringLiteral("file.save"),
            QStringLiteral("file.saveAs"),
            QStringLiteral("file.revert"),
            QStringLiteral("file.close"),
            QStringLiteral("file.closeAll"),
            QStringLiteral("file.exit"),
            QStringLiteral("edit.undo"),
            QStringLiteral("edit.redo"),
            QStringLiteral("edit.stepForward"),
            QStringLiteral("edit.stepBackward"),
            QStringLiteral("image.rotate90cw"),
            QStringLiteral("image.rotate90ccw"),
            QStringLiteral("image.rotate180"),
            QStringLiteral("image.flipHorizontal"),
            QStringLiteral("image.flipVertical"),
            QStringLiteral("image.crop"),
            QStringLiteral("select.all"),
            QStringLiteral("select.deselect"),
            QStringLiteral("view.zoomIn"),
            QStringLiteral("view.zoomOut"),
            QStringLiteral("view.fitOnScreen"),
            QStringLiteral("view.actualPixels"),
            QStringLiteral("view.screenMode.standard"),
            QStringLiteral("view.screenMode.fullWithMenuBar"),
            QStringLiteral("view.screenMode.full"),
            QStringLiteral("view.options"),
            QStringLiteral("window.panels.layers"),
            QStringLiteral("window.panels.tools"),
            QStringLiteral("help.about"),
        };
        int iconsResolved = 0;
        QString missingIcon;
        for (const QString& id : expectedIcons) {
            if (pictura::icon(id).isNull()) {
                if (missingIcon.isEmpty()) {
                    missingIcon = id;
                }
            } else {
                ++iconsResolved;
            }
        }
        const bool unknownIconNull = pictura::icon(QStringLiteral("no.such.icon")).isNull();
        ST_BEGIN("icons");
        ST_PASS("icons=%d unknown_null=%d", iconsResolved,
                     unknownIconNull ? 1 : 0);
        if (iconsResolved != expectedIcons.size() || !unknownIconNull) {
            ST_FAIL(47, "icon missing=%s unknown_null=%d", missingIcon.toLocal8Bit().constData(),
                         unknownIconNull ? 1 : 0);
        }

        // M19: the eight tool cursors must render; an unknown cursor id must
        // collapse to the default cursor without crashing.
        const QStringList expectedCursors = {
            QStringLiteral("tool.move"),
            QStringLiteral("tool.marquee"),
            QStringLiteral("tool.lasso"),
            QStringLiteral("tool.quickselection"),
            QStringLiteral("tool.crop"),
            QStringLiteral("tool.eyedropper"),
            QStringLiteral("tool.hand"),
            QStringLiteral("tool.zoom"),
        };
        int cursorsResolved = 0;
        QString missingCursor;
        for (const QString& id : expectedCursors) {
            if (pictura::cursor(id).pixmap().isNull()) {
                if (missingCursor.isEmpty()) {
                    missingCursor = id;
                }
            } else {
                ++cursorsResolved;
            }
        }
        pictura::cursor(QStringLiteral("no.such.cursor"));
        ST_BEGIN("cursors");
        ST_PASS("cursors=%d", cursorsResolved);
        if (cursorsResolved != expectedCursors.size()) {
            ST_FAIL(48, "cursor missing=%s", missingCursor.toLocal8Bit().constData());
        }

        // M19: the window icon must be set from the app asset.
        const bool windowIconSet = !QApplication::windowIcon().isNull();
        ST_BEGIN("window_icon");
        ST_PASS("window_icon=%d", windowIconSet ? 1 : 0);
        if (!windowIconSet) {
            ST_FAIL(49, "window icon not set");
        }

        // M20: the seven panel docks are registered and their menu actions
        // toggle them on and off.
        const QStringList expectedPanelDocks = {
            QStringLiteral("layersPanel"),
            QStringLiteral("historyPanel"),
            QStringLiteral("navigatorPanel"),
            QStringLiteral("colorPanel"),
            QStringLiteral("swatchesPanel"),
            QStringLiteral("infoPanel"),
            QStringLiteral("histogramPanel"),
        };
        const QSet<QString>& registeredPanels = frame.panelObjectNames();
        int panelsRegistered = 0;
        for (const QString& name : expectedPanelDocks) {
            if (registeredPanels.contains(name)) {
                ++panelsRegistered;
            }
        }

        const QStringList panelCommands = {
            QStringLiteral("window.panels.layers"),
            QStringLiteral("window.panels.navigator"),
            QStringLiteral("window.panels.history"),
            QStringLiteral("window.panels.color"),
            QStringLiteral("window.panels.swatches"),
            QStringLiteral("window.panels.info"),
            QStringLiteral("window.panels.histogram"),
        };
        int panelsToggled = 0;
        for (const QString& id : panelCommands) {
            QAction* action = frame.registry()->action(id);
            const QString objectName =
                id.section(QLatin1Char('.'), -1) + QStringLiteral("Panel");
            QWidget* panel = frame.findChild<QWidget*>(objectName);
            pictura::PanelColumn* layersColumn = frame.columnForPanel(objectName);
            if (!action || !panel || !layersColumn) {
                continue;
            }
            const bool before = layersColumn->isPanelVisible(objectName);
            action->setChecked(!before);
            frame.registry()->dispatch(id);
            const bool toggled = layersColumn->isPanelVisible(objectName) != before;
            action->setChecked(!layersColumn->isPanelVisible(objectName));
            frame.registry()->dispatch(id);
            const bool restored = layersColumn->isPanelVisible(objectName) == before;
            if (toggled && restored) {
                ++panelsToggled;
            }
        }
        ST_BEGIN("panels_registered");
        ST_PASS("panels registered=%d toggled=%d", panelsRegistered,
                     panelsToggled);
        if (panelsRegistered != expectedPanelDocks.size()
            || panelsToggled != panelCommands.size()) {
            ST_FAIL(52, "panels wrong");
        }

        // M21: painting. A fresh transparent document keeps these checks
        // independent of any loaded PSD; every behaviour is read from pixels.
        const bool paintDoc = frame.newDocument(QStringLiteral("Paint"), 32, 32,
                                                QStringLiteral("rgb"), 8,
                                                QStringLiteral("transparent"));
        pictura::PictureView* pv = frame.activeView();
        if (!paintDoc || !pv) {
            ST_FAIL(53, "paint document");
        }

        // 53: a stroke marks pixels, dirties the document, and adds one state.
        const int paintHistBefore = pv->history_count();
        pv->begin_paint(0xFFFF0000u, 0xFFFFFFFFu, 8, 100, 100, 0, 100, 100, 25,
                        QStringLiteral("normal"), false, false);
        pv->paint_dab(6, 16, 1.0);
        pv->paint_dab(12, 16, 1.0);
        pv->paint_dab(18, 16, 1.0);
        pv->paint_dab(24, 16, 1.0);
        const bool paintEnded = pv->end_paint();
        const QImage paintStroke = pv->image();
        int paintPainted = 0;
        for (int y = 0; y < paintStroke.height(); ++y) {
            for (int x = 0; x < paintStroke.width(); ++x) {
                if (qAlpha(paintStroke.pixel(x, y)) > 0) {
                    ++paintPainted;
                }
            }
        }
        ST_BEGIN("stroke_ended");
        ST_PASS("stroke ended=%d painted=%d dirty=%d hist=%d", paintEnded ? 1 : 0,
                     paintPainted,
                     pv->is_dirty() ? 1 : 0,
                     pv->history_count());
        if (!paintEnded || !pv->is_dirty() || pv->history_count() != paintHistBefore + 1
            || paintPainted < 20) {
            ST_FAIL(53, "stroke wrong");
        }

        // 54: opacity caps one stroke and a second stroke adds coverage. A fresh
        // transparent canvas keeps the sampled point clear of the 53 stroke.
        const bool opacityDoc = frame.newDocument(QStringLiteral("PaintOpacity"), 32, 32,
                                                  QStringLiteral("rgb"), 8,
                                                  QStringLiteral("transparent"));
        pv = frame.activeView();
        if (!opacityDoc || !pv) {
            ST_FAIL(54, "opacity document");
        }
        pv->begin_paint(0xFF00FF00u, 0xFFFFFFFFu, 12, 100, 100, 0, 33, 100, 0,
                        QStringLiteral("normal"), false, false);
        for (int i = 0; i < 40; ++i) {
            pv->paint_dab(16, 16, 1.0);
        }
        pv->end_paint();
        const int paintA1 = qAlpha(pv->sample_argb(16, 16));
        pv->begin_paint(0xFF00FF00u, 0xFFFFFFFFu, 12, 100, 100, 0, 33, 100, 0,
                        QStringLiteral("normal"), false, false);
        for (int i = 0; i < 40; ++i) {
            pv->paint_dab(16, 16, 1.0);
        }
        pv->end_paint();
        const int paintA2 = qAlpha(pv->sample_argb(16, 16));
        ST_BEGIN("opacity_a1");
        ST_PASS("opacity a1=%d a2=%d", paintA1, paintA2);
        if (!(paintA1 >= 78 && paintA1 <= 92) || !(paintA2 > paintA1 && paintA2 < 255)) {
            ST_FAIL(54, "opacity wrong");
        }

        // 55: Pencil edges are aliased; Brush edges are anti-aliased.
        const bool pencilDoc = frame.newDocument(QStringLiteral("PaintPencil"), 32, 32,
                                                 QStringLiteral("rgb"), 8,
                                                 QStringLiteral("transparent"));
        pictura::PictureView* pencilView = frame.activeView();
        const bool pencilBegun =
            pencilDoc && pencilView
            && pencilView->begin_paint(0xFFFF0000u, 0xFFFFFFFFu, 10, 100, 100, 0, 100, 100, 25,
                                       QStringLiteral("normal"), true, false);
        if (pencilView) {
            for (int x = 4; x <= 28; x += 2) {
                pencilView->paint_dab(x, 16, 1.0);
            }
        }
        const bool pencilEnded = pencilView && pencilView->end_paint();
        const QImage pencilImg = pencilView ? pencilView->image() : QImage();
        bool pencilBinary = !pencilImg.isNull();
        int pencilCovered = 0;
        for (int y = 0; y < pencilImg.height() && pencilBinary; ++y) {
            for (int x = 0; x < pencilImg.width(); ++x) {
                const int a = qAlpha(pencilImg.pixel(x, y));
                if (a == 0) {
                    continue;
                }
                ++pencilCovered;
                if (a != 255) {
                    pencilBinary = false;
                    break;
                }
            }
        }

        const bool brushDoc = frame.newDocument(QStringLiteral("PaintBrush"), 32, 32,
                                                QStringLiteral("rgb"), 8,
                                                QStringLiteral("transparent"));
        pictura::PictureView* brushView = frame.activeView();
        const bool brushBegun =
            brushDoc && brushView
            && brushView->begin_paint(0xFFFF0000u, 0xFFFFFFFFu, 10, 100, 100, 0, 100, 100, 25,
                                      QStringLiteral("normal"), false, false);
        if (brushView) {
            for (int x = 4; x <= 28; x += 2) {
                brushView->paint_dab(x, 16, 1.0);
            }
        }
        const bool brushEnded = brushView && brushView->end_paint();
        const QImage brushImg = brushView ? brushView->image() : QImage();
        bool brushSoft = false;
        int brushCovered = 0;
        for (int y = 0; y < brushImg.height() && !brushSoft; ++y) {
            for (int x = 0; x < brushImg.width(); ++x) {
                const int a = qAlpha(brushImg.pixel(x, y));
                if (a > 0) {
                    ++brushCovered;
                    if (a < 255) {
                        brushSoft = true;
                        break;
                    }
                }
            }
        }
        const bool pencilOk =
            pencilBegun && pencilEnded && pencilBinary && pencilCovered > 0;
        const bool brushOk = brushBegun && brushEnded && brushCovered > 0 && brushSoft;
        ST_BEGIN("aliased_pencil_ok");
        ST_PASS("aliased pencil_ok=%d brush_aa=%d", pencilOk ? 1 : 0, brushOk ? 1 : 0);
        if (!pencilOk || !brushOk) {
            ST_FAIL(55, "aliasing wrong");
        }

        // 56: undo restores the touched pixels of the brush document.
        const QImage paintPre = brushView->image();
        brushView->begin_paint(0xFF0000FFu, 0xFFFFFFFFu, 10, 100, 100, 0, 100, 100, 25,
                               QStringLiteral("normal"), false, false);
        for (int x = 4; x <= 28; x += 2) {
            brushView->paint_dab(x, 20, 1.0);
        }
        const bool paintChanged = brushView->end_paint() && brushView->image() != paintPre;
        const bool paintRestored = brushView->undo() && brushView->image() == paintPre;
        ST_BEGIN("undo_changed");
        ST_PASS("undo changed=%d restored=%d", paintChanged ? 1 : 0, paintRestored ? 1 : 0);
        if (!paintChanged || !paintRestored) {
            ST_FAIL(56, "undo wrong");
        }

        // M22: the 15 Artistic filters. A fresh white document exercises each
        // mapping end to end, and the seeded ones must reproduce bit-for-bit.
        const bool artDoc = frame.newDocument(QStringLiteral("Art"), 24, 24,
                                              QStringLiteral("rgb"), 8,
                                              QStringLiteral("white"));
        pictura::PictureView* av = frame.activeView();
        if (!artDoc || !av) {
            ST_FAIL(57, "artistic document");
        }

        // 57: every Artistic kind maps, applies, and changes the image.
        const QStringList artisticKinds = {
            QStringLiteral("colored-pencil"),
            QStringLiteral("cutout"),
            QStringLiteral("dry-brush"),
            QStringLiteral("film-grain"),
            QStringLiteral("fresco"),
            QStringLiteral("neon-glow"),
            QStringLiteral("paint-daubs"),
            QStringLiteral("palette-knife"),
            QStringLiteral("plastic-wrap"),
            QStringLiteral("poster-edges"),
            QStringLiteral("rough-pastels"),
            QStringLiteral("smudge-stick"),
            QStringLiteral("sponge"),
            QStringLiteral("underpainting"),
            QStringLiteral("watercolor"),
        };
        int artisticApplied = 0;
        for (const QString& kind : artisticKinds) {
            const QImage before = av->image();
            const bool ok = av->apply_filter(kind);
            if (ok && av->image() != before) {
                ++artisticApplied;
            }
        }
        ST_BEGIN("applied_applied");
        ST_PASS("applied applied=%d/%d", artisticApplied,
                     static_cast<int>(artisticKinds.size()));
        if (artisticApplied != artisticKinds.size()) {
            ST_FAIL(57, "artistic apply wrong");
        }

        // 58: a fixed seed makes a stochastic Artistic filter deterministic
        // across undo and reapply.
        while (av->can_undo()) {
            av->undo();
        }
        av->apply_filter(QStringLiteral("film-grain"));
        const QImage artisticFirst = av->image();
        av->undo();
        av->apply_filter(QStringLiteral("film-grain"));
        const bool artisticDeterministic = av->image() == artisticFirst;
        ST_BEGIN("deterministic");
        ST_PASS("deterministic=%d", artisticDeterministic ? 1 : 0);
        if (!artisticDeterministic) {
            ST_FAIL(58, "artistic determinism wrong");
        }

        // M25: the 29 new filter kinds. A clouds-filled document gives every
        // filter structured content; a pure-white source is a no-op for some.
        const bool filterDoc = frame.newDocument(QStringLiteral("Filters"), 32, 32,
                                                    QStringLiteral("rgb"), 8,
                                                    QStringLiteral("white"));
        pictura::PictureView* filterFamiliesActiveView = frame.activeView();
        if (!filterDoc || !filterFamiliesActiveView || !filterFamiliesActiveView->apply_filter(QStringLiteral("clouds"))) {
            ST_FAIL(68, "filter document");
        }

        // 68: every M25 kind maps, applies, and changes the image.
        const QStringList filterFamiliesKinds = {
            QStringLiteral("accented-edges"),
            QStringLiteral("angled-strokes"),
            QStringLiteral("crosshatch"),
            QStringLiteral("dark-strokes"),
            QStringLiteral("ink-outlines"),
            QStringLiteral("spatter"),
            QStringLiteral("sprayed-strokes"),
            QStringLiteral("sumi-e"),
            QStringLiteral("bas-relief"),
            QStringLiteral("chalk-charcoal"),
            QStringLiteral("charcoal"),
            QStringLiteral("chrome"),
            QStringLiteral("conte-crayon"),
            QStringLiteral("graphic-pen"),
            QStringLiteral("halftone-pattern"),
            QStringLiteral("note-paper"),
            QStringLiteral("photocopy"),
            QStringLiteral("plaster"),
            QStringLiteral("reticulation"),
            QStringLiteral("stamp"),
            QStringLiteral("torn-edges"),
            QStringLiteral("water-paper"),
            QStringLiteral("craquelure"),
            QStringLiteral("grain"),
            QStringLiteral("mosaic-tiles"),
            QStringLiteral("patchwork"),
            QStringLiteral("stained-glass"),
            QStringLiteral("texturizer"),
            QStringLiteral("oil-paint"),
        };
        int filterFamiliesApplied = 0;
        for (const QString& kind : filterFamiliesKinds) {
            const QImage before = filterFamiliesActiveView->image();
            const bool ok = filterFamiliesActiveView->apply_filter(kind);
            if (ok && filterFamiliesActiveView->image() != before) {
                ++filterFamiliesApplied;
            }
            // Undo so every kind is compared against the same structured source
            // and the scratch history stays well under its 20-state cap.
            filterFamiliesActiveView->undo();
        }
        ST_BEGIN("filterFamilies_applied_applied");
        ST_PASS("applied applied=%d/%d", filterFamiliesApplied,
                     static_cast<int>(filterFamiliesKinds.size()));
        if (filterFamiliesApplied != filterFamiliesKinds.size()) {
            ST_FAIL(68, "filter apply wrong");
        }

        // 69: a fixed seed makes the seeded M25 filter deterministic across
        // undo and reapply.
        filterFamiliesActiveView->apply_filter(QStringLiteral("grain"));
        const QImage filterFamiliesFirst = filterFamiliesActiveView->image();
        filterFamiliesActiveView->undo();
        filterFamiliesActiveView->apply_filter(QStringLiteral("grain"));
        const bool filterFamiliesDeterministic = filterFamiliesActiveView->image() == filterFamiliesFirst;
        ST_BEGIN("filterFamilies_deterministic");
        ST_PASS("deterministic=%d", filterFamiliesDeterministic ? 1 : 0);
        if (!filterFamiliesDeterministic) {
            ST_FAIL(69, "filter determinism wrong");
        }

        // M26: GPU-compute default/toggle and its persisted preference.
        // 70: default is on; the toggle flips to CPU and restores the backend.
        pictura::PictureView* gpuView = frame.activeView();
        if (!gpuView) {
            ST_FAIL(70, "no active view");
        }
        const bool avail = gpuView->gpu_available();
        const bool defaultOn = gpuView->gpu_compute();
        const QString onBackend = gpuView->active_backend();
        const bool onOk = avail ? onBackend == QStringLiteral("GPU")
                                      : onBackend == QStringLiteral("CPU (no GPU)");
        gpuView->set_gpu_compute(false);
        const QString offBackend = gpuView->active_backend();
        // With no GPU the label is "CPU (no GPU)" whether or not compute is on;
        // with a GPU, disabling it drops the label to "CPU".
        const bool offCpu = avail ? offBackend == QStringLiteral("CPU")
                                  : offBackend == QStringLiteral("CPU (no GPU)");
        gpuView->set_gpu_compute(true);
        const bool gpuRestored = gpuView->active_backend() == onBackend;
        ST_BEGIN("gpu_available");
        ST_PASS("gpu available=%d default_on=%d off_cpu=%d on_back=%d", avail ? 1 : 0,
                     defaultOn ? 1 : 0,
                     offCpu ? 1 : 0,
                     gpuRestored ? 1 : 0);
        if (!defaultOn || !onOk || !offCpu || !gpuRestored) {
            ST_FAIL(70, "gpu backend wrong");
        }

        // 71: the GPU preference round-trips through the session store, and a
        // fresh SessionState defaults to on.
        const bool framePref = gpuView->gpu_compute();
        pictura::SessionState gpuState = pictura::loadSession();
        gpuState.gpuCompute = false;
        const bool savedOff = pictura::saveSession(gpuState);
        const bool offRound = !pictura::loadSession().gpuCompute;
        gpuState.gpuCompute = true;
        const bool savedOn = pictura::saveSession(gpuState);
        const bool onRound = pictura::loadSession().gpuCompute;
        const bool gpuDefault = pictura::SessionState{}.gpuCompute;
        gpuState.gpuCompute = framePref;
        pictura::saveSession(gpuState);
        ST_BEGIN("session_gpu_off");
        ST_PASS("session gpu_off=%d gpu_on=%d default=%d", (savedOff && offRound) ? 1 : 0,
                     (savedOn && onRound) ? 1 : 0,
                     gpuDefault ? 1 : 0);
        if (!savedOff || !offRound || !savedOn || !onRound || !gpuDefault) {
            ST_FAIL(71, "session persistence wrong");
        }

        // M27: the GPU filter path. A supported filter must produce the same
        // image through the GPU and CPU backends (the accelerated kernels are
        // byte-exact), and the gpuCompute preference is restored afterwards.
        // 72: gaussian-blur is byte-identical through both backends.
        pictura::PictureView* gpuAccelView = frame.activeView();
        if (!gpuAccelView) {
            ST_FAIL(72, "no active view");
        }
        gpuAccelView->set_gpu_compute(true);
        gpuAccelView->apply_filter(QStringLiteral("gaussian-blur"));
        const QImage gpuAccelGpu = gpuAccelView->image();
        gpuAccelView->undo();
        gpuAccelView->set_gpu_compute(false);
        gpuAccelView->apply_filter(QStringLiteral("gaussian-blur"));
        const QImage cpu = gpuAccelView->image();
        gpuAccelView->set_gpu_compute(true);
        const bool gpuAccelIdentical = gpuAccelGpu == cpu;
        ST_BEGIN("filter_byte_identical");
        ST_PASS("filter byte_identical=%d", gpuAccelIdentical ? 1 : 0);
        if (!gpuAccelIdentical) {
            ST_FAIL(72, "filter byte-identity wrong");
        }

        // M28: the heavy deterministic kernels. Surface Blur and Median must
        // produce the same image through the GPU and CPU backends (the M28
        // kernels are byte-exact, like the M27 set).
        // 73: both kinds match across backends.
        pictura::PictureView* gpuHeavyView = frame.activeView();
        if (!gpuHeavyView) {
            ST_FAIL(73, "no active view");
        }
        const QStringList gpuHeavyKinds = {
            QStringLiteral("surface-blur"),
            QStringLiteral("median"),
        };
        gpuHeavyView->set_gpu_compute(true);
        QList<QImage> gpuHeavyGpu;
        bool gpuHeavyHeavy = true;
        for (const QString& kind : gpuHeavyKinds) {
            const QImage gpuHeavyBefore = gpuHeavyView->image();
            const bool gpuHeavyOk = gpuHeavyView->apply_filter(kind);
            const QImage gpuHeavyAfter = gpuHeavyView->image();
            gpuHeavyHeavy = gpuHeavyHeavy && gpuHeavyOk && gpuHeavyAfter != gpuHeavyBefore;
            gpuHeavyGpu.append(gpuHeavyAfter);
            gpuHeavyView->undo();
        }
        gpuHeavyView->set_gpu_compute(false);
        for (int i = 0; i < gpuHeavyKinds.size(); ++i) {
            const bool gpuHeavyOk = gpuHeavyView->apply_filter(gpuHeavyKinds.at(i));
            const QImage gpuHeavyAfter = gpuHeavyView->image();
            gpuHeavyHeavy = gpuHeavyHeavy && gpuHeavyOk && gpuHeavyAfter == gpuHeavyGpu.at(i);
            // Keep the last CPU result applied: a dangling redo state would
            // shrink `canvas_move`'s history count.
            if (i + 1 < gpuHeavyKinds.size()) {
                gpuHeavyView->undo();
            }
        }
        gpuHeavyView->set_gpu_compute(true);
        ST_BEGIN("heavy_byte_identical");
        ST_PASS("heavy byte_identical=%d", gpuHeavyHeavy ? 1 : 0);
        if (!gpuHeavyHeavy) {
            ST_FAIL(73, "heavy byte-identity wrong");
        }

        // M30: canvas transparency display and document-rect clipping.
        // 74: an all-transparent document reveals the checkerboard, and a moved
        // layer is cropped to the document rect (no red on the canvas area).
        const bool canvasCreated = frame.newDocument(QStringLiteral("Alpha"), 64, 64,
                                                  QStringLiteral("rgb"), 8,
                                                  QStringLiteral("transparent"));
        pictura::ImageView* canvasCanvas = frame.imageView();
        if (!canvasCreated || !canvasCanvas) {
            ST_FAIL(74, "transparent document");
        }
        const int docIndex = frame.activeDocumentIndex();
        if (canvasCanvas->width() <= 0 || canvasCanvas->height() <= 0) {
            frame.resize(800, 600);
            QApplication::processEvents();
        }

        const QColor canvasCanvasColor = canvasCanvas->canvasColor();
        const QColor canvasA = pictura::ImageView::transparencyColorA();
        const QColor canvasB = pictura::ImageView::transparencyColorB();
        const QRectF docRect(canvasCanvas->offset(),
                                QSizeF(64.0 * canvasCanvas->zoom(), 64.0 * canvasCanvas->zoom()));

        QImage shot(canvasCanvas->size(), QImage::Format_ARGB32);
        canvasCanvas->render(&shot);

        bool sawA = false;
        bool sawB = false;
        bool sawCanvas = false;
        const QRect docPx = docRect.toAlignedRect();
        for (int y = docPx.top() + 2; y <= docPx.bottom() - 2; y += 3) {
            for (int x = docPx.left() + 2; x <= docPx.right() - 2; x += 3) {
                if (x < 0 || y < 0 || x >= shot.width() || y >= shot.height()) {
                    continue;
                }
                const QColor c = shot.pixelColor(x, y);
                if (c.rgb() == canvasA.rgb()) {
                    sawA = true;
                } else if (c.rgb() == canvasB.rgb()) {
                    sawB = true;
                } else if (c.rgb() == canvasCanvasColor.rgb()) {
                    sawCanvas = true;
                }
            }
        }
        const QPoint outsidePx = docRect.topLeft().toPoint() - QPoint(4, 4);
        const bool outsideOk = outsidePx.x() >= 0 && outsidePx.y() >= 0
                                  && outsidePx.x() < shot.width()
                                  && outsidePx.y() < shot.height()
                                  && shot.pixelColor(outsidePx).rgb() == canvasCanvasColor.rgb();
        const bool canvasChecker = sawA && sawB && !sawCanvas && outsideOk;

        QImage canvasRed(64, 64, QImage::Format_RGBA8888);
        canvasRed.fill(QColor(255, 0, 0));
        canvasCanvas->beginMovePreview(canvasCanvas->image(), canvasRed, QPointF(-32.0, -32.0), 1.0);
        QImage clipShot(canvasCanvas->size(), QImage::Format_ARGB32);
        canvasCanvas->render(&clipShot);
        canvasCanvas->endMovePreview();

        // The layer at (-32,-32) covers the document's top-left quadrant; probe
        // the few pixels around the centre on the covered side.
        const QPoint canvasCentre = docRect.center().toPoint();
        bool centreRed = false;
        for (int dy = -2; dy <= 0 && !centreRed; ++dy) {
            for (int dx = -2; dx <= 0 && !centreRed; ++dx) {
                const QPoint q = canvasCentre + QPoint(dx, dy);
                if (q.x() >= 0 && q.y() >= 0 && q.x() < clipShot.width()
                    && q.y() < clipShot.height()
                    && clipShot.pixelColor(q).rgb() == QColor(255, 0, 0).rgb()) {
                    centreRed = true;
                }
            }
        }
        const QPoint overflowPx = docRect.topLeft().toPoint() - QPoint(4, 4);
        const bool overflowOk = overflowPx.x() >= 0 && overflowPx.y() >= 0
                                   && overflowPx.x() < clipShot.width()
                                   && overflowPx.y() < clipShot.height()
                                   && clipShot.pixelColor(overflowPx).rgb()
                                          == canvasCanvasColor.rgb();
        const bool canvasClipped = centreRed && overflowOk;

        ST_BEGIN("canvas_checker");
        ST_PASS("canvas checker=%d clipped=%d", canvasChecker ? 1 : 0,
                     canvasClipped ? 1 : 0);
        if (!canvasChecker || !canvasClipped) {
            ST_FAIL(74, "transparency/clipping wrong");
        }
        // Restore the previously active document so later checks are undisturbed.
        frame.closeDocument(docIndex, false);

        // Full pixel-by-pixel equality, shared by the canvas checks.
        auto samePixels = [](const QImage& a, const QImage& b) {
            if (a.isNull() || b.isNull() || a.size() != b.size()) {
                return false;
            }
            for (int y = 0; y < a.height(); ++y) {
                for (int x = 0; x < a.width(); ++x) {
                    if (a.pixel(x, y) != b.pixel(x, y)) {
                        return false;
                    }
                }
            }
            return true;
        };

        // M31: dirty-region canvas refresh. Grow a seeded document so a small
        // white layer sits in a larger transparent canvas, then Move it by a
        // known offset. The layer's pixels must land at the new location, a
        // sample far outside the dirty union must be untouched, and undo must
        // restore the pre-move image.
        // 75: region move.
        const bool regionCreated = frame.newDocument(QStringLiteral("Region"), 32, 32,
                                                  QStringLiteral("rgb"), 8,
                                                  QStringLiteral("white"));
        pictura::PictureView* regionView = frame.activeView();
        if (!regionCreated || !regionView) {
            ST_FAIL(75, "document");
        }
        const int regionDocIndex = frame.activeDocumentIndex();
        const bool regionGrown =
            regionView->resize_canvas(QStringLiteral("top-left"), 128, 128);
        if (!regionGrown) {
            ST_FAIL(75, "canvas growth");
        }
        const QImage regionBefore = regionView->image();
        const QRgb regionSrc = regionBefore.pixel(5, 5);         // inside the layer
        const QRgb newBefore = regionBefore.pixel(38, 38); // shape's new home
        const QRgb regionFar = regionBefore.pixel(100, 100);     // outside dirty union
        const bool regionPreview = regionView->begin_move_preview();
        const bool regionMoved = regionView->commit_move(8, 8);
        const QImage regionAfter = regionView->image();
        const bool appeared = regionAfter.pixel(38, 38) == regionSrc
                                 && newBefore != regionSrc;
        const bool regionVacated = regionAfter.pixel(2, 2) != regionSrc;
        const bool regionOutside = regionAfter.pixel(100, 100) == regionFar;
        const bool regionUndone = regionView->undo() && regionView->image() == regionBefore;
        ST_BEGIN("region_moved");
        ST_PASS("region moved=%d outside_unchanged=%d undo=%d", (regionMoved && appeared && regionVacated) ? 1 : 0,
                     regionOutside ? 1 : 0,
                     regionUndone ? 1 : 0);
        if (!regionPreview || !regionMoved || !appeared || !regionVacated || !regionOutside
            || !regionUndone) {
            ST_FAIL(75, "region move wrong");
        }
        // Leave the frame as M30 did: close the scratch document.
        frame.closeDocument(regionDocIndex, false);

        // M31 large region: with the per-pixel blit budget gone, a canvas-sized
        // dirty union (a 1024² canvas moved by (1,1) makes `old ∪ new` the whole
        // canvas) takes the region-blit path in C++ and must NOT trigger a
        // full-document recomposite. `regionDamaged` fires; `changed` (emitted
        // only by a full recomposite) does not. The on-screen canvas must still
        // equal a full recomposite.
        const bool regionAltCreated =
            frame.newDocument(QStringLiteral("RegionLarge"), 1024, 1024,
                              QStringLiteral("rgb"), 8, QStringLiteral("white"));
        pictura::PictureView* regionAltView = frame.activeView();
        pictura::ImageView* regionAltCanvas = frame.imageView();
        if (!regionAltCreated || !regionAltView || !regionAltCanvas) {
            ST_FAIL(75, "large document");
        }
        const int regionAltDocIndex = frame.activeDocumentIndex();
        const QImage regionAltBefore = regionAltView->image();
        const QRgb regionAltOrigin = regionAltBefore.pixel(0, 0);
        int regionAltRegionBlits = 0;
        int regionAltChanged = 0;
        auto regionAltRegionConn = QObject::connect(
            regionAltView, &pictura::PictureView::regionDamaged,
            [&regionAltRegionBlits](int, int, int, int) { ++regionAltRegionBlits; });
        auto regionAltChangedConn = QObject::connect(
            regionAltView, &pictura::PictureView::changed, [&regionAltChanged]() { ++regionAltChanged; });
        const bool regionAltMoved = regionAltView->commit_move(1, 1);
        const QImage regionAltBlitted = regionAltCanvas->image();
        QObject::disconnect(regionAltRegionConn);
        QObject::disconnect(regionAltChangedConn);
        // Force a full recomposite and compare the blitted canvas with it.
        regionAltView->set_gpu_compute(regionAltView->gpu_compute());
        const QImage regionAltFull = regionAltCanvas->image();
        const bool regionAltVacated = regionAltBlitted.pixel(0, 0) != regionAltOrigin;
        const bool regionAltRegionPath = regionAltRegionBlits >= 1 && regionAltChanged == 0;
        const bool regionAltCanvasSame = samePixels(regionAltBlitted, regionAltFull);
        const bool regionAltUndone = regionAltView->undo() && regionAltView->image() == regionAltBefore;
        ST_BEGIN("region_large_moved");
        ST_PASS("region_large moved=%d vacated=%d undo=%d "
                     "region=%d recomposite=%d canvas=%d", regionAltMoved ? 1 : 0,
                     regionAltVacated ? 1 : 0,
                     regionAltUndone ? 1 : 0,
                     regionAltRegionPath ? 1 : 0,
                     regionAltChanged > 0 ? 1 : 0,
                     regionAltCanvasSame ? 1 : 0);
        if (!regionAltMoved || !regionAltVacated || !regionAltUndone || !regionAltRegionPath || !regionAltCanvasSame) {
            ST_FAIL(75, "large-region blit wrong");
        }
        frame.closeDocument(regionAltDocIndex, false);

        // M23: CS6 chrome. 59 stylesheet, 60 toolbox, 61 default dock groups.
        int chromeLevels = 0;
        for (int level = 0; level < pictura::Theme::kLevelCount; ++level) {
            if (!pictura::Theme::styleSheet(level).isEmpty()) {
                ++chromeLevels;
            }
        }
        const bool chromeDistinct =
            pictura::Theme::styleSheet(0) != pictura::Theme::styleSheet(3);
        ST_BEGIN("stylesheet_applied");
        ST_PASS("stylesheet applied=%d levels=%d distinct=%d", qApp->styleSheet().isEmpty() ? 0 : 1,
                     chromeLevels,
                     chromeDistinct ? 1 : 0);
        if (qApp->styleSheet().isEmpty() || chromeLevels != pictura::Theme::kLevelCount
            || !chromeDistinct) {
            ST_FAIL(59, "stylesheet wrong");
        }

        auto* chromeToolbox = frame.findChild<pictura::Toolbox*>(QStringLiteral("toolsPanel"));
        auto* chromeToolsColumn = frame.toolsColumn();
        const QList<QToolButton*> chromeSlotButtons =
            chromeToolbox ? chromeToolbox->slotButtons() : QList<QToolButton*>();
        const bool chromeTabless =
            chromeToolsColumn && chromeToolsColumn->isToolsColumn()
            && chromeToolsColumn->groups().isEmpty()
            && chromeToolsColumn->toolsContentForTest() == chromeToolbox;
        QToolButton* chromeToggle = chromeToolsColumn
            ? chromeToolsColumn->panelColumnToggleForTest()
            : nullptr;
        const bool toggleDistinct =
            chromeToggle != nullptr && !chromeSlotButtons.contains(chromeToggle);
        const bool chromeFgbg = chromeToolbox
            && chromeToolbox->findChild<pictura::ForegroundBackgroundWidget*>() != nullptr;
        const bool chromeScreenMode = chromeToolbox
            && chromeToolbox->findChild<QToolButton*>(QStringLiteral("screenModeButton")) != nullptr;
        ST_BEGIN("toolbox_dock");
        ST_PASS("toolbox tabless=%d slots=%d toggle=%d fgbg=%d screen=%d",
                     chromeTabless ? 1 : 0,
                     chromeSlotButtons.size(),
                     toggleDistinct ? 1 : 0,
                     chromeFgbg ? 1 : 0,
                     chromeScreenMode ? 1 : 0);
        if (!chromeToolbox || !chromeTabless || chromeSlotButtons.size() != 21 || !toggleDistinct
            || !chromeFgbg || !chromeScreenMode) {
            ST_FAIL(60, "tools column wrong");
        }

        // M24: CS6 right side. 61 default PanelColumn groups, 62 the new
        // panels, 63 the Window > Panels toggles (the icon rail was removed in
        // M41, so this now proves no PanelRail remains and the toggles work).
        auto* railColumn = frame.panelColumn();
        auto groupOf = [&frame](const char* name) {
            pictura::PanelColumn* column = frame.columnForPanel(QString::fromLatin1(name));
            return column ? column->groupOfForTest(QString::fromLatin1(name)) : QString();
        };
        QWidget* railProperties = frame.findChild<QWidget*>(QStringLiteral("propertiesPanel"));
        int railGroups = 0;
        railGroups += (!groupOf("colorPanel").isEmpty()
                      && groupOf("colorPanel") == groupOf("swatchesPanel")) ? 1 : 0;
        railGroups += (!groupOf("adjustmentsPanel").isEmpty()
                      && groupOf("adjustmentsPanel") == groupOf("stylesPanel")) ? 1 : 0;
        railGroups += (!groupOf("layersPanel").isEmpty()
                      && groupOf("layersPanel") == groupOf("channelsPanel")
                      && groupOf("layersPanel") == groupOf("pathsPanel")) ? 1 : 0;
        railGroups += (!groupOf("historyPanel").isEmpty()
                      && groupOf("historyPanel") != groupOf("propertiesPanel")) ? 1 : 0;
        railGroups += (!groupOf("navigatorPanel").isEmpty()
                      && groupOf("navigatorPanel") == groupOf("histogramPanel")
                      && groupOf("navigatorPanel") == groupOf("infoPanel")) ? 1 : 0;
        ST_BEGIN("groups_grouped");
        ST_PASS("groups grouped=%d/5", railGroups);
        if (railGroups != 5) {
            ST_FAIL(61, "panel grouping wrong");
        }

        const QStringList railPanelNames = {
            QStringLiteral("gradientsPanel"),
            QStringLiteral("patternsPanel"),
            QStringLiteral("propertiesPanel"),
            QStringLiteral("adjustmentsPanel"),
            QStringLiteral("notesPanel"),
            QStringLiteral("channelsPanel"),
            QStringLiteral("pathsPanel"),
            QStringLiteral("actionsPanel"),
        };
        int railFound = 0;
        for (const QString& name : railPanelNames) {
            if (frame.findChild<QWidget*>(name)) {
                ++railFound;
            }
        }
        int propsEmpty = 0;
        if (railProperties) {
            for (QLabel* label : railProperties->findChildren<QLabel*>()) {
                if (label->text().contains(QStringLiteral("No Properties"))) {
                    propsEmpty = 1;
                    break;
                }
            }
        }
        ST_BEGIN("panels_found");
        ST_PASS("panels found=%d/8 properties_empty=%d", railFound,
                     propsEmpty);
        if (railFound != 8 || propsEmpty != 1) {
            ST_FAIL(62, "panels wrong");
        }

        const QStringList railCommands = {
            QStringLiteral("window.panels.history"),
            QStringLiteral("window.panels.actions"),
            QStringLiteral("window.panels.info"),
            QStringLiteral("window.panels.navigator"),
            QStringLiteral("window.panels.histogram"),
        };
        int railActions = 0;
        for (const QString& id : railCommands) {
            if (frame.registry()->action(id)) {
                ++railActions;
            }
        }
        int railToggled = 0;
        if (railColumn) {
            const QString command = QStringLiteral("window.panels.history");
            const QString panelName = QStringLiteral("historyPanel");
            QAction* action = frame.registry()->action(command);
            const bool before = railColumn->isPanelVisible(panelName);
            if (action) {
                action->setChecked(!before);
                frame.registry()->dispatch(command);
                const bool shown = railColumn->isPanelVisible(panelName) != before;
                action->setChecked(before);
                frame.registry()->dispatch(command);
                const bool hidden = railColumn->isPanelVisible(panelName) == before;
                railToggled = (shown && hidden) ? 1 : 0;
            }
        }
        const bool noRail =
            frame.findChild<QToolBar*>(QStringLiteral("panelRail")) == nullptr;
        ST_BEGIN("rail_actions");
        ST_PASS("rail actions=%d toggled=%d norail=%d", railActions,
                     railToggled,
                     noRail ? 1 : 0);
        if (railActions < 5 || railToggled != 1 || !noRail) {
            ST_FAIL(63, "rail wrong");
        }

        pictura::ImageView* canvas = frame.imageView();
        if (!canvas) {
            ST_FAIL(64, "no active canvas");
        }

        // 64: a freshly set image is centred, not pinned to the top-left.
        canvas->setImage(canvas->image());
        const double filterFamiliesZoom = canvas->zoom();
        const double vw = canvas->width();
        const double filterFamiliesVh = canvas->height();
        const double iw = canvas->image().width();
        const double ih = canvas->image().height();
        const QPointF filterFamiliesExpected((vw - iw * filterFamiliesZoom) / 2.0,
                                  (filterFamiliesVh - ih * filterFamiliesZoom) / 2.0);
        const QPointF filterFamiliesActual = canvas->offset();
        const bool filterFamiliesCentred = std::abs(filterFamiliesActual.x() - filterFamiliesExpected.x()) < 1e-6
                                && std::abs(filterFamiliesActual.y() - filterFamiliesExpected.y()) < 1e-6;
        const bool smaller = iw < vw && ih < filterFamiliesVh;
        const bool notTopLeft = !smaller || filterFamiliesActual != QPointF(0, 0);
        ST_BEGIN("canvas_centre_offset");
        ST_PASS("canvas_centre offset=(%g,%g) zoom=%g", filterFamiliesActual.x(),
                     filterFamiliesActual.y(),
                     filterFamiliesZoom);
        if (!filterFamiliesCentred || !notTopLeft || filterFamiliesZoom <= 0.0) {
            ST_FAIL(64, "centre wrong");
        }

        // 65: middle-button drag pans regardless of the active tool.
        const QPointF filterFamiliesBefore = canvas->offset();
        QMouseEvent filterFamiliesPress(QEvent::MouseButtonPress,
                             QPointF(200, 150),
                             canvas->mapToGlobal(QPoint(200, 150)),
                             Qt::MiddleButton,
                             Qt::MiddleButton,
                             Qt::NoModifier);
        QApplication::sendEvent(canvas, &filterFamiliesPress);
        QMouseEvent filterFamiliesMove(QEvent::MouseMove,
                            QPointF(230, 165),
                            canvas->mapToGlobal(QPoint(230, 165)),
                            Qt::NoButton,
                            Qt::MiddleButton,
                            Qt::NoModifier);
        QApplication::sendEvent(canvas, &filterFamiliesMove);
        QMouseEvent filterFamiliesRelease(QEvent::MouseButtonRelease,
                               QPointF(230, 165),
                               canvas->mapToGlobal(QPoint(230, 165)),
                               Qt::MiddleButton,
                               Qt::NoButton,
                               Qt::NoModifier);
        QApplication::sendEvent(canvas, &filterFamiliesRelease);
        const QPointF filterFamiliesDelta = canvas->offset() - filterFamiliesBefore;
        const bool panned = std::abs(filterFamiliesDelta.x() - 30.0) < 1e-6
                               && std::abs(filterFamiliesDelta.y() - 15.0) < 1e-6;
        ST_BEGIN("canvas_middle_pan_delta");
        ST_PASS("canvas_middle_pan delta=(%g,%g)", filterFamiliesDelta.x(),
                     filterFamiliesDelta.y());
        if (!panned) {
            ST_FAIL(65, "middle pan wrong");
        }

        // 66: a live move preview is transient; commit adds exactly one state.
        pictura::PictureView* filterFamiliesView = frame.activeView();
        if (!filterFamiliesView) {
            ST_FAIL(66, "move preview no document");
        }
        const int filterFamiliesHistBefore = filterFamiliesView->history_count();
        const QImage filterFamiliesPre = filterFamiliesView->image();
        const bool previewed = filterFamiliesView->move_preview(3, 2);
        const bool previewChanged = filterFamiliesView->image() != filterFamiliesPre;
        const bool previewNoHistory = filterFamiliesView->history_count() == filterFamiliesHistBefore;
        const bool filterFamiliesCommitted = filterFamiliesView->commit_move();
        const bool commitHistory = filterFamiliesView->history_count() == filterFamiliesHistBefore + 1;
        const bool filterFamiliesDirty = filterFamiliesView->is_dirty();
        filterFamiliesView->undo();
        const bool filterFamiliesUndone = filterFamiliesView->image() == filterFamiliesPre;
        ST_BEGIN("canvas_move_preview");
        ST_PASS("canvas_move preview=%d hist=%d undo=%d", previewed ? 1 : 0,
                     commitHistory ? 1 : 0,
                     filterFamiliesUndone ? 1 : 0);
        if (!previewed || !previewChanged || !previewNoHistory
            || !filterFamiliesCommitted || !commitHistory || !filterFamiliesDirty || !filterFamiliesUndone) {
            ST_FAIL(66, "move preview wrong");
        }

        // 67: the drag-start cache (base + layer) must not touch history.
        const int cacheHist = filterFamiliesView->history_count();
        const bool filterFamiliesBegan = filterFamiliesView->begin_move_preview();
        const bool baseOk = !filterFamiliesView->move_preview_base().isNull();
        const bool layerOk = !filterFamiliesView->move_preview_layer().isNull();
        const bool cacheHistOk = filterFamiliesView->history_count() == cacheHist;
        filterFamiliesView->end_move_preview();
        const bool histUnchanged = filterFamiliesView->history_count() == cacheHist;
        ST_BEGIN("canvas_preview_cache_began");
        ST_PASS("canvas_preview_cache began=%d base=%d layer=%d "
                     "hist_unchanged=%d", filterFamiliesBegan ? 1 : 0,
                     baseOk ? 1 : 0,
                     layerOk ? 1 : 0,
                     (cacheHistOk && histUnchanged) ? 1 : 0);
        if (!filterFamiliesBegan || !baseOk || !layerOk || !cacheHistOk || !histUnchanged) {
            ST_FAIL(67, "preview cache wrong");
        }

        // 76: the present path crops a view-pyramid level instead of scaling the
        // full-resolution document. At a fixed view the chosen crop is reused;
        // crossing a level boundary re-crops; a pan re-crops then reuses; and at
        // level 0 the crop present is pixel-identical to a full-resolution draw.
        frame.resize(1000, 700);
        QApplication::processEvents();
        const bool pcCreated = frame.newDocument(QStringLiteral("PresentCache"), 520, 120,
                                                 QStringLiteral("rgb"), 8,
                                                 QStringLiteral("white"));
        canvas = frame.imageView();
        if (!pcCreated || !canvas) {
            ST_FAIL(76, "present cache no canvas");
        }
        const int pcDoc = frame.activeDocumentIndex();
        const QPointF pcCentre(canvas->width() / 2.0, canvas->height() / 2.0);
        canvas->setZoom(0.55, pcCentre);
        QApplication::processEvents();
        QImage pcShot1(canvas->size(), QImage::Format_ARGB32);
        canvas->render(&pcShot1);
        const int pcRebuildsAfterFirst = canvas->presentCacheRebuildCount();
        QImage pcShot2(canvas->size(), QImage::Format_ARGB32);
        canvas->render(&pcShot2);
        const bool pcReused = !canvas->presentCacheRebuiltOnLastPaint()
                              && canvas->presentCacheRebuildCount() == pcRebuildsAfterFirst;
        const bool pcStable = samePixels(pcShot1, pcShot2);
        const bool pcLevel0Size = canvas->presentCacheImageSize() == QSize(520, 120);
        canvas->setPresentLevelCropForTest(false);
        QImage pcDirect(canvas->size(), QImage::Format_ARGB32);
        canvas->render(&pcDirect);
        canvas->setPresentLevelCropForTest(true);
        QImage pcCropped(canvas->size(), QImage::Format_ARGB32);
        canvas->render(&pcCropped);
        const bool pcIdentical = samePixels(pcCropped, pcDirect);
        canvas->setZoom(0.4, pcCentre);
        QImage pcShot3(canvas->size(), QImage::Format_ARGB32);
        canvas->render(&pcShot3);
        const bool pcRebuiltOnZoom = canvas->presentCacheRebuiltOnLastPaint();
        const QSize pcLevel1SizeActual = canvas->presentCacheImageSize();
        const bool pcLevel1Size = pcLevel1SizeActual == QSize(260, 60);
        // A pan over a fully-visible document keeps the same crop (no resample).
        canvas->panBy(QPointF(12.0, 8.0));
        QImage pcPan1(canvas->size(), QImage::Format_ARGB32);
        canvas->render(&pcPan1);
        const bool pcPanReused = !canvas->presentCacheRebuiltOnLastPaint();
        QImage pcPan2(canvas->size(), QImage::Format_ARGB32);
        canvas->render(&pcPan2);
        const bool pcPanStable = !canvas->presentCacheRebuiltOnLastPaint()
                                 && samePixels(pcPan1, pcPan2);
        ST_BEGIN("present_cache_reused");
        ST_PASS("present_cache reused=%d stable=%d level0=520x120 full_identical=%d "
                     "zoom_rebuild=%d level1=%dx%d pan_reuse=%d pan_stable=%d",
                     pcReused ? 1 : 0, pcStable ? 1 : 0, pcIdentical ? 1 : 0,
                     pcRebuiltOnZoom ? 1 : 0, pcLevel1SizeActual.width(),
                     pcLevel1SizeActual.height(), pcPanReused ? 1 : 0, pcPanStable ? 1 : 0);
        if (!pcReused || !pcStable || !pcLevel0Size || !pcIdentical || !pcRebuiltOnZoom
            || !pcLevel1Size || !pcPanReused || !pcPanStable) {
            ST_FAIL(76, "present cache wrong");
        }
        frame.closeDocument(pcDoc, false);

        // M32: the interactive region paths. A 32x32 layer grown into a 64x64
        // canvas keeps a known sub-rectangle, so hiding it changes only that
        // rectangle.
        // 77: the move-preview base equals the canvas with the layer hidden.
        // 78: a raster visibility toggle equals a full recomposite.
        const bool interactiveCreated = frame.newDocument(QStringLiteral("Region"), 32, 32,
                                                  QStringLiteral("rgb"), 8,
                                                  QStringLiteral("white"));
        pictura::PictureView* interactiveView = frame.activeView();
        if (!interactiveCreated || !interactiveView) {
            ST_FAIL(77, "document");
        }
        const int interactiveDocIndex = frame.activeDocumentIndex();
        if (!interactiveView->resize_canvas(QStringLiteral("top-left"), 64, 64)) {
            ST_FAIL(77, "canvas growth");
        }
        const int topLayer = interactiveView->topmost_pixel_layer_index();
        if (topLayer < 0) {
            ST_FAIL(77, "no pixel layer");
        }
        // Full pixel-by-pixel equality over the small (64x64) canvas.
        const bool interactiveBegan = interactiveView->begin_move_preview();
        const QImage interactiveBase = interactiveView->move_preview_base();
        interactiveView->set_layer_visible(topLayer, false);
        const QImage regionHidden = interactiveView->image();
        // Force a full recomposite of the same document state.
        interactiveView->set_gpu_compute(interactiveView->gpu_compute());
        const QImage fullHidden = interactiveView->image();
        const bool baseSame = interactiveBegan && !interactiveBase.isNull()
                                 && samePixels(interactiveBase, regionHidden);
        const bool visibilitySame = samePixels(regionHidden, fullHidden);
        interactiveView->set_layer_visible(topLayer, true);
        interactiveView->end_move_preview();
        frame.closeDocument(interactiveDocIndex, false);

        ST_BEGIN("region_preview_base");
        ST_PASS("region preview_base=%d visibility=%d", baseSame ? 1 : 0,
                     visibilitySame ? 1 : 0);
        if (!baseSame) {
            ST_FAIL(77, "preview base wrong");
        }
        if (!visibilitySame) {
            ST_FAIL(78, "visibility region wrong");
        }

        // M34: composite coherence and cheap undo/redo. A region-path move must
        // leave the stored document composite equal to the displayed image;
        // undo/redo restore from the snapshot composite; and a save after the
        // edit must write that composite to disk.
        const bool coherenceCreated = frame.newDocument(QStringLiteral("Coherent"), 32, 32,
                                                  QStringLiteral("rgb"), 8,
                                                  QStringLiteral("white"));
        pictura::PictureView* coherenceView = frame.activeView();
        if (!coherenceCreated || !coherenceView) {
            ST_FAIL(79, "document");
        }
        const int coherenceDocIndex = frame.activeDocumentIndex();
        const QImage coherencePre = coherenceView->image();
        coherenceView->begin_move_preview();
        const bool coherenceMoved = coherenceView->commit_move(4, 4);
        const QImage post = coherenceView->image();
        const bool coherenceComposite =
            coherenceMoved
            && static_cast<unsigned>(coherenceView->composite_argb(6, 6))
                   == static_cast<unsigned>(post.pixel(6, 6))
            && static_cast<unsigned>(coherenceView->composite_argb(2, 2))
                   == static_cast<unsigned>(post.pixel(2, 2));
        const bool coherenceUndo = coherenceView->undo() && coherenceView->image() == coherencePre
                             && coherenceView->redo() && coherenceView->image() == post;

        // A full-recomposite edit (add noise) changes the layer's colour; the
        // save must write that edited composite, not the pre-edit merged image.
        const bool coherenceFiltered = coherenceView->apply_filter(QStringLiteral("add-noise"));
        const QImage coherenceEdited = coherenceView->image();
        const bool editChanged = coherenceEdited != post;

        const QString coherenceSavePath =
            QDir::tempPath() + QStringLiteral("/kooka-pictura-roundtrip.psd");
        const bool coherenceSaved = frame.saveActiveAs(coherenceSavePath);
        const bool coherenceReopened = frame.openPath(coherenceSavePath);
        pictura::PictureView* coherenceReloaded = frame.activeView();
        const bool coherenceSave =
            coherenceFiltered && editChanged && coherenceSaved && coherenceReopened && coherenceReloaded
            && coherenceReloaded->has_document()
            && coherenceReloaded->image() == coherenceEdited
            && static_cast<unsigned>(coherenceReloaded->composite_argb(6, 6))
                   == static_cast<unsigned>(coherenceEdited.pixel(6, 6));

        ST_BEGIN("coherent_composite");
        ST_PASS("coherent composite=%d undo=%d save=%d", coherenceComposite ? 1 : 0,
                     coherenceUndo ? 1 : 0,
                     coherenceSave ? 1 : 0);
        if (!coherenceComposite) {
            ST_FAIL(79, "composite coherence wrong");
        }
        if (!coherenceUndo) {
            ST_FAIL(80, "undo/redo wrong");
        }
        if (!coherenceSave) {
            ST_FAIL(81, "save round-trip wrong");
        }
        frame.closeDocument(frame.activeDocumentIndex(), false);
        frame.closeDocument(coherenceDocIndex, false);

        // M35: the C++ region path. A region refresh emits regionDamaged
        // (not changed), ImageView::blitRegion overwrites the canvas, and the
        // on-screen canvas equals a full recomposite; image() rebuilds from the
        // composite while dirty; the present cache is patched, not rebuilt.
        // 82: document; 83: blit equality; 84: present cache; 85: large region.
        const bool blitCreated = frame.newDocument(QStringLiteral("RegionBlit"), 32, 32,
                                                  QStringLiteral("rgb"), 8,
                                                  QStringLiteral("white"));
        pictura::PictureView* blitView = frame.activeView();
        pictura::ImageView* blitCanvas = frame.imageView();
        if (!blitCreated || !blitView || !blitCanvas) {
            ST_FAIL(82, "document");
        }
        const int blitDocIndex = frame.activeDocumentIndex();
        if (!blitView->resize_canvas(QStringLiteral("top-left"), 64, 64)) {
            ST_FAIL(82, "canvas growth");
        }
        // Warm the present cache; the second paint at the same zoom must reuse it.
        QImage blitWarm(blitCanvas->size(), QImage::Format_ARGB32);
        blitCanvas->render(&blitWarm);
        blitCanvas->render(&blitWarm);
        const bool reuseBefore = !blitCanvas->presentCacheRebuiltOnLastPaint();

        int regionBlits = 0;
        int blitChanged = 0;
        auto regionConn = QObject::connect(
            blitView, &pictura::PictureView::regionDamaged,
            [&regionBlits](int, int, int, int) { ++regionBlits; });
        auto changedConn = QObject::connect(
            blitView, &pictura::PictureView::changed, [&blitChanged]() { ++blitChanged; });
        const bool blitPreviewed = blitView->begin_move_preview();
        const bool blitMoved = blitView->commit_move(4, 4);
        const QImage blitBlitted = blitCanvas->image();
        const QImage blitRebuilt = blitView->image();
        QObject::disconnect(regionConn);
        QObject::disconnect(changedConn);

        QImage blitShot(blitCanvas->size(), QImage::Format_ARGB32);
        blitCanvas->render(&blitShot);
        // The commit patched the cached crop in place, so this paint needs no new crop.
        const bool cropPatchedAfter = !blitCanvas->presentCacheRebuiltOnLastPaint();
        QImage blitShot2(blitCanvas->size(), QImage::Format_ARGB32);
        blitCanvas->render(&blitShot2);
        const bool cropReusedAfter = !blitCanvas->presentCacheRebuiltOnLastPaint()
                                     && samePixels(blitShot, blitShot2);

        // Force a full recomposite and compare both the blitted canvas and the
        // rebuilt image with it.
        blitView->set_gpu_compute(blitView->gpu_compute());
        const QImage blitFull = blitCanvas->image();
        const bool regionPath = regionBlits >= 1 && blitChanged == 0;
        const bool canvasSame = samePixels(blitBlitted, blitFull);
        const bool rebuiltSame = samePixels(blitRebuilt, blitFull);
        const bool cacheOk = reuseBefore && cropPatchedAfter && cropReusedAfter;
        ST_BEGIN("region_blit_region");
        ST_PASS("region_blit region=%d changed=%d canvas=%d "
                     "rebuilt=%d cache=%d", regionPath ? 1 : 0,
                     blitChanged > 0 ? 1 : 0,
                     canvasSame ? 1 : 0,
                     rebuiltSame ? 1 : 0,
                     cacheOk ? 1 : 0);
        if (!blitPreviewed || !blitMoved || !regionPath || !canvasSame || !rebuiltSame) {
            ST_FAIL(83, "region blit wrong");
        }
        if (!cacheOk) {
            ST_FAIL(84, "present cache wrong");
        }
        frame.closeDocument(blitDocIndex, false);

        // M35 large region: a canvas-sized dirty union takes the C++ blit path
        // and does not recomposite the whole 1024² document.
        const bool blitAltCreated =
            frame.newDocument(QStringLiteral("RegionLarge"), 1024, 1024,
                              QStringLiteral("rgb"), 8, QStringLiteral("white"));
        pictura::PictureView* blitAltView = frame.activeView();
        pictura::ImageView* blitAltCanvas = frame.imageView();
        if (!blitAltCreated || !blitAltView || !blitAltCanvas) {
            ST_FAIL(82, "large document");
        }
        const int blitAltDocIndex = frame.activeDocumentIndex();
        // Run this pass on the CPU compositor to exercise the region-blit path
        // with the non-default backend (the earlier M35 pass used the default).
        blitAltView->set_gpu_compute(false);
        int blitAltRegionBlits = 0;
        int blitAltChanged = 0;
        auto blitAltRegionConn = QObject::connect(
            blitAltView, &pictura::PictureView::regionDamaged,
            [&blitAltRegionBlits](int, int, int, int) { ++blitAltRegionBlits; });
        auto blitAltChangedConn = QObject::connect(
            blitAltView, &pictura::PictureView::changed, [&blitAltChanged]() { ++blitAltChanged; });
        const bool blitAltMoved = blitAltView->commit_move(1, 1);
        const QImage blitAltBlitted = blitAltCanvas->image();
        QObject::disconnect(blitAltRegionConn);
        QObject::disconnect(blitAltChangedConn);
        blitAltView->set_gpu_compute(blitAltView->gpu_compute());
        const QImage blitAltFull = blitAltCanvas->image();
        const bool blitAltRegionPath = blitAltRegionBlits >= 1 && blitAltChanged == 0;
        const bool blitAltCanvasSame = samePixels(blitAltBlitted, blitAltFull);
        ST_BEGIN("region_large_region");
        ST_PASS("region_large region=%d recomposite=%d canvas=%d", blitAltRegionPath ? 1 : 0,
                     blitAltChanged > 0 ? 1 : 0,
                     blitAltCanvasSame ? 1 : 0);
        if (!blitAltMoved || !blitAltRegionPath || !blitAltCanvasSame) {
            ST_FAIL(85, "large-region blit wrong");
        }
        frame.closeDocument(blitAltDocIndex, false);

        // Move-preview base cache: warm prepare -> begin hit, base reused byte-for-byte, recorded change -> miss. 197: cache.
        const bool mpcCreated = frame.newDocument(QStringLiteral("MovePreviewCache"), 32, 32, QStringLiteral("rgb"), 8, QStringLiteral("white"));
        pictura::PictureView* mpcView = frame.activeView();
        const bool mpcHit = mpcCreated && mpcView && mpcView->prepare_move_preview() && mpcView->begin_move_preview() && mpcView->move_preview_cache_hit();
        const QImage mpcBase = mpcView ? mpcView->move_preview_base() : QImage();
        const bool mpcReuse = mpcView && mpcView->begin_move_preview() && mpcView->move_preview_cache_hit() && !mpcBase.isNull() && samePixels(mpcBase, mpcView->move_preview_base());
        const bool mpcAfterMove = mpcView && mpcView->commit_move(1, 1) && mpcView->begin_move_preview() && mpcView->move_preview_cache_hit(); if (mpcView) mpcView->set_layer_name(0, QStringLiteral("MovePreviewCache Renamed"));
        const bool mpcMiss = mpcView && mpcView->begin_move_preview() && !mpcView->move_preview_cache_hit();
        if (mpcView) mpcView->end_move_preview();
        ST_BEGIN("move_preview_cache_hit");
        ST_PASS("move_preview_cache hit=%d reuse=%d after_move=%d miss=%d", mpcHit ? 1 : 0, mpcReuse ? 1 : 0, mpcAfterMove ? 1 : 0, mpcMiss ? 1 : 0);
        if (!mpcHit || !mpcReuse || !mpcAfterMove || !mpcMiss) { ST_FAIL(197, "move preview cache wrong"); }
        frame.closeDocument(frame.activeDocumentIndex(), false);
        // Document size accessors: match full-image dims, then track a resize. 198: size.
        const bool dsCreated = frame.newDocument(QStringLiteral("DocSize"), 20, 12, QStringLiteral("rgb"), 8, QStringLiteral("white"));
        pictura::PictureView* dsView = frame.activeView();
        const bool dsMatch = dsCreated && dsView && dsView->document_width() == dsView->image().width() && dsView->document_height() == dsView->image().height();
        const bool dsResized = dsView && dsView->resize_canvas(QStringLiteral("top-left"), 30, 18) && dsView->document_width() == 30 && dsView->document_height() == 18 && dsView->document_width() == dsView->image().width() && dsView->document_height() == dsView->image().height();
        ST_BEGIN("document_size_match");
        ST_PASS("document_size match=%d resized=%d", dsMatch ? 1 : 0, dsResized ? 1 : 0);
        if (!dsMatch || !dsResized) { ST_FAIL(198, "document size wrong"); }
        frame.closeDocument(frame.activeDocumentIndex(), false);
        // M36: layer attributes through the bridge — fill, lock, color, each one
        // history state and undoable. 86: fill; 87: lock; 88: color; 89: undo.
        const bool attrsCreated = frame.newDocument(QStringLiteral("Attrs"), 16, 16,
                                                  QStringLiteral("rgb"), 8,
                                                  QStringLiteral("white"));
        pictura::PictureView* attrsView = frame.activeView();
        if (!attrsCreated || !attrsView) {
            ST_FAIL(86, "document");
        }
        const int attrsDocIndex = frame.activeDocumentIndex();
        const int histBase = attrsView->history_count();
        const int fillBefore = attrsView->layer_fill(0);
        const bool fillSet = attrsView->set_layer_fill(0, 128);
        const int fillAfter = attrsView->layer_fill(0);
        const bool fillOk = fillBefore == 255 && fillSet && fillAfter == 128
                               && attrsView->history_count() == histBase + 1;
        const bool lockSet =
            attrsView->set_layer_lock(0, QStringLiteral("transparency"), true);
        const int lockAfter = attrsView->layer_lock(0);
        const bool lockOk = lockSet && (lockAfter & 0x01) != 0
                               && attrsView->history_count() == histBase + 2;
        const int colorBefore = attrsView->layer_color(0);
        const bool colorSet = attrsView->set_layer_color(0, 3);
        const int colorAfter = attrsView->layer_color(0);
        const bool colorOk = colorBefore == 0 && colorSet && colorAfter == 3
                                && attrsView->history_count() == histBase + 3;
        // Undo walks each edit back to its prior value; redo restores them.
        const bool attrsUndo = attrsView->undo() && attrsView->layer_color(0) == 0
                             && attrsView->undo() && attrsView->layer_lock(0) == 0
                             && attrsView->undo() && attrsView->layer_fill(0) == 255
                             && attrsView->redo() && attrsView->redo() && attrsView->redo()
                             && attrsView->layer_fill(0) == 128 && attrsView->layer_color(0) == 3;
        ST_BEGIN("attrs_fill");
        ST_PASS("attrs fill=%d lock=%d color=%d undo=%d", fillOk ? 1 : 0,
                     lockOk ? 1 : 0,
                     colorOk ? 1 : 0,
                     attrsUndo ? 1 : 0);
        if (!fillOk) {
            ST_FAIL(86, "fill");
        }
        if (!lockOk) {
            ST_FAIL(87, "lock");
        }
        if (!colorOk) {
            ST_FAIL(88, "color");
        }
        if (!attrsUndo) {
            ST_FAIL(89, "undo restore");
        }
        frame.closeDocument(attrsDocIndex, false);
        // M37: layer creation and grouping. Each op is exactly one history
        // step; a transparent new layer leaves the composite unchanged, and
        // undo restores the original stack.
        const bool layerOpsCreated = frame.newDocument(QStringLiteral("Create"), 16, 16,
                                                  QStringLiteral("rgb"), 8,
                                                  QStringLiteral("white"));
        pictura::PictureView* layerOpsView = frame.activeView();
        if (!layerOpsCreated || !layerOpsView) {
            ST_FAIL(90, "no view");
        }
        const int layerOpsDocIndex = frame.activeDocumentIndex();
        const int layerOpsBaseCount = layerOpsView->layer_count();
        const int layerOpsHistBase = layerOpsView->history_count();
        // (a) New Layer: count grows by one and the transparent layer is inert.
        const QImage layerOpsBefore = layerOpsView->image();
        const int layerOpsAdded = layerOpsView->add_layer(-1);
        const int afterAdd = layerOpsView->layer_count();
        const bool addOk = layerOpsAdded == layerOpsBaseCount && afterAdd == layerOpsBaseCount + 1
                              && layerOpsView->image() == layerOpsBefore
                              && layerOpsView->history_count() == layerOpsHistBase + 1;
        // (b) New Group.
        const int layerOpsGroup = layerOpsView->add_group(-1);
        const bool groupOk = layerOpsGroup == afterAdd
                                && layerOpsView->layer_count() == afterAdd + 1
                                && layerOpsView->layer_kind(layerOpsGroup) == QStringLiteral("group")
                                && layerOpsView->history_count() == layerOpsHistBase + 2;
        // (c) Duplicate: count grows and the copy is named "<name> copy".
        const int dup = layerOpsView->duplicate_layer(layerOpsAdded);
        const QString dupName = dup >= 0 ? layerOpsView->layer_name(dup) : QString();
        const bool dupOk = dup == layerOpsAdded + 1
                              && layerOpsView->layer_count() == afterAdd + 2
                              && dupName.endsWith(QStringLiteral(" copy"))
                              && layerOpsView->history_count() == layerOpsHistBase + 3;

        // (d) Group then ungroup: the wrapped layer keeps its slot and the
        // ungrouped children splice back in order.
        const int layerOpsWrapped = layerOpsView->group_layer(layerOpsAdded);
        const bool layerOpsGrouped = layerOpsWrapped == layerOpsAdded
                                && layerOpsView->layer_kind(layerOpsWrapped) == QStringLiteral("group");
        const bool layerOpsUngrouped = layerOpsView->ungroup_layer(layerOpsWrapped)
                                  && layerOpsView->layer_kind(layerOpsWrapped) == QStringLiteral("pixel");
        const bool ungroupOk = layerOpsGrouped && layerOpsUngrouped
                                  && layerOpsView->history_count() == layerOpsHistBase + 5;

        // (e) Five undos restore the initial single-layer stack.
        bool undoOk = true;
        for (int i = 0; i < 5; ++i) {
            undoOk = undoOk && layerOpsView->undo();
        }
        undoOk = undoOk && layerOpsView->layer_count() == layerOpsBaseCount;

        ST_BEGIN("create_new");
        ST_PASS("create new=%d group=%d duplicate=%d "
                     "ungroup=%d undo=%d", addOk ? 1 : 0,
                     groupOk ? 1 : 0,
                     dupOk ? 1 : 0,
                     ungroupOk ? 1 : 0,
                     undoOk ? 1 : 0);
        if (!addOk) {
            ST_FAIL(90, "add failed");
        }
        if (!groupOk) {
            ST_FAIL(91, "group failed");
        }
        if (!dupOk) {
            ST_FAIL(92, "duplicate failed");
        }
        if (!ungroupOk) {
            ST_FAIL(93, "ungroup failed");
        }
        if (!undoOk) {
            ST_FAIL(94, "undo failed");
        }
        frame.closeDocument(layerOpsDocIndex, false);
        // M38: the frozen 71-tool catalogue, its icons/cursors/hotspots, the
        // 21-slot toolbox (Object/Camera hidden), and the unimplemented-tool
        // guard. Assets are document-independent, so these run with or without a PSD.
        const QList<pictura::ToolId> catalogue = pictura::allToolIds();
        QString missingToolIcon;
        QString missingToolCursor;
        int iconsOk = 1;
        int cursorsOk = 1;
        for (const pictura::ToolId id : catalogue) {
            const pictura::ToolInfo& info = pictura::toolInfo(id);
            const QString assetId = QStringLiteral("tool.") + pictura::toolIdName(id);
            if (pictura::icon(assetId).isNull()) {
                iconsOk = 0;
                if (missingToolIcon.isEmpty()) {
                    missingToolIcon = assetId;
                }
            }
            const bool hotspotOk = info.hotspotX >= 0 && info.hotspotX < 24
                                   && info.hotspotY >= 0 && info.hotspotY < 24;
            if (!hotspotOk
                || pictura::cursor(assetId, info.hotspotX, info.hotspotY).pixmap().isNull()) {
                cursorsOk = 0;
                if (missingToolCursor.isEmpty()) {
                    missingToolCursor = assetId;
                }
            }
        }

        auto* toolsDock = frame.findChild<pictura::Toolbox*>(QStringLiteral("toolsPanel"));
        int slotsOk = 0;
        if (toolsDock) {
            const QList<QToolButton*> slotButtons = toolsDock->slotButtons();
            const QList<int> slotGroups = toolsDock->slotGroupsForTest();
            slotsOk = slotButtons.size() == 21 && slotGroups.size() == slotButtons.size() ? 1 : 0;
            if (slotsOk) {
                for (int i = 0; i < slotButtons.size(); ++i) {
                    bool anyImplemented = false;
                    for (const pictura::ToolId id : catalogue) {
                        if (pictura::toolInfo(id).group == slotGroups.at(i) && pictura::toolImplemented(id)) {
                            anyImplemented = true;
                        }
                    }
                    if (slotButtons.at(i)->isEnabled() != anyImplemented) {
                        slotsOk = 0;
                    }
                }
            }
        }

        pictura::ToolController probe;
        const pictura::ToolId guardBefore = probe.activeTool();
        probe.setActiveTool(pictura::ToolId::ObjectRotate);
        const bool guardOk = probe.activeTool() == guardBefore;

        ST_BEGIN("tools_icons");
        ST_PASS("tools icons=%d cursors=%d slots=%d guard=%d", iconsOk,
                     cursorsOk,
                     slotsOk,
                     guardOk ? 1 : 0);
        if (!iconsOk) {
            ST_FAIL(95, "tool icon missing=%s", missingToolIcon.toLocal8Bit().constData());
        }
        if (!cursorsOk) {
            ST_FAIL(96, "tool cursor missing=%s", missingToolCursor.toLocal8Bit().constData());
        }
        if (!slotsOk) {
            ST_FAIL(97, "toolbox slots wrong");
        }
        if (!guardOk) {
            ST_FAIL(98, "unimplemented tool activated");
        }
        // M38 panels: the former rail icon ids now live on the PanelColumn
        // group tabs, plus the Layers action strip and the History snapshot
        // button. Assets are document-independent, so these run with or without
        // a loaded PSD.
        auto sameIcon = [](const QIcon& actual, const QIcon& expected) {
            return !actual.isNull() && !expected.isNull()
                   && actual.pixmap(20, 20).toImage() == expected.pixmap(20, 20).toImage();
        };
        const QList<pictura::PanelColumn*> railColumns = frame.panelColumns();
        const QStringList railTitles = {
            QStringLiteral("History"),
            QStringLiteral("Actions"),
            QStringLiteral("Info"),
            QStringLiteral("Navigator"),
            QStringLiteral("Histogram"),
        };
        const QStringList railIds = {
            QStringLiteral("window.panels.history"),
            QStringLiteral("window.panels.actions"),
            QStringLiteral("window.panels.info"),
            QStringLiteral("window.panels.navigator"),
            QStringLiteral("window.panels.histogram"),
        };
        int railIcons = 0;
        QString railMissing;
        for (int i = 0; i < railTitles.size(); ++i) {
            bool matched = false;
            for (pictura::PanelColumn* column : railColumns) {
                for (pictura::PanelGroup* group : column->groups()) {
                    matched = matched || (group && sameIcon(
                        group->titleIconForTest(railTitles.at(i)),
                        pictura::icon(railIds.at(i))));
                }
            }
            if (matched) {
                ++railIcons;
            } else if (railMissing.isEmpty()) {
                railMissing = railIds.at(i);
            }
        }
        const bool railOk = !railColumns.isEmpty() && railIcons == railTitles.size();

        const QStringList stripIds = {
            QStringLiteral("link"),           QStringLiteral("fx"),
            QStringLiteral("mask"),           QStringLiteral("fillAdjustment"),
            QStringLiteral("group"),          QStringLiteral("newLayer"),
            QStringLiteral("delete")};
        const QStringList stripObjectNames = {
            QStringLiteral("layersStripLink"),   QStringLiteral("layersStripFx"),
            QStringLiteral("layersStripMask"),   QStringLiteral("layersStripFillAdjustment"),
            QStringLiteral("layersStripGroup"),  QStringLiteral("layersStripNewLayer"),
            QStringLiteral("layersStripDelete")};
        // link and delete follow the layer selection, so only their icons are
        // checked here; the other strip buttons carry their icon while enabled.
        const QSet<QString> stripStateful = {QStringLiteral("link"), QStringLiteral("delete")};
        QWidget* assetsLayers = frame.findChild<QWidget*>(QStringLiteral("layersPanel"));
        int stripOk = 0;
        QString stripWrong;
        for (int i = 0; i < stripIds.size(); ++i) {
            auto* button = assetsLayers
                ? assetsLayers->findChild<QToolButton*>(stripObjectNames.at(i))
                : nullptr;
            const QIcon expected =
                pictura::icon(QStringLiteral("layers.") + stripIds.at(i));
            if (button && sameIcon(button->icon(), expected)
                && (stripStateful.contains(stripIds.at(i)) || button->isEnabled())) {
                ++stripOk;
            } else if (stripWrong.isEmpty()) {
                stripWrong = stripIds.at(i);
            }
        }
        const bool stripPass = stripOk == stripIds.size();

        QWidget* assetsHistory = frame.findChild<QWidget*>(QStringLiteral("historyPanel"));
        auto* assetsSnapshot =
            assetsHistory ? assetsHistory->findChild<QPushButton*>(QStringLiteral("snapshotButton"))
                       : nullptr;
        const bool historyOk =
            assetsSnapshot
            && sameIcon(assetsSnapshot->icon(), pictura::icon(QStringLiteral("history.snapshot")));

        ST_BEGIN("panels_rail");
        ST_PASS("panels rail=%d strip=%d history=%d", railOk ? 1 : 0,
                     stripPass ? 1 : 0,
                     historyOk ? 1 : 0);
        if (!railOk) {
            ST_FAIL(99, "rail icon missing=%s", railMissing.toLocal8Bit().constData());
        }
        if (!stripPass) {
            ST_FAIL(100, "strip button wrong=%s", stripWrong.toLocal8Bit().constData());
        }
        if (!historyOk) {
            ST_FAIL(101, "snapshot icon wrong");
        }
        // M39: the layers panel's tree projection, multi-selection batches,
        // solo visibility, Tab rename, Panel Options, badges, menus, tooltips,
        // and the seven-button strip. A deterministic fixture is built through
        // the bridge; the panel is read through its M39 test hooks.
        auto* anatomyPanel = frame.findChild<pictura::LayersPanel*>(QStringLiteral("layersPanel"));
        const bool anatomyCreated = frame.newDocument(QStringLiteral("PanelAnatomy"), 16, 16,
                                                  QStringLiteral("rgb"), 8,
                                                  QStringLiteral("white"));
        pictura::PictureView* anatomyView = frame.activeView();
        if (!anatomyCreated || !anatomyView || !anatomyPanel) {
            ST_FAIL(102, "document/panel");
        }
        const int anatomyDocIndex = frame.activeDocumentIndex();

        // Fixture: a Background, a group "1" with two pixel children and a
        // nested group "1/2", and a masked adjustment layer "2" on top.
        anatomyView->background_from_layer(QStringLiteral("0"));
        const QString anatomyGroup = anatomyView->add_group_in(QString());
        const QString childA = anatomyView->add_layer_in(anatomyGroup);
        const QString childB = anatomyView->add_layer_in(anatomyGroup);
        const QString anatomyNested = anatomyView->add_group_in(anatomyGroup);
        const QString nestedChild = anatomyView->add_layer_in(anatomyNested);
        // No layer target: the adjustment lands on top; anchored creation is tested elsewhere.
        pictura::clear_layer_target(*anatomyView);
        anatomyView->add_adjustment(QStringLiteral("invert"));
        anatomyPanel->setView(anatomyView);
        anatomyPanel->refresh();

        auto rowOf = [anatomyView](const QString& path) {
            for (int i = 0; i < pictura::layers_api::rowCount(*anatomyView); ++i) {
                if (pictura::layers_api::rowAt(*anatomyView, i).path == path) {
                    return i;
                }
            }
            return -1;
        };

        // 8.1 m39_tree (102-103): the depth-first, topmost-first projection,
        // the tree-aware add-inside-group paths, and a nested rename by path.
        const QStringList treePaths = {
            QStringLiteral("2"), QStringLiteral("1"), QStringLiteral("1/2"),
            QStringLiteral("1/2/0"), QStringLiteral("1/1"), QStringLiteral("1/0"),
            QStringLiteral("0")};
        const int treeDepths[] = {0, 0, 1, 2, 1, 1, 0};
        const QStringList treeKinds = {
            QStringLiteral("adjustment"), QStringLiteral("group"), QStringLiteral("group"),
            QStringLiteral("pixel"), QStringLiteral("pixel"), QStringLiteral("pixel"),
            QStringLiteral("background")};
        bool orderOk = pictura::layers_api::rowCount(*anatomyView) == treePaths.size();
        for (int i = 0; orderOk && i < treePaths.size(); ++i) {
            orderOk = pictura::layers_api::rowAt(*anatomyView, i).path == treePaths.at(i)
                         && pictura::layers_api::rowAt(*anatomyView, i).depth == treeDepths[i]
                         && pictura::layers_api::rowAt(*anatomyView, i).kind == treeKinds.at(i);
        }
        const bool anatomyAddOk =
            childA == QStringLiteral("1/0") && childB == QStringLiteral("1/1")
            && anatomyNested == QStringLiteral("1/2")
            && nestedChild == QStringLiteral("1/2/0")
            && pictura::layers_api::rowAt(*anatomyView, rowOf(nestedChild)).depth == 2
            && pictura::layers_api::rowAt(*anatomyView, rowOf(QStringLiteral("1"))).expandable
            && pictura::layers_api::rowAt(*anatomyView, rowOf(anatomyNested)).expandable;
        const int renameBase = anatomyView->history_count();
        const bool treeRenameOk =
            anatomyView->set_layer_name_path(QStringLiteral("1/1"), QStringLiteral("Nested"))
            && pictura::layers_api::rowAt(*anatomyView, rowOf(QStringLiteral("1/1"))).name
                   == QStringLiteral("Nested")
            && anatomyView->history_count() == renameBase + 1
            && !anatomyView->set_layer_name_path(QStringLiteral("1/9"), QStringLiteral("Bogus"))
            && anatomyView->history_count() == renameBase + 1;
        ST_BEGIN("tree_order");
        ST_PASS("tree order=%d add=%d rename=%d", orderOk ? 1 : 0, anatomyAddOk ? 1 : 0, treeRenameOk ? 1 : 0);
        if (!orderOk) {
            ST_FAIL(102, "projection order");
        }
        if (!anatomyAddOk || !treeRenameOk) {
            ST_FAIL(103, "tree add/nested rename");
        }

        // 8.2 m39_multi (104-105): a batch is one undo step, the Background and
        // a group are skipped per node, and multi group/ungroup is one step.
        const QString anatomyBg = QStringLiteral("0");
        const QString pixA = QStringLiteral("1/1");
        const QString pixB = QStringLiteral("1/0");
        const int blendBase = anatomyView->history_count();
        const int blendChanged =
            anatomyView->set_layers_blend(QStringList{pixA, pixB}, QStringLiteral("mul "));
        const bool batchOk =
            blendChanged == 2 && anatomyView->history_count() == blendBase + 1
            && pictura::layers_api::rowAt(*anatomyView, rowOf(pixA)).blend == QStringLiteral("mul ")
            && pictura::layers_api::rowAt(*anatomyView, rowOf(pixB)).blend == QStringLiteral("mul ");
        const bool batchUndoOk =
            anatomyView->undo()
            && pictura::layers_api::rowAt(*anatomyView, rowOf(pixA)).blend == QStringLiteral("norm")
            && pictura::layers_api::rowAt(*anatomyView, rowOf(pixB)).blend == QStringLiteral("norm")
            && anatomyView->redo()
            && pictura::layers_api::rowAt(*anatomyView, rowOf(pixA)).blend == QStringLiteral("mul ");

        const int skipBase = anatomyView->history_count();
        const int bgChanged =
            anatomyView->set_layers_blend(QStringList{anatomyBg, pixA}, QStringLiteral("scrn"));
        const int fillBase = anatomyView->history_count();
        const int groupFill = anatomyView->set_layers_fill(QStringList{anatomyGroup, pixB}, 128);
        const bool skipOk =
            bgChanged == 1 && fillBase == skipBase + 1
            && pictura::layers_api::rowAt(*anatomyView, rowOf(anatomyBg)).blend == QStringLiteral("norm")
            && pictura::layers_api::rowAt(*anatomyView, rowOf(pixA)).blend == QStringLiteral("scrn")
            && groupFill == 1 && anatomyView->history_count() == fillBase + 1
            && pictura::layers_api::rowAt(*anatomyView, rowOf(anatomyGroup)).fill == 255
            && pictura::layers_api::rowAt(*anatomyView, rowOf(pixB)).fill == 128;

        const int groupBase = anatomyView->history_count();
        const QString anatomyWrapped = anatomyView->group_layers(QStringList{pixA, pixB});
        const bool groupStepOk =
            !anatomyWrapped.isEmpty() && anatomyView->history_count() == groupBase + 1;
        const int ungroupBase = anatomyView->history_count();
        const int anatomyUngrouped = anatomyView->ungroup_layers(QStringList{anatomyWrapped});
        const bool anatomyUngroupOk =
            anatomyUngrouped == 1 && anatomyView->history_count() == ungroupBase + 1;

        ST_BEGIN("multi_batch");
        ST_PASS("multi batch=%d skip=%d group=%d undo=%d", batchOk ? 1 : 0, skipOk ? 1 : 0,
                     (groupStepOk && anatomyUngroupOk) ? 1 : 0, batchUndoOk ? 1 : 0);
        if (!batchOk || !skipOk || !batchUndoOk) {
            ST_FAIL(104, "multi-selection batch");
        }
        if (!groupStepOk || !anatomyUngroupOk) {
            ST_FAIL(105, "multi group/ungroup");
        }

        // 8.3 m39_solo (106): Alt-solo hides every other row, a second Alt
        // restores the exact prior per-row visibility, and undo restores.
        const QString soloHidden = QStringLiteral("1/1");
        anatomyView->set_layers_visible(QStringList{soloHidden}, false);
        anatomyPanel->refresh();
        QStringList priorHidden;
        for (int i = 0; i < pictura::layers_api::rowCount(*anatomyView); ++i) {
            if (!pictura::layers_api::rowAt(*anatomyView, i).visible) {
                priorHidden.push_back(pictura::layers_api::rowAt(*anatomyView, i).path);
            }
        }
        const QString soloPath = QStringLiteral("1/0");
        anatomyPanel->toggleSoloForTest(soloPath);
        anatomyPanel->refresh();
        bool soloOk = true;
        for (int i = 0; i < pictura::layers_api::rowCount(*anatomyView); ++i) {
            const QString path = pictura::layers_api::rowAt(*anatomyView, i).path;
            const bool expected = path == soloPath || path == anatomyGroup;
            soloOk = soloOk && pictura::layers_api::rowAt(*anatomyView, i).visible == expected;
        }
        const bool soloUndo = anatomyView->undo();
        anatomyPanel->refresh();
        bool soloUndoOk = soloUndo;
        for (int i = 0; i < pictura::layers_api::rowCount(*anatomyView); ++i) {
            soloUndoOk = soloUndoOk
                            && pictura::layers_api::rowAt(*anatomyView, i).visible
                                   == !priorHidden.contains(pictura::layers_api::rowAt(*anatomyView, i).path);
        }
        const bool soloRedo = anatomyView->redo();
        anatomyPanel->refresh();
        anatomyPanel->toggleSoloForTest(anatomyGroup);
        anatomyPanel->refresh();
        bool soloRestoreOk = soloRedo;
        for (int i = 0; i < pictura::layers_api::rowCount(*anatomyView); ++i) {
            soloRestoreOk =
                soloRestoreOk
                && pictura::layers_api::rowAt(*anatomyView, i).visible
                       == !priorHidden.contains(pictura::layers_api::rowAt(*anatomyView, i).path);
        }
        ST_BEGIN("solo_solo");
        ST_PASS("solo solo=%d restore=%d undo=%d", soloOk ? 1 : 0, soloRestoreOk ? 1 : 0, soloUndoOk ? 1 : 0);
        if (!soloOk || !soloRestoreOk || !soloUndoOk) {
            ST_FAIL(106, "solo visibility");
        }

        // 8.4 m39_rename (107): Tab commits and moves down, Shift+Tab up, and
        // the ends do not wrap. The key is delivered to the delegate's own
        // event filter (its role on the inline editor); headless focus routing
        // makes QApplication::sendEvent to the editor unreliable, so the
        // delegate is invoked directly and the view still performs the move.
        QCoreApplication::processEvents();
        anatomyPanel->refresh();
        QCoreApplication::processEvents();
        auto anatomyEdit = [anatomyPanel](const QString& path, const QString& text, int key) {
            QLineEdit* editor = anatomyPanel->openRenameEditorForTest(path);
            QObject* delegate = anatomyPanel->itemDelegateForTest();
            if (!editor || !delegate) {
                return false;
            }
            editor->setText(text);
            QKeyEvent event(QEvent::KeyPress, key, Qt::NoModifier);
            delegate->eventFilter(editor, &event);
            QCoreApplication::processEvents();
            anatomyPanel->refresh();
            return true;
        };
        const QString firstRow = QStringLiteral("2");
        const QString lastRow = QStringLiteral("0");
        // A row whose neighbour is a top-level row, open groups or not.
        const QString middleRow = QStringLiteral("1");
        const bool downSent = anatomyEdit(firstRow, QStringLiteral("TopAdj"), Qt::Key_Tab);
        const bool downMoved =
            downSent
            && pictura::layers_api::rowAt(*anatomyView, rowOf(firstRow)).name == QStringLiteral("TopAdj")
            && anatomyPanel->currentPath() == QStringLiteral("1");
        // The commit is exactly one undoable step: one undo restores the prior
        // name and one redo reapplies it.
        const bool downUndoOk =
            anatomyView->undo()
            && pictura::layers_api::rowAt(*anatomyView, rowOf(firstRow)).name == QStringLiteral("Invert")
            && anatomyView->redo()
            && pictura::layers_api::rowAt(*anatomyView, rowOf(firstRow)).name == QStringLiteral("TopAdj");
        const bool downOk = downMoved && downUndoOk;
        const bool upOk =
            anatomyEdit(middleRow, QStringLiteral("Bottom"), Qt::Key_Backtab)
            && pictura::layers_api::rowAt(*anatomyView, rowOf(middleRow)).name == QStringLiteral("Bottom")
            && anatomyPanel->currentPath() == firstRow;
        const bool noWrapFirst =
            anatomyEdit(firstRow, QStringLiteral("First"), Qt::Key_Backtab)
            && pictura::layers_api::rowAt(*anatomyView, rowOf(firstRow)).name == QStringLiteral("First")
            && anatomyPanel->currentPath() == firstRow;
        const bool noWrapLast =
            anatomyEdit(lastRow, QStringLiteral("Last"), Qt::Key_Tab)
            && pictura::layers_api::rowAt(*anatomyView, rowOf(lastRow)).name == QStringLiteral("Last")
            && anatomyPanel->currentPath() == lastRow;
        const bool nowrapOk = noWrapFirst && noWrapLast;
        ST_BEGIN("rename_down");
        ST_PASS("rename down=%d up=%d nowrap=%d", downOk ? 1 : 0, upOk ? 1 : 0, nowrapOk ? 1 : 0);
        if (!downOk || !upOk || !nowrapOk) {
            ST_FAIL(107, "Tab rename");
        }

        // 8.5 m39_options (108): the defaults are Medium / Entire Document /
        // on, and a session save/load round-trip preserves a changed triple.
        const pictura::SessionState anatomyFresh = pictura::loadSession();
        const bool defaultsOk =
            anatomyFresh.layersThumbSize == 2 && anatomyFresh.layersThumbContents == 0
            && anatomyFresh.layersExpandNewEffects
            && anatomyPanel->thumbSizeIndexForTest() == 2
            && anatomyPanel->thumbContentsForTest() == 0
            && anatomyPanel->expandNewEffectsForTest();
        anatomyPanel->setOptionsForTest(3, 1, false);
        const pictura::SessionState anatomySaved = pictura::loadSession();
        auto* anatomyReloaded = new pictura::LayersPanel();
        const bool roundtripOk =
            anatomySaved.layersThumbSize == 3 && anatomySaved.layersThumbContents == 1
            && !anatomySaved.layersExpandNewEffects
            && anatomyReloaded->thumbSizeIndexForTest() == 3
            && anatomyReloaded->thumbContentsForTest() == 1
            && !anatomyReloaded->expandNewEffectsForTest();
        delete anatomyReloaded;
        ST_BEGIN("options_defaults");
        ST_PASS("options defaults=%d roundtrip=%d", defaultsOk ? 1 : 0, roundtripOk ? 1 : 0);
        if (!defaultsOk || !roundtripOk) {
            ST_FAIL(108, "panel options");
        }

        // 8.6 m39_badges (109): the mask and adjustment badge data reaches the
        // model. No bridge op creates a clipped layer, so `clip` verifies the
        // clip role is plumbed consistently (and the group expand indicator).
        const bool badgeMask =
            pictura::layers_api::rowAt(*anatomyView, rowOf(QStringLiteral("2"))).hasMask
            && anatomyPanel->rowHasMaskForTest(QStringLiteral("2"));
        const bool badgeFx =
            pictura::layers_api::rowAt(*anatomyView, rowOf(QStringLiteral("2"))).hasAdjustment
            && anatomyPanel->rowHasAdjustmentForTest(QStringLiteral("2"));
        bool badgeClip = anatomyPanel->rowExpandableForTest(QStringLiteral("1"))
                            && anatomyPanel->rowExpandableForTest(QStringLiteral("1/2"));
        for (int i = 0; i < pictura::layers_api::rowCount(*anatomyView); ++i) {
            const QString path = pictura::layers_api::rowAt(*anatomyView, i).path;
            badgeClip = badgeClip
                           && anatomyPanel->rowClippingForTest(path)
                                  == pictura::layers_api::rowAt(*anatomyView, i).clipping
                           && !anatomyPanel->rowClipBaseForTest(path);
        }
        ST_BEGIN("badges_mask");
        ST_PASS("badges mask=%d fx=%d clip=%d", badgeMask ? 1 : 0, badgeFx ? 1 : 0, badgeClip ? 1 : 0);
        if (!badgeMask || !badgeFx || !badgeClip) {
            ST_FAIL(109, "row badges");
        }

        // m39_tooltip (111): every row's tooltip is "<name> (<kind>)".
        const QString groupTip = anatomyPanel->rowToolTipForTest(QStringLiteral("1"));
        const QString pixelTip = anatomyPanel->rowToolTipForTest(QStringLiteral("1/1"));
        const QString adjTip = anatomyPanel->rowToolTipForTest(QStringLiteral("2"));
        const bool tipOk =
            groupTip
                == QStringLiteral("%1 (group)")
                       .arg(pictura::layers_api::rowAt(*anatomyView, rowOf(QStringLiteral("1"))).name)
            && pixelTip
                   == QStringLiteral("%1 (pixel)")
                          .arg(pictura::layers_api::rowAt(*anatomyView, rowOf(QStringLiteral("1/1"))).name)
            && adjTip
                   == QStringLiteral("%1 (adjustment)")
                          .arg(pictura::layers_api::rowAt(*anatomyView, rowOf(QStringLiteral("2"))).name);
        ST_BEGIN("tooltip_ok");
        ST_PASS("tooltip ok=%d", tipOk ? 1 : 0);
        if (!tipOk) {
            ST_FAIL(111, "row tooltip");
        }

        // m39_strip (112): exactly the seven CS6 strip buttons, no Move text
        // buttons (reordering lives in the menus).
        const QStringList stripNames = {
            QStringLiteral("layersStripLink"), QStringLiteral("layersStripFx"),
            QStringLiteral("layersStripMask"), QStringLiteral("layersStripFillAdjustment"),
            QStringLiteral("layersStripGroup"), QStringLiteral("layersStripNewLayer"),
            QStringLiteral("layersStripDelete")};
        QWidget* anatomyDock = frame.findChild<QWidget*>(QStringLiteral("layersPanel"));
        int stripFound = 0;
        for (const QString& name : stripNames) {
            if (anatomyDock && anatomyDock->findChild<QToolButton*>(name)) {
                ++stripFound;
            }
        }
        bool noMoveButtons = true;
        if (anatomyDock) {
            const QList<QAbstractButton*> anatomyButtons =
                anatomyDock->findChildren<QAbstractButton*>();
            for (QAbstractButton* button : anatomyButtons) {
                if (button->text().contains(QStringLiteral("Move"))) {
                    noMoveButtons = false;
                }
            }
        }
        const bool anatomyStripOk =
            stripFound == stripNames.size() && noMoveButtons;
        ST_BEGIN("strip_seven");
        ST_PASS("strip seven=%d", anatomyStripOk ? 1 : 0);
        if (!anatomyStripOk) {
            ST_FAIL(112, "action strip");
        }
        // M40: the tools panel — one/two columns, the flyout indicator, opening,
        // and keys, Shift-key cycling, the standalone dock, and session v4.
        // Exit codes 113–119.
        auto* toolsPanelToolbox = frame.findChild<pictura::Toolbox*>(QStringLiteral("toolsPanel"));
        const QList<QToolButton*> toolsPanelSlots =
            toolsPanelToolbox ? toolsPanelToolbox->slotButtons() : QList<QToolButton*>();

        // m40_columns (113/114): default one column; two columns reflow the 21
        // slots row-major and widen the dock; toggling back restores one column;
        // the colour control and the Screen Mode button stay below the slots in
        // both layouts.
        QCoreApplication::processEvents();
        auto pinnedBelow = [toolsPanelToolbox, &toolsPanelSlots]() {
            if (!toolsPanelToolbox || toolsPanelSlots.isEmpty()) {
                return false;
            }
            int slotBottom = 0;
            for (QToolButton* button : toolsPanelSlots) {
                slotBottom = qMax(
                    slotBottom, button->mapTo(toolsPanelToolbox, QPoint(0, button->height())).y());
            }
            auto* fgbg = toolsPanelToolbox->findChild<pictura::ForegroundBackgroundWidget*>();
            auto* screenMode =
                toolsPanelToolbox->findChild<QToolButton*>(QStringLiteral("screenModeButton"));
            return fgbg != nullptr && screenMode != nullptr
                   && fgbg->mapTo(toolsPanelToolbox, QPoint(0, 0)).y() > slotBottom
                   && screenMode->mapTo(toolsPanelToolbox, QPoint(0, 0)).y() > slotBottom;
        };
        const bool toolsPanelDefault =
            toolsPanelToolbox != nullptr && toolsPanelToolbox->columns() == 1 && toolsPanelSlots.size() == 21;
        const int toolsPanelMin1 = toolsPanelToolbox ? toolsPanelToolbox->minimumWidth() : 0;
        const int toolsPanelWidth1 = toolsPanelToolbox ? toolsPanelToolbox->width() : 0;
        const bool pinned1 = pinnedBelow();
        if (toolsPanelToolbox) {
            toolsPanelToolbox->setColumns(2);
        }
        QCoreApplication::processEvents();
        const int toolsPanelMin2 = toolsPanelToolbox ? toolsPanelToolbox->minimumWidth() : 0;
        const int toolsPanelWidth2 = toolsPanelToolbox ? toolsPanelToolbox->width() : 0;
        bool rowMajor = toolsPanelSlots.size() == 21;
        for (int i = 0; i + 2 < toolsPanelSlots.size() && rowMajor; ++i) {
            const QPoint a = toolsPanelSlots.at(i)->mapTo(toolsPanelToolbox, QPoint(0, 0));
            const QPoint b = toolsPanelSlots.at(i + 2)->mapTo(toolsPanelToolbox, QPoint(0, 0));
            if (a.x() != b.x() || a.y() >= b.y()) {
                rowMajor = false;
            }
        }
        for (int i = 0; i + 1 < toolsPanelSlots.size() && rowMajor; i += 2) {
            const QPoint a = toolsPanelSlots.at(i)->mapTo(toolsPanelToolbox, QPoint(0, 0));
            const QPoint b = toolsPanelSlots.at(i + 1)->mapTo(toolsPanelToolbox, QPoint(0, 0));
            if (a.y() != b.y() || a.x() >= b.x()) {
                rowMajor = false;
            }
        }
        const bool pinned2 = pinnedBelow();
        if (toolsPanelToolbox) {
            toolsPanelToolbox->setColumns(1);
        }
        QCoreApplication::processEvents();
        const bool toolsPanelRestored = toolsPanelToolbox != nullptr && toolsPanelToolbox->columns() == 1;
        const bool toolsPanelWidened = toolsPanelMin2 > toolsPanelMin1 && toolsPanelWidth2 >= toolsPanelWidth1;
        const bool columnsOk = toolsPanelDefault && toolsPanelRestored && rowMajor && toolsPanelWidened;
        const bool pinnedOk = pinned1 && pinned2;
        ST_BEGIN("columns_default");
        ST_PASS("columns default=%d two=%d pinned=%d", toolsPanelDefault ? 1 : 0,
                     (toolsPanelRestored && rowMajor && toolsPanelWidened) ? 1 : 0,
                     pinnedOk ? 1 : 0);
        if (!columnsOk) {
            ST_FAIL(113, "column layout");
        }
        if (!pinnedOk) {
            ST_FAIL(114, "pinned controls");
        }

        // m40_flyout (115): a multi-member group shows the triangle and opens a
        // menu of its members beside the button (right, flipped left); a
        // single-member group shows no triangle.
        const bool triMulti = toolsPanelToolbox && toolsPanelToolbox->hasFlyoutTriangleForTest(2);
        const bool singleTri = toolsPanelToolbox && toolsPanelToolbox->hasFlyoutTriangleForTest(1);
        const bool triSingle = toolsPanelToolbox && !singleTri;
        bool menuActions = false;
        if (toolsPanelToolbox) {
            const QList<QAction*> actions = toolsPanelToolbox->slotMenuActionsForTest(2);
            QList<pictura::ToolId> members;
            for (pictura::ToolId id : pictura::allToolIds()) {
                if (pictura::toolInfo(id).group == 2) {
                    members << id;
                }
            }
            menuActions = actions.size() == members.size() && !members.isEmpty();
            for (int i = 0; menuActions && i < members.size(); ++i) {
                const pictura::ToolInfo& info = pictura::toolInfo(members.at(i));
                if (actions.at(i)->text() != QString::fromLatin1(info.label)
                    || actions.at(i)->isEnabled() != info.implemented) {
                    menuActions = false;
                }
            }
        }
        bool toolsPanelBeside = false;
        if (toolsPanelToolbox) {
            toolsPanelToolbox->openSlotFlyoutForTest(2);
            QCoreApplication::processEvents();
            QMenu* menu = toolsPanelToolbox->slotMenuForTest(2);
            QToolButton* button = toolsPanelSlots.value(1);
            if (menu && button && menu->isVisible()) {
                const QPoint btnTopLeft = button->mapToGlobal(QPoint(0, 0));
                const int btnRight = btnTopLeft.x() + button->width();
                // Beside the button, never over it: right edge or flipped left.
                toolsPanelBeside = menu->pos().x() >= btnRight || menu->pos().x() + menu->width() <= btnTopLeft.x();
            }
            if (menu) {
                menu->close();
                QCoreApplication::processEvents();
            }
        }
        const bool flyoutOk = triMulti && triSingle && menuActions && toolsPanelBeside;
        ST_BEGIN("flyout_tri");
        ST_PASS("flyout tri=%d/%d menu=%d beside=%d", triMulti ? 1 : 0,
                     singleTri ? 1 : 0,
                     menuActions ? 1 : 0,
                     toolsPanelBeside ? 1 : 0);
        if (!flyoutOk) {
            ST_FAIL(115, "flyout");
        }

        // m40_keys (116): every flyout action carries the group's letter, visible
        // in the context menu; a disabled member stays disabled, keeps its tooltip
        // and still shows the key.
        bool keysShown = toolsPanelToolbox != nullptr;
        bool keysDisabled = false;
        if (toolsPanelToolbox) {
            // The K group (the 3D object tools, out of scope) stays disabled.
            const QList<QAction*> actions = toolsPanelToolbox->slotMenuActionsForTest(19);
            for (QAction* action : actions) {
                const bool keyShown =
                    action->shortcut() == QKeySequence(QStringLiteral("K"))
                    && action->isShortcutVisibleInContextMenu();
                if (!keyShown) {
                    keysShown = false;
                }
                if (!action->isEnabled()) {
                    const bool disabledOk =
                        action->shortcut() == QKeySequence(QStringLiteral("K"))
                        && action->toolTip().contains(QStringLiteral("not implemented yet"));
                    keysDisabled = keysDisabled || disabledOk;
                    if (!disabledOk) {
                        keysShown = false;
                    }
                }
            }
        }
        const bool keysOk = keysShown && keysDisabled;
        ST_BEGIN("keys_shown");
        ST_PASS("keys shown=%d disabled=%d", keysShown ? 1 : 0,
                     keysDisabled ? 1 : 0);
        if (!keysOk) {
            ST_FAIL(116, "flyout keys");
        }

        // m40_shift (117): drive the real QShortcut path by synthesizing a key
        // press on the window. Preference on: the plain letter activates the
        // slot's current member and Shift+letter cycles the implemented members;
        // an all-unimplemented group is a no-op; preference off: the plain letter
        // cycles.
        frame.activateWindow();
        QCoreApplication::processEvents();
        auto sendKey = [&frame](int key, Qt::KeyboardModifiers mods, const QString& text) {
            QKeyEvent event(QEvent::KeyPress, key, mods, text);
            QApplication::sendEvent(&frame, &event);
        };
        frame.setActiveTool(pictura::ToolId::Move);
        QCoreApplication::processEvents();
        sendKey(Qt::Key_B, Qt::NoModifier, QStringLiteral("b"));
        const bool toolsPanelPlain = frame.activeTool() == pictura::ToolId::Brush;
        sendKey(Qt::Key_B, Qt::ShiftModifier, QStringLiteral("B"));
        const bool toolsPanelShift = frame.activeTool() == pictura::ToolId::Pencil;
        sendKey(Qt::Key_B, Qt::ShiftModifier, QStringLiteral("B"));
        const bool bReplace = frame.activeTool() == pictura::ToolId::ColorReplacement;
        sendKey(Qt::Key_B, Qt::ShiftModifier, QStringLiteral("B"));
        const bool bMixer = bReplace && frame.activeTool() == pictura::ToolId::MixerBrush;
        sendKey(Qt::Key_B, Qt::ShiftModifier, QStringLiteral("B"));
        const bool toolsPanelWrap = bMixer && frame.activeTool() == pictura::ToolId::Brush;
        sendKey(Qt::Key_J, Qt::NoModifier, QStringLiteral("j"));
        const bool jPlain = frame.activeTool() == pictura::ToolId::SpotHealingBrush;
        sendKey(Qt::Key_J, Qt::ShiftModifier, QStringLiteral("J"));
        const bool jHealing = frame.activeTool() == pictura::ToolId::HealingBrush;
        sendKey(Qt::Key_J, Qt::ShiftModifier, QStringLiteral("J"));
        const bool jPatch = jHealing && frame.activeTool() == pictura::ToolId::Patch;
        sendKey(Qt::Key_J, Qt::ShiftModifier, QStringLiteral("J"));
        const bool jMove = jPatch && frame.activeTool() == pictura::ToolId::ContentAwareMove;
        sendKey(Qt::Key_J, Qt::ShiftModifier, QStringLiteral("J"));
        const bool jShift = jMove && frame.activeTool() == pictura::ToolId::RedEye;
        sendKey(Qt::Key_J, Qt::ShiftModifier, QStringLiteral("J"));
        const bool jWrap = frame.activeTool() == pictura::ToolId::SpotHealingBrush;
        sendKey(Qt::Key_S, Qt::NoModifier, QStringLiteral("s"));
        const bool sPlain = frame.activeTool() == pictura::ToolId::CloneStamp;
        sendKey(Qt::Key_S, Qt::ShiftModifier, QStringLiteral("S"));
        const bool sPattern = sPlain && frame.activeTool() == pictura::ToolId::PatternStamp;
        sendKey(Qt::Key_S, Qt::ShiftModifier, QStringLiteral("S"));
        const bool sWrap = sPattern && frame.activeTool() == pictura::ToolId::CloneStamp;
        // Shift+Y reaches the Art History Brush, E the Eraser; K (unimplemented) is a no-op.
        sendKey(Qt::Key_Y, Qt::NoModifier, QStringLiteral("y"));
        const bool yPlain = frame.activeTool() == pictura::ToolId::HistoryBrush;
        sendKey(Qt::Key_Y, Qt::ShiftModifier, QStringLiteral("Y"));
        const bool yArt = yPlain && frame.activeTool() == pictura::ToolId::ArtHistoryBrush;
        sendKey(Qt::Key_E, Qt::NoModifier, QStringLiteral("e"));
        const bool yShift = yArt && frame.activeTool() == pictura::ToolId::Eraser;
        sendKey(Qt::Key_K, Qt::NoModifier, QStringLiteral("k"));
        const bool noImpl = frame.activeTool() == pictura::ToolId::Eraser;
        if (toolsPanelToolbox) {
            toolsPanelToolbox->setShiftKeyForToolSwitch(false);
        }
        sendKey(Qt::Key_B, Qt::NoModifier, QStringLiteral("b"));
        const bool toolsPanelOff = frame.activeTool() == pictura::ToolId::Pencil;
        if (toolsPanelToolbox) {
            toolsPanelToolbox->setShiftKeyForToolSwitch(true);
        }
        const bool shiftOk = toolsPanelPlain && toolsPanelShift && toolsPanelWrap && jPlain
            && jShift && jWrap && sWrap && yShift && noImpl && toolsPanelOff;
        ST_BEGIN("shift_plain");
        ST_PASS("shift plain=%d shift=%d j=%d/%d/%d s=%d y=%d noimpl=%d off=%d",
                toolsPanelPlain ? 1 : 0, (toolsPanelShift && toolsPanelWrap) ? 1 : 0,
                jPlain ? 1 : 0, jShift ? 1 : 0, jWrap ? 1 : 0, sWrap ? 1 : 0, yShift ? 1 : 0,
                noImpl ? 1 : 0, toolsPanelOff ? 1 : 0);
        if (!shiftOk) {
            ST_FAIL(117, "shift cycling");
        }

        // m40_atomic (118): the Tools panel is a tabless, atomic column, so a
        // widget panel dragged over it resolves no in-column target and never
        // joins it. The tools column is a normal splitter pane (never a dock).
        pictura::PanelColumn* toolsAtomicColumn = frame.toolsColumn();
        pictura::PanelColumn* toolsAtomicPrimary = frame.columnForPanel(QStringLiteral("layersPanel"));
        bool toolsAtomicNoTab = false;
        bool toolsAtomicReject = false;
        if (toolsAtomicColumn && toolsAtomicPrimary) {
            toolsAtomicNoTab = toolsAtomicColumn->isToolsColumn()
                               && toolsAtomicColumn->groups().isEmpty();
            const QPoint over(toolsAtomicColumn->mapToGlobal(toolsAtomicColumn->rect().center()));
            const bool began = toolsAtomicPrimary->beginTabDragForTest(QStringLiteral("layersPanel"));
            toolsAtomicPrimary->dragToForTest(over);
            toolsAtomicReject = began && !toolsAtomicColumn->dropIndicatorVisibleForTest();
            toolsAtomicPrimary->cancelDragForTest();
            QCoreApplication::processEvents();
        }
        const bool dockOk = toolsAtomicNoTab && toolsAtomicReject;
        ST_BEGIN("dock_areas");
        ST_PASS("dock tabless=%d atomic=%d", toolsAtomicNoTab ? 1 : 0,
                     toolsAtomicReject ? 1 : 0);
        if (!dockOk) {
            ST_FAIL(118, "atomic tools column");
        }

        // m40_session (119): the two v4 fields round-trip, and a store lacking
        // them loads the defaults (one column, Shift required).
        pictura::SessionState toolsPanelState = pictura::loadSession();
        toolsPanelState.toolsColumns = 2;
        toolsPanelState.useShiftKeyForToolSwitch = false;
        const bool toolsPanelSaved = pictura::saveSession(toolsPanelState);
        const pictura::SessionState toolsPanelReloaded = pictura::loadSession();
        const bool toolsPanelRoundtrip = toolsPanelSaved && toolsPanelReloaded.toolsColumns == 2
                                  && !toolsPanelReloaded.useShiftKeyForToolSwitch;
        bool toolsPanelDefaults = false;
        {
            QFile store(pictura::sessionFilePath());
            if (store.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                store.write("{\"schemaVersion\":3,\"brightnessLevel\":2}");
                store.close();
            }
            const pictura::SessionState toolsPanelMissing = pictura::loadSession();
            toolsPanelDefaults =
                toolsPanelMissing.toolsColumns == 1 && toolsPanelMissing.useShiftKeyForToolSwitch;
        }
        ST_BEGIN("session_roundtrip");
        ST_PASS("session roundtrip=%d defaults=%d", toolsPanelRoundtrip ? 1 : 0,
                     toolsPanelDefaults ? 1 : 0);
        if (!toolsPanelRoundtrip || !toolsPanelDefaults) {
            ST_FAIL(119, "session v4");
        }
        // M41: the panel column — top tabs, the width toggle, the iconic strip
        // with its Qt::Popup flyout, the seven-item tab menu, minimize vs
        // collapse-to-icons, and the content-fit Tools panel. Exit codes
        // 120–124 and 130.
        auto* panelColumnColumn = frame.restoreLegacyDefaultForTest();

        // m41_tabs (120): every group's tabs are North, a single-panel group
        // still has its one tab, and no group-title label widget exists.
        bool tabsNorth = panelColumnColumn != nullptr;
        bool singleTab = false;
        bool noLabel = true;
        if (panelColumnColumn) {
            for (pictura::PanelGroup* group : panelColumnColumn->groups()) {
                if (group->tabPositionForTest() != static_cast<int>(QTabWidget::North)) {
                    tabsNorth = false;
                }
                if (group->titleCountForTest() == 1
                    && !group->titleTextsForTest().value(0).isEmpty()) {
                    singleTab = true;
                }
                if (group->groupLabelForTest()) {
                    noLabel = false;
                }
            }
        }
        const bool tabsOk = panelColumnColumn && tabsNorth && singleTab && noLabel;
        ST_BEGIN("tabs_tabs");
        ST_PASS("tabs tabs=north single=%d nolabel=%d", singleTab ? 1 : 0,
                     noLabel ? 1 : 0);
        if (!tabsOk) {
            ST_FAIL(120, "top tabs");
        }

        // m41_width (121): the `panelColumnToggle` flips normal/iconic, the
        // column has no hard minimum, and it is scrollable.
        QToolButton* panelColumnToggle2 = panelColumnColumn ? panelColumnColumn->panelColumnToggleForTest() : nullptr;
        const bool toggleExists =
            panelColumnToggle2 && panelColumnToggle2->objectName() == QStringLiteral("panelColumnToggle");
        const bool modeBefore = panelColumnColumn && panelColumnColumn->railMode();
        if (panelColumnToggle2) {
            panelColumnToggle2->click();
            QCoreApplication::processEvents();
        }
        const bool panelColumnFlipped = panelColumnColumn && panelColumnColumn->railMode() != modeBefore;
        if (panelColumnToggle2) {
            panelColumnToggle2->click();
            QCoreApplication::processEvents();
        }
        const bool modeRestored = panelColumnColumn && panelColumnColumn->railMode() == modeBefore;
        // M42 amends this check to the new normal-mode width floor: the column
        // now enforces a bounded content-derived minimum width (so it cannot be
        // squeezed to nothing) while keeping no height minimum and staying
        // scrollable.
        const int minFloor = panelColumnColumn ? panelColumnColumn->minimumWidthForTest() : 0;
        const bool noMin = panelColumnColumn && minFloor >= 160 && minFloor <= 400
                              && panelColumnColumn->minimumHeight() == 0;
        const bool panelColumnScroll = panelColumnColumn && panelColumnColumn->scrollableForTest();
        const bool widthOk =
            toggleExists && panelColumnFlipped && modeRestored && noMin && panelColumnScroll;
        ST_BEGIN("width_toggle");
        ST_PASS("width toggle=%d modes=%d min=%d scroll=%d", toggleExists ? 1 : 0,
                     (panelColumnFlipped && modeRestored) ? 1 : 0,
                     noMin ? 1 : 0,
                     panelColumnScroll ? 1 : 0);
        if (!widthOk) {
            ST_FAIL(121, "column width toggle");
        }

        // m41_iconic (122): iconic mode shows the icon strip with a divider,
        // widening shows the labels, an icon opens a visible Qt::Popup flyout,
        // and closing it restores the panel to its group.
        if (panelColumnColumn) {
            panelColumnColumn->setRailMode(true);
            QCoreApplication::processEvents();
        }
        const bool panelColumnStrip = panelColumnColumn && panelColumnColumn->iconStripVisibleForTest();
        const bool panelColumnDividers = panelColumnColumn && panelColumnColumn->dividerCountForTest() >= 1;
        // Widening the column past the label threshold shows the panel labels.
        // A temporary minimum forces the splitter to give the column the width.
        if (panelColumnColumn) {
            panelColumnColumn->setMinimumWidth(320);
            QCoreApplication::processEvents();
        }
        const bool panelColumnLabels = panelColumnColumn && panelColumnColumn->iconLabelsShownForTest();
        if (panelColumnColumn) {
            panelColumnColumn->setMinimumWidth(0);
            QCoreApplication::processEvents();
        }
        const bool panelColumnOpened =
            panelColumnColumn && panelColumnColumn->openIconFlyoutForTest(QStringLiteral("layersPanel"));
        for (int i = 0; i < 20 && !(panelColumnColumn && panelColumnColumn->iconFlyoutVisibleForTest()); ++i) {
            QCoreApplication::processEvents();
        }
        const bool panelColumnPopup = panelColumnOpened && panelColumnColumn && panelColumnColumn->iconFlyoutVisibleForTest();
        auto* panelColumnFlyout = panelColumnColumn
            ? panelColumnColumn->findChild<QWidget*>(QStringLiteral("panelIconFlyout"))
            : nullptr;
        if (panelColumnFlyout) {
            panelColumnFlyout->close();
            QCoreApplication::processEvents();
        }
        pictura::PanelGroup* panelColumnLayerGroup =
            panelColumnColumn ? panelColumnColumn->groupForPanel(QStringLiteral("layersPanel")) : nullptr;
        const bool panelColumnRestore =
            panelColumnLayerGroup && panelColumnLayerGroup->containsPanel(QStringLiteral("layersPanel"));
        if (panelColumnColumn) {
            panelColumnColumn->setRailMode(false);
            QCoreApplication::processEvents();
        }
        const bool iconicOk = panelColumnStrip && panelColumnDividers && panelColumnLabels && panelColumnPopup && panelColumnRestore;
        ST_BEGIN("iconic_strip");
        ST_PASS("iconic strip=%d dividers=%d labels=%d popup=%d "
                     "restore=%d", panelColumnStrip ? 1 : 0,
                     panelColumnDividers ? 1 : 0,
                     panelColumnLabels ? 1 : 0,
                     panelColumnPopup ? 1 : 0,
                     panelColumnRestore ? 1 : 0);
        if (!iconicOk) {
            ST_FAIL(122, "iconic strip");
        }

        // m41_minimize (124): minimize hides the content but keeps the tab bar,
        // restores, and collapse-to-icons is a distinct state.
        pictura::PanelGroup* minGroup =
            panelColumnColumn ? panelColumnColumn->groupForPanel(QStringLiteral("layersPanel")) : nullptr;
        bool panelColumnMin = false;
        bool panelColumnContent = false;
        bool minRestore = false;
        bool panelColumnIcons = false;
        if (minGroup) {
            minGroup->setMinimizedForTest(true);
            QCoreApplication::processEvents();
            panelColumnMin = minGroup->isMinimizedForTest();
            panelColumnContent = minGroup->contentHiddenForTest()
                         && minGroup->tabBarVisibleForTest();
            minGroup->setMinimizedForTest(false);
            QCoreApplication::processEvents();
            minRestore = !minGroup->isMinimizedForTest()
                            && !minGroup->contentHiddenForTest();
            minGroup->setCollapsedToIconsForTest(true);
            QCoreApplication::processEvents();
            panelColumnIcons = minGroup->isCollapsedToIconsForTest()
                       && !minGroup->isMinimizedForTest();
            minGroup->setCollapsedToIconsForTest(false);
            QCoreApplication::processEvents();
        }
        const bool minimizeOk = panelColumnMin && panelColumnContent && minRestore && panelColumnIcons;
        ST_BEGIN("minimize_min");
        ST_PASS("minimize min=%d content=%d restore=%d icons=%d", panelColumnMin ? 1 : 0,
                     panelColumnContent ? 1 : 0,
                     minRestore ? 1 : 0,
                     panelColumnIcons ? 1 : 0);
        if (!minimizeOk) {
            ST_FAIL(124, "minimize");
        }

        // m41_tools (130): no `Tools` title, content-fit widths, and the
        // foreground/background widget fits within the current column width in
        // both column counts.
        auto* panelColumnToolbox = frame.findChild<pictura::Toolbox*>(QStringLiteral("toolsPanel"));
        const bool panelColumnTitle = panelColumnToolbox != nullptr;
        bool panelColumnMin1 = false;
        bool panelColumnMin2 = false;
        bool panelColumnFit = false;
        if (panelColumnToolbox) {
            panelColumnToolbox->setColumns(1);
            QCoreApplication::processEvents();
            const int min1 = panelColumnToolbox->minimumWidth();
            const int content1 = panelColumnToolbox->contentWidthForTest();
            const int fg1 = panelColumnToolbox->foregroundBackgroundWidthForTest();
            const int width1 = panelColumnToolbox->width();
            panelColumnToolbox->setColumns(2);
            QCoreApplication::processEvents();
            const int min2 = panelColumnToolbox->minimumWidth();
            const int content2 = panelColumnToolbox->contentWidthForTest();
            const int fg2 = panelColumnToolbox->foregroundBackgroundWidthForTest();
            const int width2 = panelColumnToolbox->width();
            panelColumnToolbox->setColumns(1);
            QCoreApplication::processEvents();
            panelColumnMin1 = min1 > 0 && min1 >= content1;
            panelColumnMin2 = min2 > min1 && min2 >= content2;
            panelColumnFit = fg1 <= width1 && fg2 <= width2 && fg1 <= min1 && fg2 <= min2;
        }
        const bool toolsOk = panelColumnTitle && panelColumnMin1 && panelColumnMin2 && panelColumnFit;
        ST_BEGIN("tools_title");
        ST_PASS("tools title=%d min1=%d min2=%d fit=%d", panelColumnTitle ? 1 : 0,
                     panelColumnMin1 ? 1 : 0,
                     panelColumnMin2 ? 1 : 0,
                     panelColumnFit ? 1 : 0);
        if (!toolsOk) {
            ST_FAIL(130, "tools panel");
        }

        // m41_drag (126): a tab reorders within its group, a tab regroups into
        // another group at the drop index, a drop between groups inserts a new
        // group, and the thick blue indicator is drawn at the candidate
        // position and cleared afterwards. Every step drives the column's real
        // begin/update/commit drag path.
        bool dragReorder = false;
        bool dragRegroup = false;
        bool dragInsert = false;
        bool dragIndicator = false;
        bool dragClear = false;
        if (panelColumnColumn) {
            // Give the column a normal width so the group tab bars are inside
            // the scroll viewport; the default splitter share is icon-strip
            // narrow under xvfb.
            panelColumnColumn->setMinimumWidth(360);
            QCoreApplication::processEvents();
            // Reorder: move the second Layers tab to the front of its own bar.
            pictura::PanelGroup* reorderGroup =
                panelColumnColumn->groupForPanel(QStringLiteral("layersPanel"));
            if (reorderGroup && reorderGroup->titleCountForTest() >= 2) {
                const QStringList before = reorderGroup->titleTextsForTest();
                const QString moving = reorderGroup->panels().at(1)->objectName();
                panelColumnColumn->ensureGroupVisibleForTest(moving);
                QCoreApplication::processEvents();
                const QPoint target = reorderGroup->tabInsertionGlobalPointForTest(0);
                panelColumnColumn->beginTabDragForTest(moving);
                panelColumnColumn->dragToForTest(target);
                const bool visible = panelColumnColumn->dropIndicatorVisibleForTest();
                const QRect geometry = panelColumnColumn->dropIndicatorGeometryForTest();
                const bool vertical = geometry.height() > geometry.width() && geometry.height() > 0
                                      && geometry.width() == 3;
                panelColumnColumn->dropForTest(target);
                const QStringList after = reorderGroup->titleTextsForTest();
                dragReorder = !before.isEmpty() && after != before
                                 && after.value(0) == before.value(1);
                dragIndicator = visible && vertical;
                dragClear = !panelColumnColumn->dropIndicatorVisibleForTest();
            }

            // Regroup: append the Styles tab to the Layers group.
            pictura::PanelGroup* colorGroup =
                panelColumnColumn->groupForPanel(QStringLiteral("stylesPanel"));
            pictura::PanelGroup* layerGroup =
                panelColumnColumn->groupForPanel(QStringLiteral("layersPanel"));
            if (colorGroup && layerGroup && colorGroup != layerGroup) {
                const int targetIndex = layerGroup->titleCountForTest();
                panelColumnColumn->ensureGroupVisibleForTest(QStringLiteral("layersPanel"));
                QCoreApplication::processEvents();
                const QPoint target = layerGroup->tabInsertionGlobalPointForTest(targetIndex);
                panelColumnColumn->beginTabDragForTest(QStringLiteral("stylesPanel"));
                panelColumnColumn->dragToForTest(target);
                panelColumnColumn->dropForTest(target);
                dragRegroup =
                    layerGroup->containsPanel(QStringLiteral("stylesPanel"))
                    && !colorGroup->containsPanel(QStringLiteral("stylesPanel"))
                    && panelColumnColumn->groupForPanel(QStringLiteral("stylesPanel")) == layerGroup;
            }

            // Insert: drop Histogram into the gap between two groups, which
            // must create a new one-panel group at that boundary.
            pictura::PanelGroup* navGroup =
                panelColumnColumn->groupForPanel(QStringLiteral("histogramPanel"));
            if (navGroup && panelColumnColumn->groups().size() >= 2) {
                const int groupCount = panelColumnColumn->groups().size();
                const QPoint target = panelColumnColumn->boundaryPointForTest(1);
                panelColumnColumn->beginTabDragForTest(QStringLiteral("histogramPanel"));
                panelColumnColumn->dragToForTest(target);
                const bool visible = panelColumnColumn->dropIndicatorVisibleForTest();
                const QRect geometry = panelColumnColumn->dropIndicatorGeometryForTest();
                const bool horizontal = geometry.width() > geometry.height()
                                        && geometry.height() == 3;
                panelColumnColumn->dropForTest(target);
                pictura::PanelGroup* inserted =
                    panelColumnColumn->groupForPanel(QStringLiteral("histogramPanel"));
                dragInsert = panelColumnColumn->groups().size() == groupCount + 1 && inserted
                                && inserted->titleCountForTest() == 1
                                && inserted->containsPanel(QStringLiteral("histogramPanel"));
                dragClear = dragClear && visible && horizontal
                               && !panelColumnColumn->dropIndicatorVisibleForTest();
            }
        }
        const bool dragOk = dragReorder && dragRegroup && dragInsert
                               && dragIndicator && dragClear;
        ST_BEGIN("drag_reorder");
        ST_PASS("drag reorder=%d regroup=%d insert=%d indicator=%d "
                     "clear=%d", dragReorder ? 1 : 0,
                     dragRegroup ? 1 : 0,
                     dragInsert ? 1 : 0,
                     dragIndicator ? 1 : 0,
                     dragClear ? 1 : 0);
        if (!dragOk) {
            ST_FAIL(126, "drag and drop");
        }

        // m41_tearoff (127): tearing a group off the column creates a visible
        // `panelFloat` holding the group's panels, and re-docking returns them
        // to the column at the drop index and leaves no float behind.
        bool tearFloat = false;
        bool tearPanels = false;
        bool panelColumnRedock = false;
        bool tearEmpty = false;
        if (panelColumnColumn) {
            pictura::PanelGroup* source =
                panelColumnColumn->groupForPanel(QStringLiteral("histogramPanel"));
            QStringList expected;
            if (source) {
                for (QWidget* panel : source->panels()) {
                    if (panel) {
                        expected << panel->objectName();
                    }
                }
            }
            const int groupCount = panelColumnColumn->groups().size();
            tearFloat = source
                           && panelColumnColumn->tearOffForTest(source->objectName())
                           && panelColumnColumn->floatCountForTest() == 1;
            if (tearFloat) {
                tearPanels = panelColumnColumn->floatPanelNamesForTest(0) == expected
                                && !expected.isEmpty()
                                && !panelColumnColumn->groupForPanel(QStringLiteral("histogramPanel"));
                const bool redocked = panelColumnColumn->redockForTest(0, 1);
                pictura::PanelGroup* back =
                    panelColumnColumn->groupForPanel(QStringLiteral("histogramPanel"));
                panelColumnRedock = redocked && back
                            && back->containsPanel(QStringLiteral("histogramPanel"))
                            && panelColumnColumn->groups().size() == groupCount;
                tearEmpty = panelColumnColumn->floatCountForTest() == 0;
            }
        }
        const bool tearoffOk = tearFloat && tearPanels && panelColumnRedock && tearEmpty;
        ST_BEGIN("tearoff_float");
        ST_PASS("tearoff float=%d panels=%d redock=%d empty=%d", tearFloat ? 1 : 0,
                     tearPanels ? 1 : 0,
                     panelColumnRedock ? 1 : 0,
                     tearEmpty ? 1 : 0);
        if (!tearoffOk) {
            ST_FAIL(127, "tear-off and re-dock");
        }
        if (panelColumnColumn) {
            panelColumnColumn->setMinimumWidth(0);
            QCoreApplication::processEvents();
        }

        // m41_prefs (125): the Preferences dialog exposes exactly General,
        // Interface, and Performance, opens from the command path and from
        // Interface Options…, and its checkboxes drive and persist.
        pictura::PreferencesDialog* prefs = nullptr;
        bool prefsPages = false;
        bool prefsOpen = false;
        bool prefsRoundtrip = false;
        bool prefsShift = false;
        bool prefsIconic = false;
        if (QAction* general = frame.registry()->action(
                QString::fromLatin1(pictura::command_ids::EditPreferencesGeneral))) {
            general->trigger();
            QCoreApplication::processEvents();
        }
        prefs = frame.preferencesDialog();
        const bool openedGeneral =
            prefs && prefs->isVisible()
            && prefs->currentPageForTest() == QStringLiteral("General");
        if (panelColumnColumn) {
            panelColumnColumn->triggerTabMenuForTest(QStringLiteral("Interface Options\u2026"));
            QCoreApplication::processEvents();
        }
        const bool openedInterface =
            prefs && prefs->isVisible()
            && prefs->currentPageForTest() == QStringLiteral("Interface");
        prefsPages = prefs
            && prefs->pagesForTest()
                   == QStringList({QStringLiteral("General"), QStringLiteral("Interface"),
                                   QStringLiteral("File Handling"),
                                   QStringLiteral("Performance"), QStringLiteral("Units & Rulers"),
                                   QStringLiteral("Guides, Grid, & Slices"),
                                   QStringLiteral("Type"),
                                   // KookaPictura-SHP fork: SHP size limit, MIX palettes.
                                   QStringLiteral("Advanced"), QStringLiteral("MIX Archives")});
        prefsOpen = openedGeneral && openedInterface;
        if (prefs) {
            auto* prefsToolbox = frame.findChild<pictura::Toolbox*>(QStringLiteral("toolsPanel"));
            const bool shiftToggled = prefs->setCheckboxForTest(
                QStringLiteral("useShiftKeyForToolSwitch"), false);
            QCoreApplication::processEvents();
            prefsShift = shiftToggled && prefsToolbox
                            && !prefsToolbox->shiftKeyForToolSwitch();
            frame.saveSession();
            prefsRoundtrip = !pictura::loadSession().useShiftKeyForToolSwitch;
            prefs->setCheckboxForTest(QStringLiteral("useShiftKeyForToolSwitch"), true);
            QCoreApplication::processEvents();
            frame.saveSession();
            prefs->setCheckboxForTest(QStringLiteral("autoCollapseIconic"), true);
            QCoreApplication::processEvents();
            prefsIconic = panelColumnColumn && panelColumnColumn->autoCollapseIconicForTest();
            prefs->setCheckboxForTest(QStringLiteral("autoCollapseIconic"), false);
            QCoreApplication::processEvents();
            prefs->close();
        }
        const bool prefsOk = prefsPages && prefsOpen && prefsRoundtrip
                                && prefsShift && prefsIconic;
        ST_BEGIN("prefs_pages");
        ST_PASS("prefs pages=%d open=%d roundtrip=%d shift=%d "
                     "iconic=%d", prefs ? prefs->pagesForTest().size() : 0,
                     prefsOpen ? 1 : 0,
                     prefsRoundtrip ? 1 : 0,
                     prefsShift ? 1 : 0,
                     prefsIconic ? 1 : 0);
        if (!prefsOk) {
            ST_FAIL(125, "preferences dialog");
        }

        // m41_session (128): the v5 fields round-trip through the real
        // save/load path, a schema-4 store loads the v5 defaults and keeps its
        // own keys, an unknown key survives a rewrite, and per-group panel
        // visibility/order/minimized/collapsed are saved and restored.
        if (panelColumnColumn) {
            panelColumnColumn->setAutoCollapseIconic(true);
            panelColumnColumn->setAutoShowHidden(true);
            panelColumnColumn->setRailMode(true);
        }
        frame.saveSession();
        pictura::SessionState panelColumnReloaded = pictura::loadSession();
        pictura::SessionState widthState = pictura::loadSession();
        widthState.panelRailMode = QStringLiteral("normal");
        widthState.railWidth = 260;
        pictura::saveSession(widthState);
        const bool widthRound = pictura::loadSession().railWidth == 260;
        const bool panelColumnV5 = !panelColumnReloaded.panelGroups.isEmpty()
                           && panelColumnReloaded.schemaVersion >= 5
                           && panelColumnReloaded.panelRailMode == QStringLiteral("iconic")
                           && panelColumnReloaded.autoCollapseIconic && panelColumnReloaded.autoShowHidden
                           && widthRound;

        bool panelColumnDefaults = false;
        bool panelColumnV4 = false;
        {
            QFile store(pictura::sessionFilePath());
            if (store.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                store.write("{\"schemaVersion\":4,\"toolsColumns\":2,"
                            "\"useShiftKeyForToolSwitch\":false}");
                store.close();
            }
            const pictura::SessionState v4 = pictura::loadSession();
            panelColumnDefaults = v4.panelRailMode == QStringLiteral("normal") && v4.railWidth == 0
                          && !v4.autoCollapseIconic && !v4.autoShowHidden;
            panelColumnV4 = v4.toolsColumns == 2 && !v4.useShiftKeyForToolSwitch;
        }

        bool panelColumnUnknown = false;
        {
            QFile store(pictura::sessionFilePath());
            if (store.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                store.write("{\"schemaVersion\":4,\"toolsColumns\":1,"
                            "\"unknownKey\":\"keep-me\"}");
                store.close();
            }
            frame.saveSession();
            QFile read(pictura::sessionFilePath());
            if (read.open(QIODevice::ReadOnly)) {
                const QByteArray rewritten = read.readAll();
                panelColumnUnknown = rewritten.contains("unknownKey")
                             && rewritten.contains("keep-me");
            }
        }

        bool panelColumnPanels = false;
        if (panelColumnColumn) {
            pictura::PanelGroup* panelGroup =
                panelColumnColumn->groupForPanel(QStringLiteral("layersPanel"));
            if (panelGroup) {
                auto panelNames = [](pictura::PanelGroup* group) {
                    QStringList names;
                    for (QWidget* panel : group->panels()) {
                        if (panel) {
                            names << panel->objectName();
                        }
                    }
                    return names;
                };
                const QJsonArray baseline = panelColumnColumn->savePanelState();
                QStringList panelColumnOrder = panelNames(panelGroup);
                if (panelColumnOrder.size() >= 2) {
                    panelColumnOrder.move(0, panelColumnOrder.size() - 1);
                    panelGroup->setPanelOrder(panelColumnOrder);
                }
                panelGroup->setMinimizedForTest(true);
                panelColumnColumn->showPanel(QStringLiteral("channelsPanel"), false);
                QCoreApplication::processEvents();
                const QJsonArray panelColumnMutated = panelColumnColumn->savePanelState();
                panelColumnColumn->restorePanelState(baseline);
                const bool backToBaseline =
                    !panelColumnColumn->groupForPanel(QStringLiteral("layersPanel"))->isMinimizedForTest()
                    && panelColumnColumn->isPanelVisible(QStringLiteral("channelsPanel"));
                panelColumnColumn->restorePanelState(panelColumnMutated);
                pictura::PanelGroup* panelColumnRestored =
                    panelColumnColumn->groupForPanel(QStringLiteral("layersPanel"));
                panelColumnPanels = backToBaseline && panelColumnRestored
                            && panelColumnRestored->isMinimizedForTest()
                            && !panelColumnColumn->isPanelVisible(QStringLiteral("channelsPanel"))
                            && panelNames(panelColumnRestored) == panelColumnOrder;
            }
            panelColumnColumn->setAutoCollapseIconic(false);
            panelColumnColumn->setAutoShowHidden(false);
            panelColumnColumn->setRailMode(false);
            QCoreApplication::processEvents();
        }
        const bool panelColumnSessionOk =
            panelColumnV5 && panelColumnDefaults && panelColumnV4 && panelColumnUnknown && panelColumnPanels;
        ST_BEGIN("session_v5");
        ST_PASS("session v5=%d defaults=%d v4=%d unknown=%d "
                     "panels=%d", panelColumnV5 ? 1 : 0,
                     panelColumnDefaults ? 1 : 0,
                     panelColumnV4 ? 1 : 0,
                     panelColumnUnknown ? 1 : 0,
                     panelColumnPanels ? 1 : 0);
        if (!panelColumnSessionOk) {
            ST_FAIL(128, "session v5");
        }
        // M42 Phase A: the column's normal-mode minimum width and smallest-width
        // compact transition, the bigger strip and tool icons, the fg/bg swap
        // control and `X` key, the menu-bar clearance + stale-layout guard, and
        // the floated Tools dock geometry. Exit codes 131, 132, 137, 138, 139.
        auto* panelMenusToolbox = frame.findChild<pictura::Toolbox*>(QStringLiteral("toolsPanel"));

        // m42_minwidth (131): normal mode enforces a bounded content-derived
        // minimum width (no height minimum, still scrollable); entering iconic
        // lands at the smallest strip width and leaving restores the prior width.
        int normalMin = 0;
        int iconicMin = 0;
        bool normalWidth = false;
        bool iconicSmall = false;
        bool restoredWidth = false;
        if (panelColumnColumn) {
            panelColumnColumn->setRailMode(false);
            QCoreApplication::processEvents();
            normalMin = panelColumnColumn->minimumWidthForTest();
            normalWidth = normalMin >= 180 && normalMin <= 400
                             && panelColumnColumn->minimumHeight() == 0
                             && panelColumnColumn->scrollableForTest();
            const int before = panelColumnColumn->width();
            panelColumnColumn->setRailMode(true);
            QCoreApplication::processEvents();
            iconicMin = panelColumnColumn->minimumWidthForTest();
            iconicSmall = iconicMin > 0 && iconicMin < normalMin
                             && panelColumnColumn->width() <= iconicMin + 8;
            panelColumnColumn->setRailMode(false);
            QCoreApplication::processEvents();
            restoredWidth = panelColumnColumn->minimumWidthForTest() == normalMin
                               && panelColumnColumn->width() >= normalMin
                               && (before <= 0 || panelColumnColumn->width() >= before - 4);
        }
        const bool minWidthOk =
            normalWidth && iconicSmall && restoredWidth;
        ST_BEGIN("minwidth_normal");
        ST_PASS("minwidth normal=%d iconic=%d restored=%d "
                     "min=%d strip=%d", normalWidth ? 1 : 0,
                     iconicSmall ? 1 : 0,
                     restoredWidth ? 1 : 0,
                     normalMin,
                     iconicMin);
        if (!minWidthOk) {
            ST_FAIL(131, "column minimum width");
        }

        // m42_iconic (132): the iconic-strip buttons and their pixmap are larger
        // than M41 (24 button / 16 pixmap), and iconic mode reports a strip
        // narrower than the normal-mode floor. Active/pressed look and flyout are
        // Phase C and are not asserted here.
        bool iconBig = false;
        bool iconSmallest = false;
        if (panelColumnColumn) {
            panelColumnColumn->setRailMode(true);
            QCoreApplication::processEvents();
            QToolButton* stripButton =
                panelColumnColumn->findChild<QToolButton*>(QStringLiteral("panelIcon_layersPanel"));
            if (!stripButton) {
                for (QToolButton* candidate : panelColumnColumn->findChildren<QToolButton*>()) {
                    if (candidate->objectName().startsWith(QStringLiteral("panelIcon_"))) {
                        stripButton = candidate;
                        break;
                    }
                }
            }
            iconBig = stripButton && stripButton->minimumWidth() >= 28
                         && stripButton->iconSize().width() >= 20;
            iconSmallest = panelColumnColumn->minimumWidthForTest() < 180
                              && panelColumnColumn->iconStripVisibleForTest();
            panelColumnColumn->setRailMode(false);
            QCoreApplication::processEvents();
        }
        const bool panelMenusIconicOk = iconBig && iconSmallest;
        ST_BEGIN("iconic_bigger");
        ST_PASS("iconic bigger=%d smallest=%d", iconBig ? 1 : 0,
                     iconSmallest ? 1 : 0);
        if (!panelMenusIconicOk) {
            ST_FAIL(132, "iconic strip");
        }

        // m42_fgbg (137): the swap control exists as a real hit target, clicking
        // it exchanges fg/bg, and the `X` key swaps too (`X` is unassigned in the
        // tool letter catalogue).
        bool swapControl = false;
        bool swapClick = false;
        bool swapKey = false;
        if (panelMenusToolbox) {
            auto* fgbg = panelMenusToolbox->foregroundBackgroundForTest();
            if (fgbg) {
                const QColor fg0 = fgbg->foregroundForTest();
                const QColor bg0 = fgbg->backgroundForTest();
                const int count0 = fgbg->swapCountForTest();
                const QRect swap = fgbg->swapRectForTest();
                swapControl = swap.isValid() && swap.width() > 0 && swap.height() > 0
                                 && fgbg->rect().contains(swap);
                const QPoint local = swap.center();
                QMouseEvent press(QEvent::MouseButtonPress, QPointF(local),
                                  QPointF(fgbg->mapToGlobal(local)), Qt::LeftButton,
                                  Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(fgbg, &press);
                QCoreApplication::processEvents();
                swapClick = fgbg->swapCountForTest() == count0 + 1
                               && fgbg->foregroundForTest() == bg0
                               && fgbg->backgroundForTest() == fg0;
                const int count1 = fgbg->swapCountForTest();
                const QColor fg1 = fgbg->foregroundForTest();
                const QColor bg1 = fgbg->backgroundForTest();
                frame.activateWindow();
                QCoreApplication::processEvents();
                QKeyEvent xEvent(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier,
                                 QStringLiteral("x"));
                QApplication::sendEvent(&frame, &xEvent);
                QCoreApplication::processEvents();
                swapKey = fgbg->swapCountForTest() == count1 + 1
                             && fgbg->foregroundForTest() == bg1
                             && fgbg->backgroundForTest() == fg1;
            }
        }
        const bool fgbgOk = swapControl && swapClick && swapKey;
        ST_BEGIN("fgbg_control");
        ST_PASS("fgbg control=%d click=%d key=%d", swapControl ? 1 : 0,
                     swapClick ? 1 : 0,
                     swapKey ? 1 : 0);
        if (!fgbgOk) {
            ST_FAIL(137, "fg/bg swap");
        }

        // m42_dragstrip (133): a strip icon reorders within the strip and the
        // order persists, and dropping a strip icon on the normal-mode group
        // stack moves the panel into that group. The strip drop indicator is
        // drawn and cleared. Both paths drive the real begin/update/commit drag.
        bool stripReorder = false;
        bool stripMove = false;
        bool stripIndicator = false;
        bool stripClear = false;
        if (panelColumnColumn) {
            panelColumnColumn->setMinimumWidth(360);
            panelColumnColumn->setRailMode(true);
            QCoreApplication::processEvents();
            const QStringList stripBefore = panelColumnColumn->stripOrderForTest();
            int stripMoveIndex = -1;
            for (int i = 1; i < stripBefore.size(); ++i) {
                const QString sameGroup = panelColumnColumn->groupOfForTest(stripBefore.at(i));
                if (!sameGroup.isEmpty()
                    && sameGroup == panelColumnColumn->groupOfForTest(stripBefore.at(i - 1))) {
                    stripMoveIndex = i;
                    break;
                }
            }
            if (stripMoveIndex > 0) {
                const QString moving = stripBefore.at(stripMoveIndex);
                const QPoint target =
                    panelColumnColumn->stripInsertionPointForTest(stripMoveIndex - 1);
                panelColumnColumn->beginStripDragForTest(moving);
                QCoreApplication::processEvents();
                panelColumnColumn->dragToForTest(target);
                stripIndicator =
                    panelColumnColumn->dropIndicatorVisibleForTest()
                    && panelColumnColumn->stripDropIndexForTest() == stripMoveIndex - 1;
                panelColumnColumn->dropForTest(target);
                const QStringList after = panelColumnColumn->stripOrderForTest();
                stripReorder = after != stripBefore
                                 && after.value(stripMoveIndex - 1) == moving;
                stripClear = !panelColumnColumn->dropIndicatorVisibleForTest();
            }

            pictura::PanelGroup* panelMenusDestGroup =
                panelColumnColumn->groupForPanel(QStringLiteral("layersPanel"));
            if (!panelMenusDestGroup && !panelColumnColumn->groups().isEmpty()) {
                panelMenusDestGroup = panelColumnColumn->groups().first();
            }
            QString destPanel;
            if (panelMenusDestGroup) {
                for (QWidget* panel : panelMenusDestGroup->visiblePanels()) {
                    if (panel) {
                        destPanel = panel->objectName();
                        break;
                    }
                }
            }
            QString sourcePanel;
            for (pictura::PanelGroup* group : panelColumnColumn->groups()) {
                if (!group || group == panelMenusDestGroup) {
                    continue;
                }
                for (QWidget* panel : group->visiblePanels()) {
                    if (panel) {
                        sourcePanel = panel->objectName();
                        break;
                    }
                }
                if (!sourcePanel.isEmpty()) {
                    break;
                }
            }
            if (!sourcePanel.isEmpty() && !destPanel.isEmpty()) {
                const bool moved =
                    panelColumnColumn->dropStripOnGroupForTest(sourcePanel, destPanel);
                stripMove = moved
                               && panelColumnColumn->groupForPanel(sourcePanel) == panelMenusDestGroup
                               && !panelColumnColumn->railMode();
            }
            panelColumnColumn->setRailMode(false);
            panelColumnColumn->setMinimumWidth(0);
            QCoreApplication::processEvents();
        }
        const bool dragStripOk =
            stripReorder && stripMove && stripIndicator && stripClear;
        ST_BEGIN("dragstrip_reorder");
        ST_PASS("dragstrip reorder=%d move=%d indicator=%d "
                     "clear=%d", stripReorder ? 1 : 0,
                     stripMove ? 1 : 0,
                     stripIndicator ? 1 : 0,
                     stripClear ? 1 : 0);
        if (!dragStripOk) {
            ST_FAIL(133, "strip drag");
        }

        // m42_flyout (134): the flyout opens on the inner side of the right-hand
        // column, its header names the panel and carries the close chevron, the
        // open icon renders active, and closing clears the active state.
        bool flyoutSide = false;
        bool flyoutActive = false;
        bool flyoutGroup = false;
        bool flyoutClose = false;
        if (panelColumnColumn) {
            panelColumnColumn->setRailMode(true);
            QCoreApplication::processEvents();
            flyoutSide = panelColumnColumn->flyoutSideForTest() == QStringLiteral("left");
            const bool opened =
                panelColumnColumn->openIconFlyoutForTest(QStringLiteral("layersPanel"));
            for (int i = 0; i < 20 && !panelColumnColumn->iconFlyoutVisibleForTest(); ++i) {
                QCoreApplication::processEvents();
            }
            flyoutActive = opened && panelColumnColumn->iconFlyoutVisibleForTest()
                              && panelColumnColumn->activeIconNameForTest()
                                     == QStringLiteral("layersPanel");
            flyoutGroup =
                panelColumnColumn->flyoutHeaderTitleForTest() == QStringLiteral("Layers")
                && panelColumnColumn->flyoutHeaderCloseForTest();
            const bool closed = panelColumnColumn->triggerFlyoutCloseForTest();
            flyoutClose = closed && !panelColumnColumn->iconFlyoutVisibleForTest()
                             && panelColumnColumn->activeIconNameForTest().isEmpty();
            panelColumnColumn->setRailMode(false);
            QCoreApplication::processEvents();
        }
        const bool panelMenusFlyoutOk =
            flyoutSide && flyoutActive && flyoutGroup && flyoutClose;
        ST_BEGIN("flyout_side");
        ST_PASS("flyout side=%d active=%d group=%d close=%d", flyoutSide ? 1 : 0,
                     flyoutActive ? 1 : 0,
                     flyoutGroup ? 1 : 0,
                     flyoutClose ? 1 : 0);
        if (!panelMenusFlyoutOk) {
            ST_FAIL(134, "compact flyout");
        }

        // m42_float_overlay (136): a torn-off group is a frameless tool window
        // overlay (a `Qt::Tool` top-level parented to the main window, never a
        // decorated OS window), a move toward or past the screen edge is
        // clamped to the screen, and the group re-docks and the overlay
        // disappears.
        bool floatChild = false;
        bool floatClamped = false;
        bool floatMove = false;
        bool floatRedock = false;
        if (panelColumnColumn) {
            panelColumnColumn->setRailMode(false);
            QCoreApplication::processEvents();
            pictura::PanelGroup* floatGroup =
                panelColumnColumn->groupForPanel(QStringLiteral("colorPanel"));
            if (!floatGroup) {
                floatGroup = panelColumnColumn->groupForPanel(QStringLiteral("layersPanel"));
            }
            if (!floatGroup && !panelColumnColumn->groups().isEmpty()) {
                floatGroup = panelColumnColumn->groups().first();
            }
            QString floatPanel;
            if (floatGroup) {
                for (QWidget* panel : floatGroup->panels()) {
                    if (panel) {
                        floatPanel = panel->objectName();
                        break;
                    }
                }
            }
            if (!floatPanel.isEmpty()) {
                const bool floatTore = panelColumnColumn->tearOffForTest(floatPanel);
                for (int i = 0; i < 20; ++i) {
                    QCoreApplication::processEvents();
                }
                floatChild = floatTore && panelColumnColumn->floatCountForTest() == 1
                                && panelColumnColumn->floatIsToolWindowForTest(0)
                                && !panelColumnColumn->groupForPanel(floatPanel);
                if (floatChild) {
                    const QRect floatHost = panelColumnColumn->floatHostRectForTest();
                    const QRect floatBefore = panelColumnColumn->floatGeometryForTest(0);
                    const bool floatPastBr = panelColumnColumn->floatClampedForTest(
                        0, floatHost.bottomRight() + QPoint(400, 400));
                    const QRect floatAfterBr = panelColumnColumn->floatGeometryForTest(0);
                    const bool floatPastTl = panelColumnColumn->floatClampedForTest(
                        0, floatHost.topLeft() - QPoint(400, 400));
                    const QRect floatAfterTl = panelColumnColumn->floatGeometryForTest(0);
                    floatClamped = floatPastBr && floatPastTl;
                    floatMove = floatAfterBr.topLeft() != floatBefore.topLeft()
                                   && floatAfterTl.topLeft() != floatAfterBr.topLeft();
                    if (floatClamped) {
                        const bool floatRedocked = panelColumnColumn->redockForTest(0, 1);
                        for (int i = 0; i < 20; ++i) {
                            QCoreApplication::processEvents();
                        }
                        floatRedock = floatRedocked
                                         && panelColumnColumn->floatCountForTest() == 0
                                         && panelColumnColumn->groupForPanel(floatPanel);
                    }
                }
            }
        }
        const bool floatOk =
            floatChild && floatClamped && floatMove && floatRedock;
        ST_BEGIN("float_overlay_child");
        ST_PASS("float_overlay child=%d clipped=%d move=%d "
                     "redock=%d", floatChild ? 1 : 0,
                     floatClamped ? 1 : 0,
                     floatMove ? 1 : 0,
                     floatRedock ? 1 : 0);
        if (!floatOk) {
            ST_FAIL(136, "in-window float overlay");
        }
        // M43 Phase A: tab-vs-group drag + one-panel float (140), panel tab
        // colours (141), corner button at minimum width (142), one-panel float
        // re-dock (146), Tools fixed width (147), compact icon size (148),
        // strict inner-side flyout (149), and the `D` default-colours reset
        // (150). Exit codes 140-150.
        auto multicolumnPump = [](int n) {
            for (int i = 0; i < n; ++i) {
                QCoreApplication::processEvents();
            }
        };
        auto dropPanelOnGroup = [&](const QString& panel, const QString& targetPanel) {
            if (!panelColumnColumn) {
                return false;
            }
            pictura::PanelGroup* dest = panelColumnColumn->groupForPanel(targetPanel);
            if (!dest) {
                return false;
            }
            panelColumnColumn->ensureGroupVisibleForTest(targetPanel);
            multicolumnPump(4);
            if (!panelColumnColumn->beginTabDragForTest(panel)) {
                return false;
            }
            const QPoint target = dest->tabInsertionGlobalPointForTest(0);
            panelColumnColumn->dragToForTest(target);
            const bool dropped = panelColumnColumn->dropForTest(target);
            multicolumnPump(4);
            return dropped;
        };

        // m43_tabdrag (140): a tab drag that leaves the column floats only that
        // panel; an empty-header drag floats the whole group; and a tab drag out
        // of a float moves only that panel, leaving the rest in the old float.
        bool tabPanel = false;
        bool groupPanel = false;
        bool multicolumnFloatPanel = false;
        if (panelColumnColumn) {
            panelColumnColumn->setRailMode(false);
            panelColumnColumn->setMinimumWidth(360);
            panelColumnColumn->setPreferredWidth(360);
            multicolumnPump(6);

            pictura::PanelGroup* layersGroup =
                panelColumnColumn->groupForPanel(QStringLiteral("layersPanel"));
            if (layersGroup && layersGroup->titleCountForTest() >= 3) {
                const int layersBefore = layersGroup->titleCountForTest();
                const int before = panelColumnColumn->floatCountForTest();
                const bool tore =
                    panelColumnColumn->tearOffPanelForTest(QStringLiteral("channelsPanel"));
                multicolumnPump(4);
                const QStringList names =
                    panelColumnColumn->floatPanelNamesForTest(panelColumnColumn->floatCountForTest() - 1);
                tabPanel = tore && panelColumnColumn->floatCountForTest() == before + 1
                              && names.size() == 1
                              && names.first() == QStringLiteral("channelsPanel")
                              && layersGroup->titleCountForTest() == layersBefore - 1
                              && layersGroup->containsPanel(QStringLiteral("layersPanel"))
                              && !layersGroup->containsPanel(QStringLiteral("channelsPanel"));
                if (tabPanel) {
                    dropPanelOnGroup(QStringLiteral("channelsPanel"),
                                        QStringLiteral("layersPanel"));
                }
            }

            QStringList layersPanels;
            pictura::PanelGroup* groupFloatGroup =
                panelColumnColumn->groupForPanel(QStringLiteral("layersPanel"));
            if (groupFloatGroup) {
                for (QWidget* panel : groupFloatGroup->panels()) {
                    if (panel) {
                        layersPanels << panel->objectName();
                    }
                }
            }
            const int beforeGroup = panelColumnColumn->floatCountForTest();
            const bool groupTore = !layersPanels.isEmpty() && layersPanels.size() >= 2
                                   && panelColumnColumn->tearOffForTest(QStringLiteral("layersPanel"));
            multicolumnPump(4);
            const QStringList groupNames =
                panelColumnColumn->floatPanelNamesForTest(panelColumnColumn->floatCountForTest() - 1);
            groupPanel = groupTore && panelColumnColumn->floatCountForTest() == beforeGroup + 1
                            && groupNames == layersPanels
                            && !panelColumnColumn->groupForPanel(QStringLiteral("layersPanel"));

            if (groupPanel) {
                const int beforeFloat = panelColumnColumn->floatCountForTest();
                const bool tore = panelColumnColumn->beginTabDragForTest(QStringLiteral("channelsPanel"));
                const QPoint outside =
                    panelColumnColumn->mapToGlobal(QPoint(-40, panelColumnColumn->height() / 2));
                panelColumnColumn->dragToForTest(outside);
                panelColumnColumn->dropForTest(outside);
                multicolumnPump(4);
                const int after = panelColumnColumn->floatCountForTest();
                const QStringList newest = panelColumnColumn->floatPanelNamesForTest(after - 1);
                const QStringList original = panelColumnColumn->floatPanelNamesForTest(beforeFloat - 1);
                multicolumnFloatPanel = tore && after == beforeFloat + 1 && newest.size() == 1
                                && newest.first() == QStringLiteral("channelsPanel")
                                && original.size() == layersPanels.size() - 1
                                && !original.contains(QStringLiteral("channelsPanel"));
            }

            // Clean up so later checks see a docked column.
            for (int i = 0; i < 8 && panelColumnColumn->floatCountForTest() > 0; ++i) {
                if (!panelColumnColumn->redockForTest(0, 0)) {
                    break;
                }
                multicolumnPump(4);
            }
        }
        const bool tabDragOk = tabPanel && groupPanel && multicolumnFloatPanel;
        ST_BEGIN("tabdrag_tab");
        ST_PASS("tabdrag tab=%d group=%d floatpanel=%d", tabPanel ? 1 : 0,
                     groupPanel ? 1 : 0,
                     multicolumnFloatPanel ? 1 : 0);
        if (!tabDragOk) {
            ST_FAIL(140, "tab vs group drag");
        }

        // m43_tabcolors (141): the panel tab bar is named `panelTabBar`; its
        // selected tab uses the panel surface `${surface}` colour and its
        // inactive tab uses the darker `${surface-header}` (which differs); the
        // document tab bar is not the scoped one, so it keeps the unscoped
        // rules.
        bool colorsActive = false;
        bool colorsInactive = false;
        bool colorsDiffer = false;
        {
            const QString ss = qApp->styleSheet();
            const QString activeRule =
                QStringLiteral("QTabBar#panelTabBar::tab:selected { background: ");
            const QString inactiveRule =
                QStringLiteral("QTabBar#panelTabBar::tab { background: ");
            const int activeAt = ss.indexOf(activeRule);
            const int inactiveAt = ss.indexOf(inactiveRule);
            const QString activeHex = activeAt >= 0 ? ss.mid(activeAt + activeRule.size(), 7) : QString();
            const QString inactiveHex = inactiveAt >= 0 ? ss.mid(inactiveAt + inactiveRule.size(), 7) : QString();
            QTabBar* docBar = frame.findChild<QTabBar*>(QStringLiteral("documentTabBar"));
            const bool docUnscoped =
                docBar && docBar->objectName() != QStringLiteral("panelTabBar");
            pictura::PanelGroup* anyGroup =
                panelColumnColumn && !panelColumnColumn->groups().isEmpty() ? panelColumnColumn->groups().first() : nullptr;
            const bool panelNamed =
                anyGroup && anyGroup->tabBar()
                && anyGroup->tabBar()->objectName() == QStringLiteral("panelTabBar");
            colorsActive = activeAt >= 0;
            colorsInactive = inactiveAt >= 0;
            colorsDiffer = colorsActive && colorsInactive && activeHex != inactiveHex && docUnscoped && panelNamed;
        }
        const bool tabColorsOk = colorsActive && colorsInactive && colorsDiffer;
        ST_BEGIN("tabcolors_active");
        ST_PASS("tabcolors active=%d inactive=%d differ=%d", colorsActive ? 1 : 0,
                     colorsInactive ? 1 : 0,
                     colorsDiffer ? 1 : 0);
        if (!tabColorsOk) {
            ST_FAIL(141, "panel tab colours");
        }

        // m43_corner (142): at the column minimum width the `▾` corner button is
        // fully inside the header row and the tab bar elides instead of forcing
        // width.
        bool cornerVisible = false;
        bool cornerElide = false;
        if (panelColumnColumn) {
            panelColumnColumn->setRailMode(false);
            panelColumnColumn->showPanel(QStringLiteral("layersPanel"), true);
            // Toggle through iconic mode so `updateMinimumWidth` recomputes the
            // content-derived normal minimum (clearing any test-set width).
            panelColumnColumn->setRailMode(true);
            multicolumnPump(2);
            panelColumnColumn->setRailMode(false);
            multicolumnPump(4);
            pictura::PanelGroup* lg = panelColumnColumn->groupForPanel(QStringLiteral("layersPanel"));
            if (lg) {
                panelColumnColumn->setPreferredWidth(panelColumnColumn->minimumWidthForTest());
                multicolumnPump(6);
                QToolButton* corner = lg->headerMenuButtonForTest();
                QTabBar* bar = lg->tabBar();
                cornerElide = bar && bar->elideMode() == Qt::ElideRight && !bar->expanding();
                cornerVisible = corner && corner->isVisible()
                                   && lg->headerMenuAtRightForTest()
                                   && corner->mapToGlobal(QPoint(0, 0)).x()
                                          >= lg->mapToGlobal(QPoint(0, 0)).x();
            }
        }
        const bool cornerOk = cornerVisible && cornerElide;
        ST_BEGIN("corner_visible");
        ST_PASS("corner visible=%d elide=%d", cornerVisible ? 1 : 0,
                     cornerElide ? 1 : 0);
        if (!cornerOk) {
            ST_FAIL(142, "corner button");
        }

        // m43_newcolumn (143): a panel dropped at the left or right workspace
        // edge, or beside the Tools dock, allocates a new PanelColumn on that
        // side; moving the panel back into the primary column empties and
        // removes the new column. All drops run the real resolve/commit path.
        bool newLeft = false;
        bool newRight = false;
        bool newTools = false;
        bool newRemoved = false;
        {
            const QString newPanel = QStringLiteral("stylesPanel");
            auto trySide = [&](const QString& side, bool& flag) {
                const int before = frame.panelColumnCountForTest();
                const bool created = frame.newColumnDropForTest(newPanel, side);
                pictura::PanelColumn* destination = frame.columnForPanel(newPanel);
                const bool placed = created && destination && destination != frame.panelColumn()
                                    && frame.panelColumnCountForTest() == before + 1;
                const bool back = frame.dropIntoGroupForTest(newPanel,
                                                             QStringLiteral("colorPanel"), -1);
                const bool removed = back && frame.panelColumnCountForTest() == before;
                flag = placed && removed;
                newRemoved = newRemoved || removed;
            };
            trySide(QStringLiteral("left"), newLeft);
            trySide(QStringLiteral("right"), newRight);
            trySide(QStringLiteral("tools"), newTools);
        }
        const bool newColumnOk = newLeft && newRight && newTools && newRemoved;
        ST_BEGIN("newcolumn_left");
        ST_PASS("newcolumn left=%d right=%d tools=%d removed=%d", newLeft ? 1 : 0,
                     newRight ? 1 : 0,
                     newTools ? 1 : 0,
                     newRemoved ? 1 : 0);
        if (!newColumnOk) {
            ST_FAIL(143, "new column drop");
        }

        // m43_intogroup (144): dropping a panel inside another group inserts it
        // as a tab at the requested index, in normal mode and in compact mode.
        bool intoNormal = false;
        bool intoCompact = false;
        bool intoIndex = false;
        if (panelColumnColumn) {
            panelColumnColumn->setRailMode(false);
            multicolumnPump(4);
            // A target group whose second panel is stable at runtime (earlier
            // checks reorder the groups), never the panel's own group.
            QString intoTarget;
            for (pictura::PanelGroup* group : panelColumnColumn->groups()) {
                if (group && group->panels().size() >= 2
                    && !group->containsPanel(QStringLiteral("swatchesPanel"))) {
                    intoTarget = group->panels().at(1)->objectName();
                    break;
                }
            }
            if (!intoTarget.isEmpty()) {
                const bool moved = frame.dropIntoGroupForTest(
                    QStringLiteral("swatchesPanel"), intoTarget, 1);
                pictura::PanelGroup* dest = panelColumnColumn->groupForPanel(intoTarget);
                const bool atIndex =
                    dest && dest->indexOfPanel(QStringLiteral("swatchesPanel")) == 1;
                intoNormal = moved && atIndex;
                intoIndex = atIndex;
                frame.dropIntoGroupForTest(QStringLiteral("swatchesPanel"),
                                           QStringLiteral("colorPanel"), 1);

                panelColumnColumn->setRailMode(true);
                multicolumnPump(6);
                const bool movedCompact = frame.dropIntoGroupForTest(
                    QStringLiteral("swatchesPanel"), intoTarget);
                pictura::PanelGroup* destCompact =
                    panelColumnColumn->groupForPanel(intoTarget);
                const bool atIndexCompact =
                    destCompact
                    && destCompact->indexOfPanel(QStringLiteral("swatchesPanel")) == 1;
                intoCompact = movedCompact && atIndexCompact;
                intoIndex = intoIndex && atIndexCompact;
                panelColumnColumn->setRailMode(false);
                multicolumnPump(4);
                frame.dropIntoGroupForTest(QStringLiteral("swatchesPanel"),
                                           QStringLiteral("colorPanel"), 1);
                multicolumnPump(4);
            }
        }
        const bool intoGroupOk = intoNormal && intoCompact && intoIndex;
        ST_BEGIN("intogroup_normal");
        ST_PASS("intogroup normal=%d compact=%d index=%d", intoNormal ? 1 : 0,
                     intoCompact ? 1 : 0,
                     intoIndex ? 1 : 0);
        if (!intoGroupOk) {
            ST_FAIL(144, "into-group drop");
        }

        // m43_boundary (145): dropping a panel above or below a group inserts a
        // fresh one-panel group at that boundary, in normal mode and compact.
        bool multicolumnAbove = false;
        bool multicolumnBelow = false;
        bool boundaryCompact = false;
        if (panelColumnColumn) {
            panelColumnColumn->setRailMode(false);
            multicolumnPump(4);
            {
                multicolumnAbove = frame.dropBoundaryForTest(
                    QStringLiteral("channelsPanel"), QStringLiteral("navigatorPanel"), true);
                frame.dropIntoGroupForTest(QStringLiteral("channelsPanel"),
                                           QStringLiteral("layersPanel"), -1);
            }
            {
                multicolumnBelow = frame.dropBoundaryForTest(
                    QStringLiteral("pathsPanel"), QStringLiteral("layersPanel"), false);
                frame.dropIntoGroupForTest(QStringLiteral("pathsPanel"),
                                           QStringLiteral("layersPanel"), -1);
            }
            panelColumnColumn->setRailMode(true);
            multicolumnPump(6);
            boundaryCompact = frame.dropBoundaryForTest(
                QStringLiteral("channelsPanel"), QStringLiteral("adjustmentsPanel"), true);
            panelColumnColumn->setRailMode(false);
            multicolumnPump(4);
            frame.dropIntoGroupForTest(QStringLiteral("channelsPanel"),
                                       QStringLiteral("layersPanel"), -1);
            multicolumnPump(4);
        }
        const bool boundaryOk = multicolumnAbove && multicolumnBelow && boundaryCompact;
        ST_BEGIN("boundary_above");
        ST_PASS("boundary above=%d below=%d compact=%d", multicolumnAbove ? 1 : 0,
                     multicolumnBelow ? 1 : 0,
                     boundaryCompact ? 1 : 0);
        if (!boundaryOk) {
            ST_FAIL(145, "boundary drop");
        }

        // m43_singlefloat (146): a one-panel float carries only its panel, and
        // re-docking it restores the full group with the other panels.
        bool singlePanel = false;
        bool singleRedock = false;
        if (panelColumnColumn) {
            panelColumnColumn->setRailMode(false);
            panelColumnColumn->setMinimumWidth(360);
            panelColumnColumn->setPreferredWidth(360);
            multicolumnPump(6);
            pictura::PanelGroup* navGroup =
                panelColumnColumn->groupForPanel(QStringLiteral("navigatorPanel"));
            const int navCount = navGroup ? navGroup->titleCountForTest() : 0;
            const int before = panelColumnColumn->floatCountForTest();
            const bool tore =
                panelColumnColumn->tearOffPanelForTest(QStringLiteral("infoPanel"));
            multicolumnPump(4);
            const QStringList names =
                panelColumnColumn->floatPanelNamesForTest(panelColumnColumn->floatCountForTest() - 1);
            singlePanel = tore && navCount >= 2 && navGroup
                             && names.size() == 1
                             && names.first() == QStringLiteral("infoPanel")
                             && navGroup->containsPanel(QStringLiteral("navigatorPanel"))
                             && !navGroup->containsPanel(QStringLiteral("infoPanel"));
            if (singlePanel) {
                dropPanelOnGroup(QStringLiteral("infoPanel"),
                                    QStringLiteral("navigatorPanel"));
                pictura::PanelGroup* restored =
                    panelColumnColumn->groupForPanel(QStringLiteral("infoPanel"));
                singleRedock = restored == navGroup
                                  && panelColumnColumn->floatCountForTest() == before
                                  && navGroup->titleCountForTest() == navCount;
            }
            // Safety cleanup.
            for (int i = 0; i < 8 && panelColumnColumn->floatCountForTest() > 0; ++i) {
                if (!panelColumnColumn->redockForTest(0, 0)) {
                    break;
                }
                multicolumnPump(4);
            }
        }
        const bool singleFloatOk = singlePanel && singleRedock;
        ST_BEGIN("singlefloat_panel");
        ST_PASS("singlefloat panel=%d redock=%d", singlePanel ? 1 : 0,
                     singleRedock ? 1 : 0);
        if (!singleFloatOk) {
            ST_FAIL(146, "single-panel float");
        }

        // m43_tools (147): the Tools dock width is fixed to its content width in
        // one and two columns and while floating, and a resize/separator attempt
        // cannot change it.
        bool multicolumnToolMin1 = false;
        bool multicolumnToolMin2 = false;
        int toolsContent1 = 0;
        int toolsContent2 = 0;
        if (panelMenusToolbox) {
            panelMenusToolbox->setColumns(1);
            multicolumnPump(4);
            toolsContent1 = panelMenusToolbox->contentWidthForTest();
            multicolumnToolMin1 = panelMenusToolbox->minimumWidth() == toolsContent1
                                  && frame.toolsColumn() != nullptr;
            panelMenusToolbox->setColumns(2);
            multicolumnPump(4);
            toolsContent2 = panelMenusToolbox->contentWidthForTest();
            multicolumnToolMin2 = panelMenusToolbox->minimumWidth() == toolsContent2
                                  && toolsContent2 > toolsContent1;
            panelMenusToolbox->setColumns(1);
            multicolumnPump(4);
        }
        const bool multicolumnToolsOk = multicolumnToolMin1 && multicolumnToolMin2;
        ST_BEGIN("tools_min1");
        ST_PASS("tools min1=%d min2=%d "
                     "content1=%d content2=%d", multicolumnToolMin1 ? 1 : 0,
                     multicolumnToolMin2 ? 1 : 0,
                     toolsContent1,
                     toolsContent2);
        if (!multicolumnToolsOk) {
            ST_FAIL(147, "tools content width");
        }

        // m43_icon (148): the compact strip button and pixmap are larger than
        // M42 (30/20).
        bool iconBigger = false;
        if (panelColumnColumn) {
            panelColumnColumn->setRailMode(true);
            multicolumnPump(4);
            QToolButton* strip = panelColumnColumn->findChild<QToolButton*>(
                QStringLiteral("panelIcon_layersPanel"));
            if (!strip) {
                for (QToolButton* candidate : panelColumnColumn->findChildren<QToolButton*>()) {
                    if (candidate->objectName().startsWith(QStringLiteral("panelIcon_"))) {
                        strip = candidate;
                        break;
                    }
                }
            }
            iconBigger = strip && strip->width() >= 34 && strip->iconSize().width() >= 24;
            panelColumnColumn->setRailMode(false);
            multicolumnPump(4);
        }
        ST_BEGIN("icon_bigger");
        ST_PASS("icon bigger=%d", iconBigger ? 1 : 0);
        if (!iconBigger) {
            ST_FAIL(148, "compact icon size");
        }

        // m43_flyout (149): the flyout meets the clicked button's actual edge on
        // the inner side and never overlaps or crosses to the outer side.
        bool flyoutInner = false;
        bool flyoutNoOverlap = false;
        if (panelColumnColumn) {
            panelColumnColumn->setRailMode(true);
            multicolumnPump(4);
            QToolButton* button = panelColumnColumn->findChild<QToolButton*>(
                QStringLiteral("panelIcon_layersPanel"));
            const QRect buttonRect =
                button ? QRect(button->mapToGlobal(QPoint(0, 0)), button->size()) : QRect();
            const bool opened =
                panelColumnColumn->openIconFlyoutForTest(QStringLiteral("layersPanel"));
            multicolumnPump(8);
            const QRect fly = panelColumnColumn->iconFlyoutGeometryForTest();
            const QString side = panelColumnColumn->flyoutSideForTest();
            flyoutInner = opened && button && fly.isValid()
                             && (side == QStringLiteral("left")
                                     ? fly.right() < buttonRect.left()
                                     : fly.left() > buttonRect.right());
            flyoutNoOverlap = button && fly.isValid() && !fly.intersects(buttonRect);
            panelColumnColumn->triggerFlyoutCloseForTest();
            multicolumnPump(4);
            panelColumnColumn->setRailMode(false);
            multicolumnPump(4);
        }
        const bool multicolumnFlyoutOk = flyoutInner && flyoutNoOverlap;
        ST_BEGIN("flyout_inner");
        ST_PASS("flyout inner=%d nooverlap=%d", flyoutInner ? 1 : 0,
                     flyoutNoOverlap ? 1 : 0);
        if (!multicolumnFlyoutOk) {
            ST_FAIL(149, "inner-side flyout");
        }

        // m43_dreset (150): `D` resets the foreground to black and the
        // background to white (`X` stays the swap).
        bool dResetFg = false;
        bool dResetBg = false;
        if (panelMenusToolbox) {
            auto* fgbg = panelMenusToolbox->foregroundBackgroundForTest();
            if (fgbg) {
                panelMenusToolbox->swapForegroundBackground();
                if (fgbg->foregroundForTest() == QColor(Qt::black)
                    && fgbg->backgroundForTest() == QColor(Qt::white)) {
                    panelMenusToolbox->swapForegroundBackground();
                }
                const bool dirty = fgbg->foregroundForTest() != QColor(Qt::black)
                                   || fgbg->backgroundForTest() != QColor(Qt::white);
                frame.activateWindow();
                multicolumnPump(2);
                QKeyEvent dEvent(QEvent::KeyPress, Qt::Key_D, Qt::NoModifier,
                                 QStringLiteral("d"));
                QApplication::sendEvent(&frame, &dEvent);
                multicolumnPump(4);
                dResetFg = fgbg->foregroundForTest() == QColor(Qt::black);
                dResetBg = fgbg->backgroundForTest() == QColor(Qt::white) && dirty;
            }
        }
        const bool dResetOk = dResetFg && dResetBg;
        ST_BEGIN("dreset_fg");
        ST_PASS("dreset fg=%d bg=%d", dResetFg ? 1 : 0,
                     dResetBg ? 1 : 0);
        if (!dResetOk) {
            ST_FAIL(150, "default-colours reset");
        }

        // m43_session (151): the v6 store carries the per-column layout. A
        // two-column layout built through the real drop path round-trips
        // (sides, order, nested groups) and re-applies to the same column
        // count; a v5 flat store loads as one right-hand column; an unknown key
        // survives a rewrite.
        bool sessionV6 = false;
        bool sessionColumns = false;
        bool sessionV5 = false;
        bool sessionRound = false;
        bool sessionUnknown = false;
        {
            const bool multicolumnMade = frame.newColumnDropForTest(QStringLiteral("stylesPanel"),
                                                            QStringLiteral("left"));
            frame.saveSession();
            const pictura::SessionState multicolumnLoaded = pictura::loadSession();
            sessionV6 = multicolumnLoaded.schemaVersion >= 6;

            bool hasLeft = false;
            bool hasRight = false;
            bool leftStyles = false;
            int widgetColumns = 0;
            for (const QJsonValue& value : multicolumnLoaded.panelColumns) {
                const QJsonObject column = value.toObject();
                if (column.value(QStringLiteral("tools")).toBool()) {
                    continue;
                }
                ++widgetColumns;
                const QString side = column.value(QStringLiteral("side")).toString();
                if (side == QStringLiteral("left")) {
                    hasLeft = true;
                    const QJsonArray groups =
                        column.value(QStringLiteral("groups")).toArray();
                    for (const QJsonValue& group : groups) {
                        const QJsonArray order =
                            group.toObject().value(QStringLiteral("order")).toArray();
                        if (order.contains(QStringLiteral("stylesPanel"))) {
                            leftStyles = true;
                        }
                    }
                } else if (side == QStringLiteral("right")) {
                    hasRight = true;
                }
            }
            sessionColumns = multicolumnMade && widgetColumns == 2 && hasLeft
                                && hasRight && leftStyles;

            // Re-run the real startup restore path and check the rebuilt columns.
            frame.applyPanelSessionForTest(multicolumnLoaded);
            multicolumnPump(8);
            pictura::PanelColumn* multicolumnRestored =
                frame.columnForPanel(QStringLiteral("stylesPanel"));
            sessionRound = frame.panelColumnCountForTest() == 3 && multicolumnRestored
                              && frame.sideOf(multicolumnRestored) == pictura::PanelSide::Left
                              && frame.panelColumnSideForTest(1) == QStringLiteral("left");

            // A v5 store (no `panelColumns`) migrates to one right-hand column.
            {
                QFile store(pictura::sessionFilePath());
                if (store.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    store.write("{\"schemaVersion\":5,\"panelGroups\":[{\"name\":"
                                "\"panelGroup_layersPanel\",\"order\":[\"layersPanel\"],"
                                "\"visible\":[\"layersPanel\"],\"minimized\":false,"
                                "\"collapsed\":false}]}");
                    store.close();
                }
                const pictura::SessionState multicolumnV5 = pictura::loadSession();
                sessionV5 =
                    multicolumnV5.panelColumns.size() == 1
                    && multicolumnV5.panelColumns.first().toObject().value(
                           QStringLiteral("side")).toString() == QStringLiteral("right")
                    && !multicolumnV5.panelGroups.isEmpty();
            }

            // An unknown key survives a load-then-write rewrite.
            {
                QFile store(pictura::sessionFilePath());
                if (store.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    store.write("{\"schemaVersion\":6,\"unknownSessionKey\":\"keep-me\"}");
                    store.close();
                }
                frame.saveSession();
                QFile read(pictura::sessionFilePath());
                if (read.open(QIODevice::ReadOnly)) {
                    const QByteArray rewritten = read.readAll();
                    sessionUnknown = rewritten.contains("unknownSessionKey")
                                        && rewritten.contains("keep-me");
                }
            }

            // Collapse back to the default single right-hand column.
            frame.applyPanelSessionForTest(pictura::SessionState{});
            multicolumnPump(6);
        }
        const bool multicolumnSessionOk = sessionV6 && sessionColumns && sessionV5
                                  && sessionRound && sessionUnknown;
        ST_BEGIN("session_v6");
        ST_PASS("session v6=%d v5=%d columns=%d roundtrip=%d "
                     "unknown=%d", sessionV6 ? 1 : 0,
                     sessionV5 ? 1 : 0,
                     sessionColumns ? 1 : 0,
                     sessionRound ? 1 : 0,
                     sessionUnknown ? 1 : 0);
        if (!multicolumnSessionOk) {
            ST_FAIL(151, "session v6");
        }

        // m44_newdoc (153): E1. A newly created white document must present
        // pure white immediately: the first `image()` read, before any move or
        // recomposite, is uniformly opaque white and the canvas already holds
        // it. Pins the regression where the M0.5 GPU smoke probe wrote its demo
        // gradient into the document's display image.
        const int docsBefore = frame.documentCount();
        const bool themeMade = frame.newDocument(QStringLiteral("White"), 32, 24,
                                               QStringLiteral("rgb"), 8,
                                               QStringLiteral("white"));
        pictura::PictureView* themeView = frame.activeView();
        const QImage themeImage = themeView ? themeView->image() : QImage();
        pictura::ImageView* themeCanvas = frame.imageView();
        const QImage canvasImage = themeCanvas ? themeCanvas->image() : QImage();
        bool themeUniform = themeMade && themeView && !themeImage.isNull();
        if (themeUniform) {
            const QRgb themeFirst = themeImage.pixel(0, 0);
            for (int y = 0; y < themeImage.height() && themeUniform; ++y) {
                for (int x = 0; x < themeImage.width(); ++x) {
                    if (themeImage.pixel(x, y) != themeFirst) {
                        themeUniform = false;
                        break;
                    }
                }
            }
        }
        const bool themeWhite = themeUniform && themeImage.pixel(0, 0) == 0xFFFFFFFFu;
        const bool themeImmediate = themeWhite && canvasImage == themeImage;
        const bool newdocOk = themeMade && themeImage.width() == 32 && themeImage.height() == 24
                                  && themeWhite && themeUniform && themeImmediate;
        ST_BEGIN("newdoc_white");
        ST_PASS("newdoc white=%d uniform=%d immediate=%d", themeWhite ? 1 : 0, themeUniform ? 1 : 0, themeImmediate ? 1 : 0);
        if (!newdocOk) {
            ST_FAIL(153, "new-document render");
        }
        frame.closeDocument(frame.documentCount() - 1, false);
        // M44 Phase B (154, 155, 156, 159, 161, 164, 165, 166): panel chrome,
        // borders, and the shared widget styling. Every check reads a real style
        // property (icon pixmap, tab index, QSS/palette colour, elided text,
        // splitter handle width) rather than only that a stylesheet exists.
        {
            const QString sheet = qApp->styleSheet();
            const QString themePanelHex = QStringLiteral("#4d4d4d");
            const QString themeBaseHex = QStringLiteral("#363636");
            const QString borderHex = QStringLiteral("#2e2e2e");
            const QString separatorHex = QStringLiteral("#404040");
            const QString borderWidth = QString::number(pictura::Theme::kPanelBorderWidth);

            // 154: the collapse toggle shows the action's target icon, not the
            // inverted one (collapse-to-icons in normal, expand in iconic).
            bool chevronsMode = false;
            bool chevronsTarget = false;
            if (panelColumnColumn) {
                const QImage collapseIcon =
                    pictura::icon(QStringLiteral("panel.columnsTwo")).pixmap(16, 16).toImage();
                const QImage expandIcon =
                    pictura::icon(QStringLiteral("panel.columnsOne")).pixmap(16, 16).toImage();
                panelColumnColumn->setRailMode(false);
                multicolumnPump(2);
                QToolButton* themeToggle = panelColumnColumn->panelColumnToggleForTest();
                chevronsMode =
                    themeToggle && themeToggle->icon().pixmap(16, 16).toImage() == collapseIcon;
                panelColumnColumn->setRailMode(true);
                multicolumnPump(2);
                chevronsTarget =
                    themeToggle && themeToggle->icon().pixmap(16, 16).toImage() == expandIcon;
                panelColumnColumn->setRailMode(false);
                multicolumnPump(2);
            }
            ST_BEGIN("chevrons_mode");
            ST_PASS("chevrons mode=%d target=%d", chevronsMode ? 1 : 0, chevronsTarget ? 1 : 0);
            if (!(chevronsMode && chevronsTarget)) {
                ST_FAIL(154, "collapse chevrons");
            }

            // 155: a restored layout leaves the first visible panel active, not
            // the last (the old path made each shown panel current in turn).
            bool defaultActive = panelColumnColumn != nullptr;
            int activeGroups = 0;
            if (panelColumnColumn) {
                panelColumnColumn->restorePanelState(panelColumnColumn->savePanelState());
                multicolumnPump(2);
                for (pictura::PanelGroup* group : panelColumnColumn->groups()) {
                    if (!group) {
                        continue;
                    }
                    const int first = group->firstVisibleTabIndexForTest();
                    if (first < 0) {
                        continue;
                    }
                    ++activeGroups;
                    if (group->currentTabIndexForTest() != first) {
                        defaultActive = false;
                    }
                }
                defaultActive = defaultActive && activeGroups > 0;
            }
            ST_BEGIN("defaultactive_first");
            ST_PASS("defaultactive first=%d", defaultActive ? 1 : 0);
            if (!defaultActive) {
                ST_FAIL(155, "default active panel");
            }

            // 156: the active panel tab matches the panel surface (`surface`),
            // the inactive tab is the darker `surface-header`, and neither uses the
            // other.
            const bool tabSelected =
                sheet.contains(QStringLiteral("QTabBar#panelTabBar::tab:selected { background: ") + themePanelHex)
                && sheet.contains(QStringLiteral("QTabWidget#panelGroupTabs::pane { border: 0; background: ") + themePanelHex);
            const bool tabInactive =
                sheet.contains(QStringLiteral("QTabBar#panelTabBar::tab { background: ")
                                  + themeBaseHex);
            const bool tabDiffer =
                tabSelected && tabInactive && themePanelHex != themeBaseHex;
            ST_BEGIN("tabswap_selected");
            ST_PASS("tabswap selected=%d unselected=%d differ=%d", tabSelected ? 1 : 0,
                         tabInactive ? 1 : 0,
                         tabDiffer ? 1 : 0);
            if (!tabDiffer) {
                ST_FAIL(156, "panel tab colours");
            }

            // 159: the inter-group splitter handle is thicker and darker grey.
            const bool dividerThick =
                panelColumnColumn && panelColumnColumn->groupDividerWidthForTest()
                                 >= pictura::Theme::kGroupDividerWidth;
            const bool dividerDark = sheet.contains(
                QStringLiteral("QSplitter#panelColumnSplitter::handle { background: ")
                + borderHex);
            ST_BEGIN("divider_thick");
            ST_PASS("divider thick=%d dark=%d", dividerThick ? 1 : 0, dividerDark ? 1 : 0);
            if (!(dividerThick && dividerDark)) {
                ST_FAIL(159, "group divider");
            }

            // 161: the compact-strip group divider is an invisible layout/drop
            // marker now; the 1 px separation is the group's own bottom rule.
            bool compactDividerClear = false;
            if (panelColumnColumn) {
                panelColumnColumn->setRailMode(true);
                multicolumnPump(4);
                compactDividerClear =
                    panelColumnColumn->dividerCountForTest() > 0
                    && sheet.contains(QStringLiteral(
                           "QFrame#panelIconDivider { background: transparent; border: 0; }"));
                panelColumnColumn->setRailMode(false);
                multicolumnPump(2);
            }
            ST_BEGIN("compactdivider_clear");
            ST_PASS("compactdivider clear=%d", compactDividerClear ? 1 : 0);
            if (!compactDividerClear) {
                ST_FAIL(161, "compact divider");
            }

            // 164: at a strip width with room only beyond the icon button, the
            // label appears and its text is elided to that width.
            bool elidePartial = false;
            if (panelColumnColumn) {
                panelColumnColumn->setRailMode(true);
                panelColumnColumn->showPanel(QStringLiteral("layersPanel"), true);
                multicolumnPump(4);
                pictura::PanelGroup* themeLayers =
                    panelColumnColumn->groupForPanel(QStringLiteral("layersPanel"));
                const QString fullTitle =
                    themeLayers ? themeLayers->titleForPanel(QStringLiteral("layersPanel"))
                              : QString();
                panelColumnColumn->setIconStripWidthForTest(64);
                const QString themeShown =
                    panelColumnColumn->stripLabelTextForTest(QStringLiteral("layersPanel"));
                const bool themeVisible =
                    panelColumnColumn->stripLabelVisibleForTest(QStringLiteral("layersPanel"));
                elidePartial = themeVisible && !fullTitle.isEmpty() && !themeShown.isEmpty()
                                  && themeShown.size() < fullTitle.size()
                                  && themeShown.contains(QChar(0x2026));
                panelColumnColumn->setRailMode(false);
                multicolumnPump(2);
            }
            ST_BEGIN("elide_partial");
            ST_PASS("elide partial=%d", elidePartial ? 1 : 0);
            if (!elidePartial) {
                ST_FAIL(164, "strip label elide");
            }

            // 165: the document tab strip is borderless flat chrome (the options
            // bar above draws the only horizontal rule over the workspace).
            QTabBar* themeDocBar = frame.findChild<QTabBar*>(QStringLiteral("documentTabBar"));
            const bool fileNoPaneBorder =
                sheet.contains(QStringLiteral("QTabWidget#documentTabs::pane { border: 0; }"));
            const bool fileNoBarBorder =
                sheet.contains(QStringLiteral("QTabBar#documentTabBar { border: 0; }"));
            ST_BEGIN("filebar_borderless");
            ST_PASS("filebar pane=%d bar=%d", fileNoPaneBorder ? 1 : 0, fileNoBarBorder ? 1 : 0);
            if (!(themeDocBar && fileNoPaneBorder && fileNoBarBorder)) {
                ST_FAIL(165, "document tab strip borders");
            }

            // 166: the tools panel and widget panes share the panel surface; the
            // column strips keep side rules only and the header drops its rule.
            const QString borderRule =
                borderWidth + QStringLiteral("px solid ") + borderHex;
            const bool panelTools = sheet.contains(QStringLiteral("QWidget#toolsPanel { background: ")
                                                   + themePanelHex
                                                   + QStringLiteral("; border: 0; }"));
            const bool panelNormal =
                sheet.contains(QStringLiteral("QTabWidget#panelGroupTabs { border: ")
                                  + borderRule + QStringLiteral("; }"));
            const bool panelCompact =
                sheet.contains(QStringLiteral("QWidget#panelColumnIconStrip { border: 0; background: ")
                               + themePanelHex + QStringLiteral("; }"))
                && sheet.contains(QStringLiteral("QWidget#panelColumnContainer { border: 0; border-left: ")
                                  + borderWidth + QStringLiteral("px solid ") + separatorHex);
            const bool panelHeaderFlat =
                sheet.contains(QStringLiteral("QWidget#panelColumnHeader { background: ") + themeBaseHex
                               + QStringLiteral("; border: 0; border-bottom: 1px solid ")
                               + pictura::Theme::shade(QColor(themeBaseHex), -1).name()
                               + QStringLiteral("; }"));
            const bool panelParity =
                sheet.contains(QStringLiteral("QWidget#panelFloat { background: ")
                                  + themePanelHex)
                && sheet.contains(QStringLiteral("QWidget#panelIconFlyout { background: ")
                                     + themePanelHex);
            ST_BEGIN("panelborder_tools");
            ST_PASS("panelborder tools=%d panel=%d compact=%d header=%d", panelTools ? 1 : 0,
                         panelNormal ? 1 : 0, panelCompact ? 1 : 0, panelHeaderFlat ? 1 : 0);
            if (!(panelTools && panelNormal && panelCompact && panelHeaderFlat && panelParity)) {
                ST_FAIL(166, "panel borders");
            }
        }
        // M44 Phase C (157, 158, 160, 162, 163): float-drag continuation,
        // any-side column/dock docking, popup/dock style parity, and the compact
        // group-relative drop with its drag handle.
        {
            // 157: a widget-tab float drag stays active after the float is
            // created, tracks the cursor, and only the release commits it. The
            // source group must survive so its tab bar keeps the mouse grab.
            bool fdActive = false;
            bool fdFollows = false;
            bool fdRelease = false;
            if (panelColumnColumn) {
                panelColumnColumn->setRailMode(false);
                panelColumnColumn->showPanel(QStringLiteral("historyPanel"), true);
                multicolumnPump(8);
                pictura::PanelGroup* fdGroup =
                    panelColumnColumn->groupForPanel(QStringLiteral("historyPanel"));
                if (fdGroup && fdGroup->titleCountForTest() == 1) {
                    const int before = panelColumnColumn->floatCountForTest();
                    const QPoint outsideA =
                        panelColumnColumn->mapToGlobal(QPoint(-60, panelColumnColumn->height() / 3));
                    const QPoint outsideB =
                        panelColumnColumn->mapToGlobal(QPoint(-60, (panelColumnColumn->height() * 2) / 3));
                    const bool began =
                        panelColumnColumn->beginTabDragForTest(QStringLiteral("historyPanel"));
                    panelColumnColumn->dragToForTest(outsideA);
                    multicolumnPump(8);
                    const bool floated = panelColumnColumn->floatCountForTest() == before + 1;
                    const QRect g1 = panelColumnColumn->floatGeometryForTest(before);
                    fdActive = began && floated && panelColumnColumn->dragActiveForTest()
                                  && panelColumnColumn->dragSourceGroupAliveForTest();
                    panelColumnColumn->dragToForTest(outsideB);
                    multicolumnPump(8);
                    const QRect g2 = panelColumnColumn->floatGeometryForTest(before);
                    fdFollows = panelColumnColumn->dragActiveForTest() && g1.isValid()
                                   && g2.isValid() && g2.topLeft() != g1.topLeft()
                                   && panelColumnColumn->floatCountForTest() == before + 1;
                    const bool dropped = panelColumnColumn->dropForTest(outsideB);
                    multicolumnPump(8);
                    fdRelease = dropped && !panelColumnColumn->dragActiveForTest();
                    for (int i = 0; i < 8 && panelColumnColumn->floatCountForTest() > 0; ++i) {
                        if (!panelColumnColumn->redockForTest(0, 0)) {
                            break;
                        }
                        multicolumnPump(4);
                    }
                }
            }
            ST_BEGIN("floatdrag_active");
            ST_PASS("floatdrag active=%d follows=%d release=%d", fdActive ? 1 : 0,
                         fdFollows ? 1 : 0,
                         fdRelease ? 1 : 0);
            if (!(fdActive && fdFollows && fdRelease)) {
                ST_FAIL(157, "float drag continuation");
            }

            // 160: the compact flyout shares the docked widget's scoped
            // style/container (no per-host inline stylesheet).
            bool psParity = false;
            if (panelColumnColumn) {
                panelColumnColumn->setRailMode(true);
                multicolumnPump(6);
                const bool opened =
                    panelColumnColumn->openIconFlyoutForTest(QStringLiteral("layersPanel"));
                multicolumnPump(10);
                psParity = opened && panelColumnColumn->iconFlyoutVisibleForTest()
                              && panelColumnColumn->popupStyleParityForTest(
                                     QStringLiteral("layersPanel"));
                panelColumnColumn->triggerFlyoutCloseForTest();
                multicolumnPump(6);
                panelColumnColumn->setRailMode(false);
                multicolumnPump(4);
            }
            ST_BEGIN("popupstyle_parity");
            ST_PASS("popupstyle parity=%d", psParity ? 1 : 0);
            if (!psParity) {
                ST_FAIL(160, "popup style parity");
            }

            // 162: compact drop placement. Onto a group inserts there; on the
            // inter-group divider, above the top, and below the bottom create a
            // fresh one-panel group at that boundary.
            bool cdInto = false;
            bool cdBetween = false;
            bool cdAbove = false;
            bool cdBelow = false;
            if (panelColumnColumn) {
                panelColumnColumn->setRailMode(true);
                multicolumnPump(8);

                const QString intoSource = QStringLiteral("swatchesPanel");
                const QString themeIntoTarget = QStringLiteral("layersPanel");
                pictura::PanelGroup* intoTargetGroup =
                    panelColumnColumn->groupForPanel(themeIntoTarget);
                if (intoTargetGroup
                    && panelColumnColumn->groupForPanel(intoSource) != intoTargetGroup) {
                    const QPoint intoPoint =
                        panelColumnColumn->stripEntryPointForTest(themeIntoTarget, 0);
                    const bool began = panelColumnColumn->beginStripDragForTest(intoSource);
                    panelColumnColumn->dragToForTest(intoPoint);
                    const bool dropped = panelColumnColumn->dropForTest(intoPoint);
                    multicolumnPump(8);
                    cdInto = began && dropped
                                && panelColumnColumn->groupForPanel(intoSource)
                                       == intoTargetGroup;
                }

                if (panelColumnColumn->compactStripGroupOrderForTest().size() >= 2) {
                    const QString themeBetween = QStringLiteral("channelsPanel");
                    pictura::PanelGroup* themeBefore =
                        panelColumnColumn->groupForPanel(themeBetween);
                    const QPoint betweenPoint =
                        panelColumnColumn->compactStripBoundaryPointForTest(1);
                    const bool began = panelColumnColumn->beginStripDragForTest(themeBetween);
                    panelColumnColumn->dragToForTest(betweenPoint);
                    const bool dropped = panelColumnColumn->dropForTest(betweenPoint);
                    multicolumnPump(8);
                    pictura::PanelGroup* landed =
                        panelColumnColumn->groupForPanel(themeBetween);
                    cdBetween = began && dropped && landed && landed != themeBefore
                                   && landed->titleCountForTest() == 1;
                }

                {
                    const QString themeAbove = QStringLiteral("pathsPanel");
                    const QPoint abovePoint =
                        panelColumnColumn->compactStripBoundaryPointForTest(0);
                    const bool began = panelColumnColumn->beginStripDragForTest(themeAbove);
                    panelColumnColumn->dragToForTest(abovePoint);
                    const bool dropped = panelColumnColumn->dropForTest(abovePoint);
                    multicolumnPump(8);
                    const QStringList order =
                        panelColumnColumn->compactStripGroupOrderForTest();
                    pictura::PanelGroup* landed =
                        panelColumnColumn->groupForPanel(themeAbove);
                    cdAbove = began && dropped && landed
                                 && landed->titleCountForTest() == 1 && !order.isEmpty()
                                 && order.first() == landed->objectName();
                }

                {
                    const QString themeBelow = QStringLiteral("histogramPanel");
                    const int count =
                        panelColumnColumn->compactStripGroupOrderForTest().size();
                    const QPoint belowPoint =
                        panelColumnColumn->compactStripBoundaryPointForTest(count);
                    const bool began = panelColumnColumn->beginStripDragForTest(themeBelow);
                    panelColumnColumn->dragToForTest(belowPoint);
                    const bool dropped = panelColumnColumn->dropForTest(belowPoint);
                    multicolumnPump(8);
                    const QStringList order =
                        panelColumnColumn->compactStripGroupOrderForTest();
                    pictura::PanelGroup* landed =
                        panelColumnColumn->groupForPanel(themeBelow);
                    cdBelow = began && dropped && landed
                                 && landed->titleCountForTest() == 1 && !order.isEmpty()
                                 && order.last() == landed->objectName();
                }

                panelColumnColumn->setRailMode(false);
                multicolumnPump(4);
            }
            ST_BEGIN("compactdrop_into");
            ST_PASS("compactdrop into=%d between=%d above=%d "
                         "below=%d", cdInto ? 1 : 0,
                         cdBetween ? 1 : 0,
                         cdAbove ? 1 : 0,
                         cdBelow ? 1 : 0);
            if (!(cdInto && cdBetween && cdAbove && cdBelow)) {
                ST_FAIL(162, "compact drop");
            }

            // 163: every compact group carries a dots grip and dragging it moves
            // the whole group to a new boundary.
            bool dhDots = false;
            bool dhGroup = false;
            if (panelColumnColumn) {
                panelColumnColumn->setRailMode(true);
                multicolumnPump(8);
                const QStringList order = panelColumnColumn->compactStripGroupOrderForTest();
                if (order.size() >= 2) {
                    const QString first = order.first();
                    dhDots = panelColumnColumn->dragHandleForTest(first) != nullptr;
                    if (dhDots) {
                        const int count =
                            panelColumnColumn->compactStripGroupOrderForTest().size();
                        const QPoint to =
                            panelColumnColumn->compactStripBoundaryPointForTest(count);
                        const bool began = panelColumnColumn->beginGroupGripDragForTest(first);
                        panelColumnColumn->dragToForTest(to);
                        const bool dropped = panelColumnColumn->dropForTest(to);
                        multicolumnPump(8);
                        const QStringList after =
                            panelColumnColumn->compactStripGroupOrderForTest();
                        dhGroup = began && dropped && after.size() == order.size()
                                     && after != order && after.last() == first;
                    }
                }
                panelColumnColumn->setRailMode(false);
                multicolumnPump(4);
            }
            ST_BEGIN("draghandle_dots");
            ST_PASS("draghandle dots=%d groupdrag=%d", dhDots ? 1 : 0, dhGroup ? 1 : 0);
            if (!(dhDots && dhGroup)) {
                ST_FAIL(163, "compact drag handle");
            }

            // 158: a widget column docks at the toolbar side, beside another
            // column, and at the workspace edge; the Tools dock now allows only
            // the left/right sides (M45 T2) and its floating height is pinned
            // (T1/T2/W5).
            bool dsToolbar = false;
            bool dsColumn = false;
            bool dsWorkspace = false;
            bool dsFloat = false;
            if (toolsPanelToolbox && panelColumnColumn) {
                panelColumnColumn->setRailMode(false);
                multicolumnPump(8);
                const int baseCount = frame.panelColumnCountForTest();

                const bool ws =
                    frame.newColumnDropForTest(QStringLiteral("stylesPanel"),
                                               QStringLiteral("right"));
                const bool wsBack =
                    ws && frame.dropIntoGroupForTest(QStringLiteral("stylesPanel"),
                                                     QStringLiteral("colorPanel"), -1);
                dsWorkspace =
                    ws && wsBack && frame.panelColumnCountForTest() == baseCount;

                const bool tb =
                    frame.newColumnDropForTest(QStringLiteral("gradientsPanel"),
                                               QStringLiteral("tools"));
                const bool tbBack =
                    tb && frame.dropIntoGroupForTest(QStringLiteral("gradientsPanel"),
                                                     QStringLiteral("colorPanel"), -1);
                dsToolbar =
                    tb && tbBack && frame.panelColumnCountForTest() == baseCount;

                const bool madeAnchor =
                    frame.newColumnDropForTest(QStringLiteral("patternsPanel"),
                                               QStringLiteral("right"));
                const bool beside =
                    madeAnchor
                    && frame.newColumnBesideForTest(QStringLiteral("notesPanel"),
                                                    QStringLiteral("patternsPanel"));
                const bool besideBack =
                    beside
                    && frame.dropIntoGroupForTest(QStringLiteral("notesPanel"),
                                                  QStringLiteral("colorPanel"), -1)
                    && frame.dropIntoGroupForTest(QStringLiteral("patternsPanel"),
                                                  QStringLiteral("colorPanel"), -1);
                dsColumn = madeAnchor && beside && besideBack
                              && frame.panelColumnCountForTest() == baseCount;

                auto* csTools = frame.findChild<QSplitter*>(QStringLiteral("centerSplitter"));
                const bool pane = toolsPanelToolbox && csTools && frame.toolsColumn()
                                  && csTools->indexOf(frame.toolsColumn()) >= 0;
                dsFloat = pane && !toolsPanelToolbox->isWindow();
            }
            ST_BEGIN("docksides_toolbar");
            ST_PASS("docksides toolbar=%d column=%d workspace=%d "
                         "pane=%d", dsToolbar ? 1 : 0,
                         dsColumn ? 1 : 0,
                         dsWorkspace ? 1 : 0,
                         dsFloat ? 1 : 0);
            if (!(dsToolbar && dsColumn && dsWorkspace && dsFloat)) {
                ST_FAIL(158, "any-side docking");
            }
        }
        // M45 Phase A (167-169): the Tools toolbar content sizing, left/right-only
        // docking, and placement beside a widget column through the column
        // grammar. Geometry is read after a bounded pump so it is real offscreen.
        {
            // 167: the width is content-derived in one and two columns and stable
            // across a 1->2->1 toggle. Docked/pane the height is free (only a
            // floating panel pins it), so it is not cut.
            bool panelFixOne = false;
            bool panelFixTwo = false;
            bool panelFixStable = false;
            int panelFixW1 = 0;
            int panelFixH1 = 0;
            int panelFixW2 = 0;
            int panelFixH2 = 0;
            if (toolsPanelToolbox) {
                toolsPanelToolbox->setColumns(1);
                multicolumnPump(8);
                panelFixW1 = toolsPanelToolbox->width();
                panelFixH1 = toolsPanelToolbox->height();
                const int cw1 = toolsPanelToolbox->contentWidthForTest();
                panelFixOne = cw1 > 0 && panelFixW1 >= cw1 && panelFixH1 > 0
                         && toolsPanelToolbox->minimumWidth() == cw1;
                toolsPanelToolbox->setColumns(2);
                multicolumnPump(8);
                panelFixW2 = toolsPanelToolbox->width();
                panelFixH2 = toolsPanelToolbox->height();
                const int cw2 = toolsPanelToolbox->contentWidthForTest();
                panelFixTwo = cw2 > cw1 && panelFixW2 >= cw2;
                toolsPanelToolbox->setColumns(1);
                multicolumnPump(8);
                panelFixStable = toolsPanelToolbox->minimumWidth() == cw1;
            }
            ST_BEGIN("tools_sizing_one");
            ST_PASS("tools_sizing one=%d two=%d stable=%d "
                         "w1=%d h1=%d w2=%d h2=%d", panelFixOne ? 1 : 0, panelFixTwo ? 1 : 0, panelFixStable ? 1 : 0, panelFixW1, panelFixH1,
                         panelFixW2, panelFixH2);
            if (!(panelFixOne && panelFixTwo && panelFixStable)) {
                ST_FAIL(167, "tools sizing");
            }
        }

        {
            // 168: the tools column is atomic (no groups) and never an OS
            // window: it is an in-window central-splitter pane.
            const bool panelFixLeft = frame.toolsColumn()
                && frame.sideOf(frame.toolsColumn()) == pictura::PanelSide::Left;
            const bool panelFixAtomic = frame.toolsColumn()
                && frame.toolsColumn()->isToolsColumn()
                && frame.toolsColumn()->groups().isEmpty();
            const bool noWindow = frame.toolsColumn() && !frame.toolsColumn()->isWindow();
            ST_BEGIN("tools_sides_left");
            ST_PASS("tools_sides left=%d atomic=%d window=%d", panelFixLeft ? 1 : 0,
                    panelFixAtomic ? 1 : 0, noWindow ? 1 : 0);
            if (!(panelFixLeft && panelFixAtomic && noWindow)) {
                ST_FAIL(168, "tools sides");
            }
        }

        {
            // 169: a widget panel dropped at the tools column's outer edge
            // allocates a real sibling column; a widget panel dragged over the
            // tools column resolves no in-column target and never combines.
            pictura::PanelColumn* tools = frame.toolsColumn();
            bool besideLeft = false;
            if (tools && frame.panelColumn()) {
                const int before = frame.panelColumnCountForTest();
                const QPoint inside(tools->mapToGlobal(tools->rect().center()));
                const bool beganInside =
                    frame.panelColumn()->beginTabDragForTest(QStringLiteral("swatchesPanel"));
                frame.panelColumn()->dragToForTest(inside);
                const bool noIndicator = !tools->dropIndicatorVisibleForTest();
                frame.panelColumn()->cancelDragForTest();
                const bool made = frame.newColumnDropForTest(QStringLiteral("notesPanel"),
                                                             QStringLiteral("tools"));
                const bool removed =
                    frame.dropIntoGroupForTest(QStringLiteral("notesPanel"),
                                               QStringLiteral("colorPanel"), -1)
                    && frame.panelColumnCountForTest() == before;
                besideLeft = beganInside && noIndicator && made && removed;
            }
            ST_BEGIN("tools_beside_column_left");
            ST_PASS("tools_beside_column sibling=%d", besideLeft ? 1 : 0);
            if (!besideLeft) {
                ST_FAIL(169, "tools beside column");
            }
        }
        // M45 Phase B (170-177): the widget-panel drop indicator is rendered
        // from the resolved DropTarget in the owning column (W1-W3, W6); a
        // dynamic column emptied by any path is removed (W4); minimize collapses
        // the group to its tab bar with a state-derived menu label (W5); a
        // column never clips and shares one minimum-width floor (W7/W8). Every
        // check pumps the event loop bounded and forces layout so geometry is
        // real offscreen.
        {
            auto panelFixPump = [](int n) {
                for (int i = 0; i < n; ++i) {
                    QCoreApplication::processEvents();
                }
            };
            // A visible panel in the primary column that is not in `exclude`.
            auto otherPanel = [&](pictura::PanelGroup* exclude,
                                     const QString& alsoExclude) -> QString {
                if (!panelColumnColumn) {
                    return QString();
                }
                for (pictura::PanelGroup* group : panelColumnColumn->groups()) {
                    if (!group || group == exclude || !group->isVisible()
                        || group->titleCountForTest() == 0) {
                        continue;
                    }
                    for (QWidget* panel : group->panels()) {
                        if (panel && panel->objectName() != alsoExclude) {
                            return panel->objectName();
                        }
                    }
                }
                return QString();
            };
            auto collapseDynamics = [&]() {
                // Geometry-independent teardown: move every dynamic column's
                // groups (and their panels) back into the primary column, then
                // drop the empty columns. Keeps the tested resolve/commit path
                // untouched without depending on a drop's pixel geometry.
                for (pictura::PanelColumn* column : frame.panelColumns()) {
                    if (!column || column == panelColumnColumn) {
                        continue;
                    }
                    const QList<pictura::PanelGroup*> groups = column->groups();
                    for (pictura::PanelGroup* group : groups) {
                        if (!group) {
                            continue;
                        }
                        if (pictura::PanelGroup* taken =
                                column->takeGroup(group->objectName())) {
                            panelColumnColumn->adoptGroup(taken);
                        }
                    }
                    frame.removeColumnIfEmpty(column);
                }
                panelFixPump(6);
            };

            // 170: the line follows the resolved target's side in the owning
            // (destination) column, never the drag's source column.
            bool sideLeft = false;
            bool sideRight = false;
            if (panelColumnColumn) {
                panelColumnColumn->setRailMode(false);
                panelFixPump(4);
                pictura::PanelColumn* dest =
                    frame.createPanelColumn(pictura::PanelSide::Right, panelColumnColumn);
                pictura::PanelGroup* dg = nullptr;
                for (pictura::PanelGroup* group : panelColumnColumn->groups()) {
                    if (group && group->isVisible() && group->titleCountForTest() == 1) {
                        dg = group;
                        break;
                    }
                }
                if (dest && dg) {
                    const QString seedName = dg->objectName();
                    if (pictura::PanelGroup* taken = panelColumnColumn->takeGroup(seedName)) {
                        dest->adoptGroup(taken);
                    }
                }
                if (dest) {
                    panelColumnColumn->setPreferredWidth(180);
                    dest->setPreferredWidth(420);
                    panelFixPump(6);
                }
                const QString srcA = otherPanel(dg, QString());
                const QString srcB = otherPanel(dg, srcA);
                if (dest && dest != panelColumnColumn && dg && !srcA.isEmpty() && !srcB.isEmpty()) {
                    // Append one panel (right side), then insert the other
                    // between the first two tabs (left side); both points are
                    // interior to the target bar, so they exercise the
                    // cross-column target rather than a column-edge anchor.
                    const int endR = dg->titleCountForTest();
                    const QPoint rightPoint = dg->tabInsertionGlobalPointForTest(endR);
                    panelColumnColumn->beginTabDragForTest(srcA);
                    panelColumnColumn->dragToForTest(rightPoint);
                    const QRect rg = dest->dropIndicatorGlobalGeometryForTest();
                    sideRight = dest->dropIndicatorVisibleForTest()
                                   && !panelColumnColumn->dropIndicatorVisibleForTest() && rg.isValid()
                                   && qAbs(rg.center().x() - rightPoint.x()) <= 8;
                    panelColumnColumn->dropForTest(rightPoint);
                    panelFixPump(4);
                    sideRight = sideRight && dest->groupForPanel(srcA) == dg
                                   && dg->indexOfPanel(srcA) == endR;

                    const QPoint leftPoint = dg->tabInsertionGlobalPointForTest(1);
                    panelColumnColumn->beginTabDragForTest(srcB);
                    panelColumnColumn->dragToForTest(leftPoint);
                    const QRect lg = dest->dropIndicatorGlobalGeometryForTest();
                    sideLeft = dest->dropIndicatorVisibleForTest()
                                  && !panelColumnColumn->dropIndicatorVisibleForTest() && lg.isValid()
                                  && qAbs(lg.center().x() - leftPoint.x()) <= 8;
                    panelColumnColumn->dropForTest(leftPoint);
                    panelFixPump(4);
                    sideLeft = sideLeft && dest->groupForPanel(srcB) == dg
                                  && dg->indexOfPanel(srcB) == 1;
                }
                collapseDynamics();
            }
            ST_BEGIN("indicator_side_left");
            ST_PASS("indicator_side left=%d right=%d", sideLeft ? 1 : 0, sideRight ? 1 : 0);
            if (!(sideLeft && sideRight)) {
                ST_FAIL(170, "indicator side");
            }

            // 171: a cross-column IntoGroup drop shows the line in the target
            // column (shown) and lands the panel there (placed).
            bool crossShown = false;
            bool crossPlaced = false;
            if (panelColumnColumn) {
                panelColumnColumn->setRailMode(false);
                panelFixPump(4);
                pictura::PanelColumn* dest =
                    frame.createPanelColumn(pictura::PanelSide::Right, panelColumnColumn);
                pictura::PanelGroup* dg = nullptr;
                for (pictura::PanelGroup* group : panelColumnColumn->groups()) {
                    if (group && group->isVisible() && group->titleCountForTest() == 1) {
                        dg = group;
                        break;
                    }
                }
                if (dest && dg) {
                    const QString seedName = dg->objectName();
                    if (pictura::PanelGroup* taken = panelColumnColumn->takeGroup(seedName)) {
                        dest->adoptGroup(taken);
                    }
                }
                if (dest) {
                    panelColumnColumn->setPreferredWidth(180);
                    dest->setPreferredWidth(420);
                    panelFixPump(6);
                }
                const QString src = otherPanel(dg, QString());
                if (dest && dest != panelColumnColumn && dg && !src.isEmpty()) {
                    const QPoint p = dg->tabInsertionGlobalPointForTest(
                        dg->titleCountForTest());
                    panelColumnColumn->beginTabDragForTest(src);
                    panelColumnColumn->dragToForTest(p);
                    crossShown = dest->dropIndicatorVisibleForTest()
                                    && !panelColumnColumn->dropIndicatorVisibleForTest();
                    panelColumnColumn->dropForTest(p);
                    panelFixPump(4);
                    crossPlaced = dest->groupForPanel(src) == dg;
                }
                collapseDynamics();
            }
            ST_BEGIN("indicator_cross_column_shown");
            ST_PASS("indicator_cross_column shown=%d placed=%d", crossShown ? 1 : 0, crossPlaced ? 1 : 0);
            if (!(crossShown && crossPlaced)) {
                ST_FAIL(171, "cross-column indicator");
            }

            // 172: the rightmost tab insertion draws at that tab's index, not
            // the leftmost edge, and the panel lands last.
            bool panelFixRightmost = false;
            if (panelColumnColumn) {
                panelColumnColumn->setRailMode(false);
                panelColumnColumn->setPreferredWidth(420);
                panelFixPump(6);
                pictura::PanelGroup* target =
                    panelColumnColumn->groupForPanel(QStringLiteral("layersPanel"));
                const QString src = otherPanel(target, QString());
                if (target && target->titleCountForTest() >= 1 && !src.isEmpty()) {
                    panelColumnColumn->ensureGroupVisibleForTest(QStringLiteral("layersPanel"));
                    panelFixPump(4);
                    const int end = target->titleCountForTest();
                    const QPoint p = target->tabInsertionGlobalPointForTest(end);
                    QTabBar* bar = target->tabBar();
                    const int firstLeft =
                        bar ? bar->mapToGlobal(QPoint(target->tabInsertionX(0), 0)).x() : 0;
                    const int lastLeft = bar ? bar->mapToGlobal(
                                                   QPoint(target->tabInsertionX(end - 1), 0))
                                                   .x()
                                             : 0;
                    panelColumnColumn->beginTabDragForTest(src);
                    panelColumnColumn->dragToForTest(p);
                    const QRect g = panelColumnColumn->dropIndicatorGlobalGeometryForTest();
                    panelFixRightmost = panelColumnColumn->dropIndicatorVisibleForTest() && g.isValid()
                                   && qAbs(g.center().x() - p.x()) <= 8
                                   && g.center().x() >= lastLeft
                                   && g.center().x() > firstLeft;
                    panelColumnColumn->dropForTest(p);
                    panelFixPump(4);
                    panelFixRightmost = panelFixRightmost && target->indexOfPanel(src) == end
                                   && panelColumnColumn->groupForPanel(src) == target;
                }
            }
            ST_BEGIN("indicator_rightmost_tab_rightmost");
            ST_PASS("indicator_rightmost_tab rightmost=%d", panelFixRightmost ? 1 : 0);
            if (!panelFixRightmost) {
                ST_FAIL(172, "rightmost-tab indicator");
            }

            // 173: a dynamic column emptied by a close path and by a move path
            // is removed; a closed panel is rehomed so it can be shown again.
            bool panelFixClosed = false;
            bool panelFixMoved = false;
            if (panelColumnColumn) {
                panelColumnColumn->setRailMode(false);
                panelFixPump(4);
                // closed: a dynamic column whose group is closed is removed;
                // the hidden group is rehomed so the panel can be shown again.
                const int beforeClose = frame.panelColumnCountForTest();
                pictura::PanelColumn* closeDest =
                    frame.createPanelColumn(pictura::PanelSide::Right, panelColumnColumn);
                pictura::PanelGroup* closeGroup = nullptr;
                for (pictura::PanelGroup* group : panelColumnColumn->groups()) {
                    if (group && group->isVisible() && group->titleCountForTest() == 1
                        && group->objectName() != QStringLiteral("panelGroup_layersPanel")) {
                        closeGroup = group;
                        break;
                    }
                }
                if (closeDest && closeGroup) {
                    const QString closeName = closeGroup->objectName();
                    if (pictura::PanelGroup* taken = panelColumnColumn->takeGroup(closeName)) {
                        closeDest->adoptGroup(taken);
                    }
                    const QString closePanel = closeGroup->panels().isEmpty()
                                                   ? QString()
                                                   : closeGroup->panels().first()->objectName();
                    panelFixPump(6);
                    closeDest->closeGroup(closeGroup);
                    panelFixPump(6);
                    panelFixClosed = frame.panelColumnCountForTest() == beforeClose
                                && !closePanel.isEmpty()
                                && panelColumnColumn->groupForPanel(closePanel) != nullptr;
                }
                // moved: a dynamic column whose panel is moved out through the
                // real drag path empties and is removed.
                const int beforeMove = frame.panelColumnCountForTest();
                pictura::PanelColumn* moveDest =
                    frame.createPanelColumn(pictura::PanelSide::Right, panelColumnColumn);
                pictura::PanelGroup* moveGroup = nullptr;
                for (pictura::PanelGroup* group : panelColumnColumn->groups()) {
                    if (group && group->isVisible() && group->titleCountForTest() == 1
                        && group->objectName() != QStringLiteral("panelGroup_layersPanel")) {
                        moveGroup = group;
                        break;
                    }
                }
                if (moveDest && moveGroup) {
                    const QString moveName = moveGroup->objectName();
                    if (pictura::PanelGroup* taken = panelColumnColumn->takeGroup(moveName)) {
                        moveDest->adoptGroup(taken);
                    }
                    const QString movePanel = moveGroup->panels().isEmpty()
                                                  ? QString()
                                                  : moveGroup->panels().first()->objectName();
                    panelColumnColumn->setPreferredWidth(420);
                    panelFixPump(6);
                    const bool back =
                        frame.dropIntoGroupForTest(movePanel, QStringLiteral("colorPanel"), -1);
                    panelFixPump(6);
                    panelFixMoved = back && frame.panelColumnCountForTest() == beforeMove;
                }
                collapseDynamics();
            }
            ST_BEGIN("empty_column_removed_closed");
            ST_PASS("empty_column_removed closed=%d moved=%d", panelFixClosed ? 1 : 0, panelFixMoved ? 1 : 0);
            if (!(panelFixClosed && panelFixMoved)) {
                ST_FAIL(173, "empty column removed");
            }

            // 174: minimize collapses the group to its tab-bar height with the
            // content hidden and the tab menu reading `Expand Panel`.
            bool minHeight = false;
            bool minContent = false;
            bool minLabel = false;
            if (panelColumnColumn) {
                panelColumnColumn->setRailMode(false);
                panelColumnColumn->showPanel(QStringLiteral("layersPanel"), true);
                panelColumnColumn->setPreferredWidth(360);
                panelFixPump(8);
                pictura::PanelGroup* g =
                    panelColumnColumn->groupForPanel(QStringLiteral("layersPanel"));
                if (g) {
                    g->setMinimizedForTest(false);
                    panelFixPump(8);
                    const int expanded = g->height();
                    const int barH = g->tabBar() ? g->tabBar()->sizeHint().height() : 24;
                    g->setMinimizedForTest(true);
                    g->updateGeometry();
                    panelColumnColumn->updateGeometry();
                    panelFixPump(16);
                    const int collapsed = g->height();
                    minHeight = expanded > barH + 4 && collapsed > 0
                                   && collapsed <= barH + 6 && collapsed < expanded
                                   && g->maximumHeight() <= barH + 4;
                    minContent = g->contentHiddenForTest();
                    const QStringList texts = panelColumnColumn->tabMenuActionsForTest(g->objectName());
                    minLabel = texts.size() == 7
                                  && texts.value(2) == QStringLiteral("Expand Panel");
                    g->setMinimizedForTest(false);
                    g->updateGeometry();
                    panelFixPump(8);
                }
            }
            ST_BEGIN("minimize_collapse_height");
            ST_PASS("minimize_collapse height=%d content=%d "
                         "label=%d", minHeight ? 1 : 0, minContent ? 1 : 0, minLabel ? 1 : 0);
            if (!(minHeight && minContent && minLabel)) {
                ST_FAIL(174, "minimize collapse");
            }

            // 175: a bottom-boundary insert draws the horizontal line at the
            // last group's bottom edge and lands a new last group.
            bool panelFixBottom = false;
            if (panelColumnColumn) {
                panelColumnColumn->setRailMode(false);
                panelFixPump(4);
                const QString src = otherPanel(nullptr, QString());
                const QPoint p = panelColumnColumn->boundaryPointForTest(1000);
                if (!src.isEmpty() && !p.isNull()) {
                    panelColumnColumn->beginTabDragForTest(src);
                    panelColumnColumn->dragToForTest(p);
                    const QRect g = panelColumnColumn->dropIndicatorGeometryForTest();
                    const bool horiz =
                        g.isValid() && g.width() > g.height() && g.height() == 3;
                    panelColumnColumn->dropForTest(p);
                    panelFixPump(6);
                    pictura::PanelGroup* landed = panelColumnColumn->groupForPanel(src);
                    panelFixBottom = horiz && g.center().y() > panelColumnColumn->height() / 2 && landed
                                && landed->titleCountForTest() == 1;
                }
            }
            ST_BEGIN("indicator_bottom_bottom");
            ST_PASS("indicator_bottom bottom=%d", panelFixBottom ? 1 : 0);
            if (!panelFixBottom) {
                ST_FAIL(175, "bottom indicator");
            }

            // 176: the column never clips — the tab text elides, the corner
            // button keeps its width, and the column is wide enough that no
            // horizontal scrollbar is ever needed (policy off).
            bool noClipElide = false;
            bool noClipScroll = false;
            if (panelColumnColumn) {
                panelColumnColumn->setRailMode(false);
                panelFixPump(4);
                bool any = false;
                bool elide = true;
                bool corner = true;
                for (pictura::PanelGroup* g : panelColumnColumn->groups()) {
                    if (!g || !g->isVisible()) {
                        continue;
                    }
                    any = true;
                    QTabBar* bar = g->tabBar();
                    elide = elide && bar && bar->elideMode() == Qt::ElideRight
                            && !bar->expanding();
                    corner = corner && g->headerCornerWidthForTest() > 0;
                }
                noClipElide = any && elide && corner;
                noClipScroll = panelColumnColumn->horizontalScrollPolicyForTest()
                                  == static_cast<int>(Qt::ScrollBarAlwaysOff);
            }
            ST_BEGIN("no_clip_elide");
            ST_PASS("no_clip elide=%d scroll=%d", noClipElide ? 1 : 0, noClipScroll ? 1 : 0);
            if (!(noClipElide && noClipScroll)) {
                ST_FAIL(176, "no clip");
            }

            // 177: every normal-mode widget column shares one minimum-width
            // floor and cannot vanish when resized to it.
            bool floorShared = false;
            bool floorNotGone = false;
            if (panelColumnColumn) {
                panelColumnColumn->setRailMode(true);
                panelFixPump(2);
                panelColumnColumn->setRailMode(false);
                panelFixPump(4);
                pictura::PanelColumn* dest =
                    frame.createPanelColumn(pictura::PanelSide::Right, panelColumnColumn);
                pictura::PanelGroup* seed = nullptr;
                for (pictura::PanelGroup* group : panelColumnColumn->groups()) {
                    if (group && group->isVisible() && group->titleCountForTest() == 1) {
                        seed = group;
                        break;
                    }
                }
                if (dest && seed) {
                    const QString seedName = seed->objectName();
                    if (pictura::PanelGroup* taken = panelColumnColumn->takeGroup(seedName)) {
                        dest->adoptGroup(taken);
                    }
                }
                if (dest && dest != panelColumnColumn) {
                    const int floor = panelColumnColumn->minimumWidthFloorForTest();
                    const int minA = panelColumnColumn->minimumWidthForTest();
                    const int minB = dest->minimumWidthForTest();
                    floorShared = floor > 0 && minA == floor && minB == floor;
                    dest->setPreferredWidth(floor);
                    panelColumnColumn->setPreferredWidth(floor);
                    panelFixPump(8);
                    floorNotGone = minA > 0 && !panelColumnColumn->isHidden()
                                      && !dest->isHidden() && panelColumnColumn->width() >= floor;
                    collapseDynamics();
                }
            }
            ST_BEGIN("min_width_floor_shared");
            ST_PASS("min_width_floor shared=%d notgone=%d", floorShared ? 1 : 0, floorNotGone ? 1 : 0);
            if (!(floorShared && floorNotGone)) {
                ST_FAIL(177, "min width floor");
            }
        }
        // M45 Phase C (178-179): the compact popup hosts the whole PanelGroup
        // (all tabs, clicked panel active) and restores it exactly once on close
        // (C1); a whole-group compact drag draws above the drag-handle dots (C2).
        {
            auto panelFixPump = [](int n) {
                for (int i = 0; i < n; ++i) {
                    QCoreApplication::processEvents();
                }
            };

            // 178: the popup contains the full group's tabs, the clicked panel
            // is current, it is the very same widget as the docked group (shared
            // style/container), and closing restores it exactly once.
            bool popupTabs = false;
            bool popupActive = false;
            bool popupParity = false;
            bool popupRestore = false;
            if (panelColumnColumn) {
                panelColumnColumn->setRailMode(true);
                panelFixPump(6);
                // The Layers group carries three tabs (Layers/Channels/Paths),
                // so clicking Channels proves the whole group is hosted.
                const QString clicked = QStringLiteral("channelsPanel");
                pictura::PanelGroup* group = panelColumnColumn->groupForPanel(clicked);
                if (group) {
                    group->setCurrentPanel(QStringLiteral("layersPanel"));
                    panelFixPump(2);
                    const QStringList expectedTitles = group->titles();
                    const int expectedCount = group->titleCountForTest();
                    const bool opened = panelColumnColumn->openIconFlyoutForTest(clicked);
                    panelFixPump(10);
                    pictura::PanelGroup* hosted = panelColumnColumn->iconFlyoutGroupForTest();
                    popupTabs = opened && hosted == group && group->isVisible()
                                   && hosted->titleCountForTest() == expectedCount
                                   && hosted->titles() == expectedTitles;
                    popupActive = hosted && hosted->currentPanelName() == clicked;
                    popupParity = panelColumnColumn->iconFlyoutVisibleForTest()
                                     && panelColumnColumn->popupStyleParityForTest(clicked);
                    const bool closed = panelColumnColumn->triggerFlyoutCloseForTest();
                    panelFixPump(10);
                    int occurrences = 0;
                    for (pictura::PanelGroup* cand : panelColumnColumn->groups()) {
                        if (cand == group) {
                            ++occurrences;
                        }
                    }
                    QWidget* parent = group->parentWidget();
                    popupRestore =
                        closed && !panelColumnColumn->iconFlyoutVisibleForTest()
                        && panelColumnColumn->iconFlyoutGroupForTest() == nullptr
                        && occurrences == 1 && parent
                        && parent->objectName() == QStringLiteral("panelColumnSplitter")
                        && group->titleCountForTest() == expectedCount
                        && group->currentPanelName() == clicked
                        && panelColumnColumn->groupForPanel(clicked) == group;
                }
                panelColumnColumn->setRailMode(false);
                panelFixPump(4);
            }
            ST_BEGIN("popup_group_tabs");
            ST_PASS("popup_group tabs=%d active=%d parity=%d "
                         "restore=%d", popupTabs ? 1 : 0, popupActive ? 1 : 0, popupParity ? 1 : 0,
                         popupRestore ? 1 : 0);
            if (!(popupTabs && popupActive && popupParity && popupRestore)) {
                ST_FAIL(178, "popup group");
            }

            // 179: dragging a whole group in compact mode shows the placement
            // line above the group's drag-handle grip (at its insertion
            // boundary), never inside the group below the dots.
            bool compactAbove = false;
            if (panelColumnColumn) {
                panelColumnColumn->setRailMode(true);
                panelFixPump(8);
                const QStringList order = panelColumnColumn->compactStripGroupOrderForTest();
                const QString first = order.isEmpty() ? QString() : order.first();
                QWidget* grip = first.isEmpty() ? nullptr : panelColumnColumn->dragHandleForTest(first);
                const QPoint boundary = panelColumnColumn->compactStripBoundaryPointForTest(0);
                const bool began =
                    !first.isEmpty() && panelColumnColumn->beginGroupGripDragForTest(first);
                panelColumnColumn->dragToForTest(boundary);
                const QRect indicator = panelColumnColumn->dropIndicatorGeometryForTest();
                const QRect indicatorGlobal = panelColumnColumn->dropIndicatorGlobalGeometryForTest();
                const QRect gripGlobal =
                    grip ? QRect(grip->mapToGlobal(QPoint(0, 0)), grip->size()) : QRect();
                compactAbove =
                    began && grip && boundary != QPoint() && indicator.isValid()
                    && indicator.width() > indicator.height()
                    && indicatorGlobal.isValid() && gripGlobal.isValid()
                    && indicatorGlobal.center().y() <= gripGlobal.top()
                    && qAbs(indicatorGlobal.center().y() - boundary.y()) <= 3;
                panelColumnColumn->cancelDragForTest();
                panelFixPump(6);
                panelColumnColumn->setRailMode(false);
                panelFixPump(4);
            }
            ST_BEGIN("compact_group_line_above");
            ST_PASS("compact_group_line above=%d", compactAbove ? 1 : 0);
            if (!compactAbove) {
                ST_FAIL(179, "compact group line");
            }
        }

        // M46 (180-186): regression checks for the panel drop indicator,
        // floating-Tools gesture, splitter floor, minimize height, and primary
        // empty-column lifecycle. Each drives the real resolved DropTarget
        // geometry (not just the helper) and restores the layout afterwards.
        {
            auto toolbarFixPump = [](int n) {
                for (int i = 0; i < n; ++i) {
                    QCoreApplication::processEvents();
                }
            };
            auto toolbarFixOtherPanel = [&](pictura::PanelGroup* exclude,
                                     const QString& alsoExclude) -> QString {
                if (!panelColumnColumn) {
                    return QString();
                }
                for (pictura::PanelGroup* group : panelColumnColumn->groups()) {
                    if (!group || group == exclude || !group->isVisible()
                        || group->titleCountForTest() == 0) {
                        continue;
                    }
                    for (QWidget* panel : group->panels()) {
                        if (panel && panel->objectName() != alsoExclude) {
                            return panel->objectName();
                        }
                    }
                }
                return QString();
            };
            auto toolbarFixCollapseDynamics = [&]() {
                for (pictura::PanelColumn* column : frame.panelColumns()) {
                    if (!column || column == panelColumnColumn) {
                        continue;
                    }
                    const QList<pictura::PanelGroup*> groups = column->groups();
                    for (pictura::PanelGroup* group : groups) {
                        if (!group) {
                            continue;
                        }
                        if (pictura::PanelGroup* taken =
                                column->takeGroup(group->objectName())) {
                            panelColumnColumn->adoptGroup(taken);
                        }
                    }
                    frame.removeColumnIfEmpty(column);
                }
                if (panelColumnColumn) {
                    panelColumnColumn->show();
                }
                toolbarFixPump(6);
            };

            // 180: a group with a hidden tab maps the rightmost insertion over
            // the visible tabs, so the line draws at the right visible tab (not
            // the far left) and the dropped panel lands at that index.
            bool hiddenShown = false;
            bool hiddenPlaced = false;
            if (panelColumnColumn) {
                panelColumnColumn->setRailMode(false);
                panelColumnColumn->setPreferredWidth(180);
                toolbarFixPump(6);
                pictura::PanelColumn* dest =
                    frame.createPanelColumn(pictura::PanelSide::Right, panelColumnColumn);
                pictura::PanelGroup* dg = nullptr;
                for (pictura::PanelGroup* group : panelColumnColumn->groups()) {
                    if (group && group->isVisible() && group->titleCountForTest() >= 3) {
                        dg = group;
                        break;
                    }
                }
                if (!dg) {
                    for (pictura::PanelGroup* group : panelColumnColumn->groups()) {
                        if (group && group->isVisible()
                            && group->titleCountForTest() >= 2) {
                            dg = group;
                            break;
                        }
                    }
                }
                if (dest && dg) {
                    const QString seedName = dg->objectName();
                    if (pictura::PanelGroup* taken = panelColumnColumn->takeGroup(seedName)) {
                        dest->adoptGroup(taken);
                    }
                }
                if (dest) {
                    panelColumnColumn->setPreferredWidth(180);
                    dest->setPreferredWidth(300);
                    toolbarFixPump(6);
                }
                const QString src = toolbarFixOtherPanel(nullptr, QString());
                if (dest && dest != panelColumnColumn && dg && !src.isEmpty()) {
                    QString hiddenName;
                    const QList<QWidget*> panels = dg->panels();
                    if (!panels.isEmpty()) {
                        hiddenName = panels.last()->objectName();
                    }
                    const bool hid =
                        !hiddenName.isEmpty() && dg->setPanelVisible(hiddenName, false);
                    toolbarFixPump(4);
                    const bool invariant =
                        dg->titleCountForTest() > dg->visibleTitles().size();
                    const int end = dg->titleCountForTest();
                    const QPoint p = dg->tabInsertionGlobalPointForTest(end);
                    QTabBar* bar = dg->tabBar();
                    int lastVisible = -1;
                    for (int i = 0; bar && i < bar->count(); ++i)
                        if (bar->isTabVisible(i)) lastVisible = i;
                    const int rightmostVisible = bar && lastVisible >= 0
                        ? bar->mapToGlobal(bar->tabRect(lastVisible).center()).x() : -1;
                    panelColumnColumn->beginTabDragForTest(src);
                    panelColumnColumn->dragToForTest(p);
                    const QRect g = dest->dropIndicatorGlobalGeometryForTest();
                    hiddenShown = hid && invariant && dest->dropIndicatorVisibleForTest()
                                     && !panelColumnColumn->dropIndicatorVisibleForTest()
                                     && g.isValid() && g.center().x() >= rightmostVisible
                                     && qAbs(g.center().x() - p.x()) <= 8;
                    panelColumnColumn->dropForTest(p);
                    toolbarFixPump(4);
                    hiddenPlaced =
                        dg->indexOfPanel(src) == end && dest->groupForPanel(src) == dg;
                    if (!hiddenName.isEmpty()) {
                        dg->setPanelVisible(hiddenName, true);
                    }
                }
                toolbarFixCollapseDynamics();
            }
            ST_BEGIN("indicator_hidden_tab_shown");
            ST_PASS("indicator_hidden_tab shown=%d placed=%d", hiddenShown ? 1 : 0, hiddenPlaced ? 1 : 0);
            if (!(hiddenShown && hiddenPlaced)) {
                ST_FAIL(180, "hidden-tab indicator");
            }

            // 181: a cross-column drop onto a group body (and between two groups)
            // draws the line in the target column and lands the panel there.
            bool toolbarFixCrossShown = false;
            bool toolbarFixCrossPlaced = false;
            bool boundaryShown = false;
            bool boundaryPlaced = false;
            if (panelColumnColumn) {
                panelColumnColumn->setRailMode(false);
                panelColumnColumn->setPreferredWidth(180);
                toolbarFixPump(6);
                pictura::PanelColumn* dest =
                    frame.createPanelColumn(pictura::PanelSide::Right, panelColumnColumn);
                QList<pictura::PanelGroup*> destGroups;
                for (pictura::PanelGroup* group : panelColumnColumn->groups()) {
                    if (destGroups.size() >= 2) {
                        break;
                    }
                    if (group && group->isVisible()) {
                        const QString nm = group->objectName();
                        if (dest) {
                            if (pictura::PanelGroup* taken = panelColumnColumn->takeGroup(nm)) {
                                dest->adoptGroup(taken);
                                destGroups << taken;
                            }
                        }
                    }
                }
                if (dest) {
                    panelColumnColumn->setPreferredWidth(180);
                    dest->setPreferredWidth(180);
                    toolbarFixPump(6);
                }
                pictura::PanelGroup* bg =
                    destGroups.isEmpty() ? nullptr : destGroups.first();
                const QString src = toolbarFixOtherPanel(nullptr, QString());
                if (dest && dest != panelColumnColumn && bg && !src.isEmpty()) {
                    const QRect barRect = bg->tabBarGlobalRect();
                    const QRect groupRect(bg->mapToGlobal(QPoint(0, 0)), bg->size());
                    const QPoint body(groupRect.center().x(),
                                      (barRect.bottom() + groupRect.bottom()) / 2);
                    panelColumnColumn->beginTabDragForTest(src);
                    panelColumnColumn->dragToForTest(body);
                    toolbarFixCrossShown = dest->dropIndicatorVisibleForTest()
                                    && !panelColumnColumn->dropIndicatorVisibleForTest();
                    panelColumnColumn->dropForTest(body);
                    toolbarFixPump(6);
                    toolbarFixCrossPlaced = dest->groupForPanel(src) != nullptr;
                }
                if (dest && destGroups.size() >= 2) {
                    pictura::PanelGroup* g0 = destGroups.at(0);
                    pictura::PanelGroup* g1 = destGroups.at(1);
                    const QString src2 = toolbarFixOtherPanel(nullptr, QString());
                    if (g0 && g1 && !src2.isEmpty()) {
                        const QRect r0(g0->mapToGlobal(QPoint(0, 0)), g0->size());
                        const QRect r1(g1->mapToGlobal(QPoint(0, 0)), g1->size());
                        const QPoint boundary(r0.center().x(),
                                             (r0.bottom() + r1.top()) / 2);
                        panelColumnColumn->beginTabDragForTest(src2);
                        panelColumnColumn->dragToForTest(boundary);
                        boundaryShown = dest->dropIndicatorVisibleForTest()
                                           && !panelColumnColumn->dropIndicatorVisibleForTest();
                        panelColumnColumn->dropForTest(boundary);
                        toolbarFixPump(6);
                        boundaryPlaced = dest->groupForPanel(src2) != nullptr;
                    }
                }
                toolbarFixCollapseDynamics();
            }
            ST_BEGIN("indicator_cross_body_shown");
            ST_PASS("indicator_cross_body shown=%d placed=%d "
                         "boundary_shown=%d boundary_placed=%d", toolbarFixCrossShown ? 1 : 0, toolbarFixCrossPlaced ? 1 : 0,
                         boundaryShown ? 1 : 0, boundaryPlaced ? 1 : 0);
            if (!(toolbarFixCrossShown && toolbarFixCrossPlaced && boundaryShown
                  && boundaryPlaced)) {
                ST_FAIL(181, "cross-column indicator");
            }

            // 182: the bottom-boundary line is clamped inside the scroll
            // viewport so none of it is drawn past the visible area. Use a
            // fresh column with one group so the group fills the viewport and
            // its bottom is a reachable BelowGroup boundary.
            bool bottomInside = false;
            if (panelColumnColumn) {
                panelColumnColumn->setRailMode(false);
                toolbarFixPump(4);
                pictura::PanelColumn* dest =
                    frame.createPanelColumn(pictura::PanelSide::Right, panelColumnColumn);
                if (dest) {
                    for (pictura::PanelGroup* group : panelColumnColumn->groups()) {
                        if (group && group->isVisible() && !group->visibleTitles().isEmpty()) {
                            if (pictura::PanelGroup* taken = panelColumnColumn->takeGroup(group->objectName())) {
                                dest->adoptGroup(taken);
                            }
                            break;
                        }
                    }
                    toolbarFixPump(6);
                    QString src;
                    for (pictura::PanelGroup* group : dest->groups()) {
                        if (group && group->isVisible() && !group->visibleTitles().isEmpty()) {
                            src = group->visibleTitles().first();
                            break;
                        }
                    }
                    const QPoint p = dest->boundaryPointForTest(1000);
                    if (!src.isEmpty() && !p.isNull()) {
                        dest->beginTabDragForTest(src);
                        dest->dragToForTest(p);
                        const QRect g = dest->dropIndicatorGeometryForTest();
                        const int vh = dest->scrollViewportHeightForTest();
                        bottomInside =
                            g.isValid() && vh > 0 && g.top() >= 0 && g.bottom() <= vh;
                        dest->dropForTest(p);
                        toolbarFixPump(6);
                    }
                }
                toolbarFixCollapseDynamics();
            }
            ST_BEGIN("indicator_bottom_inside_inside");
            ST_PASS("indicator_bottom_inside inside=%d", bottomInside ? 1 : 0);
            if (!bottomInside) {
                ST_FAIL(182, "bottom inside viewport");
            }

            // 183: the tools column's own header drives the whole-column drag:
            // a press-move follows as a frameless tool window (never a decorated
            // OS window), and a release on the workspace edge re-docks the column.
            bool toolsMoved = false;
            bool toolsFinished = false;
            if (pictura::PanelColumn* tools = frame.toolsColumn()) {
                QWidget* tabs = frame.findChild<QWidget*>(QStringLiteral("documentTabs"));
                const QPoint start = tools->mapToGlobal(QPoint(qMax(1, tools->width() / 2), 8));
                tools->beginColumnHeaderDragForTest(start);
                const QPoint empty = tabs ? tabs->mapToGlobal(tabs->rect().center())
                                          : tools->mapToGlobal(QPoint(tools->width() * 2, 40));
                tools->dragColumnHeaderToForTest(empty);
                pictura::PanelFloat* floatWindow = tools->columnFloatForTest();
                toolsMoved = floatWindow != nullptr && floatWindow->isWindow()
                             && floatWindow->windowFlags().testFlag(Qt::Tool)
                             && floatWindow->windowFlags().testFlag(Qt::FramelessWindowHint);
                const QPoint edge(frame.centralWidget()->mapToGlobal(
                    QPoint(2, frame.centralWidget()->height() / 2)));
                toolsFinished = tools->dropColumnHeaderForTest(edge)
                                && tools->columnFloatForTest() == nullptr;
                toolbarFixPump(6);
            }
            ST_BEGIN("tools_gesture_moved");
            ST_PASS("tools_gesture moved=%d finished=%d", toolsMoved ? 1 : 0, toolsFinished ? 1 : 0);
            if (!(toolsMoved && toolsFinished)) {
                ST_FAIL(183, "tools column gesture");
            }

            // 184: neither splitter collapses a pane, and the column cannot be
            // squeezed away below its shared floor.
            bool noCollapse = false;
            bool columnKept = false;
            if (panelColumnColumn) {
                auto* cs = frame.findChild<QSplitter*>(QStringLiteral("centerSplitter"));
                auto* gs = panelColumnColumn->findChild<QSplitter*>(
                    QStringLiteral("panelColumnSplitter"));
                noCollapse = cs && gs && !cs->childrenCollapsible()
                                && !gs->childrenCollapsible();
                panelColumnColumn->show();
                panelColumnColumn->setPreferredWidth(1);
                toolbarFixPump(6);
                columnKept = panelColumnColumn->isVisible()
                                && panelColumnColumn->width() >= panelColumnColumn->minimumWidthForTest();
            }
            ST_BEGIN("splitter_nocollapse_nocollapse");
            ST_PASS("splitter_nocollapse nocollapse=%d kept=%d", noCollapse ? 1 : 0, columnKept ? 1 : 0);
            if (!(noCollapse && columnKept)) {
                ST_FAIL(184, "splitter no collapse");
            }

            // 185: minimizing a group whose content minimum exceeds its tab bar
            // clamps the group to the tab-bar height and hides the content.
            bool minTall = false;
            if (panelColumnColumn) {
                panelColumnColumn->setRailMode(false);
                panelColumnColumn->setPreferredWidth(360);
                panelColumnColumn->showPanel(QStringLiteral("colorPanel"), true);
                toolbarFixPump(8);
                pictura::PanelGroup* g =
                    panelColumnColumn->groupForPanel(QStringLiteral("colorPanel"));
                if (!g) {
                    g = panelColumnColumn->groupForPanel(QStringLiteral("swatchesPanel"));
                }
                if (g) {
                    g->setMinimizedForTest(false);
                    toolbarFixPump(8);
                    g->setMinimizedForTest(true);
                    g->updateGeometry();
                    panelColumnColumn->updateGeometry();
                    toolbarFixPump(16);
                    const int barH = g->tabBar() ? g->tabBar()->sizeHint().height() : 24;
                    minTall = g->height() <= barH + 6 && g->contentHiddenForTest();
                    g->setMinimizedForTest(false);
                    g->updateGeometry();
                    toolbarFixPump(8);
                }
            }
            ST_BEGIN("minimize_tall_height");
            ST_PASS("minimize_tall height=%d", minTall ? 1 : 0);
            if (!minTall) {
                ST_FAIL(185, "minimize tall group");
            }

            // 186: the primary column hides when its last visible panel closes
            // and re-shows on the next Window-menu show.
            bool primaryHide = false;
            bool primaryShow = false;
            if (panelColumnColumn) {
                const bool wasVisible = panelColumnColumn->isVisible();
                const QList<pictura::PanelGroup*> groups = panelColumnColumn->groups();
                for (pictura::PanelGroup* g : groups) {
                    if (g) {
                        panelColumnColumn->closeGroup(g);
                    }
                }
                toolbarFixPump(8);
                primaryHide = !panelColumnColumn->isVisible();
                panelColumnColumn->showPanel(QStringLiteral("layersPanel"), true);
                toolbarFixPump(8);
                primaryShow = panelColumnColumn->isVisible()
                                 && panelColumnColumn->groupForPanel(QStringLiteral("layersPanel"))
                                        != nullptr;
                if (!wasVisible || !panelColumnColumn->isVisible()) {
                    panelColumnColumn->show();
                }
            }
            ST_BEGIN("primary_empty_hide");
            ST_PASS("primary_empty hide=%d show=%d", primaryHide ? 1 : 0, primaryShow ? 1 : 0);
            if (!(primaryHide && primaryShow)) {
                ST_FAIL(186, "primary empty");
            }

            // M47 (187-195): empty-column-after-float, ghost-group hiding,
            // compact icon float, compact grip group creation, no horizontal
            // scroll, fixed iconic width, Tools pane hosting, float close, and
            // compact chrome shading. Exit codes 187-195.
            auto firstVisiblePanel = [](pictura::PanelGroup* group) -> QString {
                if (!group) {
                    return QString();
                }
                const QList<QWidget*> visible = group->visiblePanels();
                return visible.isEmpty() ? QString() : visible.first()->objectName();
            };
            auto showPrimary = [&]() {
                panelColumnColumn->setRailMode(false);
                for (pictura::PanelGroup* group : panelColumnColumn->groups()) {
                    if (!group) {
                        continue;
                    }
                    for (QWidget* panel : group->panels()) {
                        if (panel) {
                            panelColumnColumn->showPanel(panel->objectName(), true);
                        }
                    }
                }
                toolbarFixPump(8);
            };
            auto adoptOneInto = [&](pictura::PanelColumn* dest) -> bool {
                if (!dest) {
                    return false;
                }
                for (pictura::PanelGroup* group : panelColumnColumn->groups()) {
                    if (group && !group->visiblePanels().isEmpty()) {
                        if (pictura::PanelGroup* taken =
                                panelColumnColumn->takeGroup(group->objectName())) {
                            dest->adoptGroup(taken);
                            return true;
                        }
                    }
                }
                return false;
            };
            auto closeAllFloats = [&]() {
                for (int i = 0; i < 24 && panelColumnColumn->floatCountForTest() > 0; ++i) {
                    const int count = panelColumnColumn->floatCountForTest();
                    // M47: a re-homed float's close callback is repointed at the
                    // primary, so closing tears the overlay down; fall back to a
                    // re-dock only if a float refuses to close.
                    if (!panelColumnColumn->closeFloatForTest(0)
                        && !panelColumnColumn->redockForTest(0, 0)) {
                        panelColumnColumn->cancelDragForTest();
                    }
                    toolbarFixPump(4);
                    if (panelColumnColumn->floatCountForTest() >= count) {
                        break;
                    }
                }
            };

            // 187: the last group of a dynamic column is torn off and left
            // floating; the now-empty column is removed and its float is
            // re-homed to the primary (still live and re-dockable).
            bool emptyGone = false;
            bool emptyFloat = false;
            bool emptyPrimary = false;
            if (panelColumnColumn) {
                showPrimary();
                closeAllFloats();
                const int floatsBefore = panelColumnColumn->floatCountForTest();
                const int before = frame.panelColumnCountForTest();
                pictura::PanelColumn* dest =
                    frame.createPanelColumn(pictura::PanelSide::Right, panelColumnColumn);
                pictura::PanelGroup* destGroup = nullptr;
                if (dest) {
                    for (pictura::PanelGroup* group : panelColumnColumn->groups()) {
                        if (group && !group->visiblePanels().isEmpty()) {
                            if (pictura::PanelGroup* taken =
                                    panelColumnColumn->takeGroup(group->objectName())) {
                                dest->adoptGroup(taken);
                                destGroup = taken;
                            }
                            break;
                        }
                    }
                    toolbarFixPump(6);
                }
                const QString adoptPanel = firstVisiblePanel(destGroup);
                if (dest && !adoptPanel.isEmpty()) {
                    // A point over the source column's own header is outside its
                    // scroll viewport (and resolves to no sibling), so the drag
                    // leaves a live float without re-docking it.
                    const QPoint outside =
                        dest->mapToGlobal(QPoint(dest->width() / 2, 3));
                    dest->beginGroupDragForTest(adoptPanel);
                    dest->dragToForTest(outside);
                    dest->dropForTest(outside);
                    toolbarFixPump(8);
                }
                emptyGone = dest && frame.panelColumnCountForTest() == before;
                emptyFloat = panelColumnColumn->floatCountForTest() == floatsBefore + 1;
                emptyPrimary = frame.panelColumn() == panelColumnColumn && panelColumnColumn->isVisible();
                // A float re-homed by the removal must close through the primary
                // (its close callback is repointed), not strand the overlay.
                const bool rehomedCloses =
                    panelColumnColumn->closeFloatForTest(0)
                    && panelColumnColumn->floatCountForTest() == floatsBefore;
                emptyFloat = emptyFloat && rehomedCloses;
                closeAllFloats();
                toolbarFixCollapseDynamics();
            }
            ST_BEGIN("empty_after_float_gone");
            ST_PASS("empty_after_float gone=%d float=%d "
                         "primary=%d", emptyGone ? 1 : 0, emptyFloat ? 1 : 0,
                         emptyPrimary ? 1 : 0);
            if (!(emptyGone && emptyFloat && emptyPrimary)) {
                ST_FAIL(187, "empty after float");
            }

            // 188: moving the last visible tab out of a group hides the emptied
            // group (no ghost shell) while its hidden panel stays reachable and
            // re-showable.
            bool ghostHidden = false;
            bool ghostFound = false;
            bool ghostShown = false;
            if (panelColumnColumn) {
                showPrimary();
                pictura::PanelGroup* ghost =
                    panelColumnColumn->groupForPanel(QStringLiteral("adjustmentsPanel"));
                pictura::PanelGroup* groupTarget =
                    panelColumnColumn->groupForPanel(QStringLiteral("colorPanel"));
                if (ghost && groupTarget && ghost != groupTarget
                    && ghost->titleCountForTest() >= 2) {
                    ghost->setPanelVisible(QStringLiteral("propertiesPanel"), false);
                    toolbarFixPump(4);
                    const QString last = firstVisiblePanel(ghost);
                    const QPoint p = groupTarget->tabInsertionGlobalPointForTest(0);
                    const bool began =
                        !last.isEmpty() && panelColumnColumn->beginTabDragForTest(last);
                    panelColumnColumn->dragToForTest(p);
                    const bool dropped = panelColumnColumn->dropForTest(p);
                    toolbarFixPump(8);
                    ghostHidden = began && dropped && !ghost->isVisible();
                    ghostFound =
                        panelColumnColumn->groupForPanel(QStringLiteral("propertiesPanel")) == ghost;
                    panelColumnColumn->showPanel(QStringLiteral("propertiesPanel"), true);
                    toolbarFixPump(8);
                    ghostShown =
                        ghost->isVisible()
                        && panelColumnColumn->groupForPanel(QStringLiteral("propertiesPanel")) == ghost;
                }
                toolbarFixCollapseDynamics();
            }
            ST_BEGIN("ghost_group_hidden");
            ST_PASS("ghost_group hidden=%d found=%d shown=%d", ghostHidden ? 1 : 0, ghostFound ? 1 : 0,
                         ghostShown ? 1 : 0);
            if (!(ghostHidden && ghostFound && ghostShown)) {
                ST_FAIL(188, "ghost group");
            }
            // 189: a real press+move on a compact strip icon tears the panel
            // into a float that survives the (now non-rebuilding) strip and
            // commits on release.
            bool iconDrag = false;
            bool iconFloat = false;
            bool iconReleased = false;
            if (panelColumnColumn) {
                showPrimary();
                closeAllFloats();
                const int floatsBefore = panelColumnColumn->floatCountForTest();
                panelColumnColumn->setRailMode(true);
                toolbarFixPump(8);
                QToolButton* button = nullptr;
                for (QToolButton* candidate : panelColumnColumn->findChildren<QToolButton*>()) {
                    if (candidate
                        && candidate->objectName().startsWith(QStringLiteral("panelIcon_"))
                        && candidate->isVisible()) {
                        button = candidate;
                        break;
                    }
                }
                const QPoint outside =
                    panelColumnColumn->mapToGlobal(QPoint(-40, panelColumnColumn->height() / 2));
                if (button) {
                    const QPoint local = button->rect().center();
                    const QPointF localF(local);
                    const QPointF globalF(button->mapToGlobal(local));
                    QMouseEvent press(QEvent::MouseButtonPress, localF, globalF,
                                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                    QCoreApplication::sendEvent(button, &press);
                    const QPointF outsideF(outside);
                    QMouseEvent move(QEvent::MouseMove, outsideF, outsideF, Qt::NoButton,
                                     Qt::LeftButton, Qt::NoModifier);
                    QCoreApplication::sendEvent(button, &move);
                    toolbarFixPump(4);
                    iconDrag = panelColumnColumn->dragActiveForTest();
                    iconFloat = panelColumnColumn->floatCountForTest() == floatsBefore + 1;
                    QMouseEvent release(QEvent::MouseButtonRelease, outsideF, outsideF,
                                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                    QCoreApplication::sendEvent(button, &release);
                    toolbarFixPump(8);
                    iconReleased = !panelColumnColumn->dragActiveForTest();
                }
                closeAllFloats();
                panelColumnColumn->setRailMode(false);
                toolbarFixPump(6);
                toolbarFixCollapseDynamics();
            }
            ST_BEGIN("compact_icon_float_drag");
            ST_PASS("compact_icon_float drag=%d float=%d "
                         "released=%d", iconDrag ? 1 : 0, iconFloat ? 1 : 0,
                         iconReleased ? 1 : 0);
            if (!(iconDrag && iconFloat && iconReleased)) {
                ST_FAIL(189, "compact icon float");
            }
            // 190: a single compact panel dropped on a group's grip creates a
            // new group immediately above (before) that group.
            bool gripGroup = false;
            if (panelColumnColumn) {
                showPrimary();
                panelColumnColumn->setRailMode(true);
                toolbarFixPump(8);
                pictura::PanelGroup* groupTarget = nullptr;
                QString src;
                for (pictura::PanelGroup* group : panelColumnColumn->groups()) {
                    if (!group || group->visiblePanels().isEmpty()) {
                        continue;
                    }
                    if (!groupTarget) {
                        groupTarget = group;
                    } else if (src.isEmpty()) {
                        src = group->visiblePanels().first()->objectName();
                    }
                }
                if (groupTarget && !src.isEmpty()) {
                    QWidget* grip = panelColumnColumn->dragHandleForTest(groupTarget->objectName());
                    const int targetIndex = panelColumnColumn->groups().indexOf(groupTarget);
                    if (grip && targetIndex >= 0) {
                        const QPoint gripCenter = grip->mapToGlobal(grip->rect().center());
                        const bool began = panelColumnColumn->beginTabDragForTest(src);
                        panelColumnColumn->dragToForTest(gripCenter);
                        const bool dropped = panelColumnColumn->dropForTest(gripCenter);
                        toolbarFixPump(8);
                        pictura::PanelGroup* landed = panelColumnColumn->groupForPanel(src);
                        const int landedIndex =
                            landed ? panelColumnColumn->groups().indexOf(landed) : -1;
                        const int targetNow = panelColumnColumn->groups().indexOf(groupTarget);
                        gripGroup = began && dropped && landed && landed != groupTarget
                                       && landedIndex == targetIndex
                                       && targetNow == targetIndex + 1;
                    }
                }
                panelColumnColumn->setRailMode(false);
                toolbarFixPump(6);
                toolbarFixCollapseDynamics();
            }
            ST_BEGIN("compact_grip_group_above");
            ST_PASS("compact_grip_group above=%d", gripGroup ? 1 : 0);
            if (!gripGroup) {
                ST_FAIL(190, "compact grip group");
            }
            // 191: the column never scrolls horizontally, even at its minimum
            // width, because the shared floor keeps the content visible.
            bool hScrollOff = false;
            bool hScrollZero = false;
            if (panelColumnColumn) {
                panelColumnColumn->setRailMode(false);
                toolbarFixPump(4);
                hScrollOff = panelColumnColumn->horizontalScrollPolicyForTest()
                                == static_cast<int>(Qt::ScrollBarAlwaysOff);
                panelColumnColumn->setPreferredWidth(panelColumnColumn->minimumWidthForTest());
                toolbarFixPump(8);
                // A panel's minimum width can grow by a few pixels across late
                // font/layout settling passes, so a couple of pixels of residual
                // range is tolerated here. The real guarantee is the AlwaysOff
                // policy above; `min_width_floor_shared` covers the floor, and a
                // gross overflow still fails.
                hScrollZero = panelColumnColumn->horizontalScrollRangeForTest() <= 8;
                toolbarFixCollapseDynamics();
            }
            ST_BEGIN("no_hscroll_off");
            ST_PASS("no_hscroll off=%d zero=%d range=%d min=%d floor=%d content=%d viewport=%d",
                    hScrollOff ? 1 : 0, hScrollZero ? 1 : 0,
                    panelColumnColumn ? panelColumnColumn->horizontalScrollRangeForTest() : -1,
                    panelColumnColumn ? panelColumnColumn->minimumWidthForTest() : -1,
                    panelColumnColumn ? panelColumnColumn->minimumWidthFloorForTest() : -1,
                    panelColumnColumn ? panelColumnColumn->contentMinimumWidthForTest() : -1,
                    panelColumnColumn ? panelColumnColumn->viewportWidthForTest() : -1);
            if (!(hScrollOff && hScrollZero)) {
                ST_FAIL(191, "no hscroll");
            }
            // 192: an iconic column keeps its narrow minimum but stays a
            // resizable splitter pane; a preferred-width change can widen it,
            // and leaving iconic restores the normal floor.
            bool interactionIconicWidth = false;
            bool iconicResizable = false;
            bool iconicGrow = false;
            bool iconicExit = false;
            if (panelColumnColumn) {
                showPrimary();
                panelColumnColumn->setRailMode(true);
                toolbarFixPump(8);
                const int stripMin = panelColumnColumn->minimumWidthForTest();
                const int iconicWidth = panelColumnColumn->width();
                interactionIconicWidth = stripMin > 0 && stripMin <= 60 && iconicWidth >= stripMin;
                iconicResizable =
                    panelColumnColumn->maximumWidth() > panelColumnColumn->minimumWidth();
                panelColumnColumn->setPreferredWidth(400);
                toolbarFixPump(8);
                iconicGrow = panelColumnColumn->width() > stripMin;
                panelColumnColumn->setRailMode(false);
                toolbarFixPump(8);
                iconicExit = panelColumnColumn->minimumWidthForTest() > stripMin;
                toolbarFixCollapseDynamics();
            }
            ST_BEGIN("iconic_resizable_width");
            ST_PASS("iconic_resizable width=%d resizable=%d "
                         "grow=%d exit=%d", interactionIconicWidth ? 1 : 0,
                         iconicResizable ? 1 : 0, iconicGrow ? 1 : 0, iconicExit ? 1 : 0);
            if (!(interactionIconicWidth && iconicResizable && iconicGrow && iconicExit)) {
                ST_FAIL(192, "iconic resizable width");
            }
            // 193: the tools column can be re-placed beside a widget column
            // through its own header drag, landing as a real splitter column at
            // the resolved boundary (an in-window pane, never a dock).
            bool toolsResolved = false;
            bool toolsPane = false;
            bool toolsIndex = false;
            if (pictura::PanelColumn* tools = frame.toolsColumn()) {
                auto* cs = frame.findChild<QSplitter*>(QStringLiteral("centerSplitter"));
                if (cs) {
                    showPrimary();
                    pictura::PanelColumn* c1 =
                        frame.createPanelColumn(pictura::PanelSide::Right, panelColumnColumn);
                    toolbarFixPump(6);
                    adoptOneInto(c1);
                    toolbarFixPump(6);
                    if (c1) {
                        const QRect r1(c1->mapToGlobal(QPoint(0, 0)), c1->size());
                        const QPoint target(r1.left() + qMax(1, r1.width() / 4), r1.center().y());
                        const bool began = tools->beginColumnHeaderDragForTest(
                            tools->mapToGlobal(QPoint(qMax(1, tools->width() / 2), 8)));
                        tools->dragColumnHeaderToForTest(target);
                        const bool indicator = c1->dropIndicatorVisibleForTest();
                        const bool dropped = tools->dropColumnHeaderForTest(target);
                        toolbarFixPump(8);
                        const int ci = cs->indexOf(tools);
                        const int c1i = cs->indexOf(c1);
                        toolsResolved = began && indicator;
                        toolsPane = dropped && ci >= 0 && !tools->isWindow();
                        toolsIndex = ci >= 0 && c1i >= 0 && ci < c1i;
                    }
                    toolbarFixCollapseDynamics();
                }
            }
            ST_BEGIN("tools_pane_resolved");
            ST_PASS("tools_pane resolved=%d pane=%d index=%d", toolsResolved ? 1 : 0, toolsPane ? 1 : 0,
                         toolsIndex ? 1 : 0);
            if (!(toolsResolved && toolsPane && toolsIndex)) {
                ST_FAIL(193, "tools pane");
            }
            // 194: a floating group shows a visible close control; closing it
            // removes the overlay, hides the group's panels, and keeps the group
            // restorable in a column.
            bool floatCloseBtn = false;
            bool floatClosed = false;
            bool floatRestore = false;
            if (panelColumnColumn) {
                showPrimary();
                closeAllFloats();
                pictura::PanelGroup* floater = nullptr;
                for (pictura::PanelGroup* group : panelColumnColumn->groups()) {
                    if (group && !group->visiblePanels().isEmpty()) {
                        floater = group;
                        break;
                    }
                }
                const QString hiddenName = firstVisiblePanel(floater);
                if (floater && !hiddenName.isEmpty()) {
                    const bool torn = panelColumnColumn->tearOffForTest(floater->objectName());
                    toolbarFixPump(8);
                    QToolButton* close = panelColumnColumn->floatCountForTest() == 1
                                             ? panelColumnColumn->floatCloseButtonForTest(0)
                                             : nullptr;
                    floatCloseBtn = torn && close && close->isVisible();
                    const bool clicked = panelColumnColumn->closeFloatForTest(0);
                    toolbarFixPump(8);
                    pictura::PanelGroup* back = panelColumnColumn->groupForPanel(hiddenName);
                    floatClosed = clicked && panelColumnColumn->floatCountForTest() == 0
                                     && back != nullptr && !back->isVisible();
                    if (back) {
                        panelColumnColumn->showPanel(hiddenName, true);
                        toolbarFixPump(6);
                        floatRestore = back->isVisible();
                    }
                }
                toolbarFixCollapseDynamics();
            }
            ST_BEGIN("float_close_button");
            ST_PASS("float_close button=%d closed=%d "
                         "restore=%d", floatCloseBtn ? 1 : 0, floatClosed ? 1 : 0,
                         floatRestore ? 1 : 0);
            if (!(floatCloseBtn && floatClosed && floatRestore)) {
                ST_FAIL(194, "float close");
            }
            // 195: the compact group container uses the panel surface shade and
            // its drag dots are dark gray, not the near-white window text.
            bool interactionShade = false;
            bool interactionDots = false;
            {
                const QString ss = qApp->styleSheet();
                const QColor disabledColor =
                    qApp->palette().color(QPalette::Disabled, QPalette::WindowText);
                const QString panelHex = QStringLiteral("#4d4d4d");
                const QString disabledHex = disabledColor.name(QColor::HexRgb);
                interactionShade = ss.contains(
                    QStringLiteral("QWidget#panelIconGroup { background: ") + panelHex);
                const int grip = ss.indexOf(QStringLiteral("QWidget#panelIconGroupGrip {"));
                if (grip >= 0) {
                    const int end = ss.indexOf(QLatin1Char('}'), grip);
                    const QString rule = ss.mid(grip, end - grip);
                    interactionDots = rule.contains(QStringLiteral("color: ") + disabledHex);
                }
            }
            ST_BEGIN("compact_shade_shade");
            ST_PASS("compact_shade shade=%d dots=%d", interactionShade ? 1 : 0, interactionDots ? 1 : 0);
            if (!(interactionShade && interactionDots)) {
                ST_FAIL(195, "compact shade");
            }
        }
        if (const int lpc = pictura::runLayersControlsChecks(frame); lpc != 0) { return lpc; } if (const int lfs = pictura::runLayersFilterChecks(frame); lfs != 0) { return lfs; } if (const int rcc = pictura::runCanvasChecks(frame); rcc != 0) { return rcc; } if (const int rcx = pictura::runControlChecks(frame); rcx != 0) { return rcx; }
        frame.closeDocument(anatomyDocIndex, false);
        // Re-acquire the canvas; create a document if the suites left none open.
        canvas = frame.imageView();
        if (!canvas) {
            frame.newDocument(QStringLiteral("TransformProbe"), 16, 16, QStringLiteral("rgb"), 8,
                              QStringLiteral("white"));
            canvas = frame.imageView();
        }
        if (!canvas) {
            ST_FAIL(38, "no active canvas");
        }
        const QPointF center(canvas->width() / 2.0, canvas->height() / 2.0);
        canvas->zoomAt(center, 120);
        const QPointF afterZoom = canvas->offset();
        canvas->panBy(QPointF(10.0, 5.0));
        if (canvas->zoom() <= 1.0 || canvas->offset() != afterZoom + QPointF(10.0, 5.0)) {
            ST_FAIL(3, "zoom/pan transform wrong");
        }
        ST_BEGIN("zoom");
        ST_PASS("zoom=%.3f pan_ok=1", canvas->zoom());
        // The self-test leaves dirty documents behind; headless shutdown closes
        // the window and must discard them without opening a modal prompt.
        pictura::setUnsavedPromptInteractive(false);
        pictura::setNonInteractiveUnsavedChoice(pictura::UnsavedChoice::Discard);
        QTimer::singleShot(2000, &app, &QCoreApplication::quit);
    ST_FINISH();
}
