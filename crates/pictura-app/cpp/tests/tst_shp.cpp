// SPDX-License-Identifier: GPL-3.0-or-later
// KookaPictura-SHP fork: SHP documents in the shell. Every sprite and
// palette here is generated; no game file is used.

#include <QtTest/QtTest>

#include "api/document_api.h"
#include "commands.h"
#include "dialogs.h"
#include "frame.h"
#include "image_view.h"
#include "panels/shp_frames_panel.h"
#include "panels/shp_palette_panel.h"
#include "shp_authoring.h"
#include "shp_palettes.h"
#include "shp_support.h"

#include "pictura_app/src/cxxqt_object.cxxqt.h"
#include "pictura_app/src/cxxqt_object/shp.cxxqt.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QSettings>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QDialog>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QStatusBar>

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

// Answers the size-limit dialogs as they appear, one action per dialog in
// order: "go" ticks the box and clicks the go button, "alternative" and
// "cancel" click theirs. Records which dialogs came up and whether go was
// disabled until the box was ticked. A refusal message box is closed.
class LimitClicker {
public:
    explicit LimitClicker(QStringList actions)
        : actions_(std::move(actions))
    {
        QObject::connect(&poll_, &QTimer::timeout, [this] {
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                // Marked on the widget: a later dialog can reuse an address.
                if (!widget->isVisible() || widget->property("shpTestHandled").toBool()) {
                    continue;
                }
                if (widget->objectName() == QLatin1String("shpLimitRefused")) {
                    widget->setProperty("shpTestHandled", true);
                    refused_ = true;
                    if (auto* box = qobject_cast<QMessageBox*>(widget)) {
                        box->button(QMessageBox::Ok)->click();
                    }
                    continue;
                }
                auto* dialog = qobject_cast<QDialog*>(widget);
                if (!dialog || !dialog->objectName().startsWith(QLatin1String("shpLimit"))) {
                    continue;
                }
                widget->setProperty("shpTestHandled", true);
                seen_ << dialog->objectName();
                auto* go = dialog->findChild<QPushButton*>(QStringLiteral("shpLimitGo"));
                auto* box = dialog->findChild<QCheckBox*>(QStringLiteral("shpLimitUnderstand"));
                gatedByBox_ = gatedByBox_ && go && box && !go->isEnabled() && !box->isChecked();
                const QString action = actions_.isEmpty() ? QStringLiteral("cancel")
                                                          : actions_.takeFirst();
                if (action == QLatin1String("go")) {
                    box->setChecked(true);
                    gatedByBox_ = gatedByBox_ && go->isEnabled();
                    go->click();
                } else if (action == QLatin1String("alternative")) {
                    dialog->findChild<QPushButton*>(QStringLiteral("shpLimitAlternative"))
                        ->click();
                } else {
                    dialog->findChild<QPushButton*>(QStringLiteral("shpLimitCancel"))->click();
                }
            }
        });
        poll_.start(20);
    }
    QStringList seen() const { return seen_; }
    bool refused() const { return refused_; }
    // Every dialog had go disabled and the box unticked when it appeared,
    // and go enabled once the box was ticked.
    bool gatedByBox() const { return gatedByBox_ && !seen_.isEmpty(); }

private:
    QStringList actions_;
    QTimer poll_;
    QStringList seen_;
    bool refused_ = false;
    bool gatedByBox_ = true;
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
    void aPagedSpriteListsEveryFrameAndPages();
    void anEditSurvivesPagingAwayAndBack();
    void pagingRefusesOffPalettePixelsWithoutDialogs();
    void overTheLimitIsRefusedWithoutTheSetting();
    void withinTenPercentOpenWaitsForTheCheckbox();
    void pastTenPercentIsRefusedWithoutTheCeilingSetting();
    void pastTenPercentAsksTwiceWithTheCeilingSetting();
    void theFloorHoldsWhateverTheSettings();
    void limitSettingsAreNotKept();
    void withoutDialogsWhatNeedsConfirmingIsRefused();
    void underTheLimitNeverAsks();
    void aWholeSheetOverTheLimitOffersTheWindow();
    void growingPastTheLimitAsksAgainPerStage();
    void aBiggerWindowNeedsTheAllowance();
    void frameOpButtonsEditTheSheetAndUndo();
    void reverseUsesTheSelectedRange();
    void aFrameOpPastTheLimitIsRefusedWithoutTheSetting();
    void newShpMakesBlankFramesThatSaveAndReopen();
    void newShpIsInTheFileMenu();
    void saveAsShpConvertsAnImageIntoANewDocument();

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
    if (support_) {
        support_->setAllowOverLimit(false);
        support_->setRemoveCeiling(false);
    }
    pictura::shp_set_test_limits(0, 0);
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

