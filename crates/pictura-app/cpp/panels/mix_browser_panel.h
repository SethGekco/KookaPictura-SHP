// SPDX-License-Identifier: GPL-3.0-or-later
// KookaPictura-SHP fork: the MIX browser. Every open archive is a top-level
// row; its entries (and the entries of archives nested in it) below, with
// their name (or ID when no list names it), the kind told from their
// content, size and ID. A filter box narrows the rows by name, ID or kind.
#pragma once

#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVector>
#include <QtWidgets/QWidget>

class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace pictura {

struct MixListing;

// One entry, as the browser hands it on.
struct MixEntryRef {
    QString archive;          // the outermost archive file
    QVector<quint32> chain;   // IDs from its index down to the entry
    QStringList members;      // names (or #ID) on the same path
    QString kind;             // "SHP", "TMP", "PAL", "MIX", ...
    QString fileName;         // the name, or <ID>.<suffix> when unnamed
    quint64 size = 0;
};

class MixBrowserPanel : public QWidget {
    Q_OBJECT

public:
    explicit MixBrowserPanel(QWidget* parent = nullptr);

    // Show (or refresh) the listing of `archive`.
    void setListing(const QString& archive, const MixListing& listing);
    void removeArchive(const QString& archive);
    QStringList archives() const;

    // The selected entries (archive rows and nested-archive rows excluded
    // unless `withArchives`).
    QList<MixEntryRef> selectedEntries(bool withArchives = false) const;
    // The archive of the selected row (or the only one open).
    QString currentArchive() const;

    QTreeWidget* tree() const { return tree_; }
    QLineEdit* filter() const { return filter_; }
    // Rows shown under the filter, archives excluded.
    int visibleEntryCount() const;
    // Select the first entry whose name is `name` (any case); false when none.
    bool selectEntryNamed(const QString& name);

signals:
    void openArchiveRequested();
    void closeArchiveRequested(const QString& archive);
    void entryActivated(const pictura::MixEntryRef& entry);
    void exportRequested(const QList<pictura::MixEntryRef>& entries);
    void message(const QString& text);

private:
    void applyFilter();
    void refreshButtons();
    MixEntryRef refOf(const QTreeWidgetItem* item) const;

    QPushButton* open_ = nullptr;
    QPushButton* close_ = nullptr;
    QPushButton* openEntry_ = nullptr;
    QPushButton* export_ = nullptr;
    QLineEdit* filter_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QLabel* status_ = nullptr;
};

} // namespace pictura
