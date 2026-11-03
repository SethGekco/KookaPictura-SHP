// SPDX-License-Identifier: GPL-3.0-or-later
// KookaPictura-SHP fork: the Tiles panel of a TMP terrain document.

#include "panels/tmp_tiles_panel.h"

#include "image_view.h"

#include <QtCore/QSignalBlocker>
#include <QtGui/QColor>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
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
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->verticalHeader()->setVisible(false);
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->setToolTip(tr("Land and ramp show the number the file holds and the game's name for "
                          "it; several numbers share a name, and the number is what is saved. "
                          "Z: whether the tile has z-data (painted on the hidden Z layers)."));
    layout->addWidget(table_, 1);

    connect(table_, &QTableWidget::currentCellChanged, this,
            [this](int row, int, int, int) {
                if (row >= 0) {
                    focusSlot(table_->item(row, Slot)->data(Qt::UserRole).toInt());
                }
            });
    connect(outlines_, &QCheckBox::toggled, this, [this] { syncGrid(); });
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
        return;
    }
    const quint64 revision = view_->canvas_revision();
    const quint64 generation = view_->document_generation();
    if (!built_ || revision != builtRevision_ || generation != builtGeneration_) {
        rebuild();
        builtRevision_ = revision;
        builtGeneration_ = generation;
        built_ = true;
    }
    syncGrid();
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
