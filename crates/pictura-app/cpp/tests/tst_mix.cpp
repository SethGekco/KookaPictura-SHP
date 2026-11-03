// SPDX-License-Identifier: GPL-3.0-or-later
// KookaPictura-SHP fork: MIX archives in the shell: the browser, opening an
// entry as a document, the save prompt, export, and palettes found inside
// archives. Every archive, sprite and palette here is generated; no game
// file is used.

#include <QtTest/QtTest>

#include "api/document_api.h"
#include "dialogs.h"
#include "frame.h"
#include "mix_support.h"
#include "panels/mix_browser_panel.h"
#include "shp_palettes.h"
#include "shp_support.h"

#include "pictura_app/src/cxxqt_object.cxxqt.h"
#include "pictura_app/src/cxxqt_object/shp.cxxqt.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtWidgets/QApplication>
#include <QtWidgets/QDockWidget>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTreeWidget>

#include <algorithm>
#include <functional>

#include "qt_test_support.h"
#include "shp_fixtures.h"

namespace {

constexpr int kCx = 60;
constexpr int kCy = 30;
constexpr quint32 kUnnamed = 0x12345678u;

quint32 crc32(const QByteArray& bytes)
{
    quint32 crc = 0xFFFFFFFFu;
    for (const char c : bytes) {
        crc ^= quint8(c);
        for (int k = 0; k < 8; ++k) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

// The Tiberian Sun / Red Alert 2 ID of a name (ModdingWiki "MIX (TS)").
quint32 tsId(const QString& name)
{
    QByteArray bytes = name.toUpper().toLatin1();
    const qsizetype len = bytes.size();
    if (len % 4 != 0) {
        const char last = bytes.at(len & ~qsizetype(3));
        bytes.append(char(len % 4));
        while (bytes.size() % 4 != 0) {
            bytes.append(last);
        }
    }
    return crc32(bytes);
}

struct Entry {
    quint32 id;
    QByteArray data;
};

// An unencrypted archive in the Red Alert 2 layout, index sorted by signed ID.
QByteArray archive(QList<Entry> entries)
{
    QByteArray index;
    QByteArray body;
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return qint32(a.id) < qint32(b.id); });
    for (const Entry& e : entries) {
        put32(index, e.id);
        put32(index, quint32(body.size()));
        put32(index, quint32(e.data.size()));
        body.append(e.data);
    }
    QByteArray out;
    put32(out, 0);
    put16(out, int(entries.size()));
    put32(out, quint32(body.size()));
    return out + index + body;
}

// A 1 x 1 template: one diamond of index 40.
QByteArray terrain()
{
    QByteArray out;
    put32(out, 1);
    put32(out, 1);
    put32(out, kCx);
    put32(out, kCy);
    put32(out, 20);
    for (const int v : {0, 0, 0, 0, 0, 0, 0, 0, 0}) {
        put32(out, quint32(v));
    }
    put32(out, 0);
    out.append(QByteArray(12, '\0'));
    out.append(QByteArray(kCx * kCy / 2, char(40)));
    return out;
}

// A palette told apart from palette() by its first colour.
QByteArray otherPalette()
{
    QByteArray out = palette();
    out[0] = char(63);
    return out;
}

// Click `button` in the visible message box named `box` when it appears;
// remember its text.
class BoxClicker {
public:
    BoxClicker(const QString& box, const QString& button)
    {
        QObject::connect(&poll_, &QTimer::timeout, [this, box, button] {
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                auto* found = qobject_cast<QMessageBox*>(widget);
                if (!found || !found->isVisible() || found->objectName() != box) {
                    continue;
                }
                seen_ = true;
                text_ = found->text() + QLatin1Char('\n') + found->informativeText();
                if (QAbstractButton* go = found->defaultButton()) {
                    defaultName_ = go->objectName();
                }
                if (auto* target = found->findChild<QPushButton*>(button)) {
                    target->click();
                }
            }
        });
        poll_.start(20);
    }
    bool seen() const { return seen_; }
    QString text() const { return text_; }
    QString defaultButton() const { return defaultName_; }

private:
    QTimer poll_;
    bool seen_ = false;
    QString text_;
    QString defaultName_;
};

