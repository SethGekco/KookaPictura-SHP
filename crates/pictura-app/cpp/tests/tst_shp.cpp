// SPDX-License-Identifier: GPL-3.0-or-later
// KookaPictura-SHP fork: SHP documents in the shell. Every sprite and
// palette here is generated; no game file is used.

#include <QtTest/QtTest>

#include "api/document_api.h"
#include "dialogs.h"
#include "frame.h"
#include "image_view.h"
#include "panels/shp_frames_panel.h"
#include "panels/shp_palette_panel.h"
#include "shp_palettes.h"
#include "shp_support.h"

#include "pictura_app/src/cxxqt_object.cxxqt.h"
#include "pictura_app/src/cxxqt_object/shp.cxxqt.h"

#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtWidgets/QApplication>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>

#include "qt_test_support.h"

namespace {

constexpr int kW = 6;
constexpr int kH = 5;
constexpr int kFrames = 10;

void put16(QByteArray& out, int v)
{
    out.append(char(v & 0xFF));
    out.append(char((v >> 8) & 0xFF));
}

void put32(QByteArray& out, quint32 v)
{
    put16(out, int(v & 0xFFFF));
    put16(out, int(v >> 16));
}

// Frame k: index 40 + k everywhere except a transparent corner, stored raw
// on the full canvas.
QByteArray sprite()
{
    QByteArray out;
    put16(out, 0);
    put16(out, kW);
    put16(out, kH);
    put16(out, kFrames);
    const int base = 8 + 24 * kFrames;
    for (int k = 0; k < kFrames; ++k) {
        put16(out, 0);
        put16(out, 0);
        put16(out, kW);
        put16(out, kH);
        put32(out, 0); // raw
        put32(out, 0x00112233u);
        put32(out, 0);
        put32(out, quint32(base + k * kW * kH));
    }
    for (int k = 0; k < kFrames; ++k) {
        for (int i = 0; i < kW * kH; ++i) {
            out.append(char(i == 0 ? 0 : 40 + k));
        }
    }
    return out;
}

// A 6-bit VGA palette that repeats each colour on four indices, as game
// palettes repeat colours.
QByteArray palette()
{
    QByteArray out;
    for (int i = 0; i < 256; ++i) {
        const int v = (i / 4) % 64;
        out.append(char(v));
        out.append(char(63 - v));
        out.append(char(v / 2));
    }
    return out;
}

bool writeFile(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// One hard Pencil dab of `argb` on the pixel (x, y) of the sheet.
bool dab(pictura::PictureView* view, quint32 argb, int x, int y)
{
    view->set_active_layer(QStringLiteral("0"));
    return view->begin_paint(argb, 0xFFFFFFFFu, 1, 100, 100, 0, 100, 100, 25,
                             QStringLiteral("normal"), true, false)
        && view->paint_dab(x + 0.5, y + 0.5, 1.0) && view->end_paint();
}

// Click the button named `button` in the gate when it appears; true once
// the gate was seen.
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

} // namespace

class ShpTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();
    void opensWithThePaletteBesideIt();
    void showsOneThumbnailPerFrame();
    void frameClickFocusesAndSelectsTheCell();
    void swatchClickSetsTheExactForeground();
    void rebindIsOneUndoableStep();
    void saveFormatsListShp();
    void unchangedSaveIsByteIdentical();
    void ctrlSWritesTheEditedFrame();
    void offPaletteSaveAsksFirst();
    void offPaletteSaveIsRefusedWithoutDialogs();

private:
    pictura::PictureView* open();

    pictura::test::ScopedStateHome stateHome_;
    QTemporaryDir dir_;
    QString shp_;
    std::unique_ptr<pictura::PicturaMainWindow> window_;
    pictura::ShpSupport* support_ = nullptr;
};

void ShpTest::init()
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

void ShpTest::cleanup()
{
    pictura::ShpSupport::setGateInteractive(-1);
    window_.reset();
    pictura::forgetFolderPalette(shp_);
}

pictura::PictureView* ShpTest::open()
{
    if (!window_->openDocumentAtPath(shp_)) {
        return nullptr;
    }
    window_->refresh();
    return window_->viewAt(0);
}

void ShpTest::opensWithThePaletteBesideIt()
{
    QVERIFY(pictura::shp_sniff_file(shp_));
    QCOMPARE(pictura::resolveShpPalette(shp_), dir_.filePath(QStringLiteral("gtank.pal")));
    pictura::PictureView* view = open();
    QVERIFY(view);
    const pictura::ShpInfo info = pictura::shp_info(*view);
    QVERIFY(info.valid);
    QCOMPARE(info.frames, kFrames);
    QCOMPARE(info.width, kW);
    QCOMPARE(info.palette_name, QStringLiteral("gtank.pal"));
    QVERIFY(info.has_source);
    // 10 frames: 4 columns, 3 rows, 1 px gutters.
    QCOMPARE(view->document_width(), 4 * kW + 3);
    QCOMPARE(view->document_height(), 3 * kH + 2);
}

