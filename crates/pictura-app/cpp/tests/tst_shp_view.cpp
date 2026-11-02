// SPDX-License-Identifier: GPL-3.0-or-later
// KookaPictura-SHP fork: the view side of SHP documents (onion skin,
// playback, overlays, readouts, the commands an SHP sheet disables). Every
// sprite and palette here is generated; no game file is used.

#include <QtTest/QtTest>

#include "commands.h"
#include "control_server.h"
#include "control_test_client.h"
#include "frame.h"
#include "image_view.h"
#include "panels/actions_panel.h"
#include "panels/actions_tree_model.h"
#include "panels/shp_frames_panel.h"
#include "panels/shp_palette_panel.h"
#include "panels/shp_playback_panel.h"
#include "shp_palettes.h"
#include "shp_support.h"
#include "shp_undo_focus.h"
#include "tools.h"

#include "pictura_app/src/cxxqt_object.cxxqt.h"
#include "pictura_app/src/cxxqt_object/eyedropper_sample.cxxqt.h"
#include "pictura_app/src/cxxqt_object/shp.cxxqt.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonObject>
#include <QtCore/QTemporaryDir>
#include <QtGui/QAction>
#include <QtGui/QImage>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QMenu>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QStatusBar>

#include "qt_test_support.h"
#include "shp_fixtures.h"

namespace {

constexpr quint64 kPagedLimit = 600;

// The toolbox flyout entry of tool `id`.
QAction* flyoutEntry(QWidget* window, pictura::ToolId id)
{
    for (QMenu* menu : window->findChildren<QMenu*>(QStringLiteral("toolFlyoutMenu"))) {
        for (QAction* action : menu->actions()) {
            if (action->data().isValid() && action->data().toInt() == int(id)) {
                return action;
            }
        }
    }
    return nullptr;
}

// An 8 x 6 image that is not an SHP sheet.
QString plainImage(const QTemporaryDir& dir)
{
    const QString path = dir.filePath(QStringLiteral("plain.png"));
    QImage plain(8, 6, QImage::Format_ARGB32);
    plain.fill(Qt::red);
    return plain.save(path) ? path : QString();
}

// The colour the canvas widget shows for sheet pixel (x, y).
QRgb shown(pictura::ImageView* canvas, int x, int y)
{
    const QPointF at = canvas->imageToWidget(QPointF(x + 0.5, y + 0.5));
    return canvas->grab().toImage().pixel(at.toPoint());
}

} // namespace

class ShpViewTest : public QObject {
    Q_OBJECT

private:
    pictura::PictureView* open();

    pictura::test::ScopedStateHome stateHome_;
    QTemporaryDir dir_;
    QString shp_;
    std::unique_ptr<pictura::PicturaMainWindow> window_;
    pictura::ShpSupport* support_ = nullptr;

private slots:
    void init();
    void cleanup();
    void onionGhostsNeighboursWithoutChangingSamplesOrBytes();
    void onionStopsAtThePagedWindowEdge();
    void playbackStepsThroughTheRange();
    void playbackDecodesFramesOffAPagedSheet();
    void undoAndRedoFocusTheFrameTheyChanged();
    void offPaletteMarksFollowEditsAndSnapFrameFixesOneFrame();
    void sheetCommandsAreDisabledForAnShpOnly();
    void frameSizeResizesEveryFrameAsOneStep();
    void cropToolsAreRefusedForAnShpOnly();
    void actionStepsThatReshapeAnShpStopThePlayback();
    void hoverReadsOutTheIndexUnderTheCursor();
    void realSpritesThroughDuplicateMoveDelete();
    void newShpForOtherReaders();
};

void ShpViewTest::init()
{
    QVERIFY(dir_.isValid());
    shp_ = dir_.filePath(QStringLiteral("gtank.shp"));
    QVERIFY(writeFile(shp_, sprite()));
    QVERIFY(writeFile(dir_.filePath(QStringLiteral("gtank.pal")), palette()));
    window_ = pictura::test::makeMainWindow();
    window_->resize(1200, 900);
    window_->show();
    support_ = pictura::ShpSupport::of(window_.get());
    QVERIFY(support_);
}

void ShpViewTest::cleanup()
{
    pictura::ShpSupport::setGateInteractive(-1);
    pictura::shp_set_test_limits(0, 0);
    window_.reset();
    pictura::forgetFolderPalette(shp_);
}