// Answer the first visible file dialog with `path`. The dialog selects the
// document's own file name once its folder has loaded, so it is answered
// after it settles, with the name typed into its file-name field.
class FileDialogAnswer {
public:
    explicit FileDialogAnswer(const QString& path)
    {
        QObject::connect(&poll_, &QTimer::timeout, [this, path] {
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                auto* dialog = qobject_cast<QFileDialog*>(widget);
                if (!dialog || !dialog->isVisible() || answered_) {
                    continue;
                }
                if (!seen_) {
                    seen_ = true;
                    shown_.start();
                }
                if (shown_.elapsed() < 500) {
                    continue;
                }
                answered_ = true;
                dialog->setDirectory(QFileInfo(path).absolutePath());
                dialog->selectFile(QFileInfo(path).fileName());
                if (auto* name = dialog->findChild<QLineEdit*>(QStringLiteral("fileNameEdit"))) {
                    name->setText(QFileInfo(path).fileName());
                }
                // QFileDialog::accept, through QDialog's public slot.
                static_cast<QDialog*>(dialog)->accept();
            }
        });
        poll_.start(20);
    }
    bool seen() const { return seen_; }

private:
    QTimer poll_;
    QElapsedTimer shown_;
    bool seen_ = false;
    bool answered_ = false;
};

} // namespace

class MixTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();
    void openingAnArchiveListsItInTheBrowser();
    void theFilterNarrowsTheRows();
    void aBrokenArchiveSaysWhy();
    void anEntryOpensTitledWithItsArchive();
    void onlySpritesAndTemplatesOpen();
    void saveAsksAndDefaultsToALooseFile();
    void saveAsLooseFileDropsTheArchiveFromTheTitle();
    void replaceInACopyLeavesTheArchiveAlone();
    void replacingInPlaceNeedsAConfirmationNamingTheFile();
    void saveFromAnArchiveIsRefusedWithoutDialogs();
    void exportWritesTheSelectedEntries();
    void aSpriteFromAnArchiveFindsItsPaletteThere();
    void aTemplateFromAnArchiveFindsItsTheaterPalette();
    void preferencesListArchivesForLoosePalettes();
    void theOpenFiltersOfferMix();

private:
    bool openGame();
    pictura::PictureView* openEntry(const QString& name);

    pictura::test::ScopedStateHome stateHome_;
    QTemporaryDir dir_;
    QString game_;
    QByteArray gameBytes_;
    std::unique_ptr<pictura::PicturaMainWindow> window_;
    pictura::MixSupport* mix_ = nullptr;
};

void MixTest::init()
{
    QVERIFY(dir_.isValid());
    const QByteArray cache = archive({{tsId(QStringLiteral("unittem.pal")), palette()}});
    gameBytes_ = archive({
        {tsId(QStringLiteral("gi.shp")), sprite()},
        {kUnnamed, sprite()},
        {tsId(QStringLiteral("clear01.tem")), terrain()},
        {tsId(QStringLiteral("isotem.pal")), otherPalette()},
        {tsId(QStringLiteral("cache.mix")), cache},
    });
    game_ = dir_.filePath(QStringLiteral("game.mix"));
    QVERIFY(writeFile(game_, gameBytes_));
    window_ = pictura::test::makeMainWindow();
    window_->resize(1200, 900);
    window_->show();
    mix_ = pictura::MixSupport::of(window_.get());
    QVERIFY(mix_);
}