void ShpTest::showsOneThumbnailPerFrame()
{
    QVERIFY(open());
    QTRY_COMPARE(support_->frames()->frameCount(), kFrames);
    QTRY_VERIFY(support_->dock()->isVisible());
}

void ShpTest::frameClickFocusesAndSelectsTheCell()
{
    pictura::PictureView* view = open();
    QVERIFY(view);
    QTRY_COMPARE(support_->frames()->frameCount(), kFrames);
    QListWidget* list = support_->frames()->list();
    const QRect item = list->visualItemRect(list->item(5));
    QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, item.center());
    QCOMPARE(support_->frames()->current(), 5);
    const QRect cell = support_->frames()->cellRect(5);
    QCOMPARE(cell, QRect(1 * (kW + 1), 1 * (kH + 1), kW, kH));
    const rust::Vec<int32_t> bounds = view->selection_bounds_extents();
    QCOMPARE(int(bounds.size()), 4);
    QCOMPARE(QRect(bounds[0], bounds[1], bounds[2], bounds[3]), cell);
    pictura::ImageView* canvas = window_->canvasAt(0);
    QVERIFY(canvas);
    QCOMPARE(canvas->shpGrid().active, 5);
    QCOMPARE(canvas->shpGrid().frames, kFrames);

    // , and . step through the frames.
    support_->frames()->step(1);
    QCOMPARE(support_->frames()->current(), 6);
    support_->frames()->step(-7);
    QCOMPARE(support_->frames()->current(), 9);
}

void ShpTest::swatchClickSetsTheExactForeground()
{
    pictura::PictureView* view = open();
    QVERIFY(view);
    QTRY_VERIFY(support_->palette()->isVisible());
    pictura::ShpPaletteGrid* grid = support_->palette()->grid();
    QTRY_VERIFY(grid->width() > 0);
    QTest::mouseClick(grid, Qt::LeftButton, {}, grid->cellRect(22).center());
    const rust::Vec<uint32_t> unique = pictura::shp_palette_unique(*view);
    QCOMPARE(window_->foregroundColor().rgb() & 0xFFFFFFu, unique[22] & 0xFFFFFFu);
    QCOMPARE(pictura::shp_index_of(*view, unique[22] & 0xFFFFFFu), 22);
    QVERIFY(support_->palette()->foregroundReadoutForTest().contains(QStringLiteral("22")));
    // Index 22 shares its colour with 20, 21 and 23 in the .pal, so only the
    // unique colour can tell them apart.
    QVERIFY(unique[22] != unique[21]);

    // A Pencil dab with it lands on the palette exactly.
    view->set_active_layer(QStringLiteral("0"));
    QVERIFY(view->begin_paint(window_->foregroundColor().rgba(), 0xFFFFFFFFu, 1, 100, 100, 0,
                              100, 100, 25, QStringLiteral("normal"), true, false));
    QVERIFY(view->paint_dab(2.5, 2.5, 1.0));
    QVERIFY(view->end_paint());
    QCOMPARE(view->composite_argb(2, 2) & 0xFFFFFFu, unique[22] & 0xFFFFFFu);
    const pictura::ShpCheck check = pictura::shp_check(*view);
    QVERIFY(check.error.isEmpty());
    QCOMPARE(check.off_palette, 0);
}

void ShpTest::rebindIsOneUndoableStep()
{
    pictura::PictureView* view = open();
    QVERIFY(view);
    QByteArray other = palette();
    for (int i = 0; i < other.size(); ++i) {
        other[i] = char(63 - other.at(i));
    }
    const QString path = dir_.filePath(QStringLiteral("other.pal"));
    QVERIFY(writeFile(path, other));
    const quint32 before = view->composite_argb(1, 0);
    const int history = view->history_index();
    QVERIFY(support_->palette()->loadPalette(path, true));
    QCOMPARE(pictura::shp_info(*view).palette_name, QStringLiteral("other.pal"));
    QVERIFY(view->composite_argb(1, 0) != before);
    QCOMPARE(view->history_index(), history + 1);
    // Remembered for the folder: the next open of a sprite here uses it.
    QCOMPARE(pictura::resolveShpPalette(shp_), path);
    QVERIFY(view->undo());
    QCOMPARE(pictura::shp_info(*view).palette_name, QStringLiteral("gtank.pal"));
    QCOMPARE(view->composite_argb(1, 0), before);
}

