// SPDX-License-Identifier: GPL-3.0-or-later
// KookaPictura-SHP fork: the MIX browser (see mix_browser_panel.h).

#include "panels/mix_browser_panel.h"

#include <QtCore/QFileInfo>
#include <QtCore/QLocale>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QVBoxLayout>

#include <functional>

#include "pictura_app/src/cxxqt_object.cxxqt.h"
#include "pictura_app/src/cxxqt_object/shp.cxxqt.h"

namespace pictura {

namespace {

enum Column { NameColumn, KindColumn, SizeColumn, IdColumn };

enum Role {
    ArchiveRole = Qt::UserRole,
    ChainRole,
    KindRole,
    MembersRole,
    FileNameRole,
    SizeRole,
    TopRole,
};

// Sizes and IDs sort as numbers.
class Item : public QTreeWidgetItem {
public:
    using QTreeWidgetItem::QTreeWidgetItem;
    bool operator<(const QTreeWidgetItem& other) const override
    {
        const int column = treeWidget() ? treeWidget()->sortColumn() : 0;
        if (column == SizeColumn) {
            return data(0, SizeRole).toULongLong() < other.data(0, SizeRole).toULongLong();
        }
        if (column == IdColumn) {
            return text(IdColumn).toUInt(nullptr, 16) < other.text(IdColumn).toUInt(nullptr, 16);
        }
        return text(column).compare(other.text(column), Qt::CaseInsensitive) < 0;
    }
};

QString idText(quint32 id)
{
    return QStringLiteral("%1").arg(id, 8, 16, QLatin1Char('0')).toUpper();
}

QString sizeText(quint64 bytes)
{
    return QLocale().formattedDataSize(qint64(bytes), 1, QLocale::DataSizeTraditionalFormat);
}

} // namespace

MixBrowserPanel::MixBrowserPanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("mixBrowserPanel"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    auto* row = new QHBoxLayout;
    open_ = new QPushButton(tr("Open MIX…"), this);
    open_->setObjectName(QStringLiteral("mixOpenArchive"));
    close_ = new QPushButton(tr("Close"), this);
    close_->setObjectName(QStringLiteral("mixCloseArchive"));
    close_->setToolTip(tr("Close the archive of the selected row"));
    filter_ = new QLineEdit(this);
    filter_->setObjectName(QStringLiteral("mixFilter"));
    filter_->setPlaceholderText(tr("Filter by name, ID or type"));
    filter_->setClearButtonEnabled(true);
    openEntry_ = new QPushButton(tr("Open"), this);
    openEntry_->setObjectName(QStringLiteral("mixOpenEntry"));
    openEntry_->setToolTip(tr("Open the selected SHP or TMP as a document"));
    export_ = new QPushButton(tr("Export…"), this);
    export_->setObjectName(QStringLiteral("mixExport"));
    export_->setToolTip(tr("Copy the selected entries into a folder"));
    row->addWidget(open_);
    row->addWidget(close_);
    row->addWidget(filter_, 1);
    row->addWidget(openEntry_);
    row->addWidget(export_);
    layout->addLayout(row);

    tree_ = new QTreeWidget(this);
    tree_->setObjectName(QStringLiteral("mixTree"));
    tree_->setHeaderLabels({tr("Name"), tr("Type"), tr("Size"), tr("ID")});
    tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tree_->setUniformRowHeights(true);
    tree_->setSortingEnabled(true);
    tree_->sortByColumn(NameColumn, Qt::AscendingOrder);
    tree_->header()->setSectionResizeMode(NameColumn, QHeaderView::Stretch);
    tree_->header()->setStretchLastSection(false);
    layout->addWidget(tree_, 1);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("mixStatus"));
    status_->setWordWrap(true);
    layout->addWidget(status_);