void MixTest::cleanup()
{
    pictura::MixSupport::setInteractiveForTest(-1);
    pictura::MixSupport::setCopyPathForTest(QString());
    pictura::setMixPaletteSources({});
    for (const QString& archive : mix_->openArchives()) {
        mix_->closeArchive(archive);
    }
    window_.reset();
    QFile::remove(dir_.filePath(QStringLiteral("game-edited.mix")));
}

bool MixTest::openGame()
{
    QSignalSpy finished(mix_, &pictura::MixSupport::listingFinished);
    // File > Open of a .mix shows it in the browser; no document opens.
    if (window_->startOpenDocumentAtPath(game_) != 0) {
        return false;
    }
    return (finished.count() > 0 || finished.wait(10000))
           && finished.first().at(1).toBool();
}

pictura::PictureView* MixTest::openEntry(const QString& name)
{
    if (!mix_->browser()->selectEntryNamed(name)) {
        return nullptr;
    }
    const QList<pictura::MixEntryRef> entries = mix_->browser()->selectedEntries();
    if (entries.size() != 1 || !mix_->openEntry(entries.first())) {
        return nullptr;
    }
    window_->waitForOpensForTest();
    window_->refresh();
    return window_->documentCount() > 0 ? window_->viewAt(window_->documentCount() - 1)
                                        : nullptr;
}

void MixTest::openingAnArchiveListsItInTheBrowser()
{
    QVERIFY(pictura::mix_sniff_file(game_));
    QVERIFY(openGame());
    QCOMPARE(window_->documentCount(), 0);
    QTRY_VERIFY(mix_->dock()->isVisible());
    QTreeWidget* tree = mix_->browser()->tree();
    QCOMPARE(tree->topLevelItemCount(), 1);
    QTreeWidgetItem* top = tree->topLevelItem(0);
    QCOMPARE(top->text(0), QStringLiteral("game.mix"));
    QCOMPARE(top->childCount(), 5);
    QCOMPARE(mix_->browser()->visibleEntryCount(), 6);
    QStringList names;
    QTreeWidgetItem* cache = nullptr;
    for (int i = 0; i < top->childCount(); ++i) {
        names << top->child(i)->text(0) + QLatin1Char(' ') + top->child(i)->text(1);
        if (top->child(i)->text(0) == QStringLiteral("cache.mix")) {
            cache = top->child(i);
        }
    }
    names.sort();
    QCOMPARE(names, QStringList({QStringLiteral("#12345678 SHP"), QStringLiteral("cache.mix MIX"),
                                 QStringLiteral("clear01.tem TMP"), QStringLiteral("gi.shp SHP"),
                                 QStringLiteral("isotem.pal PAL")}));
    QVERIFY(cache);
    QCOMPARE(cache->childCount(), 1);
    QCOMPARE(cache->child(0)->text(0), QStringLiteral("unittem.pal"));
    QCOMPARE(cache->child(0)->text(3), QStringLiteral("%1").arg(tsId(QStringLiteral("unittem.pal")),
                                                              8, 16, QLatin1Char('0')).toUpper());
    QCOMPARE(mix_->openArchives(), QStringList{QFileInfo(game_).absoluteFilePath()});
}

void MixTest::theFilterNarrowsTheRows()
{
    QVERIFY(openGame());
    pictura::MixBrowserPanel* browser = mix_->browser();
    browser->filter()->setText(QStringLiteral("pal"));
    // isotem.pal, and unittem.pal with cache.mix kept to show it.
    QCOMPARE(browser->visibleEntryCount(), 3);
    browser->filter()->setText(QStringLiteral("12345678"));
    QCOMPARE(browser->visibleEntryCount(), 1);
    browser->filter()->setText(QStringLiteral("tmp"));
    QCOMPARE(browser->visibleEntryCount(), 1);
    browser->filter()->clear();
    QCOMPARE(browser->visibleEntryCount(), 6);
}

