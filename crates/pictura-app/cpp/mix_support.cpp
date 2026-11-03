// SPDX-License-Identifier: GPL-3.0-or-later
// KookaPictura-SHP fork: MIX archives in the shell (see mix_support.h).

#include "mix_support.h"

#include "api/document_api.h"
#include "frame.h"
#include "panels/mix_browser_panel.h"
#include "shp_palettes.h"
#include "shp_support.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QHash>
#include <QtCore/QPointer>
#include <QtCore/QRegularExpression>
#include <QtCore/QSettings>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtGui/QGuiApplication>
#include <QtWidgets/QDockWidget>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QStatusBar>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <tuple>

#include "pictura_app/src/cxxqt_object.cxxqt.h"
#include "pictura_app/src/cxxqt_object/shp.cxxqt.h"

namespace pictura {

namespace {

QHash<QString, MixOrigin>& members()
{
    static QHash<QString, MixOrigin> table;
    return table;
}

QStringList& browserArchives()
{
    static QStringList list;
    return list;
}

QString g_copyPath;
int g_interactive = -1;

QString key(const QString& path)
{
    return QFileInfo(path).absoluteFilePath();
}

QSettings settings()
{
    return QSettings(shpSettingsPath(), QSettings::IniFormat);
}

::rust::Slice<const uint32_t> slice(const QVector<quint32>& chain)
{
    return ::rust::Slice<const uint32_t>(chain.constData(), size_t(chain.size()));
}

// "ra2.mix/conquer.mix": the archive and the nested archives holding the
// entry.
QString containerLabel(const MixOrigin& origin)
{
    QStringList parts{QFileInfo(origin.archive).fileName()};
    parts << origin.members.mid(0, origin.members.size() - 1);
    return parts.join(QLatin1Char('/'));
}

// The games' override order for the archives in a folder: expandmdNN
// (highest first), expandNN (highest first), ra2md, ra2, the rest by name.
std::tuple<int, int, QString> overrideKey(const QString& file)
{
    const QString name = file.toLower();
    static const QRegularExpression numbered(QStringLiteral("^(expandmd|expand)(\\d+)\\.mix$"));
    if (const auto match = numbered.match(name); match.hasMatch()) {
        return {match.captured(1) == QLatin1String("expandmd") ? 0 : 1,
                -match.captured(2).toInt(), name};
    }
    if (name == QLatin1String("ra2md.mix")) {
        return {2, 0, name};
    }
    if (name == QLatin1String("ra2.mix")) {
        return {3, 0, name};
    }
    return {4, 0, name};
}

} // namespace

bool isMixPath(const QString& path)
{
    return QFileInfo(path).suffix().compare(QStringLiteral("mix"), Qt::CaseInsensitive) == 0;
}

bool isMixMember(const QString& path)
{
    return !path.isEmpty() && members().contains(key(path));
}

MixOrigin mixOrigin(const QString& path)
{
    return members().value(key(path));
}

QString mixMemberTitle(const QString& path)
{
    const QString name = QFileInfo(path).fileName();
    if (!isMixMember(path)) {
        return name;
    }
    return QObject::tr("%1 in %2").arg(name, containerLabel(mixOrigin(path)));
}

QStringList mixPaletteSources()
{
    return settings().value(QStringLiteral("MixPaletteSources")).toStringList();
}

void setMixPaletteSources(const QStringList& sources)
{
    settings().setValue(QStringLiteral("MixPaletteSources"), sources);
}

QStringList paletteArchives(const QString& path)
{
    QStringList out;
    const auto add = [&out](const QString& archive) {
        const QString absolute = key(archive);
        if (QFileInfo(absolute).isFile() && !out.contains(absolute)) {
            out << absolute;
        }
    };
    if (isMixMember(path)) {
        add(mixOrigin(path).archive);
    }
    for (const QString& archive : browserArchives()) {
        add(archive);
    }
    for (const QString& source : mixPaletteSources()) {
        if (QFileInfo(source).isDir()) {
            QStringList files =
                QDir(source).entryList({QStringLiteral("*.mix"), QStringLiteral("*.MIX")},
                                       QDir::Files);
            std::sort(files.begin(), files.end(), [](const QString& a, const QString& b) {
                return overrideKey(a) < overrideKey(b);
            });
            for (const QString& file : files) {
                add(QDir(source).filePath(file));
            }
        } else {
            add(source);
        }
    }
    return out;
}

QString findInArchives(const QStringList& archives, const QString& name)
{
    for (const QString& archive : archives) {
        if (const QString found = mix_find(archive, name); !found.isEmpty()) {
            return found;
        }
    }
    return QString();
}

QString findPalettePrefixInArchives(const QStringList& archives, const QString& stem)
{
    QString best;
    qsizetype bestLength = 0;
    for (const QString& archive : archives) {
        const QString found = mix_find_prefix(archive, stem, QStringLiteral(".pal"));
        const qsizetype length = QFileInfo(found).completeBaseName().size();
        if (!found.isEmpty() && length > bestLength) {
            best = found;
            bestLength = length;
        }
    }
    return best;
}

bool palettePathExists(const QString& path)
{
    return !path.isEmpty() && (QFileInfo(path).isFile() || mix_path_exists(path));
}

// --- MixSupport -----------------------------------------------------------

MixSupport::MixSupport(QMainWindow* frame)
    : QObject(frame)
    , frame_(frame)
{
    setObjectName(QStringLiteral("mixSupport"));
    dock_ = new QDockWidget(tr("MIX Browser"), frame);
    dock_->setObjectName(QStringLiteral("mixDock"));
    browser_ = new MixBrowserPanel(dock_);
    dock_->setWidget(browser_);
    dock_->hide();
    connect(browser_, &MixBrowserPanel::openArchiveRequested, this, &MixSupport::openWithDialog);
    connect(browser_, &MixBrowserPanel::closeArchiveRequested, this, &MixSupport::closeArchive);
    connect(browser_, &MixBrowserPanel::entryActivated, this,
            [this](const MixEntryRef& entry) { openEntry(entry); });
    connect(browser_, &MixBrowserPanel::exportRequested, this, &MixSupport::exportWithDialog);
    connect(browser_, &MixBrowserPanel::message, this, &MixSupport::report);
}

MixSupport::~MixSupport()
{
    for (QThread* thread : findChildren<QThread*>()) {
        thread->wait();
    }
}

MixSupport* MixSupport::install(QMainWindow* frame)
{
    if (MixSupport* existing = of(frame)) {
        return existing;
    }
    return new MixSupport(frame);
}

MixSupport* MixSupport::of(const QObject* frame)
{
    return frame ? frame->findChild<MixSupport*>(QStringLiteral("mixSupport"),
                                                 Qt::FindDirectChildrenOnly)
                 : nullptr;
}

void MixSupport::setCopyPathForTest(const QString& path)
{
    g_copyPath = path;
}

void MixSupport::setInteractiveForTest(int interactive)
{
    g_interactive = interactive;
}

bool MixSupport::interactive() const
{
    return g_interactive < 0 ? QGuiApplication::platformName() != QLatin1String("offscreen")
                             : g_interactive == 1;
}

void MixSupport::report(const QString& text)
{
    message_ = text;
    if (frame_ && frame_->statusBar()) {
        frame_->statusBar()->showMessage(text, 8000);
    }
}

bool MixSupport::openArchive(const QString& path)
{
    const QString archive = key(path);
    if (!QFileInfo(archive).isFile()) {
        report(tr("%1 is not a file.").arg(path));
        return false;
    }
    dock_->show();
    dock_->raise();
    report(tr("Reading %1…").arg(QFileInfo(archive).fileName()));
    ++pending_;
    // The index and every entry's first bytes are read on a worker: a
    // large archive on a slow disk takes seconds.
    auto listing = std::make_shared<MixListing>();
    QThread* thread = QThread::create([archive, listing] { *listing = mix_list(archive); });
    thread->setParent(this);
    connect(thread, &QThread::finished, this, [this, thread, archive, listing] {
        --pending_;
        thread->deleteLater();
        const QString error(listing->error);
        if (!error.isEmpty()) {
            report(tr("Could not read the MIX archive: %1").arg(error));
            emit listingFinished(archive, false);
            return;
        }
        browser_->setListing(archive, *listing);
        if (!browserArchives().contains(archive)) {
            browserArchives() << archive;
        }
        report(tr("%1: %2").arg(QFileInfo(archive).fileName(), QString(listing->format)));
        emit listingFinished(archive, true);
    });
    thread->start();
    return true;
}

void MixSupport::closeArchive(const QString& path)
{
    browser_->removeArchive(key(path));
    browserArchives().removeAll(key(path));
}

QStringList MixSupport::openArchives() const
{
    return browserArchives();
}

void MixSupport::openWithDialog()
{
    const QString path = QFileDialog::getOpenFileName(
        frame_, tr("Open MIX Archive"), QString(),
        tr("Westwood MIX archives (*.mix *.MIX);;All files (*)"));
    if (!path.isEmpty()) {
        openArchive(path);
    }
}

bool MixSupport::openEntry(const MixEntryRef& entry)
{
    if (entry.kind != QLatin1String("SHP") && entry.kind != QLatin1String("TMP")) {
        report(tr("%1 is %2 data: only SHP sprites and TMP templates open as documents. Export "
                  "it to keep a copy.")
                   .arg(entry.fileName, entry.kind));
        return false;
    }
    auto* window = qobject_cast<PicturaMainWindow*>(frame_);
    auto dir = std::make_unique<QTemporaryDir>(QDir::tempPath()
                                               + QStringLiteral("/kooka-mix-XXXXXX"));
    if (!window || !dir->isValid()) {
        report(tr("Could not make a folder for %1.").arg(entry.fileName));
        return false;
    }
    const QString file = dir->filePath(entry.fileName);
    if (!mix_extract(entry.archive, slice(entry.chain), file)) {
        report(tr("Could not read %1: %2").arg(entry.fileName, QString(shp_last_error())));
        return false;
    }
    members().insert(key(file), MixOrigin{entry.archive, entry.chain, entry.members});
    extracted_.push_back(std::move(dir));
    return window->startOpenImagePath(file) != 0;
}

int MixSupport::exportEntries(const QList<MixEntryRef>& entries, const QString& folder)
{
    int written = 0;
    QStringList failed;
    for (const MixEntryRef& entry : entries) {
        QString target = QDir(folder).filePath(entry.fileName);
        // Never over an existing file: "name (2).shp".
        for (int n = 2; QFileInfo::exists(target); ++n) {
            const QFileInfo info(entry.fileName);
            target = QDir(folder).filePath(QStringLiteral("%1 (%2)%3").arg(
                info.completeBaseName(), QString::number(n),
                info.suffix().isEmpty() ? QString() : QLatin1Char('.') + info.suffix()));
        }
        if (mix_extract(entry.archive, slice(entry.chain), target)) {
            ++written;
        } else {
            failed << entry.fileName;
        }
    }
    report(failed.isEmpty() ? tr("Exported %n entries to %1.", nullptr, written).arg(folder)
                            : tr("Exported %1 of %2 entries; could not write %3: %4")
                                  .arg(written)
                                  .arg(entries.size())
                                  .arg(failed.join(QStringLiteral(", ")),
                                       QString(shp_last_error())));
    return written;
}

void MixSupport::exportWithDialog(const QList<MixEntryRef>& entries)
{
    const QString folder =
        QFileDialog::getExistingDirectory(frame_, tr("Export to Folder"), QString());
    if (!folder.isEmpty()) {
        exportEntries(entries, folder);
    }
}

bool MixSupport::saveMember(PictureView* view)
{
    auto* window = qobject_cast<PicturaMainWindow*>(frame_);
    const QString path = view ? document_api::filePath(*view) : QString();
    if (!window || !isMixMember(path)) {
        return false;
    }
    const MixOrigin origin = mixOrigin(path);
    const QString entry = QFileInfo(path).fileName();
    const QString where = containerLabel(origin);
    if (!interactive()) {
        report(tr("%1 came from inside %2. Save it with Save As as a loose file, or choose "
                  "Replace in a Copy of the MIX when saving with dialogs.")
                   .arg(entry, where));
        return false;
    }
    QMessageBox box(QMessageBox::Question, tr("Save %1").arg(entry),
                    tr("%1 came from inside %2.").arg(entry, where), QMessageBox::NoButton,
                    frame_);
    box.setObjectName(QStringLiteral("mixSaveChoice"));
    box.setInformativeText(tr("Save it as a loose file, or write a copy of %1 with this file "
                              "replaced. %1 itself is not changed.")
                               .arg(QFileInfo(origin.archive).fileName()));
    QPushButton* loose = box.addButton(tr("Save As Loose File…"), QMessageBox::AcceptRole);
    loose->setObjectName(QStringLiteral("mixSaveLoose"));
    QPushButton* copy = box.addButton(tr("Replace in a Copy of the MIX…"), QMessageBox::ActionRole);
    copy->setObjectName(QStringLiteral("mixSaveCopy"));
    box.addButton(QMessageBox::Cancel)->setObjectName(QStringLiteral("mixSaveCancel"));
    box.setDefaultButton(loose);
    box.exec();
    if (box.clickedButton() == loose) {
        return window->saveAsWithDialog();
    }
    if (box.clickedButton() != copy) {
        return false;
    }
    QString out = g_copyPath;
    if (out.isEmpty()) {
        const QString folder =
            settings().value(QStringLiteral("MixCopyFolder"), QDir::homePath()).toString();
        out = QFileDialog::getSaveFileName(
            frame_, tr("Replace in a Copy of the MIX"),
            QDir(folder).filePath(QFileInfo(origin.archive).completeBaseName()
                                  + QStringLiteral("-edited.mix")),
            tr("Westwood MIX archives (*.mix *.MIX)"));
        if (out.isEmpty()) {
            return false;
        }
        settings().setValue(QStringLiteral("MixCopyFolder"), QFileInfo(out).absolutePath());
    }
    return replaceInCopy(view, out);
}

bool MixSupport::replaceInCopy(PictureView* view, const QString& out)
{
    auto* window = qobject_cast<PicturaMainWindow*>(frame_);
    const QString path = view ? document_api::filePath(*view) : QString();
    if (!window || !isMixMember(path) || out.isEmpty()) {
        report(tr("This document did not come from a MIX archive."));
        return false;
    }
    MixOrigin origin = mixOrigin(path);
    const QString entry = QFileInfo(path).fileName();
    const QString name = QFileInfo(origin.archive).fileName();
    const QFileInfo target(out);
    if (target.exists()
        && target.canonicalFilePath() == QFileInfo(origin.archive).canonicalFilePath()) {
        if (!interactive()) {
            report(tr("Writing %1 in place needs a confirmation; choose a new file.")
                       .arg(origin.archive));
            return false;
        }
        QMessageBox box(QMessageBox::Warning, tr("Replace %1 in Place?").arg(name),
                        tr("This rewrites %1 itself:\n%2").arg(name, origin.archive),
                        QMessageBox::NoButton, frame_);
        box.setObjectName(QStringLiteral("mixReplaceInPlace"));
        box.setInformativeText(
            tr("Its other entries are copied unchanged, but the archives holding %1 are "
               "written without encryption, and there is no undo. Keep a backup of %2.")
                .arg(entry, name));
        QPushButton* go = box.addButton(tr("Replace %1").arg(name), QMessageBox::DestructiveRole);
        go->setObjectName(QStringLiteral("mixReplaceGo"));
        QPushButton* cancel = box.addButton(QMessageBox::Cancel);
        cancel->setObjectName(QStringLiteral("mixReplaceCancel"));
        box.setDefaultButton(cancel);
        box.exec();
        if (box.clickedButton() != go) {
            return false;
        }
    }
    ShpSupport* shp = ShpSupport::of(frame_);
    if (!shp || !shp->saveGate(view, interactive(), path)) {
        report(shp ? shp->lastGateMessage() : tr("The document cannot be saved."));
        return false;
    }
    if (!document_api::save(*view, path)) {
        report(tr("Could not write %1: %2").arg(entry, shp ? shp->lastSaveError() : QString()));
        return false;
    }
    if (!mix_replace(origin.archive, slice(origin.chain), path, out)) {
        report(tr("Could not write %1: %2").arg(out, QString(shp_last_error())));
        return false;
    }
    origin.archive = key(out);
    members().insert(key(path), origin);
    window->refresh(); // the title now names the copy (documentDisplayName)
    report(tr("Wrote %1 with %2 replaced (its index is not encrypted).")
               .arg(target.fileName(), entry));
    if (browserArchives().contains(key(out))) {
        openArchive(out);
    }
    return true;
}

QWidget* MixSupport::palettePage(QWidget* parent)
{
    auto* page = new QWidget(parent);
    page->setObjectName(QStringLiteral("mixPalettePage"));
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* heading = new QLabel(tr("<b>Palettes from MIX archives</b>"), page);
    auto* about = new QLabel(
        tr("When no loose .pal file fits a sprite or terrain template, its palette is looked "
           "for inside these MIX archives (a folder stands for the .mix files in it, in the "
           "game's override order), after the archive the document came from and the "
           "archives open in the MIX browser. Add your game folder to find unittem.pal and "
           "isotem.pal."),
        page);
    about->setWordWrap(true);
    auto* list = new QListWidget(page);
    list->setObjectName(QStringLiteral("mixPaletteSources"));
    list->addItems(mixPaletteSources());
    auto* buttons = new QHBoxLayout;
    auto* addFile = new QPushButton(tr("Add MIX File…"), page);
    addFile->setObjectName(QStringLiteral("mixPaletteAddFile"));
    auto* addFolder = new QPushButton(tr("Add Folder…"), page);
    addFolder->setObjectName(QStringLiteral("mixPaletteAddFolder"));
    auto* remove = new QPushButton(tr("Remove"), page);
    remove->setObjectName(QStringLiteral("mixPaletteRemove"));
    buttons->addWidget(addFile);
    buttons->addWidget(addFolder);
    buttons->addWidget(remove);
    buttons->addStretch(1);
    layout->addWidget(heading);
    layout->addWidget(about);
    layout->addWidget(list);
    layout->addLayout(buttons);
    const auto store = [list] {
        QStringList sources;
        for (int i = 0; i < list->count(); ++i) {
            sources << list->item(i)->text();
        }
        setMixPaletteSources(sources);
    };
    const auto addPath = [list, store](const QString& path) {
        if (!path.isEmpty() && list->findItems(path, Qt::MatchExactly).isEmpty()) {
            list->addItem(path);
            store();
        }
    };
    connect(addFile, &QPushButton::clicked, page, [page, addPath] {
        addPath(QFileDialog::getOpenFileName(page, tr("Add MIX File"), QString(),
                                             tr("Westwood MIX archives (*.mix *.MIX)")));
    });
    connect(addFolder, &QPushButton::clicked, page, [page, addPath] {
        addPath(QFileDialog::getExistingDirectory(page, tr("Add Folder of MIX Files")));
    });
    connect(remove, &QPushButton::clicked, page, [list, store] {
        delete list->currentItem();
        store();
    });
    return page;
}

} // namespace pictura

#include "moc_mix_support.cpp"
