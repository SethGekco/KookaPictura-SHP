// SPDX-License-Identifier: GPL-3.0-or-later
// KookaPictura-SHP fork: the Tiles panel of a TMP terrain document.

#include "panels/tmp_tiles_panel.h"

#include "image_view.h"
#include "shp_support.h"
#include "tmp_authoring.h"

#include <QtCore/QRegularExpression>
#include <QtCore/QSignalBlocker>
#include <QtGui/QColor>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "pictura_app/src/cxxqt_object.cxxqt.h"
#include "pictura_app/src/cxxqt_object/shp.cxxqt.h"

namespace pictura {

namespace {

enum Column { Slot, Cell, Position, Height, Land, Ramp, Extra, Depth, Radar, Columns };

QString hex(quint32 rgb)
{
    return QColor::fromRgb(rgb).name();
}

} // namespace

TmpTilesPanel::TmpTilesPanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("tmpTilesPanel"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(4);
    auto* top = new QHBoxLayout;
    summary_ = new QLabel(this);
    summary_->setObjectName(QStringLiteral("tmpTilesSummary"));
    outlines_ = new QCheckBox(tr("Outlines"), this);
    outlines_->setObjectName(QStringLiteral("tmpTileOutlines"));
    outlines_->setChecked(true);
    outlines_->setToolTip(tr("Outline every tile's diamond, and every extra block dashed (view "
                             "only)."));
    top->addWidget(summary_, 1);
    top->addWidget(outlines_);
    layout->addLayout(top);

    table_ = new QTableWidget(0, Columns, this);
    table_->setObjectName(QStringLiteral("tmpTilesTable"));
    table_->setHorizontalHeaderLabels({tr("Tile"), tr("Cell"), tr("Position"), tr("Height"),
                                       tr("Land"), tr("Ramp"), tr("Extra"), tr("Z"),
                                       tr("Radar")});
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    table_->verticalHeader()->setVisible(false);
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->setToolTip(tr("Land and ramp show the number the file holds and the game's name for "
                          "it; several numbers share a name, and the number is what is saved. "
                          "Z: whether the tile has z-data (painted on the hidden Z layers). "
                          "Double-click (or F2) Height, Land, Ramp or Radar to change it: a "
                          "number 0-255, and two colors #rrggbb for the radar; each change "
                          "is one undoable step that Save writes into the tile header."));
    layout->addWidget(table_, 1);

    auto* edits = new QHBoxLayout;
    const auto button = [this, edits](const QString& text, const char* name,
                                      const QString& tip) {
        auto* b = new QPushButton(text, this);
        b->setObjectName(QLatin1String(name));
        b->setToolTip(tip);
        edits->addWidget(b);
        return b;
    };
    move_ = button(tr("Move..."), "tmpTileMove",
                   tr("Move the selected tile to a free cell: its header position follows the "
                      "grid and it is redrawn there with its extra block."));
    add_ = button(tr("Add..."), "tmpTileAdd",
                  tr("Add a blank tile in a free cell (placed by the grid from the first tile, "
                     "with its height, land and ramp)."));
    remove_ = button(tr("Remove"), "tmpTileRemove", tr("Remove the selected tile."));
    addExtra_ = button(tr("Add Extra..."), "tmpTileAddExtra",
                       tr("Give the selected tile an extra block (transparent, no z-data)."));
    removeExtra_ = button(tr("Remove Extra"), "tmpTileRemoveExtra",
                          tr("Remove the selected tile's extra block."));
    edits->addStretch(1);
    layout->addLayout(edits);
    connect(move_, &QPushButton::clicked, this, [this] { askMove(); });
    connect(add_, &QPushButton::clicked, this, [this] { askAdd(); });
    connect(remove_, &QPushButton::clicked, this, [this] { removeTile(currentSlot()); });
    connect(addExtra_, &QPushButton::clicked, this, [this] { askExtra(); });
    connect(removeExtra_, &QPushButton::clicked, this, [this] { removeExtra(currentSlot()); });

    connect(table_, &QTableWidget::currentCellChanged, this,
            [this](int row, int, int, int) {
                if (row >= 0) {
                    focusSlot(table_->item(row, Slot)->data(Qt::UserRole).toInt());
                }
                syncButtons();
            });
    connect(outlines_, &QCheckBox::toggled, this, [this] { syncGrid(); });
    // Queued: applying rebuilds the rows, which must not happen while the
    // table is still inside the edit that changed the item.
    connect(
        table_, &QTableWidget::itemChanged, this,
        [this](QTableWidgetItem* item) {
            const int row = item->row();
            QMetaObject::invokeMethod(this, [this, row] { applyRow(row); }, Qt::QueuedConnection);
        });
}

void TmpTilesPanel::setView(PictureView* view, ImageView* canvas)
{
    if (view_ != view) {
        built_ = false;
    }
    view_ = view;
    canvas_ = canvas;
    refresh();
}

void TmpTilesPanel::refresh()
{
    const bool tmp = view_ && tmp_info(*view_).valid;
    if (!tmp) {
        table_->setRowCount(0);
        summary_->setText(tr("Not a TMP document"));
        built_ = false;
        syncButtons();
        return;
    }
    const quint64 revision = view_->canvas_revision();
    const quint64 generation = view_->document_generation();
    // A field edit, its undo and redo move the history without a repaint.
    const int history = view_->history_index();
    if (!built_ || revision != builtRevision_ || generation != builtGeneration_
        || history != builtHistory_) {
        rebuild();
        builtRevision_ = revision;
        builtGeneration_ = generation;
        builtHistory_ = history;
        built_ = true;
    }
    syncGrid();
    syncButtons();
}

void TmpTilesPanel::rebuild()
{
    const int keep = currentSlot();
    const TmpInfo info = tmp_info(*view_);
    summary_->setText(tr("%1 tiles, %2 x %3 template of %4 x %5 tiles")
                          .arg(info.tiles)
                          .arg(info.blocks_x)
                          .arg(info.blocks_y)
                          .arg(info.cx)
                          .arg(info.cy));
    const ::rust::Vec<TmpTile> tiles = tmp_tiles(*view_);
    const QSignalBlocker block(table_);
    table_->setRowCount(int(tiles.size()));
    for (int row = 0; row < int(tiles.size()); ++row) {
        const TmpTile& t = tiles[size_t(row)];
        const auto set = [&](int column, const QString& text) {
            auto* item = new QTableWidgetItem(text);
            item->setData(Qt::UserRole, t.slot);
            const bool editable =
                column == Height || column == Land || column == Ramp || column == Radar;
            item->setFlags(editable ? item->flags() | Qt::ItemIsEditable
                                    : item->flags() & ~Qt::ItemIsEditable);
            table_->setItem(row, column, item);
        };
        set(Slot, QString::number(t.slot));
        set(Cell, QStringLiteral("%1, %2").arg(t.col).arg(t.row));
        set(Position, QStringLiteral("%1, %2").arg(t.x).arg(t.y));
        set(Height, QString::number(t.height));
        set(Land, QStringLiteral("%1 %2").arg(t.land).arg(QString(tmp_land_label(t.land))));
        set(Ramp, QStringLiteral("%1 %2").arg(t.ramp).arg(QString(tmp_ramp_label(t.ramp))));
        QString extra = tr("no");
        if (t.has_extra) {
            extra = t.extra_layer > 0 ? tr("yes (Extras %1)").arg(t.extra_layer + 1)
                                      : tr("yes");
            if (!t.has_extra_z) {
                extra += tr(", no z");
            }
        }
        set(Extra, extra);
        set(Depth, t.has_z ? tr("yes") : tr("no"));
        set(Radar, QStringLiteral("%1 / %2").arg(hex(t.radar_low), hex(t.radar_high)));
    }
    table_->resizeColumnsToContents();
    const int row = rowOfSlot(keep);
    if (row >= 0) {
        table_->setCurrentCell(row, Slot);
    }
}

bool TmpTilesPanel::applyRow(int row)
{
    if (!view_ || row < 0 || row >= table_->rowCount()) {
        return false;
    }
    const int slot = table_->item(row, Slot)->data(Qt::UserRole).toInt();
    // The leading number of "15 Cliff", or -1.
    const auto number = [this, row](int column) {
        static const QRegularExpression lead(QStringLiteral("^\\s*(\\d+)"));
        const QRegularExpressionMatch m = lead.match(table_->item(row, column)->text());
        bool ok = false;
        const int v = m.hasMatch() ? m.captured(1).toInt(&ok) : -1;
        return ok && v <= 255 ? v : -1;
    };
    const int height = number(Height);
    const int land = number(Land);
    const int ramp = number(Ramp);
    static const QRegularExpression hexes(QStringLiteral("#?([0-9A-Fa-f]{6})\\b"));
    QList<quint32> radar;
    for (auto it = hexes.globalMatch(table_->item(row, Radar)->text()); it.hasNext();) {
        radar << it.next().captured(1).toUInt(nullptr, 16);
    }
    QString problem;
    if (height < 0 || land < 0 || ramp < 0) {
        problem = tr("Tile %1: height, land and ramp are numbers 0 to 255.").arg(slot);
    } else if (radar.size() != 2) {
        problem = tr("Tile %1: the radar colors are two colors, #rrggbb / #rrggbb.").arg(slot);
    } else if (!tmp_set_tile_fields(*view_, slot, height, land, ramp, radar[0], radar[1])) {
        problem = tr("Tile %1: %2").arg(slot).arg(QString(shp_last_error()));
    }
    built_ = false;
    refresh();
    if (!problem.isEmpty()) {
        emit message(problem);
        return false;
    }
    emit message(tr("Tile %1 fields changed (Save writes them into its header).").arg(slot));
    return true;
}

QList<bool> TmpTilesPanel::takenCells() const
{
    const TmpInfo info = tmp_info(*view_);
    QList<bool> taken(qsizetype(std::max(0, info.blocks_x * info.blocks_y)), false);
    for (const TmpTile& t : tmp_tiles(*view_)) {
        if (t.slot >= 0 && t.slot < taken.size()) {
            taken[t.slot] = true;
        }
    }
    return taken;
}

void TmpTilesPanel::syncButtons()
{
    const bool tmp = view_ && tmp_info(*view_).valid;
    bool tile = false;
    bool extra = false;
    bool full = true;
    if (tmp) {
        const int slot = currentSlot();
        for (const TmpTile& t : tmp_tiles(*view_)) {
            if (t.slot == slot) {
                tile = true;
                extra = t.has_extra;
            }
        }
        full = !takenCells().contains(false);
    }
    move_->setEnabled(tile && !full);
    add_->setEnabled(tmp && !full);
    remove_->setEnabled(tile && tileCount() > 1);
    addExtra_->setEnabled(tile && !extra);
    removeExtra_->setEnabled(tile && extra);
}

bool TmpTilesPanel::reshape(int op, int slot, const QList<int>& args, const QString& done,
                            int focus)
{
    if (!view_ || !tmp_info(*view_).valid) {
        return false;
    }
    const int dropped =
        tmp_edit_tiles(*view_, op, slot, ::rust::Slice<const int32_t>(args.data(), args.size()),
                       ShpSupport::allowanceOf(view_));
    built_ = false;
    refresh();
    if (dropped < 0) {
        emit message(QString(shp_last_error()));
        return false;
    }
    if (focus >= 0) {
        focusSlot(focus);
    }
    QString text = done;
    if (dropped > 0) {
        text += tr(" %n pixel(s) outside the tiles had no place on the new canvas and were "
                   "dropped (Undo brings them back).",
                   nullptr, dropped);
    }
    emit message(text);
    return true;
}

bool TmpTilesPanel::moveTile(int slot, int column, int row)
{
    const int blocksX = view_ ? tmp_info(*view_).blocks_x : 0;
    return reshape(0, slot, {column, row},
                   tr("Tile %1 moved to cell (%2, %3).").arg(slot).arg(column).arg(row),
                   row * blocksX + column);
}

bool TmpTilesPanel::addTile(int column, int row)
{
    const int blocksX = view_ ? tmp_info(*view_).blocks_x : 0;
    const int slot = row * blocksX + column;
    return reshape(1, -1, {column, row},
                   tr("Blank tile %1 added in cell (%2, %3).").arg(slot).arg(column).arg(row),
                   slot);
}

bool TmpTilesPanel::removeTile(int slot)
{
    return reshape(2, slot, {}, tr("Tile %1 removed.").arg(slot), -1);
}

bool TmpTilesPanel::addExtra(int slot, int x, int y, int width, int height)
{
    return reshape(3, slot, {x, y, width, height},
                   tr("Tile %1 has a new %2 x %3 extra block (no z-data).")
                       .arg(slot)
                       .arg(width)
                       .arg(height),
                   slot);
}

bool TmpTilesPanel::removeExtra(int slot)
{
    return reshape(4, slot, {}, tr("Tile %1's extra block removed.").arg(slot), slot);
}

bool TmpTilesPanel::askMove()
{
    const int slot = currentSlot();
    if (!view_ || slot < 0) {
        return false;
    }
    const TmpInfo info = tmp_info(*view_);
    TmpCellDialog dialog(tr("Move Tile"),
                         tr("Move tile %1 to the cell (it is redrawn there, with its extra "
                            "block).")
                             .arg(slot),
                         info.blocks_x, info.blocks_y, takenCells(), this);
    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }
    return moveTile(slot, dialog.column(), dialog.row());
}

