// SPDX-License-Identifier: GPL-3.0-or-later
// KookaPictura-SHP fork: Westwood MIX archives in the shell. File > Open of
// a .mix shows it in the MIX browser (a dock); an SHP or TMP entry opens as
// a document backed by an extracted copy in a temporary folder, titled
// "gi.shp in ra2.mix". Saving such a document asks: Save As a loose file
// (the default) or Replace in a Copy of the MIX (a new archive file; the
// archive itself is written only after a confirmation that names it).
// Palette resolution searches the archives too (see shp_palettes.h).
// The hooks into upstream files are listed in crates/pictura-shp/HOOKS.md.
#pragma once

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVector>

#include <memory>
#include <vector>

class QDockWidget;
class QMainWindow;
class QTemporaryDir;
class QWidget;

namespace pictura {

class MixBrowserPanel;
class PictureView;
struct MixEntryRef;

// Where a document opened from inside a MIX came from.
struct MixOrigin {
    QString archive;          // the outermost archive file
    QVector<quint32> chain;   // IDs from its index down to the entry
    QStringList members;      // names (or #ID) on the same path
};

// A path File > Open hands to the MIX browser (a .mix suffix, any case).
bool isMixPath(const QString& path);
// The extracted file of an entry opened from a MIX.
bool isMixMember(const QString& path);
MixOrigin mixOrigin(const QString& path);
// "gi.shp in ra2.mix" (nested: "gi.shp in ra2.mix/conquer.mix") for an
// extracted entry, else the file name.
QString mixMemberTitle(const QString& path);

// The archives a palette is looked for in, for a document at `path`: the
// archive it came from, then the archives open in the browser, then the
// Preferences list (a folder stands for the .mix files in it, the games'
// own override order first: expandmd99..00, expand99..00, ra2md, ra2, the
// rest by name). Duplicates are dropped.
QStringList paletteArchives(const QString& path);
// The first entry called `name` in `archives` (and their named nested
// archives), as a member path (ra2.mix/cache.mix/unittem.pal), or empty.
QString findInArchives(const QStringList& archives, const QString& name);
// The .pal entry whose stem is the longest that starts `stem`, or empty.
QString findPalettePrefixInArchives(const QStringList& archives, const QString& stem);
// A palette path exists: a file, or an entry inside a MIX.
bool palettePathExists(const QString& path);

// Preferences > Advanced: MIX files and folders searched for palettes
// (default: none).
QStringList mixPaletteSources();
void setMixPaletteSources(const QStringList& sources);

class MixSupport : public QObject {
    Q_OBJECT

public:
    // Create the support object and its dock (a child of `frame`; the frame
    // registers the dock).
    static MixSupport* install(QMainWindow* frame);
    static MixSupport* of(const QObject* frame);
    ~MixSupport() override;

    QDockWidget* dock() const { return dock_; }
    MixBrowserPanel* browser() const { return browser_; }

    // List `path` in the browser and show it. The listing reads on a worker
    // thread; listingFinished() tells when it is in. False (with the reason
    // in lastMessage()) when the file cannot be read at all.
    bool openArchive(const QString& path);
    void closeArchive(const QString& path);
    QStringList openArchives() const;
    // Whether a listing is still being read.
    bool busy() const { return pending_ > 0; }

    // Extract the entry and open it as a document titled "x.shp in ra2.mix".
    bool openEntry(const MixEntryRef& entry);
    // Copy entries into `folder` (an unnamed one as <ID>.<suffix>); how many.
    int exportEntries(const QList<MixEntryRef>& entries, const QString& folder);

    // Save of a document opened from inside a MIX: [Save As Loose File…]
    // (default) / [Replace in a Copy of the MIX…] / [Cancel]. Without an
    // interactive platform it is refused with the reason.
    bool saveMember(PictureView* view);
    // Run the save gate, write the entry and a copy of its archive at `out`
    // with the entry replaced. `out` being the archive itself needs a
    // confirmation naming it (refused without dialogs). The document then
    // belongs to `out`.
    bool replaceInCopy(PictureView* view, const QString& out);

    // The Preferences block listing the palette archives (owned by `parent`).
    QWidget* palettePage(QWidget* parent);

    QString lastMessage() const { return message_; }

    // Test seams: the path the Replace in a Copy file dialog would return
    // (empty: show the dialog), and dialogs on or off whatever the platform
    // (-1 follows the platform).
    static void setCopyPathForTest(const QString& path);
    static void setInteractiveForTest(int interactive);

signals:
    void listingFinished(const QString& path, bool ok);

private:
    explicit MixSupport(QMainWindow* frame);
    void report(const QString& text);
    bool interactive() const;
    void openWithDialog();
    void exportWithDialog(const QList<MixEntryRef>& entries);

    QMainWindow* frame_ = nullptr;
    QDockWidget* dock_ = nullptr;
    MixBrowserPanel* browser_ = nullptr;
    QString message_;
    int pending_ = 0;
    // The folders extracted entries live in, removed when Kooka closes.
    std::vector<std::unique_ptr<QTemporaryDir>> extracted_;
};

} // namespace pictura
