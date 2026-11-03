// SPDX-License-Identifier: GPL-3.0-or-later
// KookaPictura-SHP fork: TMP terrain templates in the shell. Every template,
// sprite and palette here is generated; no game file is used.

#include <QtTest/QtTest>

#include "api/document_api.h"
#include "dialogs.h"
#include "commands.h"
#include "frame.h"
#include "image_view.h"
#include "panels/shp_frames_panel.h"
#include "panels/shp_palette_panel.h"
#include "panels/tmp_tiles_panel.h"
#include "shp_palettes.h"
#include "shp_support.h"

#include "pictura_app/src/cxxqt_object.cxxqt.h"
#include "pictura_app/src/cxxqt_object/shp.cxxqt.h"

#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QDockWidget>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTableWidget>

#include "qt_test_support.h"
#include "shp_fixtures.h"

namespace {

constexpr int kCx = 60;
constexpr int kCy = 30;
constexpr int kDiamond = kCx * kCy / 2;

// One 52-byte tile header.
void tileHeader(QByteArray& out, int x, int y, int extraOff, int zOff, int extraZOff, int ex,
                int ey, int ew, int eh, quint32 flags, int height, int land, int ramp)
{
    put32(out, quint32(x));
    put32(out, quint32(y));
    put32(out, quint32(extraOff));
    put32(out, quint32(zOff));
    put32(out, quint32(extraZOff));
    put32(out, quint32(ex));
    put32(out, quint32(ey));
    put32(out, quint32(ew));
    put32(out, quint32(eh));
    put32(out, flags);
    out.append(char(height));
    out.append(char(land));
    out.append(char(ramp));
    out.append("\x10\x20\x30\x40\x50\x60", 6);
    out.append(QByteArray(3, '\0'));
}

// A 2 x 1 template of 60 x 30 tiles. Slot 0 at (0, 0) has z-data and a
// 4 x 4 extra block 6 px above the template; slot 1 at (30, 15) has neither.
QByteArray terrain()
{
    QByteArray out;
    put32(out, 2);
    put32(out, 1);
    put32(out, kCx);
    put32(out, kCy);
    const int first = 16 + 2 * 4;
    const int extraOff = 52 + 2 * kDiamond;
    const int second = first + extraOff + 2 * 16;
    put32(out, quint32(first));
    put32(out, quint32(second));
    tileHeader(out, 0, 0, extraOff, 52 + kDiamond, extraOff + 16, 20, -6, 4, 4, 3, 1, 0, 0);
    out.append(QByteArray(kDiamond, char(40)));
    out.append(QByteArray(kDiamond, char(5)));
    out.append(QByteArray(16, char(50)));
    out.append(QByteArray(16, char(7)));
    tileHeader(out, 30, 15, 0, 0, 0, 0, 0, 0, 0, 0, 2, 7, 4);
    out.append(QByteArray(kDiamond, char(60)));
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

// The offsets of the bytes where `a` and `b` differ (same size).
QList<int> differing(const QByteArray& a, const QByteArray& b)
{
    QList<int> out;
    for (int i = 0; i < std::min(a.size(), b.size()); ++i) {
        if (a[i] != b[i]) {
            out << i;
        }
    }
    return out;
}

// Slot 0's diamond bytes in terrain().
constexpr int kTile0Pixels = 16 + 2 * 4 + 52;

} // namespace

class TmpTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();
    void opensWithTheTerrainPaletteBesideIt();
    void theTilesPanelListsEveryTile();
    void theCanvasOutlinesTilesAndExtras();
    void aRowClickHighlightsItsTile();
    void hoverNamesTheTileAndIndex();
    void reshapingCommandsAreBlocked();
    void aTheaterSuffixedShpOpensAsASprite();
    void theOpenFiltersListTheTheaterSuffixes();
    void unchangedSaveIsByteIdentical();
    void ctrlSWritesOneByteOfTheEditedTile();
    void offPaletteSaveAsksFirst();
    void offPaletteSaveIsRefusedWithoutDialogs();
    void showMeSelectsTheFirstOffPixel();
    void aPixelOutsideTheTilesIsClipped();
    void theSaveFiltersOfferTmp();
    void aTemplateDoesNotSaveAsShp();
    void aTheaterSuffixedShpSavesByteIdentical();
    void tileFieldsAreEditedInThePanelAndUndo();
    void aBadFieldValueIsRefused();

private:
    pictura::PictureView* open(const QString& path);

