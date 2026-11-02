// SPDX-License-Identifier: GPL-3.0-or-later
// KookaPictura-SHP fork: the unit tools of SHP documents (the shadow half).
// Every sprite and palette here is generated; no game file is used.

#include <QtTest/QtTest>

#include "frame.h"
#include "image_view.h"
#include "panels/shp_frames_panel.h"
#include "panels/shp_unit_panel.h"
#include "shp_palettes.h"
#include "shp_support.h"

#include "pictura_app/src/cxxqt_object.cxxqt.h"
#include "pictura_app/src/cxxqt_object/eyedropper_sample.cxxqt.h"
#include "pictura_app/src/cxxqt_object/shp.cxxqt.h"

#include <QtCore/QTemporaryDir>
#include <QtGui/QAction>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QStatusBar>

#include "qt_test_support.h"
#include "shp_fixtures.h"

namespace {

constexpr int kArt = 4;

// The art of frame k (k < kArt): index 40 + k in a 3 x 3 block standing on
// row 3 of the 6 x 5 canvas; its shadow (frame kArt + k) is the same block
// as index 1.
bool inBlock(int x, int y)
{
    return x >= 1 && x <= 3 && y >= 1 && y <= 3;
}

QByteArray unitSprite()
{
    QByteArray out;
    const int frames = 2 * kArt;
    put16(out, 0);
    put16(out, kW);
    put16(out, kH);
    put16(out, frames);
    const int base = 8 + 24 * frames;
    for (int k = 0; k < frames; ++k) {
        put16(out, 0);
        put16(out, 0);
        put16(out, kW);
        put16(out, kH);
        put32(out, 0); // raw
        put32(out, 0);
        put32(out, 0);
        put32(out, quint32(base + k * kW * kH));
    }
    for (int k = 0; k < frames; ++k) {
        for (int y = 0; y < kH; ++y) {
            for (int x = 0; x < kW; ++x) {
                out.append(char(!inBlock(x, y) ? 0 : k < kArt ? 40 + k : 1));
            }
        }
    }
    return out;
}

// Click the button named `button` in the gate when it appears.
class GateClicker {
public:
    explicit GateClicker(const QString& button)
    {
        QObject::connect(&poll_, &QTimer::timeout, [this, button] {
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                auto* box = qobject_cast<QMessageBox*>(widget);
                if (!box || !box->isVisible()
                    || box->objectName() != QLatin1String("shpGate")) {
                    continue;
                }
                seen_ = true;
                if (auto* target = box->findChild<QPushButton*>(button)) {
                    target->click();
                }
            }
        });
        poll_.start(20);
    }
    bool seen() const { return seen_; }

private:
    QTimer poll_;
    bool seen_ = false;
};

// The colour the canvas widget shows for sheet pixel (x, y).
QRgb shown(pictura::ImageView* canvas, int x, int y)
{
    const QPointF at = canvas->imageToWidget(QPointF(x + 0.5, y + 0.5));
    return canvas->grab().toImage().pixel(at.toPoint());
}

} // namespace

class ShpUnitTest : public QObject {
    Q_OBJECT

private:
    pictura::PictureView* open(const QString& path);
    // Sheet pixel (x, y) of file frame `frame`'s cell, as the composite has it.
    quint32 pixel(pictura::PictureView* view, int frame, int x, int y);
    quint32 shadowColor(pictura::PictureView* view);

    pictura::test::ScopedStateHome stateHome_;
    QTemporaryDir dir_;
    QString unit_;
    QString plain_;
    std::unique_ptr<pictura::PicturaMainWindow> window_;
    pictura::ShpSupport* support_ = nullptr;

private slots:
    void init();
    void cleanup();
    void theShadowHalfIsFoundAndMarked();
    void generateAndClearAreUndoableSteps();
    void generateAddsAShadowHalfWithinTheSizeLimit();
    void underlayShowsShadowsWithoutChangingSamplesOrBytes();
    void saveWarnsAboutColoredShadowFramesButSaves();
    void aPagedSpriteGeneratesShadowsOffTheSheet();
};

void ShpUnitTest::init()
{
    QVERIFY(dir_.isValid());
    unit_ = dir_.filePath(QStringLiteral("gtnk.shp"));
    plain_ = dir_.filePath(QStringLiteral("gplain.shp"));
    QVERIFY(writeFile(unit_, unitSprite()));
    QVERIFY(writeFile(plain_, sprite()));
    QVERIFY(writeFile(dir_.filePath(QStringLiteral("gtnk.pal")), palette()));
    QVERIFY(writeFile(dir_.filePath(QStringLiteral("gplain.pal")), palette()));
    window_ = pictura::test::makeMainWindow();
    window_->resize(1200, 900);
    window_->show();
    support_ = pictura::ShpSupport::of(window_.get());
    QVERIFY(support_);
}