// 10 frames of 6x5 in 600 bytes of sheet: windows of 4 (2 x 2 cells, 13 x 11
// x 4 = 572 bytes), so frames 0-3, 4-7 and 8-9.
constexpr quint64 kPagedLimit = 600;

void ShpTest::aPagedSpriteListsEveryFrameAndPages()
{
    pictura::shp_set_test_limits(kPagedLimit, 0);
    pictura::PictureView* view = open();
    QVERIFY(view);
    pictura::ShpInfo info = pictura::shp_info(*view);
    QCOMPARE(info.total, kFrames);
    QCOMPARE(info.frames, 4);
    QCOMPARE(info.first, 0);
    QCOMPARE(view->document_width(), 2 * kW + 1);
    QTRY_COMPARE(support_->frames()->frameCount(), kFrames);
    QVERIFY(support_->frames()->onSheet(3));
    QVERIFY(!support_->frames()->onSheet(9));

    // Clicking a frame off the sheet loads its window and focuses it.
    QListWidget* list = support_->frames()->list();
    QTest::mouseClick(list->viewport(), Qt::LeftButton, {},
                      list->visualItemRect(list->item(9)).center());
    info = pictura::shp_info(*view);
    QCOMPARE(info.first, 8);
    QCOMPARE(info.frames, 2);
    QCOMPARE(info.span, 4);
    QCOMPARE(support_->frames()->current(), 9);
    QCOMPARE(support_->frames()->cellRect(9), QRect(kW + 1, 0, kW, kH));
    QCOMPARE(window_->canvasAt(0)->shpGrid().active, 1);
    QVERIFY(window_->statusBar()->currentMessage().contains(QStringLiteral("undo history")));
    QVERIFY(view->history_count() <= 2);

    // Stepping past the window's edge pages too: 9 -> 0 wraps to the first.
    support_->frames()->step(1);
    QCOMPARE(support_->frames()->current(), 0);
    QCOMPARE(pictura::shp_info(*view).first, 0);
    support_->frames()->step(-1);
    QCOMPARE(support_->frames()->current(), 9);
    QCOMPARE(pictura::shp_info(*view).first, 8);
    // Nothing was edited: an unchanged save is still the file.
    const QString out = dir_.filePath(QStringLiteral("paged.shp"));
    QVERIFY(window_->saveActiveAs(out));
    QCOMPARE(readFile(out), sprite());
}

void ShpTest::anEditSurvivesPagingAwayAndBack()
{
    pictura::shp_set_test_limits(kPagedLimit, 0);
    pictura::PictureView* view = open();
    QVERIFY(view);
    support_->frames()->setSelectsCell(false);
    QVERIFY(support_->pageTo(9));
    const rust::Vec<uint32_t> unique = pictura::shp_palette_unique(*view);
    // Frame 9's pixel (2, 3): cell 1 of the window, at sheet (7 + 2, 3).
    QVERIFY(dab(view, unique[22] | 0xFF000000u, 9, 3));
    QVERIFY(support_->pageTo(0));
    QCOMPARE(pictura::shp_info(*view).first, 0);
    QVERIFY(pictura::document_api::isDirty(*view));
    QVERIFY(support_->pageTo(9));
    QCOMPARE(view->composite_argb(9, 3) & 0xFFFFFFu, unique[22] & 0xFFFFFFu);
    QVERIFY(support_->pageTo(4));
    // Saved from a window that does not hold it: only that byte changed.
    QVERIFY(window_->saveActive());
    QByteArray expected = sprite();
    const int base = 8 + 24 * kFrames;
    expected[base + 9 * kW * kH + 3 * kW + 2] = char(22);
    QCOMPARE(readFile(shp_), expected);
}