    pictura::test::ScopedStateHome stateHome_;
    QTemporaryDir dir_;
    QString tmp_;
    std::unique_ptr<pictura::PicturaMainWindow> window_;
    pictura::ShpSupport* support_ = nullptr;
};

void TmpTest::init()
{
    QVERIFY(dir_.isValid());
    tmp_ = dir_.filePath(QStringLiteral("clear01.tem"));
    QVERIFY(writeFile(tmp_, terrain()));
    QVERIFY(writeFile(dir_.filePath(QStringLiteral("isotem.pal")), palette()));
    window_ = pictura::test::makeMainWindow();
    window_->resize(1200, 900);
    window_->show();
    support_ = pictura::ShpSupport::of(window_.get());
    QVERIFY(support_);
}

void TmpTest::cleanup()
{
    pictura::ShpSupport::setGateInteractive(-1);
    window_.reset();
    pictura::forgetFolderPalette(tmp_, true);
}

pictura::PictureView* TmpTest::open(const QString& path)
{
    if (!window_->openDocumentAtPath(path)) {
        return nullptr;
    }
    window_->refresh();
    return window_->viewAt(0);
}

void TmpTest::opensWithTheTerrainPaletteBesideIt()
{
    QVERIFY(pictura::tmp_sniff_file(tmp_));
    QVERIFY(!pictura::shp_sniff_file(tmp_));
    QCOMPARE(pictura::resolveTmpPalette(tmp_), dir_.filePath(QStringLiteral("isotem.pal")));
    pictura::PictureView* view = open(tmp_);
    QVERIFY(view);
    QVERIFY(!pictura::shp_info(*view).valid);
    const pictura::TmpInfo info = pictura::tmp_info(*view);
    QVERIFY(info.valid);
    QCOMPARE(info.tiles, 2);
    QCOMPARE(info.cx, kCx);
    QCOMPARE(info.palette_name, QStringLiteral("isotem.pal"));
    // Tiles span 90 x 45; the extra block reaches 6 px above them.
    QCOMPARE(view->document_width(), 90);
    QCOMPARE(view->document_height(), 51);
    QTRY_VERIFY(support_->palette()->isVisible());
    QVERIFY(pictura::ShpPalettePanel::paletteOf(view).terrain);
}

void TmpTest::theTilesPanelListsEveryTile()
{
    QVERIFY(open(tmp_));
    QTRY_VERIFY(support_->dock()->isVisible());
    QTRY_COMPARE(support_->tiles()->tileCount(), 2);
    QVERIFY(support_->tiles()->isVisible());
    QVERIFY(!support_->frames()->isVisible());
    QCOMPARE(support_->dock()->windowTitle(), QStringLiteral("TMP Tiles & Palette"));
    QTableWidget* table = support_->tiles()->table();
    QCOMPARE(table->item(0, 1)->text(), QStringLiteral("0, 0"));
    QCOMPARE(table->item(1, 1)->text(), QStringLiteral("1, 0"));
    QCOMPARE(table->item(1, 3)->text(), QStringLiteral("2"));
    QVERIFY(table->item(1, 4)->text().startsWith(QStringLiteral("7 ")));
    QVERIFY(table->item(1, 5)->text().startsWith(QStringLiteral("4 ")));
    QCOMPARE(table->item(0, 6)->text(), QStringLiteral("yes"));
    QCOMPARE(table->item(1, 6)->text(), QStringLiteral("no"));
    QCOMPARE(table->item(0, 7)->text(), QStringLiteral("yes"));
    QCOMPARE(table->item(1, 7)->text(), QStringLiteral("no"));
    QVERIFY(support_->tiles()->summaryForTest().startsWith(QStringLiteral("2 tiles")));
}