void ShpUnitTest::cleanup()
{
    pictura::ShpSupport::setGateInteractive(-1);
    pictura::shp_set_test_limits(0, 0);
    window_.reset();
    pictura::forgetFolderPalette(unit_);
}

pictura::PictureView* ShpUnitTest::open(const QString& path)
{
    const int before = window_->documentCount();
    if (!window_->openDocumentAtPath(path)) {
        return nullptr;
    }
    window_->refresh();
    return window_->documentCount() > before ? window_->viewAt(before) : nullptr;
}

quint32 ShpUnitTest::pixel(pictura::PictureView* view, int frame, int x, int y)
{
    const QRect cell = support_->frames()->cellRect(frame);
    return cell.isNull() ? 0xDEADBEEFu : view->composite_argb(cell.x() + x, cell.y() + y);
}

quint32 ShpUnitTest::shadowColor(pictura::PictureView* view)
{
    return pictura::shp_palette_unique(*view)[1] | 0xFF000000u;
}

void ShpUnitTest::theShadowHalfIsFoundAndMarked()
{
    pictura::PictureView* view = open(unit_);
    QVERIFY(view);
    pictura::ShpUnitPanel* unit = support_->unit();
    QVERIFY(unit);
    QTRY_COMPARE(support_->frames()->frameCount(), 2 * kArt);
    QVERIFY(unit->shadowToggle()->isChecked());
    QCOMPARE(unit->shadowHalf(), kArt);
    QVERIFY(unit->shadowReadout()->text().contains(QStringLiteral("Shadows: frames 4-7")));
    QListWidget* list = support_->frames()->list();
    QCOMPARE(list->item(5)->text(), QStringLiteral("5 S"));
    QVERIFY(list->item(5)->toolTip().contains(QStringLiteral("shadow of frame 1")));
    QCOMPARE(list->item(1)->text(), QStringLiteral("1"));
    support_->frames()->focusFrame(6);
    QVERIFY(window_->findChild<QLabel*>(QStringLiteral("shpFrameLabel"))
                ->text()
                .contains(QStringLiteral("shadow of 2")));

    // Turning it off is the user's call and unmarks the frames; per document.
    unit->shadowToggle()->setChecked(false);
    QCOMPARE(unit->shadowHalf(), 0);
    QCOMPARE(list->item(5)->text(), QStringLiteral("5"));
    QVERIFY(!unit->action(QStringLiteral("clearAll"))->isEnabled());
    unit->shadowToggle()->setChecked(true);

    // A sprite of art only has no shadow half.
    pictura::PictureView* plain = open(plain_);
    QVERIFY(plain);
    QTRY_COMPARE(support_->frames()->frameCount(), kFrames);
    QVERIFY(!unit->shadowToggle()->isChecked());
    QCOMPARE(list->item(5)->text(), QStringLiteral("5"));
}

void ShpUnitTest::generateAndClearAreUndoableSteps()
{
    pictura::PictureView* view = open(unit_);
    QVERIFY(view);
    pictura::ShpUnitPanel* unit = support_->unit();
    QTRY_COMPARE(unit->shadowHalf(), kArt);
    pictura::ShpFramesPanel* frames = support_->frames();
    frames->setSelectsCell(false);
    const quint32 shade = shadowColor(view);
    QCOMPARE(pixel(view, 5, 1, 1), shade);

    // Clear: the selected frame's shadow (frame 1 stands for frame 5).
    frames->focusFrame(1);
    const int history = view->history_index();
    unit->action(QStringLiteral("clear"))->trigger();
    QCOMPARE(view->history_index(), history + 1);
    QCOMPARE(pixel(view, 5, 1, 1) >> 24, 0u);
    QCOMPARE(pixel(view, 6, 1, 1), shade);

    // Generate frame 1's shadow moved one pixel right and one down.
    unit->offsetX()->setValue(1);
    unit->offsetY()->setValue(1);
    unit->action(QStringLiteral("generate"))->trigger();
    QCOMPARE(view->history_index(), history + 2);
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const bool on = inBlock(x - 1, y - 1);
            QCOMPARE(pixel(view, 5, x, y), on ? shade : 0u);
        }
    }
    // The art is untouched.
    QCOMPARE(pixel(view, 1, 1, 1), pictura::shp_palette_unique(*view)[41] | 0xFF000000u);
    QVERIFY(view->undo());
    QCOMPARE(pixel(view, 5, 1, 1) >> 24, 0u);
    QVERIFY(view->undo());
    QCOMPARE(pixel(view, 5, 1, 1), shade);
    QVERIFY(window_->saveActive());
    QCOMPARE(readFile(unit_), unitSprite());

    // Clear All, then Generate All with no offset brings back the file.
    unit->action(QStringLiteral("clearAll"))->trigger();
    for (int k = kArt; k < 2 * kArt; ++k) {
        QCOMPARE(pixel(view, k, 2, 2) >> 24, 0u);
    }
    unit->offsetX()->setValue(0);
    unit->offsetY()->setValue(0);
    unit->action(QStringLiteral("generateAll"))->trigger();
    QVERIFY(window_->saveActive());
    QCOMPARE(readFile(unit_), unitSprite());
}