void MixTest::aBrokenArchiveSaysWhy()
{
    const QString bad = dir_.filePath(QStringLiteral("bad.mix"));
    QVERIFY(writeFile(bad, QByteArray("not an archive")));
    QSignalSpy finished(mix_, &pictura::MixSupport::listingFinished);
    QVERIFY(mix_->openArchive(bad));
    QVERIFY(finished.wait(10000));
    QVERIFY(!finished.first().at(1).toBool());
    QVERIFY(mix_->lastMessage().contains(QStringLiteral("Could not read")));
    QCOMPARE(mix_->browser()->tree()->topLevelItemCount(), 0);
}

void MixTest::anEntryOpensTitledWithItsArchive()
{
    QVERIFY(openGame());
    pictura::PictureView* view = openEntry(QStringLiteral("gi.shp"));
    QVERIFY(view);
    QCOMPARE(window_->documentCount(), 1);
    QCOMPARE(window_->documentName(0), QStringLiteral("gi.shp in game.mix"));
    const QString path = pictura::document_api::filePath(*view);
    QVERIFY(pictura::isMixMember(path));
    QCOMPARE(pictura::mixOrigin(path).archive, QFileInfo(game_).absoluteFilePath());
    QCOMPARE(pictura::shp_info(*view).frames, kFrames);
    // An unnamed entry opens under its ID.
    QVERIFY(openEntry(QStringLiteral("#12345678")));
    QCOMPARE(window_->documentName(1), QStringLiteral("12345678.shp in game.mix"));
    // A template too.
    QVERIFY(openEntry(QStringLiteral("clear01.tem")));
    QCOMPARE(window_->documentName(2), QStringLiteral("clear01.tem in game.mix"));
}

void MixTest::onlySpritesAndTemplatesOpen()
{
    QVERIFY(openGame());
    QVERIFY(mix_->browser()->selectEntryNamed(QStringLiteral("isotem.pal")));
    QVERIFY(!mix_->openEntry(mix_->browser()->selectedEntries().first()));
    QVERIFY(mix_->lastMessage().contains(QStringLiteral("Export")));
    QCOMPARE(window_->documentCount(), 0);
}

void MixTest::saveAsksAndDefaultsToALooseFile()
{
    QVERIFY(openGame());
    QVERIFY(openEntry(QStringLiteral("gi.shp")));
    pictura::MixSupport::setInteractiveForTest(1);
    BoxClicker clicker(QStringLiteral("mixSaveChoice"), QStringLiteral("mixSaveCancel"));
    QVERIFY(!window_->saveActive());
    QVERIFY(clicker.seen());
    QCOMPARE(clicker.defaultButton(), QStringLiteral("mixSaveLoose"));
    QVERIFY(clicker.text().contains(QStringLiteral("game.mix")));
    QCOMPARE(readFile(game_), gameBytes_);
}

void MixTest::saveAsLooseFileDropsTheArchiveFromTheTitle()
{
    QVERIFY(openGame());
    QVERIFY(openEntry(QStringLiteral("gi.shp")));
    pictura::MixSupport::setInteractiveForTest(1);
    const QString loose = dir_.filePath(QStringLiteral("gi-loose.shp"));
    BoxClicker clicker(QStringLiteral("mixSaveChoice"), QStringLiteral("mixSaveLoose"));
    FileDialogAnswer answer(loose);
    QVERIFY(window_->saveActive());
    QVERIFY(clicker.seen() && answer.seen());
    QVERIFY2(QFileInfo::exists(loose),
             qPrintable(QDir(dir_.path()).entryList(QDir::Files).join(QLatin1Char(' '))
                        + QLatin1Char('|') + window_->documentName(0) + QLatin1Char('|')
                        + window_->activeFilePath()));
    QCOMPARE(readFile(loose), sprite());
    QCOMPARE(window_->documentName(0), QStringLiteral("gi-loose.shp"));
    QCOMPARE(readFile(game_), gameBytes_);
}