void TmpTest::theCanvasOutlinesTilesAndExtras()
{
    QVERIFY(open(tmp_));
    QTRY_COMPARE(support_->tiles()->tileCount(), 2);
    pictura::ImageView* canvas = window_->canvasAt(0);
    QVERIFY(canvas);
    QTRY_COMPARE(int(canvas->shpGrid().tileOutlines.size()), 2);
    QCOMPARE(int(canvas->shpGrid().extraOutlines.size()), 1);
    // Slot 1 sits at (30, 15) in the file, 6 px lower on the canvas.
    const QPolygonF diamond = canvas->shpGrid().tileOutlines[1];
    QCOMPARE(diamond.boundingRect(), QRectF(30, 21, kCx, kCy));
    QCOMPARE(canvas->shpGrid().extraOutlines[0], QRectF(20, 0, 4, 4));
    support_->tiles()->outlines()->setChecked(false);
    QVERIFY(!canvas->shpGrid().visible);
}

void TmpTest::aRowClickHighlightsItsTile()
{
    QVERIFY(open(tmp_));
    QTRY_COMPARE(support_->tiles()->tileCount(), 2);
    QTableWidget* table = support_->tiles()->table();
    table->setCurrentCell(1, 0);
    QCOMPARE(support_->tiles()->currentSlot(), 1);
    pictura::ImageView* canvas = window_->canvasAt(0);
    QVERIFY(canvas);
    QCOMPARE(canvas->shpGrid().activeTile, 1);
}

void TmpTest::hoverNamesTheTileAndIndex()
{
    QVERIFY(open(tmp_));
    QTRY_COMPARE(support_->tiles()->tileCount(), 2);
    pictura::ShpPalettePanel* palette = support_->palette();
    pictura::ImageView* canvas = window_->canvasAt(0);
    QVERIFY(canvas);
    // The middle of slot 1, and the extra block of slot 0.
    emit canvas->mouseMoved(QPointF(60.5, 36.5));
    QVERIFY2(palette->canvasHoverForTest().startsWith(QStringLiteral("Tile 1 (60, 36): index 60")),
             qPrintable(palette->canvasHoverForTest()));
    emit canvas->mouseMoved(QPointF(21.5, 1.5));
    QVERIFY2(palette->canvasHoverForTest().startsWith(QStringLiteral("Tile 0 (21, 1): index 50")),
             qPrintable(palette->canvasHoverForTest()));
    emit canvas->mouseMoved(QPointF(0.5, 50.5));
    QVERIFY2(palette->canvasHoverForTest().startsWith(QStringLiteral("Canvas (0, 50), no tile")),
             qPrintable(palette->canvasHoverForTest()));
}

void TmpTest::reshapingCommandsAreBlocked()
{
    pictura::PictureView* view = open(tmp_);
    QVERIFY(view);
    QTRY_COMPARE(support_->tiles()->tileCount(), 2);
    for (const QString& id : {QStringLiteral("image.image.size"), QStringLiteral("image.canvas.size"),
                              QStringLiteral("image.crop"), QStringLiteral("image.rotate180")}) {
        QVERIFY2(pictura::ShpSupport::blocks(window_.get(), id), qPrintable(id));
    }
    QVERIFY(!window_->registry()->dispatch(QStringLiteral("image.rotate180")));
    QCOMPARE(view->document_width(), 90);
    QVERIFY(!pictura::ShpSupport::blocks(window_.get(), QStringLiteral("edit.fill")));
}

void TmpTest::aTheaterSuffixedShpOpensAsASprite()
{
    const QString path = dir_.filePath(QStringLiteral("tree01.tem"));
    QVERIFY(writeFile(path, sprite()));
    QVERIFY(pictura::shp_sniff_file(path));
    QVERIFY(!pictura::tmp_sniff_file(path));
    pictura::PictureView* view = open(path);
    QVERIFY(view);
    QVERIFY(pictura::shp_info(*view).valid);
    QVERIFY(!pictura::tmp_info(*view).valid);
    QTRY_COMPARE(support_->frames()->frameCount(), kFrames);
    QTRY_VERIFY(support_->frames()->isVisible());
    QVERIFY(!support_->tiles()->isVisible());
    QCOMPARE(support_->dock()->windowTitle(), QStringLiteral("SHP Frames & Palette"));
    pictura::forgetFolderPalette(path);
}