    connect(open_, &QPushButton::clicked, this, &MixBrowserPanel::openArchiveRequested);
    connect(close_, &QPushButton::clicked, this, [this] {
        if (const QString archive = currentArchive(); !archive.isEmpty()) {
            emit closeArchiveRequested(archive);
        }
    });
    connect(filter_, &QLineEdit::textChanged, this, &MixBrowserPanel::applyFilter);
    connect(tree_, &QTreeWidget::itemSelectionChanged, this, &MixBrowserPanel::refreshButtons);
    connect(tree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item) {
        const MixEntryRef ref = refOf(item);
        if (item->data(0, TopRole).toBool() || ref.kind == QLatin1String("MIX")) {
            return; // the tree expands it
        }
        emit entryActivated(ref);
    });
    connect(openEntry_, &QPushButton::clicked, this, [this] {
        const QList<MixEntryRef> entries = selectedEntries();
        if (!entries.isEmpty()) {
            emit entryActivated(entries.first());
        }
    });
    connect(export_, &QPushButton::clicked, this, [this] {
        const QList<MixEntryRef> entries = selectedEntries(true);
        if (!entries.isEmpty()) {
            emit exportRequested(entries);
        }
    });
    refreshButtons();
}

void MixBrowserPanel::setListing(const QString& archive, const MixListing& listing)
{
    removeArchive(archive);
    tree_->setSortingEnabled(false);
    auto* top = new Item(tree_);
    top->setText(NameColumn, QFileInfo(archive).fileName());
    top->setText(KindColumn, QStringLiteral("MIX"));
    top->setToolTip(NameColumn, archive + QLatin1Char('\n') + QString(listing.format));
    top->setData(0, ArchiveRole, archive);
    top->setData(0, TopRole, true);
    top->setData(0, KindRole, QStringLiteral("MIX"));
    top->setData(0, FileNameRole, QFileInfo(archive).fileName());
    top->setData(0, SizeRole, QFileInfo(archive).size());
    top->setText(SizeColumn, sizeText(quint64(QFileInfo(archive).size())));
    QVector<QTreeWidgetItem*> items;
    items.reserve(qsizetype(listing.rows.size()));
    int named = 0;
    for (const MixRow& row : listing.rows) {
        QTreeWidgetItem* parent = row.parent < 0 ? top : items.value(row.parent, top);
        auto* item = new Item(parent);
        const QString name(row.name);
        const QString shown = name.isEmpty() ? QLatin1Char('#') + idText(row.id) : name;
        named += name.isEmpty() ? 0 : 1;
        item->setText(NameColumn, shown);
        if (name.isEmpty()) {
            QFont font = item->font(NameColumn);
            font.setItalic(true);
            item->setFont(NameColumn, font);
            item->setToolTip(NameColumn, tr("No name list knows this ID"));
        }
        item->setText(KindColumn, QString(row.kind));
        item->setText(SizeColumn, sizeText(row.size));
        item->setText(IdColumn, idText(row.id));
        item->setTextAlignment(SizeColumn, Qt::AlignRight | Qt::AlignVCenter);
        QVariantList chain;
        for (const quint32 id : row.chain) {
            chain << id;
        }
        QStringList members = parent == top ? QStringList()
                                            : parent->data(0, MembersRole).toStringList();
        members << shown;
        item->setData(0, ArchiveRole, archive);
        item->setData(0, ChainRole, chain);
        item->setData(0, KindRole, QString(row.kind));
        item->setData(0, MembersRole, members);
        item->setData(0, FileNameRole,
                      name.isEmpty() ? idText(row.id) + QLatin1Char('.') + QString(row.suffix)
                                     : name);
        item->setData(0, SizeRole, quint64(row.size));
        items.push_back(item);
    }
    top->setExpanded(true);
    tree_->setSortingEnabled(true);
    status_->setText(tr("%1: %2. %3 of %4 entries named.")
                         .arg(QFileInfo(archive).fileName(), QString(listing.format))
                         .arg(named)
                         .arg(listing.rows.size()));
    applyFilter();
    refreshButtons();
}

void MixBrowserPanel::removeArchive(const QString& archive)
{
    for (int i = tree_->topLevelItemCount() - 1; i >= 0; --i) {
        if (tree_->topLevelItem(i)->data(0, ArchiveRole).toString() == archive) {
            delete tree_->takeTopLevelItem(i);
        }
    }
    refreshButtons();
}