pictura::PictureView* ShpViewTest::open()
{
    if (!window_->openDocumentAtPath(shp_)) {
        return nullptr;
    }
    window_->refresh();
    return window_->viewAt(0);
}

void ShpViewTest::onionGhostsNeighboursWithoutChangingSamplesOrBytes()
{
    pictura::PictureView* view = open();
    QVERIFY(view);
    pictura::ShpFramesPanel* frames = support_->frames();
    QTRY_COMPARE(frames->frameCount(), kFrames);
    frames->setSelectsCell(false);
    // A blank frame between two full ones: every pixel of it is transparent.
    QVERIFY(support_->frameOp(QStringLiteral("insert"), 5, 1));
    QTRY_COMPARE(frames->frameCount(), kFrames + 1);
    frames->focusFrame(5);
    pictura::ImageView* canvas = window_->canvasAt(0);
    QVERIFY(canvas);
    const QRect cell = frames->cellRect(5);
    const QPoint p = cell.topLeft() + QPoint(2, 2);
    QTRY_COMPARE(canvas->shpGrid().active, 5);

    const QRgb plain = shown(canvas, p.x(), p.y());
    const quint32 layer = pictura::sample_argb_scoped(*view, p.x(), p.y(), 1, 0);
    const quint32 all = pictura::sample_argb_scoped(*view, p.x(), p.y(), 3, 2);
    const quint32 composite = view->composite_argb(p.x(), p.y());
    QCOMPARE(layer >> 24, 0u);
    const QString offPath = dir_.filePath(QStringLiteral("off.shp"));
    QVERIFY(window_->saveActiveAs(offPath));

    QCheckBox* onion = frames->onion();
    QVERIFY(onion && onion->isEnabled());
    onion->setChecked(true);
    QVERIFY(canvas->shpGrid().onion);
    frames->onionOpacity()->setValue(60);
    QCOMPARE(canvas->shpGrid().onionOpacity, 60);
    // The ghost shows on screen ...
    QTRY_VERIFY(shown(canvas, p.x(), p.y()) != plain);
    // ... and nowhere else: not in the samples, the composite or the file.
    QCOMPARE(pictura::sample_argb_scoped(*view, p.x(), p.y(), 1, 0), layer);
    QCOMPARE(pictura::sample_argb_scoped(*view, p.x(), p.y(), 3, 2), all);
    QCOMPARE(view->composite_argb(p.x(), p.y()), composite);
    const QString onPath = dir_.filePath(QStringLiteral("on.shp"));
    QVERIFY(window_->saveActiveAs(onPath));
    QCOMPARE(readFile(onPath), readFile(offPath));

    // An opaque pixel of the current frame is never covered by the ghost.
    frames->focusFrame(4);
    const QPoint q = frames->cellRect(4).topLeft() + QPoint(2, 2);
    onion->setChecked(false);
    const QRgb solid = shown(canvas, q.x(), q.y());
    onion->setChecked(true);
    QCOMPARE(shown(canvas, q.x(), q.y()), solid);
}

void ShpViewTest::onionStopsAtThePagedWindowEdge()
{
    pictura::shp_set_test_limits(kPagedLimit, 0);
    pictura::PictureView* view = open();
    QVERIFY(view);
    pictura::ShpFramesPanel* frames = support_->frames();
    QTRY_COMPARE(frames->frameCount(), kFrames);
    frames->setSelectsCell(false);
    QCOMPARE(pictura::shp_info(*view).frames, 4);
    // Two blank frames at 4 and 5; the window is 4-7, so frame 4's "before"
    // (frame 3, full) is off the sheet and its "after" is blank.
    QVERIFY(support_->frameOp(QStringLiteral("insert"), 4, 2));
    QTRY_COMPARE(frames->frameCount(), kFrames + 2);
    QCOMPARE(pictura::shp_info(*view).first, 4);
    pictura::ImageView* canvas = window_->canvasAt(0);
    frames->onion()->setChecked(true);

    frames->focusFrame(4);
    QTRY_COMPARE(canvas->shpGrid().active, 0);
    const QPoint p4 = frames->cellRect(4).topLeft() + QPoint(2, 2);
    const QRgb with4 = shown(canvas, p4.x(), p4.y());
    frames->onion()->setChecked(false);
    QCOMPARE(shown(canvas, p4.x(), p4.y()), with4);

    // Frame 5's "after" (frame 6) is on the sheet: that one is ghosted.
    frames->focusFrame(5);
    const QPoint p5 = frames->cellRect(5).topLeft() + QPoint(2, 2);
    const QRgb plain5 = shown(canvas, p5.x(), p5.y());
    frames->onion()->setChecked(true);
    QTRY_VERIFY(shown(canvas, p5.x(), p5.y()) != plain5);
}