void TmpTest::theOpenFiltersListTheTheaterSuffixes()
{
    const QStringList& filters = pictura::openFileFilters();
    for (const QString& suffix : pictura::theaterSuffixes()) {
        const QString glob = QStringLiteral("*.") + suffix.toUpper();
        QVERIFY2(filters.first().contains(glob), qPrintable(suffix));
    }
    QVERIFY(filters.first().contains(QStringLiteral("*.TMP")));
    QVERIFY(std::any_of(filters.begin(), filters.end(), [](const QString& f) {
        return f.startsWith(QStringLiteral("Westwood TMP terrain"));
    }));
    QVERIFY(pictura::isTheaterPath(QStringLiteral("/x/CLEAR01.SNO")));
    QVERIFY(!pictura::isTheaterPath(QStringLiteral("/x/clear01.shp")));
}

void TmpTest::unchangedSaveIsByteIdentical()
{
    QVERIFY(open(tmp_));
    const QString out = dir_.filePath(QStringLiteral("copy.sno"));
    QVERIFY(window_->saveActiveAs(out));
    QCOMPARE(readFile(out), terrain());
    QCOMPARE(window_->documentPath(0), out);
}

void TmpTest::ctrlSWritesOneByteOfTheEditedTile()
{
    pictura::PictureView* view = open(tmp_);
    QVERIFY(view);
    const rust::Vec<uint32_t> unique = pictura::shp_palette_unique(*view);
    QCOMPARE(int(unique.size()), 256);
    // Slot 0's diamond centre, 6 px down on the canvas.
    QVERIFY(dab(view, unique[22] | 0xFF000000u, 30, 21));
    QVERIFY(pictura::document_api::isDirty(*view));
    QVERIFY(window_->saveActive());
    QVERIFY(!pictura::document_api::isDirty(*view));
    const QByteArray saved = readFile(tmp_);
    const QList<int> changed = differing(saved, terrain());
    QCOMPARE(saved.size(), terrain().size());
    QCOMPARE(changed.size(), 1);
    QVERIFY(changed[0] >= kTile0Pixels && changed[0] < kTile0Pixels + kDiamond);
    QCOMPARE(int(quint8(saved[changed[0]])), 22);
}

void TmpTest::offPaletteSaveAsksFirst()
{
    pictura::ShpSupport::setGateInteractive(1);
    pictura::PictureView* view = open(tmp_);
    QVERIFY(view);
    QVERIFY(dab(view, 0xFF010203u, 60, 36));
    QCOMPARE(pictura::tmp_check(*view).off_palette, 1);
    {
        GateClicker cancel(QStringLiteral("shpGateCancel"));
        QVERIFY(!window_->saveActive());
        QVERIFY(cancel.seen());
    }
    QCOMPARE(readFile(tmp_), terrain());
    const int history = view->history_index();
    {
        GateClicker snap(QStringLiteral("shpGateSnap"));
        QVERIFY(window_->saveActive());
        QVERIFY(snap.seen());
    }
    QCOMPARE(pictura::tmp_check(*view).off_palette, 0);
    QCOMPARE(view->history_index(), history + 1);
    QVERIFY(pictura::tmp_sniff_file(tmp_));
    const QList<int> changed = differing(readFile(tmp_), terrain());
    QCOMPARE(changed.size(), 1);
    QVERIFY(changed[0] >= kTile0Pixels + kDiamond);
}

void TmpTest::offPaletteSaveIsRefusedWithoutDialogs()
{
    pictura::ShpSupport::setGateInteractive(0);
    pictura::PictureView* view = open(tmp_);
    QVERIFY(view);
    QVERIFY(dab(view, 0xFF010203u, 60, 36));
    const QString out = dir_.filePath(QStringLiteral("out.tem"));
    QVERIFY(!window_->saveActiveAs(out));
    QVERIFY(!QFileInfo::exists(out));
    QVERIFY2(support_->lastGateMessage().contains(QStringLiteral("palette colours (tiles 1)")),
             qPrintable(support_->lastGateMessage()));
    QVERIFY(support_->lastGateMessage().contains(QStringLiteral("Snap them")));
}