void ShpTest::pagingRefusesOffPalettePixelsWithoutDialogs()
{
    pictura::ShpSupport::setGateInteractive(0);
    pictura::shp_set_test_limits(kPagedLimit, 0);
    pictura::PictureView* view = open();
    QVERIFY(view);
    QVERIFY(dab(view, 0xFF010203u, 1, 1));
    QVERIFY(!support_->pageTo(5));
    QVERIFY(support_->lastGateMessage().contains(QStringLiteral("palette")));
    QCOMPARE(pictura::shp_info(*view).first, 0);
    QVERIFY(view->can_undo());
}

// The size limit. One 6x5 frame is 120 bytes of sheet: a 110-byte limit is
// 9% over (within the 10% allowance), a 100-byte one 20% over (past it).
// The whole 10-frame sheet is 27 x 17 x 4 = 1836 bytes.
constexpr quint64 kWithinTen = 110;
constexpr quint64 kPastTen = 100;

void ShpTest::overTheLimitIsRefusedWithoutTheSetting()
{
    pictura::ShpSupport::setGateInteractive(1);
    pictura::shp_set_test_limits(kWithinTen, 0);
    LimitClicker clicker({QStringLiteral("go")});
    QVERIFY(!window_->openDocumentAtPath(shp_));
    QCOMPARE(window_->documentCount(), 0);
    QVERIFY(clicker.seen().isEmpty());
    QVERIFY(clicker.refused());
    QVERIFY(support_->lastLimitMessage().contains(QStringLiteral("Preferences > Advanced")));
    QVERIFY(support_->lastLimitMessage().contains(QStringLiteral("over the size limit")));
}

void ShpTest::withinTenPercentOpenWaitsForTheCheckbox()
{
    pictura::ShpSupport::setGateInteractive(1);
    pictura::shp_set_test_limits(kWithinTen, 0);
    support_->setAllowOverLimit(true);
    {
        LimitClicker cancel({QStringLiteral("cancel")});
        QVERIFY(!window_->openDocumentAtPath(shp_));
        QCOMPARE(cancel.seen(), QStringList{QStringLiteral("shpLimitDialog")});
        QCOMPARE(window_->documentCount(), 0);
    }
    LimitClicker go({QStringLiteral("go")});
    pictura::PictureView* view = open();
    QVERIFY(view);
    QCOMPARE(go.seen(), QStringList{QStringLiteral("shpLimitDialog")});
    QVERIFY(go.gatedByBox());
    const pictura::ShpInfo info = pictura::shp_info(*view);
    QCOMPARE(info.frames, 1);
    QCOMPARE(info.total, kFrames);
    QCOMPARE(pictura::ShpSupport::allowanceOf(view), quint64(kW * kH * 4));
    // The allowance holds for the file: paging needs no new prompt.
    QVERIFY(support_->pageTo(7));
    QCOMPARE(pictura::shp_info(*view).first, 7);
    QCOMPARE(go.seen().size(), 1);
}

void ShpTest::pastTenPercentIsRefusedWithoutTheCeilingSetting()
{
    pictura::ShpSupport::setGateInteractive(1);
    pictura::shp_set_test_limits(kPastTen, 0);
    support_->setAllowOverLimit(true);
    LimitClicker clicker({QStringLiteral("go"), QStringLiteral("go")});
    QVERIFY(!window_->openDocumentAtPath(shp_));
    QVERIFY(clicker.seen().isEmpty());
    QVERIFY(clicker.refused());
    QVERIFY(support_->lastLimitMessage().contains(QStringLiteral("20% over")));
    QVERIFY(support_->lastLimitMessage().contains(QStringLiteral("Remove the 10% ceiling")));
}