void MixTest::replaceInACopyLeavesTheArchiveAlone()
{
    QVERIFY(openGame());
    QVERIFY(openEntry(QStringLiteral("gi.shp")));
    pictura::MixSupport::setInteractiveForTest(1);
    const QString copy = dir_.filePath(QStringLiteral("game-edited.mix"));
    pictura::MixSupport::setCopyPathForTest(copy);
    BoxClicker clicker(QStringLiteral("mixSaveChoice"), QStringLiteral("mixSaveCopy"));
    QVERIFY(window_->saveActive());
    QVERIFY(clicker.seen());
    QCOMPARE(readFile(game_), gameBytes_);
    // An unchanged entry saves byte for byte, so the copy is the archive.
    QCOMPARE(readFile(copy), gameBytes_);
    QCOMPARE(window_->documentName(0), QStringLiteral("gi.shp in game-edited.mix"));
    QCOMPARE(pictura::mixOrigin(pictura::document_api::filePath(*window_->viewAt(0))).archive,
             QFileInfo(copy).absoluteFilePath());
    QVERIFY(mix_->lastMessage().contains(QStringLiteral("not encrypted")));
}

void MixTest::replacingInPlaceNeedsAConfirmationNamingTheFile()
{
    QVERIFY(openGame());
    pictura::PictureView* view = openEntry(QStringLiteral("gi.shp"));
    QVERIFY(view);
    pictura::MixSupport::setInteractiveForTest(1);
    {
        BoxClicker clicker(QStringLiteral("mixReplaceInPlace"),
                           QStringLiteral("mixReplaceCancel"));
        QVERIFY(!mix_->replaceInCopy(view, game_));
        QVERIFY(clicker.seen());
        QVERIFY(clicker.text().contains(QFileInfo(game_).absoluteFilePath()));
        QCOMPARE(clicker.defaultButton(), QStringLiteral("mixReplaceCancel"));
        QCOMPARE(readFile(game_), gameBytes_);
    }
    {
        BoxClicker clicker(QStringLiteral("mixReplaceInPlace"), QStringLiteral("mixReplaceGo"));
        QVERIFY(mix_->replaceInCopy(view, game_));
        QVERIFY(clicker.seen());
        // Written through a partial file; unchanged, so the same bytes.
        QCOMPARE(readFile(game_), gameBytes_);
        QVERIFY(!QFileInfo::exists(game_ + QStringLiteral(".kooka-partial")));
    }
    pictura::MixSupport::setInteractiveForTest(0);
    QVERIFY(!mix_->replaceInCopy(view, game_));
    QVERIFY(mix_->lastMessage().contains(QStringLiteral("confirmation")));
}

void MixTest::saveFromAnArchiveIsRefusedWithoutDialogs()
{
    QVERIFY(openGame());
    QVERIFY(openEntry(QStringLiteral("gi.shp")));
    pictura::MixSupport::setInteractiveForTest(0);
    QVERIFY(!window_->saveActive());
    QVERIFY(mix_->lastMessage().contains(QStringLiteral("Save As")));
    QCOMPARE(readFile(game_), gameBytes_);
}

void MixTest::exportWritesTheSelectedEntries()
{
    QVERIFY(openGame());
    QTreeWidget* tree = mix_->browser()->tree();
    QTreeWidgetItem* top = tree->topLevelItem(0);
    for (int i = 0; i < top->childCount(); ++i) {
        const QString name = top->child(i)->text(0);
        top->child(i)->setSelected(name == QStringLiteral("gi.shp")
                                   || name == QStringLiteral("#12345678")
                                   || name == QStringLiteral("cache.mix"));
    }
    // Archive rows are left out unless asked for.
    QCOMPARE(mix_->browser()->selectedEntries().size(), 2);
    const QList<pictura::MixEntryRef> entries = mix_->browser()->selectedEntries(true);
    QCOMPARE(entries.size(), 3);
    QTemporaryDir out;
    QCOMPARE(mix_->exportEntries(entries, out.path()), 3);
    QCOMPARE(readFile(out.filePath(QStringLiteral("gi.shp"))), sprite());
    QCOMPARE(readFile(out.filePath(QStringLiteral("12345678.shp"))), sprite());
    QVERIFY(pictura::mix_sniff_file(out.filePath(QStringLiteral("cache.mix"))));
    // A second export never writes over the first.
    QCOMPARE(mix_->exportEntries(entries.mid(0, 1), out.path()), 1);
    QVERIFY(QFileInfo::exists(out.filePath(QStringLiteral("%1 (2).%2")
                                               .arg(QFileInfo(entries.first().fileName)
                                                        .completeBaseName(),
                                                    QFileInfo(entries.first().fileName).suffix()))));
}

