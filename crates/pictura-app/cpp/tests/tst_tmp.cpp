// SPDX-License-Identifier: GPL-3.0-or-later
// KookaPictura-SHP fork: TMP terrain templates in the shell. Every template,
// sprite and palette here is generated; no game file is used.

#include <QtTest/QtTest>

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
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QDockWidget>
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

QTEST_MAIN(TmpTest)
#include "tst_tmp.moc"