void ShpTest::pastTenPercentAsksTwiceWithTheCeilingSetting()
{
    pictura::ShpSupport::setGateInteractive(1);
    pictura::shp_set_test_limits(kPastTen, 0);
    support_->setAllowOverLimit(true);
    support_->setRemoveCeiling(true);
    {
        LimitClicker second({QStringLiteral("go"), QStringLiteral("cancel")});
        QVERIFY(!window_->openDocumentAtPath(shp_));
        QCOMPARE(second.seen(), (QStringList{QStringLiteral("shpLimitDialog"),
                                             QStringLiteral("shpLimitCeilingDialog")}));
        QCOMPARE(window_->documentCount(), 0);
    }
    LimitClicker both({QStringLiteral("go"), QStringLiteral("go")});
    pictura::PictureView* view = open();
    QVERIFY(view);
    QCOMPARE(both.seen(), (QStringList{QStringLiteral("shpLimitDialog"),
                                       QStringLiteral("shpLimitCeilingDialog")}));
    QVERIFY(both.gatedByBox());
    QCOMPARE(pictura::shp_info(*view).frames, 1);
}

void ShpTest::theFloorHoldsWhateverTheSettings()
{
    pictura::ShpSupport::setGateInteractive(1);
    // The estimate for one frame is 120 x the factor; one byte less is free.
    pictura::shp_set_test_limits(kWithinTen,
                                 kW * kH * 4 * pictura::shp_footprint_factor() - 1);
    support_->setAllowOverLimit(true);
    support_->setRemoveCeiling(true);
    LimitClicker clicker({QStringLiteral("go"), QStringLiteral("go")});
    QVERIFY(!window_->openDocumentAtPath(shp_));
    QVERIFY(clicker.seen().isEmpty());
    QVERIFY(clicker.refused());
    QVERIFY(support_->lastLimitMessage().contains(QStringLiteral("available now")));
    QVERIFY(support_->lastLimitMessage().contains(QStringLiteral("No setting")));
}

void ShpTest::limitSettingsAreNotKept()
{
    QWidget host;
    QWidget* page = support_->advancedPage(&host);
    auto* allow = page->findChild<QCheckBox*>(QStringLiteral("shpAllowOverLimit"));
    auto* ceiling = page->findChild<QCheckBox*>(QStringLiteral("shpRemoveCeiling"));
    QVERIFY(allow && ceiling);
    QVERIFY(!allow->isChecked());
    QVERIFY(!ceiling->isChecked());
    QVERIFY(!ceiling->isEnabled());
    allow->setChecked(true);
    QVERIFY(support_->allowOverLimit());
    QVERIFY(ceiling->isEnabled());
    ceiling->setChecked(true);
    QVERIFY(support_->removeCeiling());

    // Kooka restarts: a new main window starts with both off, and nothing
    // was written to the settings store or the state folder.
    window_.reset();
    window_ = pictura::test::makeMainWindow();
    support_ = pictura::ShpSupport::of(window_.get());
    QVERIFY(support_);
    QVERIFY(!support_->allowOverLimit());
    QVERIFY(!support_->removeCeiling());
    QSettings settings;
    for (const QString& key : settings.allKeys()) {
        QVERIFY2(!key.contains(QStringLiteral("Limit"), Qt::CaseInsensitive)
                     && !key.contains(QStringLiteral("Ceiling"), Qt::CaseInsensitive),
                 qPrintable(key));
    }
    QFile ini(QDir(qEnvironmentVariable("XDG_STATE_HOME"))
                  .filePath(QStringLiteral("kooka-pictura/shp.ini")));
    if (ini.open(QIODevice::ReadOnly)) {
        const QByteArray text = ini.readAll();
        QVERIFY(!text.contains("Limit") && !text.contains("Ceiling"));
    }
}