void ShpViewTest::playbackStepsThroughTheRange()
{
    pictura::PictureView* view = open();
    QVERIFY(view);
    QTRY_COMPARE(support_->frames()->frameCount(), kFrames);
    pictura::ShpPlaybackPanel* play = support_->playback();
    QVERIFY(play);
    QCOMPARE(play->to(), kFrames - 1);
    support_->frames()->focusFrame(7);
    const int history = view->history_index();

    // The preview shows the live sheet pixels of the frame.
    const QRect c3 = support_->frames()->cellRect(3);
    const QImage f3 = play->frameImage(3);
    QCOMPARE(f3.size(), QSize(kW, kH));
    QCOMPARE(f3.pixel(1, 0), QRgb(view->composite_argb(c3.x() + 1, c3.y())));
    QCOMPARE(qAlpha(f3.pixel(0, 0)), 0);

    play->setRange(2, 4);
    play->setLoop(true);
    play->setFrame(2);
    QList<int> seen;
    for (int i = 0; i < 4; ++i) {
        play->advance();
        seen << play->frame();
    }
    QCOMPARE(seen, QList<int>({3, 4, 2, 3}));

    // Once through: it stops on the last frame, and Play starts it again.
    play->setLoop(false);
    play->setFrame(2);
    play->setPlaying(true);
    QVERIFY(play->playing());
    play->setFps(10);
    QCOMPARE(play->intervalMs(), 100);
    play->advance();
    play->advance();
    QCOMPARE(play->frame(), 4);
    QVERIFY(!play->playing());
    play->setPlaying(true);
    QCOMPARE(play->frame(), 2);

    // The timer drives it; the editor's frame and History stay put.
    play->setLoop(true);
    play->setFps(60);
    QTRY_VERIFY(play->frame() != 2);
    play->setPlaying(false);
    QCOMPARE(support_->frames()->current(), 7);
    QCOMPARE(view->history_index(), history);
}

void ShpViewTest::playbackDecodesFramesOffAPagedSheet()
{
    pictura::shp_set_test_limits(kPagedLimit, 0);
    pictura::PictureView* view = open();
    QVERIFY(view);
    QTRY_COMPARE(support_->frames()->frameCount(), kFrames);
    pictura::ShpPlaybackPanel* play = support_->playback();
    QCOMPARE(pictura::shp_info(*view).frames, 4);
    play->setLoop(true);
    play->setFrame(0);
    for (int i = 0; i < kFrames; ++i) {
        play->advance();
    }
    QCOMPARE(play->frame(), 0);
    // Playing never pages: the window is where it was.
    QCOMPARE(pictura::shp_info(*view).first, 0);
    // The six frames off the sheet were decoded once, into the cache.
    QCOMPARE(play->cachedBytes(), qint64(6 * kW * kH * 4));
    QVERIFY(play->cachedBytes() <= pictura::ShpPlaybackPanel::kCacheBytes);

    const QImage f9 = play->frameImage(9);
    QCOMPARE(f9.size(), QSize(kW, kH));
    QCOMPARE(qAlpha(f9.pixel(0, 0)), 0);
    QVERIFY(f9.pixel(1, 0) != play->frameImage(8).pixel(1, 0));
    // The decoded frame is what the sheet shows once its window is loaded.
    support_->frames()->focusFrame(9);
    QCOMPARE(pictura::shp_info(*view).first, 8);
    const QRect c9 = support_->frames()->cellRect(9);
    QCOMPARE(f9.pixel(1, 0), QRgb(view->composite_argb(c9.x() + 1, c9.y())));
    QCOMPARE(play->frameImage(9).pixel(1, 0), f9.pixel(1, 0));
}