void ShpTest::saveFormatsListShp()
{
    const QStringList& filters = pictura::saveFileFilters();
    const int shp = filters.indexOf(QStringLiteral("Westwood SHP (TS/RA2) (*.SHP)"));
    QCOMPARE(shp, 8);
    QCOMPARE(pictura::defaultSuffixForFilter(shp), QStringLiteral(".shp"));
    QVERIFY(filters.last().contains(QStringLiteral("*.SHP")));
    QVERIFY(pictura::openFileFilters().first().contains(QStringLiteral("*.SHP")));
    QVERIFY(pictura::openFileFilters().contains(filters.at(shp)));
    QCOMPARE(pictura::pathForSaveFilter(QStringLiteral("/tmp/unit.png"), shp, 0),
             QStringLiteral("/tmp/unit.shp"));

    pictura::PictureView* view = open();
    QVERIFY(view);
    QCOMPARE(pictura::defaultSaveFilterIndex(view), shp);
    QVERIFY(!pictura::savePathNeedsFormatDialog(shp_, view, false));
    // Anything but an SHP sheet goes to Save As (and its gate) instead.
    QVERIFY(window_->newDocument(QStringLiteral("Plain"), 8, 8, QStringLiteral("rgb"), 8,
                                 QStringLiteral("white")));
    pictura::PictureView* plain = window_->activeView();
    QVERIFY(pictura::savePathNeedsFormatDialog(shp_, plain, false));
    const QString out = dir_.filePath(QStringLiteral("plain.shp"));
    QVERIFY(!window_->saveActiveAs(out));
    QVERIFY(!QFileInfo::exists(out));
    QVERIFY(!support_->lastGateMessage().isEmpty());
}

void ShpTest::unchangedSaveIsByteIdentical()
{
    QVERIFY(open());
    const QString out = dir_.filePath(QStringLiteral("copy.shp"));
    QVERIFY(window_->saveActiveAs(out));
    QCOMPARE(readFile(out), sprite());
    QCOMPARE(window_->documentPath(0), out);
}

void ShpTest::ctrlSWritesTheEditedFrame()
{
    pictura::PictureView* view = open();
    QVERIFY(view);
    const rust::Vec<uint32_t> unique = pictura::shp_palette_unique(*view);
    // Frame 5's pixel (2, 3), at sheet (7 + 2, 6 + 3).
    QVERIFY(dab(view, unique[22] | 0xFF000000u, 9, 9));
    QVERIFY(pictura::document_api::isDirty(*view));
    QVERIFY(window_->saveActive());
    QVERIFY(!pictura::document_api::isDirty(*view));
    const QByteArray saved = readFile(shp_);
    QVERIFY(pictura::shp_sniff_file(shp_));
    // Raw frames keep their offsets: only that one byte changed.
    QByteArray expected = sprite();
    const int base = 8 + 24 * kFrames;
    expected[base + 5 * kW * kH + 3 * kW + 2] = char(22);
    QCOMPARE(saved, expected);
}

void ShpTest::offPaletteSaveAsksFirst()
{
    pictura::ShpSupport::setGateInteractive(1);
    pictura::PictureView* view = open();
    QVERIFY(view);
    // An opaque colour no palette index draws.
    QVERIFY(dab(view, 0xFF010203u, 1, 1));
    QCOMPARE(pictura::shp_check(*view).off_palette, 1);
    {
        GateClicker cancel(QStringLiteral("shpGateCancel"));
        QVERIFY(!window_->saveActive());
        QVERIFY(cancel.seen());
    }
    QCOMPARE(readFile(shp_), sprite());
    const int history = view->history_index();
    {
        GateClicker snap(QStringLiteral("shpGateSnap"));
        QVERIFY(window_->saveActive());
        QVERIFY(snap.seen());
    }
    QCOMPARE(pictura::shp_check(*view).off_palette, 0);
    QCOMPARE(view->history_index(), history + 1);
    QVERIFY(pictura::shp_sniff_file(shp_));
    QVERIFY(readFile(shp_) != sprite());
}

void ShpTest::offPaletteSaveIsRefusedWithoutDialogs()
{
    pictura::ShpSupport::setGateInteractive(0);
    pictura::PictureView* view = open();
    QVERIFY(view);
    QVERIFY(dab(view, 0xFF010203u, 1, 1));
    const QString out = dir_.filePath(QStringLiteral("out.shp"));
    QVERIFY(!window_->saveActiveAs(out));
    QVERIFY(!QFileInfo::exists(out));
    QVERIFY(support_->lastGateMessage().contains(QStringLiteral("palette")));
    QVERIFY(support_->lastGateMessage().contains(QStringLiteral("frames 0")));
}

QTEST_MAIN(ShpTest)
#include "tst_shp.moc"