void ShpTest::withoutDialogsWhatNeedsConfirmingIsRefused()
{
    pictura::shp_set_test_limits(kWithinTen, 0);
    support_->setAllowOverLimit(true);
    LimitClicker clicker({QStringLiteral("go")});
    // Offscreen: refused with the reason, never confirmed.
    pictura::ShpSupport::setGateInteractive(0);
    QVERIFY(!window_->openDocumentAtPath(shp_));
    QVERIFY(support_->lastLimitMessage().contains(QStringLiteral("confirmed in a dialog")));
    // A control request, even on an interactive platform: the same, and the
    // request learns why.
    pictura::ShpSupport::setGateInteractive(1);
    {
        const pictura::ShpSupport::Quiet quiet;
        QVERIFY(!window_->openDocumentAtPath(shp_));
        QVERIFY(quiet.refusal().contains(QStringLiteral("confirmed in a dialog")));
    }
    QVERIFY(clicker.seen().isEmpty());
    QVERIFY(!clicker.refused());
    QCOMPARE(window_->documentCount(), 0);
}

void ShpTest::underTheLimitNeverAsks()
{
    pictura::ShpSupport::setGateInteractive(1);
    support_->setAllowOverLimit(true);
    LimitClicker clicker({});
    QVERIFY(open());
    QCOMPARE(pictura::shp_info(*window_->viewAt(0)).frames, kFrames);
    // Over the limit, but the whole sheet is past the ceiling: it opens as a
    // window, without asking.
    pictura::shp_set_test_limits(kPagedLimit, 0);
    QVERIFY(window_->openDocumentAtPath(shp_));
    QCOMPARE(window_->documentCount(), 2);
    QCOMPARE(pictura::shp_info(*window_->activeView()).frames, 4);
    QVERIFY(clicker.seen().isEmpty());
    QVERIFY(!clicker.refused());
}

void ShpTest::aWholeSheetOverTheLimitOffersTheWindow()
{
    pictura::ShpSupport::setGateInteractive(1);
    // 1836 bytes is 8% over 1700.
    pictura::shp_set_test_limits(1700, 0);
    support_->setAllowOverLimit(true);
    {
        LimitClicker window({QStringLiteral("alternative")});
        QVERIFY(window_->openDocumentAtPath(shp_));
        QCOMPARE(window.seen(), QStringList{QStringLiteral("shpLimitDialog")});
        const pictura::ShpInfo info = pictura::shp_info(*window_->activeView());
        QVERIFY(info.frames < kFrames);
        QCOMPARE(pictura::ShpSupport::allowanceOf(window_->activeView()), quint64(0));
    }
    LimitClicker whole({QStringLiteral("go")});
    QVERIFY(window_->openDocumentAtPath(shp_));
    QCOMPARE(whole.seen(), QStringList{QStringLiteral("shpLimitDialog")});
    pictura::PictureView* view = window_->activeView();
    QCOMPARE(pictura::shp_info(*view).frames, kFrames);
    QCOMPARE(view->document_width(), 4 * kW + 3);
    const QString out = dir_.filePath(QStringLiteral("whole.shp"));
    QVERIFY(window_->saveActiveAs(out));
    QCOMPARE(readFile(out), sprite());
}

void ShpTest::growingPastTheLimitAsksAgainPerStage()
{
    // The whole 1836-byte sheet fits 2000. 27 x 19 x 4 = 2052 is 3% over it,
    // 27 x 21 x 4 = 2268 is 14% over.
    pictura::shp_set_test_limits(2000, 0);
    pictura::PictureView* view = open();
    QVERIFY(view);
    const QString canvasSize =
        pictura::commandIdForPath({QStringLiteral("Image"), QStringLiteral("Canvas Size…")});
    const auto grow = [&](int height) {
        pictura::CommandParams params;
        params.values.insert(QStringLiteral("width"), 4 * kW + 3);
        params.values.insert(QStringLiteral("height"), height);
        params.values.insert(QStringLiteral("anchor"), QStringLiteral("top-left"));
        params.values.insert(QStringLiteral("color"), 0.0);
        return window_->registry()->executeRecorded(canvasSize, params);
    };
    pictura::ShpSupport::setGateInteractive(0);
    QVERIFY(!grow(19));
    QCOMPARE(view->document_height(), 3 * kH + 2);
    QVERIFY(support_->lastLimitMessage().contains(QStringLiteral("Preferences > Advanced")));

    pictura::ShpSupport::setGateInteractive(1);
    support_->setAllowOverLimit(true);
    {
        LimitClicker first({QStringLiteral("go")});
        QVERIFY(grow(19));
        QCOMPARE(first.seen(), QStringList{QStringLiteral("shpLimitDialog")});
        QCOMPARE(view->document_height(), 19);
    }
    {
        // Past 10%: refused without the ceiling setting...
        LimitClicker refused({QStringLiteral("go")});
        QVERIFY(!grow(21));
        QVERIFY(refused.seen().isEmpty());
        QVERIFY(refused.refused());
        QCOMPARE(view->document_height(), 19);
    }
    support_->setRemoveCeiling(true);
    // ...and with it, only the second prompt comes up again.
    LimitClicker second({QStringLiteral("go")});
    QVERIFY(grow(21));
    QCOMPARE(second.seen(), QStringList{QStringLiteral("shpLimitCeilingDialog")});
    QCOMPARE(view->document_height(), 21);
}