void ShpViewTest::undoAndRedoFocusTheFrameTheyChanged()
{
    pictura::PictureView* view = open();
    QVERIFY(view);
    pictura::ShpFramesPanel* frames = support_->frames();
    QTRY_COMPARE(frames->frameCount(), kFrames);
    frames->setSelectsCell(false);
    const QRect c6 = frames->cellRect(6);
    const quint32 other = view->composite_argb(frames->cellRect(2).x() + 1, 0);
    QVERIFY(dab(view, other, c6.x() + 2, c6.y() + 2));
    // The fingerprints follow an edit from the event loop after it.
    QCoreApplication::processEvents();

    frames->focusFrame(1);
    QVERIFY(view->undo());
    QCOMPARE(support_->undoFocus()->lastChanged(), QVector<int>({6}));
    QCOMPARE(frames->current(), 6);
    QCOMPARE(window_->canvasAt(0)->shpGrid().active, 6);
    // Focusing never touched the selection or History: Redo is still there.
    QVERIFY(view->can_redo());
    frames->focusFrame(0);
    QVERIFY(view->redo());
    QCOMPARE(frames->current(), 6);
    QCoreApplication::processEvents();

    // A frame operation: undoing a delete focuses the frame it brings back.
    QVERIFY(support_->frameOp(QStringLiteral("delete"), 3, 1));
    QCoreApplication::processEvents();
    frames->focusFrame(0);
    QVERIFY(view->undo());
    QCOMPARE(pictura::shp_info(*view).frames, kFrames);
    QCOMPARE(frames->current(), 3);
    // Deleting the last frame and undoing it focuses that frame.
    QVERIFY(support_->frameOp(QStringLiteral("delete"), kFrames - 1, 1));
    QCoreApplication::processEvents();
    frames->focusFrame(0);
    QVERIFY(view->undo());
    QCOMPARE(frames->current(), kFrames - 1);
    // A selection-only step changes no frame and moves nothing.
    view->select_rect(0, 0, 2, 2, QStringLiteral("new"), 0.0);
    QCoreApplication::processEvents();
    frames->focusFrame(4);
    QVERIFY(view->undo());
    QVERIFY(support_->undoFocus()->lastChanged().isEmpty());
    QCOMPARE(frames->current(), 4);
}

void ShpViewTest::offPaletteMarksFollowEditsAndSnapFrameFixesOneFrame()
{
    pictura::PictureView* view = open();
    QVERIFY(view);
    pictura::ShpFramesPanel* frames = support_->frames();
    QTRY_COMPARE(frames->frameCount(), kFrames);
    frames->setSelectsCell(false);
    const quint32 off = 0xFF0BADF0u;
    QCOMPARE(pictura::shp_index_of(*view, off & 0xFFFFFFu), -1);
    const QPoint p3 = frames->cellRect(3).topLeft() + QPoint(2, 2);
    const QPoint p5 = frames->cellRect(5).topLeft() + QPoint(1, 1);
    QVERIFY(dab(view, off, p3.x(), p3.y()));
    QVERIFY(dab(view, off, p5.x(), p5.y()));
    window_->refresh();
    frames->focusFrame(3);
    pictura::ImageView* canvas = window_->canvasAt(0);
    // Below 4x a mark fills its pixel.
    canvas->setZoom(2.0, QPointF(canvas->width() / 2.0, canvas->height() / 2.0));
    canvas->centerOnImageRect(QRectF(frames->cellRect(3)));
    const QRgb plain = shown(canvas, p3.x(), p3.y());
    const quint32 sampled = pictura::sample_argb_scoped(*view, p3.x(), p3.y(), 1, 0);

    QCheckBox* marks = frames->offPaletteMarks();
    QVERIFY(marks && marks->isEnabled());
    marks->setChecked(true);
    QCOMPARE(canvas->shpGrid().offPalette, QVector<QPoint>({p3, p5}));
    QTRY_VERIFY(shown(canvas, p3.x(), p3.y()) != plain);
    QCOMPARE(pictura::sample_argb_scoped(*view, p3.x(), p3.y(), 1, 0), sampled);

    // Live: a new off-palette pixel is marked after the edit.
    const QPoint q3 = p3 + QPoint(1, 1);
    QVERIFY(dab(view, off, q3.x(), q3.y()));
    QTRY_COMPARE(canvas->shpGrid().offPalette.size(), 3);

    // Snap Frame fixes frame 3 only, as one undoable step.
    frames->focusFrame(3);
    frames->snapFrameAction()->trigger();
    QCOMPARE(view->history_label(view->history_index()), QStringLiteral("Snap Frame to Palette"));
    QCOMPARE(pictura::shp_check(*view).off_frames.size(), size_t(1));
    QCOMPARE(pictura::shp_check(*view).off_frames[0], 5);
    QTRY_COMPARE(canvas->shpGrid().offPalette, QVector<QPoint>({p5}));
    QVERIFY(view->undo());
    QCOMPARE(pictura::shp_check(*view).off_frames.size(), size_t(2));
    marks->setChecked(false);
    QVERIFY(canvas->shpGrid().offPalette.isEmpty());
}