bool TmpTilesPanel::askAdd()
{
    if (!view_) {
        return false;
    }
    const TmpInfo info = tmp_info(*view_);
    TmpCellDialog dialog(tr("Add Tile"), tr("Add a blank tile in the cell."), info.blocks_x,
                         info.blocks_y, takenCells(), this);
    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }
    return addTile(dialog.column(), dialog.row());
}

bool TmpTilesPanel::askExtra()
{
    const int slot = currentSlot();
    if (!view_ || slot < 0) {
        return false;
    }
    const TmpInfo info = tmp_info(*view_);
    TmpExtraDialog dialog(slot, info.cx, info.cy, this);
    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }
    const QList<int> v = dialog.values();
    return addExtra(slot, v[0], v[1], v[2], v[3]);
}

int TmpTilesPanel::tileCount() const
{
    return table_->rowCount();
}

int TmpTilesPanel::rowOfSlot(int slot) const
{
    for (int row = 0; row < table_->rowCount(); ++row) {
        if (table_->item(row, Slot)->data(Qt::UserRole).toInt() == slot) {
            return row;
        }
    }
    return -1;
}

int TmpTilesPanel::currentSlot() const
{
    const int row = table_->currentRow();
    return row >= 0 && table_->item(row, Slot) ? table_->item(row, Slot)->data(Qt::UserRole).toInt()
                                               : -1;
}