void ShpUnitTest::generateAddsAShadowHalfWithinTheSizeLimit()
{
    pictura::ShpSupport::setGateInteractive(0);
    pictura::PictureView* view = open(plain_);
    QVERIFY(view);
    pictura::ShpUnitPanel* unit = support_->unit();
    QTRY_COMPARE(support_->frames()->frameCount(), kFrames);
    QCOMPARE(unit->shadowHalf(), 0);
    QVERIFY(unit->shadowReadout()->text().contains(QStringLiteral("Generate adds")));
    // Twice the sheet is over a limit the sheet fits now: refused.
    const pictura::ShpPlan plan = pictura::shp_plan_shadow_half(*view);
    QVERIFY(plan.valid);
    QCOMPARE(plan.total, 2 * kFrames);
    pictura::shp_set_test_limits(plan.needed - 1, 0);
    const int history = view->history_index();
    QCOMPARE(support_->generateShadows(unit->artFrames(true), 0, 0, 0.0), -1);
    QVERIFY(support_->lastGateMessage().contains(QStringLiteral("size limit")));
    QCOMPARE(pictura::shp_info(*view).total, kFrames);
    QCOMPARE(view->history_index(), history);

    pictura::shp_set_test_limits(0, 0);
    unit->action(QStringLiteral("generateAll"))->trigger();
    QTRY_COMPARE(support_->frames()->frameCount(), 2 * kFrames);
    QCOMPARE(view->history_index(), history + 1);
    QVERIFY(unit->shadowToggle()->isChecked());
    QCOMPARE(unit->shadowHalf(), kFrames);
    const quint32 shade = shadowColor(view);
    // Frame k's art covers every pixel but the corner (0, 0).
    QCOMPARE(pixel(view, kFrames + 3, 0, 0) >> 24, 0u);
    QCOMPARE(pixel(view, kFrames + 3, 4, 3), shade);
    QVERIFY(view->undo());
    QTRY_COMPARE(support_->frames()->frameCount(), kFrames);
}

void ShpUnitTest::underlayShowsShadowsWithoutChangingSamplesOrBytes()
{
    pictura::PictureView* view = open(unit_);
    QVERIFY(view);
    pictura::ShpUnitPanel* unit = support_->unit();
    QTRY_COMPARE(unit->shadowHalf(), kArt);
    pictura::ShpFramesPanel* frames = support_->frames();
    frames->setSelectsCell(false);
    // Frame 2's shadow, moved right by two, sticks out of the art at x 4-5.
    support_->frames()->focusFrame(2);
    QVERIFY(support_->generateShadows({2}, 2, 0, 0.0) == 1);
    pictura::ImageView* canvas = window_->canvasAt(0);
    QVERIFY(canvas);
    frames->focusFrame(2);
    const QRect cell = frames->cellRect(2);
    const QPoint out = cell.topLeft() + QPoint(5, 2);  // shadow, no art
    const QPoint in = cell.topLeft() + QPoint(2, 2);   // art over shadow
    const QRgb plainOut = shown(canvas, out.x(), out.y());
    const QRgb plainIn = shown(canvas, in.x(), in.y());
    const quint32 layer = pictura::sample_argb_scoped(*view, out.x(), out.y(), 1, 0);
    const quint32 all = pictura::sample_argb_scoped(*view, out.x(), out.y(), 3, 2);
    const quint32 composite = view->composite_argb(out.x(), out.y());
    QCOMPARE(layer >> 24, 0u);
    const QString offPath = dir_.filePath(QStringLiteral("off.shp"));
    QVERIFY(window_->saveActiveAs(offPath));

    QVERIFY(unit->underlay()->isEnabled());
    unit->underlay()->setChecked(true);
    QVERIFY(canvas->shpGrid().shadowUnderlay);
    QCOMPARE(canvas->shpGrid().shadowHalf, kArt);
    QTRY_VERIFY(shown(canvas, out.x(), out.y()) != plainOut);
    QCOMPARE(shown(canvas, in.x(), in.y()), plainIn);
    QCOMPARE(pictura::sample_argb_scoped(*view, out.x(), out.y(), 1, 0), layer);
    QCOMPARE(pictura::sample_argb_scoped(*view, out.x(), out.y(), 3, 2), all);
    QCOMPARE(view->composite_argb(out.x(), out.y()), composite);
    const QString onPath = dir_.filePath(QStringLiteral("on.shp"));
    QVERIFY(window_->saveActiveAs(onPath));
    QCOMPARE(readFile(onPath), readFile(offPath));
}