void ShpTest::aBiggerWindowNeedsTheAllowance()
{
    // 800 bytes holds windows of 4 (572 bytes); 5 frames (3 x 2 cells,
    // 20 x 11 x 4 = 880 bytes) are exactly 10% over.
    pictura::shp_set_test_limits(800, 0);
    pictura::PictureView* view = open();
    QVERIFY(view);
    QCOMPARE(pictura::shp_info(*view).frames, 4);
    QSpinBox* frames = support_->frames()->windowFrames();
    QTRY_VERIFY(frames->isVisible());
    QCOMPARE(frames->value(), 4);

    pictura::ShpSupport::setGateInteractive(0);
    QVERIFY(!support_->setWindowFrames(5));
    QCOMPARE(pictura::shp_info(*view).frames, 4);

    pictura::ShpSupport::setGateInteractive(1);
    support_->setAllowOverLimit(true);
    LimitClicker go({QStringLiteral("go")});
    frames->setValue(5);
    QCOMPARE(go.seen(), QStringList{QStringLiteral("shpLimitDialog")});
    pictura::ShpInfo info = pictura::shp_info(*view);
    QCOMPARE(info.frames, 5);
    QCOMPARE(info.span, 5);
    QCOMPARE(frames->value(), 5);
    // Windows of that size page without asking again.
    QVERIFY(support_->pageTo(9));
    info = pictura::shp_info(*view);
    QCOMPARE(info.first, 5);
    QCOMPARE(info.frames, 5);
    QCOMPARE(go.seen().size(), 1);
    const QString out = dir_.filePath(QStringLiteral("window5.shp"));
    QVERIFY(window_->saveActiveAs(out));
    QCOMPARE(readFile(out), sprite());
}


namespace {

int frameCount(const pictura::PictureView& view)
{
    return pictura::shp_info(view).frames;
}

} // namespace