void ShpViewTest::sheetCommandsAreDisabledForAnShpOnly()
{
    pictura::PictureView* view = open();
    QVERIFY(view);
    QTRY_COMPARE(support_->frames()->frameCount(), kFrames);
    pictura::CommandRegistry* registry = window_->registry();
    const QStringList ids = {
        QStringLiteral("image.image.size"),   QStringLiteral("image.canvas.size"),
        QStringLiteral("image.crop"),         QStringLiteral("image.trim"),
        QStringLiteral("image.rotate90cw"),
        QStringLiteral("image.rotate180"),    QStringLiteral("image.flipHorizontal"),
        QStringLiteral("image.mode.grayscale"), QStringLiteral("image.mode.16.bits"),
    };
    for (const QString& id : ids) {
        QAction* action = registry->action(id);
        QVERIFY2(action, qPrintable(id));
        QVERIFY2(!action->isEnabled(), qPrintable(id));
        QVERIFY2(pictura::ShpSupport::blocks(window_.get(), id), qPrintable(id));
    }
    QVERIFY(pictura::ShpSupport::blocks(window_.get(), QStringLiteral("image.revealAll")));
    QVERIFY(!registry->dispatch(QStringLiteral("image.rotate180")));
    QCOMPARE(view->document_width(), 4 * kW + 3);
    QVERIFY(!pictura::ShpSupport::blocks(window_.get(), QStringLiteral("edit.fill")));
    QVERIFY(support_->frameSizeAction()->isEnabled());

    // The control server refuses them too (its replay skips the registry's gate).
    pictura::ControlServer server(window_.get(), dir_.filePath(QStringLiteral("c.sock")));
    const QJsonObject resize{{QStringLiteral("id"), QStringLiteral("image.image.size")},
                             {QStringLiteral("params"),
                              QJsonObject{{QStringLiteral("width"), 20},
                                          {QStringLiteral("height"), 10},
                                          {QStringLiteral("constrainProportions"), false}}}};
    QCOMPARE(pictura::test::errorCode(server.dispatch(QStringLiteral("run_command"), resize)),
             QStringLiteral("refused"));
    QCOMPARE(view->document_width(), 4 * kW + 3);

    // Another document that is not an SHP gets them back.
    const QString png = dir_.filePath(QStringLiteral("plain.png"));
    QImage plain(8, 8, QImage::Format_ARGB32);
    plain.fill(Qt::red);
    QVERIFY(plain.save(png));
    QVERIFY(window_->openDocumentAtPath(png));
    window_->refresh();
    QTRY_VERIFY(registry->action(QStringLiteral("image.image.size"))->isEnabled());
    QVERIFY(registry->action(QStringLiteral("image.mode.grayscale"))->isEnabled());
    QVERIFY(!support_->frameSizeAction()->isEnabled());
    QVERIFY(pictura::test::succeeded(server.dispatch(QStringLiteral("run_command"), resize)));
}

