// SPDX-License-Identifier: GPL-3.0-or-later
// KookaPictura-SHP fork: the view side of SHP documents (onion skin,
// playback, overlays, readouts, the commands an SHP sheet disables). Every
// sprite and palette here is generated; no game file is used.

#include <QtTest/QtTest>

#include "frame.h"
#include "image_view.h"
#include "panels/shp_frames_panel.h"
#include "panels/shp_playback_panel.h"
#include "shp_palettes.h"
#include "shp_support.h"
#include "shp_undo_focus.h"

#include "pictura_app/src/cxxqt_object.cxxqt.h"
#include "pictura_app/src/cxxqt_object/eyedropper_sample.cxxqt.h"
#include "pictura_app/src/cxxqt_object/shp.cxxqt.h"

#include <QtCore/QTemporaryDir>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QSpinBox>

#include "qt_test_support.h"
#include "shp_fixtures.h"

namespace {

constexpr quint64 kPagedLimit = 600;

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

QTEST_MAIN(ShpViewTest)
#include "tst_shp_view.moc"