void ShpTest::frameOpButtonsEditTheSheetAndUndo()
{
    pictura::PictureView* view = open();
    QVERIFY(view);
    pictura::ShpFramesPanel* frames = support_->frames();
    QTRY_COMPARE(frames->frameCount(), kFrames);
    frames->setSelectsCell(false);
    frames->focusFrame(2);
    const QRect c2 = frames->cellRect(2);
    const quint32 colour2 = view->composite_argb(c2.x() + 1, c2.y());
    const int history = view->history_index();

    frames->opAction(QStringLiteral("duplicate"))->trigger();
    QCOMPARE(frameCount(*view), kFrames + 1);
    QTRY_COMPARE(frames->frameCount(), kFrames + 1);
    QCOMPARE(frames->current(), 3);
    const QRect c3 = frames->cellRect(3);
    QCOMPARE(view->composite_argb(c3.x() + 1, c3.y()), colour2);
    QCOMPARE(view->history_index(), history + 1);

    frames->opAction(QStringLiteral("later"))->trigger();
    QCOMPARE(frames->current(), 4);
    const QRect c4 = frames->cellRect(4);
    QCOMPARE(view->composite_argb(c4.x() + 1, c4.y()), colour2);
    frames->opAction(QStringLiteral("delete"))->trigger();
    QCOMPARE(frameCount(*view), kFrames);
    QCOMPARE(view->history_index(), history + 3);

    // Back to the original order: the save is the original file.
    const QString out = dir_.filePath(QStringLiteral("ops.shp"));
    QVERIFY(window_->saveActiveAs(out));
    QCOMPARE(readFile(out), sprite());

    // Undo walks back through every step, the layout with it.
    QVERIFY(view->undo());
    QCOMPARE(frameCount(*view), kFrames + 1);
    QVERIFY(view->undo());
    QVERIFY(view->undo());
    QCOMPARE(frameCount(*view), kFrames);
    QCOMPARE(view->history_index(), history);
    QVERIFY(view->redo());
    QCOMPARE(frameCount(*view), kFrames + 1);
    window_->refresh();
    QTRY_COMPARE(frames->frameCount(), kFrames + 1);

    // Insert puts a blank frame after the current one.
    frames->focusFrame(0);
    frames->opAction(QStringLiteral("insert"))->trigger();
    QCOMPARE(frameCount(*view), kFrames + 2);
    QCOMPARE(frames->current(), 1);
    const QRect c1 = frames->cellRect(1);
    QCOMPARE(view->composite_argb(c1.x() + 1, c1.y()) >> 24, 0u);
}

void ShpTest::reverseUsesTheSelectedRange()
{
    pictura::PictureView* view = open();
    QVERIFY(view);
    pictura::ShpFramesPanel* frames = support_->frames();
    QTRY_COMPARE(frames->frameCount(), kFrames);
    frames->setSelectsCell(false);
    const auto colourAt = [&](int frame) {
        const QRect c = frames->cellRect(frame);
        return view->composite_argb(c.x() + 1, c.y());
    };
    const quint32 one = colourAt(1);
    const quint32 three = colourAt(3);
    frames->focusFrame(1);
    QListWidget* list = frames->list();
    for (int row = 1; row <= 3; ++row) {
        list->item(row)->setSelected(true);
    }
    QCOMPARE(frames->selectedRange(), qMakePair(1, 3));
    frames->opAction(QStringLiteral("reverse"))->trigger();
    QCOMPARE(colourAt(1), three);
    QCOMPARE(colourAt(3), one);
    QCOMPARE(frameCount(*view), kFrames);
}

void ShpTest::aFrameOpPastTheLimitIsRefusedWithoutTheSetting()
{
    pictura::PictureView* view = open();
    QVERIFY(view);
    QTRY_COMPARE(support_->frames()->frameCount(), kFrames);
    // 10 frames of 6x5: a 27x17 sheet, 1836 bytes. 12 frames stay 4x3.
    const quint64 sheet = quint64(view->document_width()) * view->document_height() * 4;
    pictura::shp_set_test_limits(sheet, 0);
    QVERIFY(support_->frameOp(QStringLiteral("insert"), kFrames, 2));
    QCOMPARE(frameCount(*view), kFrames + 2);
    // A 13th frame needs a fourth row: over the limit, refused.
    QVERIFY(!support_->frameOp(QStringLiteral("duplicate"), 0, 1));
    QCOMPARE(frameCount(*view), kFrames + 2);
    QVERIFY(support_->lastGateMessage().contains(QStringLiteral("Allow SHP files over")));
}