void ShpViewTest::frameSizeResizesEveryFrameAsOneStep()
{
    pictura::PictureView* view = open();
    QVERIFY(view);
    pictura::ShpFramesPanel* frames = support_->frames();
    QTRY_COMPARE(frames->frameCount(), kFrames);
    frames->setSelectsCell(false);
    const QRect old3 = frames->cellRect(3);
    const quint32 colour = view->composite_argb(old3.x() + 1, old3.y() + 1);
    const int history = view->history_index();

    // Two columns wider, centred: frame pixels move one column right.
    QVERIFY(support_->frameSize(kW + 2, kH, 4));
    QCOMPARE(pictura::shp_info(*view).width, kW + 2);
    QCOMPARE(view->history_index(), history + 1);
    QCOMPARE(view->history_label(view->history_index()), QStringLiteral("Frame Size"));
    QTRY_COMPARE(frames->cellRect(3).width(), kW + 2);
    const QRect new3 = frames->cellRect(3);
    QCOMPARE(view->composite_argb(new3.x() + 2, new3.y() + 1), colour);
    QCOMPARE(view->composite_argb(new3.x(), new3.y() + 1) >> 24, 0u);
    const QString out = dir_.filePath(QStringLiteral("wide.shp"));
    QVERIFY(window_->saveActiveAs(out));
    const QByteArray bytes = readFile(out);
    QCOMPARE(int(quint8(bytes[2])) | (int(quint8(bytes[3])) << 8), kW + 2);

    QVERIFY(view->undo());
    QCOMPARE(pictura::shp_info(*view).width, kW);
    QCOMPARE(view->composite_argb(old3.x() + 1, old3.y() + 1), colour);
    // Out of range is refused with a reason.
    QVERIFY(!support_->frameSize(0, kH, 4));
    QVERIFY(!support_->lastGateMessage().isEmpty());
}

void ShpViewTest::hoverReadsOutTheIndexUnderTheCursor()
{
    pictura::PictureView* view = open();
    QVERIFY(view);
    pictura::ShpFramesPanel* frames = support_->frames();
    QTRY_COMPARE(frames->frameCount(), kFrames);
    pictura::ShpPalettePanel* palette = support_->palette();
    pictura::ImageView* canvas = window_->canvasAt(0);
    const QPoint c5 = frames->cellRect(5).topLeft();
    // Through the canvas's own hover signal.
    emit canvas->mouseMoved(QPointF(c5.x() + 1.5, c5.y() + 0.5));
    QVERIFY2(palette->canvasHoverForTest().startsWith(QStringLiteral("Frame 5 (1, 0): index 45")),
             qPrintable(palette->canvasHoverForTest()));
    palette->showCanvasHover(QPointF(c5.x() + 0.2, c5.y() + 0.7));
    QVERIFY(palette->canvasHoverForTest().contains(QStringLiteral("index 0")));
    QVERIFY(palette->canvasHoverForTest().contains(QStringLiteral("transparent"),
                                                   Qt::CaseInsensitive)
            || palette->canvasHoverForTest().endsWith(QStringLiteral("index 0")));
    palette->showCanvasHover(QPointF(kW + 0.5, 1.5));
    QVERIFY(palette->canvasHoverForTest().contains(QStringLiteral("gutter")));
    palette->showCanvasHover(QPointF(-3, 2));
    QVERIFY(palette->canvasHoverForTest().isEmpty());

    QVERIFY(dab(view, 0xFF0BADF0u, c5.x() + 2, c5.y() + 2));
    palette->showCanvasHover(QPointF(c5.x() + 2.5, c5.y() + 2.5));
    QVERIFY2(palette->canvasHoverForTest().contains(QStringLiteral("off-palette #0BADF0")),
             qPrintable(palette->canvasHoverForTest()));
}

// End to end on real sprites, skipped unless SHP_E2E_DIR names a directory
// of scratch copies (never the game files) with a palette `e2e.pal`. Each
// `NAME.shp` there gets: duplicate frame 2, move the copy to the end, delete
// frame 0, save as `NAME.ops`. The expected frames are the original's
// 1..N-1 then frame 2; an independent decoder checks that outside this test.
void ShpViewTest::realSpritesThroughDuplicateMoveDelete()
{
    const QString dir = qEnvironmentVariable("SHP_E2E_DIR");
    if (dir.isEmpty()) {
        QSKIP("needs SHP_E2E_DIR (scratch copies of real SHPs and e2e.pal)");
    }
    const QStringList names = QDir(dir).entryList({QStringLiteral("*.shp")}, QDir::Files);
    QVERIFY(!names.isEmpty());
    for (const QString& name : names) {
        const QString path = QDir(dir).filePath(name);
        const QString stem = QFileInfo(path).completeBaseName();
        QFile::remove(QDir(dir).filePath(stem + QStringLiteral(".pal")));
        QVERIFY(QFile::copy(QDir(dir).filePath(QStringLiteral("e2e.pal")),
                            QDir(dir).filePath(stem + QStringLiteral(".pal"))));
        QVERIFY(window_->openDocumentAtPath(path));
        window_->refresh();
        pictura::PictureView* view = window_->activeView();
        QVERIFY(view);
        const pictura::ShpInfo info = pictura::shp_info(*view);
        QVERIFY2(info.valid && info.frames == info.total, qPrintable(name));
        const int n = info.total;
        QVERIFY(support_->frameOp(QStringLiteral("duplicate"), 2, 1));
        QVERIFY(support_->frameOp(QStringLiteral("move"), 3, 1, n));
        QVERIFY(support_->frameOp(QStringLiteral("delete"), 0, 1));
        QCOMPARE(pictura::shp_info(*view).total, n);
        const QString out = QDir(dir).filePath(stem + QStringLiteral(".ops"));
        QFile::remove(out);
        // Not .shp-suffixed on disk until saved: save_as writes the SHP.
        const QString saved = out + QStringLiteral(".shp");
        QFile::remove(saved);
        QVERIFY2(window_->saveActiveAs(saved), qPrintable(support_->lastGateMessage()));
        QVERIFY(window_->closeDocument(window_->activeDocumentIndex(), true));
    }
}