void ShpUnitTest::saveWarnsAboutColoredShadowFramesButSaves()
{
    pictura::PictureView* view = open(unit_);
    QVERIFY(view);
    QTRY_COMPARE(support_->unit()->shadowHalf(), kArt);
    // An art color (index 45) in shadow frame 6.
    const QRect cell = support_->frames()->cellRect(6);
    const quint32 art = pictura::shp_palette_unique(*view)[45] | 0xFF000000u;
    QVERIFY(dab(view, art, cell.x() + 2, cell.y() + 2));
    QCOMPARE(pictura::shp_shadow_violations(*view, kArt).size(), size_t(1));
    // Detection alone would no longer call it shadowed; the toggle keeps it.
    QCOMPARE(pictura::shp_shadow_detect(*view), -1);
    QCOMPARE(support_->unit()->shadowHalf(), kArt);

    pictura::ShpSupport::setGateInteractive(1);
    {
        GateClicker cancel(QStringLiteral("shpGateCancel"));
        QVERIFY(!window_->saveActive());
        QVERIFY(cancel.seen());
    }
    QCOMPARE(readFile(unit_), unitSprite());
    QVERIFY(support_->lastGateMessage().contains(QStringLiteral("6")));
    {
        GateClicker save(QStringLiteral("shpGateSaveAnyway"));
        QVERIFY(window_->saveActive());
        QVERIFY(save.seen());
    }
    QVERIFY(readFile(unit_) != unitSprite());
    // Without dialogs it is a warning in the status bar, never a refusal.
    pictura::ShpSupport::setGateInteractive(0);
    const QString again = dir_.filePath(QStringLiteral("again.shp"));
    QVERIFY(window_->saveActiveAs(again));
    QVERIFY(window_->statusBar()->currentMessage().contains(QStringLiteral("Shadow frame")));
    QCOMPARE(readFile(again), readFile(unit_));
}

void ShpUnitTest::aPagedSpriteGeneratesShadowsOffTheSheet()
{
    pictura::ShpSupport::setGateInteractive(0);
    pictura::shp_set_test_limits(600, 0);
    pictura::PictureView* view = open(unit_);
    QVERIFY(view);
    const pictura::ShpInfo info = pictura::shp_info(*view);
    QCOMPARE(info.frames, kArt);
    QCOMPARE(info.total, 2 * kArt);
    pictura::ShpUnitPanel* unit = support_->unit();
    QTRY_COMPARE(unit->shadowHalf(), kArt);

    // The underlay of a frame whose shadow is off the sheet is decoded.
    support_->frames()->setSelectsCell(false);
    support_->frames()->focusFrame(1);
    unit->underlay()->setChecked(true);
    pictura::ImageView* canvas = window_->canvasAt(0);
    QTRY_VERIFY(!canvas->shpGrid().activeShadow.isNull());
    QCOMPARE(canvas->shpGrid().activeShadow.size(), QSize(kW, kH));

    QCOMPARE(support_->clearShadows({5, 7}), 2);
    QVERIFY(window_->statusBar()->currentMessage().contains(QStringLiteral("shadow")));
    QCOMPARE(support_->generateShadows({3}, 1, 0, 0.0), 1);
    QVERIFY(window_->statusBar()->currentMessage().contains(QStringLiteral("undo history")));
    const QString out = dir_.filePath(QStringLiteral("paged-out.shp"));
    QVERIFY(window_->saveActiveAs(out));

    // Read back whole: frame 5 empty, frame 7 moved right, 4 and 6 as before.
    pictura::shp_set_test_limits(0, 0);
    pictura::PictureView* back = open(out);
    QVERIFY(back);
    QTRY_COMPARE(pictura::shp_info(*back).frames, 2 * kArt);
    const quint32 shade = shadowColor(back);
    QCOMPARE(pixel(back, 5, 2, 2) >> 24, 0u);
    QCOMPARE(pixel(back, 4, 2, 2), shade);
    QCOMPARE(pixel(back, 6, 1, 1), shade);
    QCOMPARE(pixel(back, 7, 1, 1) >> 24, 0u);
    QCOMPARE(pixel(back, 7, 4, 1), shade);
}

QTEST_MAIN(ShpUnitTest)
#include "tst_shp_unit.moc"