void TmpTilesPanel::focusSlot(int slot)
{
    const int row = rowOfSlot(slot);
    if (row < 0 || !view_) {
        return;
    }
    if (table_->currentRow() != row) {
        const QSignalBlocker block(table_);
        table_->setCurrentCell(row, Slot);
    }
    table_->scrollToItem(table_->item(row, Slot));
    const TmpInfo info = tmp_info(*view_);
    for (const TmpTile& t : tmp_tiles(*view_)) {
        if (t.slot != slot || !canvas_) {
            continue;
        }
        QRectF area(t.x, t.y, info.cx, info.cy);
        if (t.has_extra) {
            area = area.united(QRectF(t.extra_x, t.extra_y, t.extra_w, t.extra_h));
        }
        // Zoom in only when the tile would be tiny, never out.
        const QSizeF room = QSizeF(canvas_->size()) * 0.8;
        const double fit = std::min(room.width() / area.width(), room.height() / area.height());
        if (fit >= 1.0 && canvas_->zoom() * 4.0 < fit) {
            canvas_->setZoom(std::floor(std::min(fit, 32.0)),
                             QPointF(canvas_->width() / 2.0, canvas_->height() / 2.0));
        }
        canvas_->centerOnImageRect(area);
    }
    syncGrid();
    syncButtons();
}