QStringList MixBrowserPanel::archives() const
{
    QStringList out;
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        out << tree_->topLevelItem(i)->data(0, ArchiveRole).toString();
    }
    return out;
}

MixEntryRef MixBrowserPanel::refOf(const QTreeWidgetItem* item) const
{
    MixEntryRef ref;
    if (!item) {
        return ref;
    }
    ref.archive = item->data(0, ArchiveRole).toString();
    for (const QVariant& id : item->data(0, ChainRole).toList()) {
        ref.chain << quint32(id.toUInt());
    }
    ref.members = item->data(0, MembersRole).toStringList();
    ref.kind = item->data(0, KindRole).toString();
    ref.fileName = item->data(0, FileNameRole).toString();
    ref.size = item->data(0, SizeRole).toULongLong();
    return ref;
}

QList<MixEntryRef> MixBrowserPanel::selectedEntries(bool withArchives) const
{
    QList<MixEntryRef> out;
    for (QTreeWidgetItem* item : tree_->selectedItems()) {
        const bool archive = item->data(0, TopRole).toBool()
            || item->data(0, KindRole).toString() == QLatin1String("MIX");
        if (archive && !withArchives) {
            continue;
        }
        out << refOf(item);
    }
    return out;
}

QString MixBrowserPanel::currentArchive() const
{
    if (QTreeWidgetItem* item = tree_->currentItem()) {
        return item->data(0, ArchiveRole).toString();
    }
    return tree_->topLevelItemCount() == 1
               ? tree_->topLevelItem(0)->data(0, ArchiveRole).toString()
               : QString();
}

int MixBrowserPanel::visibleEntryCount() const
{
    int count = 0;
    QTreeWidgetItemIterator it(tree_);
    for (; *it; ++it) {
        count += (!(*it)->isHidden() && !(*it)->data(0, TopRole).toBool()) ? 1 : 0;
    }
    return count;
}

bool MixBrowserPanel::selectEntryNamed(const QString& name)
{
    QTreeWidgetItemIterator it(tree_);
    for (; *it; ++it) {
        if (!(*it)->data(0, TopRole).toBool()
            && (*it)->text(NameColumn).compare(name, Qt::CaseInsensitive) == 0) {
            tree_->clearSelection();
            tree_->setCurrentItem(*it);
            (*it)->setSelected(true);
            return true;
        }
    }
    return false;
}

// A row shows when it or a row below it matches; an archive row always
// shows while any of its rows does.
void MixBrowserPanel::applyFilter()
{
    const QString text = filter_->text().trimmed();
    std::function<bool(QTreeWidgetItem*)> visit = [&](QTreeWidgetItem* item) {
        bool below = false;
        for (int i = 0; i < item->childCount(); ++i) {
            below = visit(item->child(i)) || below;
        }
        if (item->data(0, TopRole).toBool()) {
            return below;
        }
        const bool self = text.isEmpty()
            || item->text(NameColumn).contains(text, Qt::CaseInsensitive)
            || item->text(IdColumn).contains(text, Qt::CaseInsensitive)
            || item->text(KindColumn).compare(text, Qt::CaseInsensitive) == 0;
        item->setHidden(!(self || below));
        if (below && !text.isEmpty()) {
            item->setExpanded(true);
        }
        return self || below;
    };
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        visit(tree_->topLevelItem(i));
    }
}

void MixBrowserPanel::refreshButtons()
{
    const QList<MixEntryRef> entries = selectedEntries();
    const bool document = entries.size() == 1
        && (entries.first().kind == QLatin1String("SHP")
            || entries.first().kind == QLatin1String("TMP"));
    openEntry_->setEnabled(document);
    export_->setEnabled(!selectedEntries(true).isEmpty());
    close_->setEnabled(tree_->topLevelItemCount() > 0);
}

} // namespace pictura

#include "moc_mix_browser_panel.cpp"