void MixTest::aSpriteFromAnArchiveFindsItsPaletteThere()
{
    QVERIFY(openGame());
    pictura::PictureView* view = openEntry(QStringLiteral("gi.shp"));
    QVERIFY(view);
    const pictura::ShpInfo info = pictura::shp_info(*view);
    QCOMPARE(info.palette_name, QStringLiteral("unittem.pal"));
    QCOMPARE(info.palette_path,
             QFileInfo(game_).absoluteFilePath() + QStringLiteral("/cache.mix/unittem.pal"));
}

void MixTest::aTemplateFromAnArchiveFindsItsTheaterPalette()
{
    QVERIFY(openGame());
    pictura::PictureView* view = openEntry(QStringLiteral("clear01.tem"));
    QVERIFY(view);
    const pictura::TmpInfo info = pictura::tmp_info(*view);
    QVERIFY(info.valid);
    QCOMPARE(info.palette_name, QStringLiteral("isotem.pal"));
}

void MixTest::preferencesListArchivesForLoosePalettes()
{
    // A loose sprite and template with no .pal anywhere near them.
    QTemporaryDir loose;
    const QString shp = loose.filePath(QStringLiteral("gi.shp"));
    const QString tem = loose.filePath(QStringLiteral("clear01.tem"));
    QVERIFY(writeFile(shp, sprite()));
    QVERIFY(writeFile(tem, terrain()));
    QCOMPARE(pictura::resolveShpPalette(shp), QString());
    QCOMPARE(pictura::resolveTmpPalette(tem), QString());
    const QString archive = QFileInfo(game_).absoluteFilePath();
    // A folder stands for its archives.
    pictura::setMixPaletteSources({dir_.path()});
    QCOMPARE(pictura::resolveShpPalette(shp), archive + QStringLiteral("/cache.mix/unittem.pal"));
    QCOMPARE(pictura::resolveTmpPalette(tem), archive + QStringLiteral("/isotem.pal"));
    QVERIFY(pictura::palettePathExists(pictura::resolveShpPalette(shp)));
    // The Preferences page shows the list and edits it.
    std::unique_ptr<QWidget> page(mix_->palettePage(nullptr));
    auto* list = page->findChild<QListWidget*>(QStringLiteral("mixPaletteSources"));
    QVERIFY(list);
    QCOMPARE(list->count(), 1);
    list->setCurrentRow(0);
    page->findChild<QPushButton*>(QStringLiteral("mixPaletteRemove"))->click();
    QVERIFY(pictura::mixPaletteSources().isEmpty());
    QCOMPARE(pictura::resolveShpPalette(shp), QString());
    // An archive open in the browser is searched too.
    QVERIFY(openGame());
    QCOMPARE(pictura::resolveShpPalette(shp), archive + QStringLiteral("/cache.mix/unittem.pal"));
}

void MixTest::theOpenFiltersOfferMix()
{
    QVERIFY(pictura::openFileFilters().first().contains(QStringLiteral("*.MIX")));
    QVERIFY(pictura::openFileFilters().contains(QStringLiteral("Westwood MIX archive (*.MIX)")));
}

QTEST_MAIN(MixTest)
#include "tst_mix.moc"