void TmpTilesPanel::focusPoint(const QPoint& point)
{
    if (!canvas_) {
        return;
    }
    const QRectF area(point.x() - 8, point.y() - 8, 17, 17);
    if (canvas_->zoom() < 4.0) {
        canvas_->setZoom(4.0, QPointF(canvas_->width() / 2.0, canvas_->height() / 2.0));
    }
    canvas_->centerOnImageRect(area);
}

void TmpTilesPanel::syncGrid()
{
    if (!canvas_ || !view_ || !tmp_info(*view_).valid) {
        return;
    }
    const TmpInfo info = tmp_info(*view_);
    ImageView::ShpGrid grid;
    grid.visible = outlines_->isChecked();
    const int current = currentSlot();
    const double cx = info.cx;
    const double cy = info.cy;
    for (const TmpTile& t : tmp_tiles(*view_)) {
        QPolygonF diamond;
        diamond << QPointF(t.x + cx / 2, t.y) << QPointF(t.x + cx, t.y + cy / 2)
                << QPointF(t.x + cx / 2, t.y + cy) << QPointF(t.x, t.y + cy / 2);
        if (t.slot == current) {
            grid.activeTile = int(grid.tileOutlines.size());
        }
        grid.tileOutlines.append(diamond);
        if (t.has_extra) {
            grid.extraOutlines.append(QRectF(t.extra_x, t.extra_y, t.extra_w, t.extra_h));
        }
    }
    canvas_->setShpGrid(grid);
}

QString TmpTilesPanel::summaryForTest() const
{
    return summary_->text();
}

} // namespace pictura

#include "moc_tmp_tiles_panel.cpp"