// File > New SHP for other readers, skipped unless SHP_NEW_OUT names the
// file to write (and SHP_NEW_PAL a palette): 3 frames of 9 x 7, frame k
// painted with index 40 + k on a diagonal and index 16 + k in a corner,
// saved; pictura-shp and Ochre's reader are compared on it outside.
void ShpViewTest::newShpForOtherReaders()
{
    const QString out = qEnvironmentVariable("SHP_NEW_OUT");
    if (out.isEmpty()) {
        QSKIP("needs SHP_NEW_OUT (and SHP_NEW_PAL)");
    }
    QVERIFY(support_->newShp(9, 7, 3, qEnvironmentVariable("SHP_NEW_PAL")));
    pictura::PictureView* view = window_->activeView();
    QVERIFY(view);
    window_->refresh();
    QTRY_COMPARE(support_->frames()->frameCount(), 3);
    const rust::Vec<uint32_t> unique = pictura::shp_palette_unique(*view);
    QCOMPARE(int(unique.size()), 256);
    for (int k = 0; k < 3; ++k) {
        const QPoint o = support_->frames()->cellRect(k).topLeft();
        for (int d = 0; d < 5; ++d) {
            QVERIFY(dab(view, 0xFF000000u | unique[40 + k], o.x() + d + k, o.y() + d));
        }
        QVERIFY(dab(view, 0xFF000000u | unique[16 + k], o.x() + 8, o.y() + 6));
    }
    QFile::remove(out);
    QVERIFY2(window_->saveActiveAs(out), qPrintable(support_->lastGateMessage()));
}

void ShpViewTest::cropToolsAreRefusedForAnShpOnly()
{
    pictura::PictureView* view = open();
    QVERIFY(view);
    QTRY_COMPARE(support_->frames()->frameCount(), kFrames);
    const pictura::ToolId before = window_->activeTool();
    QVERIFY(!pictura::ShpSupport::isSheetTool(before));
    const QString reason = pictura::ShpSupport::cropToolReason();

    // Choosing either Crop tool keeps the current tool and says why.
    window_->statusBar()->clearMessage();
    window_->setActiveTool(pictura::ToolId::Crop);
    QCOMPARE(window_->activeTool(), before);
    QCOMPARE(window_->statusBar()->currentMessage(), reason);
    window_->setActiveTool(pictura::ToolId::PerspectiveCrop);
    QCOMPARE(window_->activeTool(), before);
    // Their toolbox entries are disabled with the reason; Slice is not.
    QAction* crop = flyoutEntry(window_.get(), pictura::ToolId::Crop);
    QAction* perspective = flyoutEntry(window_.get(), pictura::ToolId::PerspectiveCrop);
    QAction* slice = flyoutEntry(window_.get(), pictura::ToolId::Slice);
    QVERIFY(crop && perspective && slice);
    QVERIFY(!crop->isEnabled());
    QVERIFY(!perspective->isEnabled());
    QCOMPARE(crop->toolTip(), reason);
    QVERIFY(slice->isEnabled());
    // The control server's set_tool goes through the same gate.
    pictura::ControlServer server(window_.get(), dir_.filePath(QStringLiteral("t.sock")));
    server.dispatch(QStringLiteral("set_tool"),
                    QJsonObject{{QStringLiteral("tool"),
                                 pictura::toolIdName(pictura::ToolId::Crop)}});
    QCOMPARE(window_->activeTool(), before);

    // A plain image gets them back ...
    const QString png = plainImage(dir_);
    QVERIFY(!png.isEmpty());
    QVERIFY(window_->openDocumentAtPath(png));
    window_->refresh();
    QTRY_VERIFY(crop->isEnabled());
    QVERIFY(perspective->isEnabled());
    QVERIFY(crop->toolTip() != reason);
    window_->setActiveTool(pictura::ToolId::Crop);
    QCOMPARE(window_->activeTool(), pictura::ToolId::Crop);

    // ... and going back to the SHP sheet puts the active one down.
    window_->setActiveDocumentIndex(0);
    window_->refresh();
    QTRY_COMPARE(window_->activeView(), view);
    QTRY_VERIFY(!pictura::ShpSupport::isSheetTool(window_->activeTool()));
    QVERIFY(!crop->isEnabled());
    QCOMPARE(view->document_width(), 4 * kW + 3);
    QVERIFY(window_->saveActiveAs(dir_.filePath(QStringLiteral("kept.shp"))));
}