void ShpTest::newShpMakesBlankFramesThatSaveAndReopen()
{
    const QString pal = dir_.filePath(QStringLiteral("gtank.pal"));
    QVERIFY(support_->newShp(7, 6, 3, pal));
    pictura::PictureView* view = window_->activeView();
    QVERIFY(view);
    const pictura::ShpInfo info = pictura::shp_info(*view);
    QVERIFY(info.valid);
    QCOMPARE(info.frames, 3);
    QCOMPARE(info.width, 7);
    QCOMPARE(info.palette_name, QStringLiteral("gtank.pal"));
    QVERIFY(!info.has_source);
    window_->refresh();
    QTRY_COMPARE(support_->frames()->frameCount(), 3);
    // A dab of index 22 in frame 2, then Save As: a valid SHP that reopens.
    const rust::Vec<uint32_t> unique = pictura::shp_palette_unique(*view);
    const QRect cell = support_->frames()->cellRect(2);
    QVERIFY(dab(view, unique[22] | 0xFF000000u, cell.x() + 3, cell.y() + 2));
    const QString out = dir_.filePath(QStringLiteral("made.shp"));
    QVERIFY(window_->saveActiveAs(out));
    QVERIFY(pictura::shp_sniff_file(out));
    QVERIFY(window_->openDocumentAtPath(out));
    window_->refresh();
    pictura::PictureView* back = window_->activeView();
    QVERIFY(back && back != view);
    QCOMPARE(pictura::shp_info(*back).frames, 3);
    const QRect again = support_->frames()->cellRect(2);
    // Reopened in whatever palette resolves for it: the index is what stays.
    QCOMPARE(pictura::shp_index_of(*back, back->composite_argb(again.x() + 3, again.y() + 2)),
             22);
    // Outside the format: refused with the reason.
    QVERIFY(!support_->newShp(5000, 6, 3, pal));
    QVERIFY(support_->lastGateMessage().contains(QStringLiteral("4096")));
}

void ShpTest::newShpIsInTheFileMenu()
{
    QAction* action = support_->newShpAction();
    QVERIFY(action);
    QMenu* file = nullptr;
    for (QAction* top : window_->menuBar()->actions()) {
        if (top->text().remove(QLatin1Char('&')) == QStringLiteral("File")) {
            file = top->menu();
        }
    }
    QVERIFY(file);
    const QList<QAction*> actions = file->actions();
    const qsizetype at = actions.indexOf(action);
    QVERIFY(at > 0);
    QCOMPARE(actions.at(at - 1)->data().toString(), QStringLiteral("file.new"));
}

void ShpTest::saveAsShpConvertsAnImageIntoANewDocument()
{
    pictura::ShpSupport::setGateInteractive(1);
    // A 12x5 image: two 5x5 frames with a 2 px gap.
    QVERIFY(window_->newDocument(QStringLiteral("Plain"), 12, 5, QStringLiteral("rgb"), 8,
                                 QStringLiteral("white")));
    pictura::PictureView* plain = window_->activeView();
    QVERIFY(plain);
    const int documents = window_->documentCount();
    bool seen = false;
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, [&] {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            auto* dialog = qobject_cast<pictura::ShpConvertDialog*>(widget);
            if (!dialog || !dialog->isVisible() || seen) {
                continue;
            }
            seen = true;
            dialog->gutterBox()->setValue(2);
            dialog->colsBox()->setValue(2);
            QCOMPARE(dialog->widthBox()->value(), 5);
            QCOMPARE(dialog->framesBox()->value(), 2);
            dialog->paletteField()->edit()->setText(dir_.filePath(QStringLiteral("gtank.pal")));
            dialog->accept();
        }
    });
    poll.start(20);
    const QString out = dir_.filePath(QStringLiteral("converted.shp"));
    QVERIFY(!window_->saveActiveAs(out)); // the image itself is not saved
    QVERIFY(seen);
    QVERIFY(support_->savedCopy());
    QVERIFY(pictura::shp_sniff_file(out));
    QCOMPARE(window_->documentCount(), documents + 1);
    pictura::PictureView* made = window_->activeView();
    QVERIFY(made && made != plain);
    QCOMPARE(pictura::shp_info(*made).frames, 2);
    QCOMPARE(pictura::shp_info(*made).width, 5);
    QCOMPARE(window_->documentPath(window_->activeDocumentIndex()), out);
    QVERIFY(!pictura::shp_info(*plain).valid);
    QCOMPARE(plain->document_width(), 12);
    // White is a palette colour's nearest index: nothing is off-palette.
    QCOMPARE(pictura::shp_check(*made).off_palette, 0);
}

QTEST_MAIN(ShpTest)
#include "tst_shp.moc"