void TmpTest::showMeSelectsTheFirstOffPixel()
{
    pictura::ShpSupport::setGateInteractive(1);
    pictura::PictureView* view = open(tmp_);
    QVERIFY(view);
    QVERIFY(dab(view, 0xFF010203u, 60, 36));
    {
        GateClicker show(QStringLiteral("shpGateShow"));
        QVERIFY(!window_->saveActive());
        QVERIFY(show.seen());
    }
    QCOMPARE(support_->tiles()->currentSlot(), 1);
    const rust::Vec<int32_t> bounds = view->selection_bounds_extents();
    QCOMPARE(int(bounds.size()), 4);
    QCOMPARE(QRect(bounds[0], bounds[1], bounds[2], bounds[3]), QRect(60, 36, 1, 1));
    QCOMPARE(readFile(tmp_), terrain());
}

void TmpTest::aPixelOutsideTheTilesIsClipped()
{
    pictura::PictureView* view = open(tmp_);
    QVERIFY(view);
    const rust::Vec<uint32_t> unique = pictura::shp_palette_unique(*view);
    QVERIFY(dab(view, unique[22] | 0xFF000000u, 89, 0));
    QCOMPARE(pictura::tmp_check(*view).outside, 1);
    pictura::ShpSupport::setGateInteractive(0);
    QVERIFY(!window_->saveActive());
    QVERIFY(support_->lastGateMessage().contains(QStringLiteral("Clip them")));
    pictura::ShpSupport::setGateInteractive(1);
    {
        GateClicker clip(QStringLiteral("shpGateClip"));
        QVERIFY(window_->saveActive());
        QVERIFY(clip.seen());
    }
    QCOMPARE(pictura::tmp_check(*view).outside, 0);
    QCOMPARE(readFile(tmp_), terrain());
}

void TmpTest::theSaveFiltersOfferTmp()
{
    const QStringList& filters = pictura::saveFileFilters();
    const int tmp = filters.indexOf(
        QStringLiteral("Westwood TMP terrain (*.TEM *.SNO *.URB *.UBN *.DES *.LUN *.TMP)"));
    QCOMPARE(tmp, 9);
    QCOMPARE(filters.size(), 11);
    QVERIFY(filters.last().contains(QStringLiteral("*.LUN")));
    QCOMPARE(pictura::defaultSuffixForFilter(tmp), QStringLiteral(".tem"));
    QCOMPARE(pictura::pathForSaveFilter(QStringLiteral("/x/a.png"), tmp, 0),
             QStringLiteral("/x/a.tem"));
    QCOMPARE(pictura::pathForSaveFilter(QStringLiteral("/x/a.SNO"), tmp, tmp),
             QStringLiteral("/x/a.sno"));
    QCOMPARE(pictura::pathForSaveFilter(QStringLiteral("/x/tree.des"), 8, 8),
             QStringLiteral("/x/tree.des"));
    QVERIFY(pictura::isWritableSavePath(QStringLiteral("/x/a.lun")));
    QVERIFY(pictura::isWritableSavePath(QStringLiteral("/x/a.tmp")));

    pictura::PictureView* view = open(tmp_);
    QVERIFY(view);
    QCOMPARE(pictura::defaultSaveFilterIndex(view), tmp);
    QVERIFY(!pictura::savePathNeedsFormatDialog(tmp_, view, false));
    QVERIFY(pictura::savePathNeedsFormatDialog(dir_.filePath(QStringLiteral("a.shp")), view,
                                               false));
}

void TmpTest::aTemplateDoesNotSaveAsShp()
{
    pictura::ShpSupport::setGateInteractive(0);
    QVERIFY(open(tmp_));
    const QString out = dir_.filePath(QStringLiteral("clear01.shp"));
    QVERIFY(!window_->saveActiveAs(out));
    QVERIFY(!QFileInfo::exists(out));
    QVERIFY2(support_->lastGateMessage().contains(QStringLiteral("saves as a TMP")),
             qPrintable(support_->lastGateMessage()));
}