void ShpViewTest::actionStepsThatReshapeAnShpStopThePlayback()
{
    auto* actions = window_->findChild<pictura::ActionsPanel*>(QStringLiteral("actionsPanel"));
    QVERIFY(actions);
    actions->setPersistEnabledForTest(false);
    pictura::ActionsEngineApi* engine = actions->engineForTest();
    QVERIFY(engine);
    const QString set = engine->createSet(QStringLiteral("SHP gate"));
    QVERIFY(!set.isEmpty());
    // A recorded command step and an inserted menu-item step.
    const QString turn = engine->createAction(set, QStringLiteral("Turn"), 0, false, false, 0);
    QVERIFY(!turn.isEmpty());
    QVERIFY(engine->startRecording(turn, -1));
    engine->recordCommand(QStringLiteral("image.rotate90cw"), QJsonObject());
    engine->stopRecording();
    const QString trim = engine->createAction(set, QStringLiteral("Trim"), 0, false, false, 0);
    QVERIFY(!trim.isEmpty());
    QVERIFY(engine->createMenuItemStep(trim, QStringLiteral("image.trim")));
    actions->refresh();

    pictura::PictureView* view = open();
    QVERIFY(view);
    QTRY_COMPARE(support_->frames()->frameCount(), kFrames);
    const int history = view->history_index();
    const QString reason = pictura::ShpSupport::blockedReason();

    // On an SHP sheet the step is not run: playback stops with the reason.
    QVERIFY(actions->playActionForTest(turn));
    QTRY_VERIFY(!actions->isPlaying());
    QCOMPARE(view->document_width(), 4 * kW + 3);
    QCOMPARE(view->history_index(), history);
    QVERIFY2(actions->lastErrorForTest().contains(reason), qPrintable(actions->lastErrorForTest()));
    QVERIFY(actions->lastErrorForTest().contains(QStringLiteral("image.rotate90cw")));
    QVERIFY(window_->statusBar()->currentMessage().contains(reason));
    QVERIFY(actions->playActionForTest(trim));
    QTRY_VERIFY(!actions->isPlaying());
    QVERIFY(actions->lastErrorForTest().contains(QStringLiteral("image.trim")));
    QVERIFY(actions->lastErrorForTest().contains(reason));
    QCOMPARE(view->history_index(), history);

    // The same recorded step runs on a plain image.
    const QString png = plainImage(dir_);
    QVERIFY(!png.isEmpty());
    QVERIFY(window_->openDocumentAtPath(png));
    window_->refresh();
    pictura::PictureView* plain = window_->activeView();
    QVERIFY(plain && plain != view);
    QVERIFY(actions->playActionForTest(turn));
    QTRY_VERIFY(!actions->isPlaying());
    QVERIFY2(actions->lastErrorForTest().isEmpty(), qPrintable(actions->lastErrorForTest()));
    QCOMPARE(plain->document_width(), 6);
    QCOMPARE(plain->document_height(), 8);
}

QTEST_MAIN(ShpViewTest)
#include "tst_shp_view.moc"