void TmpTest::aTheaterSuffixedShpSavesByteIdentical()
{
    const QString path = dir_.filePath(QStringLiteral("tree01.tem"));
    QVERIFY(writeFile(path, sprite()));
    QVERIFY(open(path));
    const QString out = dir_.filePath(QStringLiteral("tree02.tem"));
    QVERIFY(window_->saveActiveAs(out));
    QCOMPARE(readFile(out), sprite());
    // A sprite is not a template: .tmp is refused.
    pictura::ShpSupport::setGateInteractive(0);
    QVERIFY(!window_->saveActiveAs(dir_.filePath(QStringLiteral("tree.tmp"))));
    pictura::forgetFolderPalette(path);
}

void TmpTest::tileFieldsAreEditedInThePanelAndUndo()
{
    pictura::PictureView* view = open(tmp_);
    QVERIFY(view);
    QTRY_COMPARE(support_->tiles()->tileCount(), 2);
    QTableWidget* table = support_->tiles()->table();
    QVERIFY(table->item(1, 3)->flags() & Qt::ItemIsEditable);
    QVERIFY(!(table->item(1, 1)->flags() & Qt::ItemIsEditable));
    const int history = view->history_index();
    table->item(1, 3)->setText(QStringLiteral("5"));
    QTRY_COMPARE(pictura::tmp_tiles(*view)[1].height, 5);
    QCOMPARE(view->history_index(), history + 1);
    QTRY_COMPARE(table->item(1, 3)->text(), QStringLiteral("5"));
    table->item(1, 4)->setText(QStringLiteral("13"));
    QTRY_COMPARE(pictura::tmp_tiles(*view)[1].land, 13);
    QTRY_VERIFY(table->item(1, 4)->text().startsWith(QStringLiteral("13 ")));
    table->item(1, 8)->setText(QStringLiteral("#010203 / #fafbfc"));
    QTRY_COMPARE(pictura::tmp_tiles(*view)[1].radar_high, 0xFAFBFCu);
    QCOMPARE(pictura::tmp_tiles(*view)[1].radar_low, 0x010203u);

    QVERIFY(window_->saveActive());
    const QByteArray saved = readFile(tmp_);
    const int second = 16 + 2 * 4 + 52 + 2 * kDiamond + 2 * 16; // slot 1's record
    for (int i : differing(saved, terrain())) {
        QVERIFY2(i >= second + 40 && i < second + 49, qPrintable(QString::number(i)));
    }
    QCOMPARE(quint8(saved[second + 40]), quint8(5));
    QCOMPARE(quint8(saved[second + 41]), quint8(13));

    window_->registry()->dispatch(QStringLiteral("edit.undo"));
    QTRY_COMPARE(pictura::tmp_tiles(*view)[1].radar_high, 0x405060u);
    QTRY_COMPARE(table->item(1, 8)->text(), QStringLiteral("#102030 / #405060"));
    window_->registry()->dispatch(QStringLiteral("edit.undo"));
    window_->registry()->dispatch(QStringLiteral("edit.undo"));
    QTRY_COMPARE(pictura::tmp_tiles(*view)[1].height, 2);
    QTRY_COMPARE(table->item(1, 3)->text(), QStringLiteral("2"));
}

void TmpTest::aBadFieldValueIsRefused()
{
    pictura::PictureView* view = open(tmp_);
    QVERIFY(view);
    QTRY_COMPARE(support_->tiles()->tileCount(), 2);
    QTableWidget* table = support_->tiles()->table();
    const int history = view->history_index();
    table->item(1, 3)->setText(QStringLiteral("300"));
    QTRY_COMPARE(table->item(1, 3)->text(), QStringLiteral("2"));
    table->item(1, 8)->setText(QStringLiteral("red"));
    QTRY_COMPARE(table->item(1, 8)->text(), QStringLiteral("#102030 / #405060"));
    QCOMPARE(view->history_index(), history);
    QCOMPARE(pictura::tmp_tiles(*view)[1].height, 2);
}

QTEST_MAIN(TmpTest)
#include "tst_tmp.moc"
