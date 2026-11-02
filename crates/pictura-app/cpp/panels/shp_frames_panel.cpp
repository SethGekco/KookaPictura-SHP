// SPDX-License-Identifier: GPL-3.0-or-later
// KookaPictura-SHP fork: the Frames panel of an SHP document.

#include "panels/shp_frames_panel.h"

#include "image_view.h"

#include <QtGui/QIcon>
#include <QtGui/QPainter>
#include <QtGui/QPixmap>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "pictura_app/src/cxxqt_object.cxxqt.h"
#include "pictura_app/src/cxxqt_object/shp.cxxqt.h"

namespace pictura {

namespace {

constexpr int kThumb = 56;

} // namespace

ShpFramesPanel::ShpFramesPanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("shpFramesPanel"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(4);

    auto* row = new QHBoxLayout;
    prev_ = new QToolButton(this);
    prev_->setText(QStringLiteral("‹"));
    prev_->setToolTip(tr("Previous frame (,)"));
    next_ = new QToolButton(this);
    next_->setText(QStringLiteral("›"));
    next_->setToolTip(tr("Next frame (.)"));
    label_ = new QLabel(this);
    label_->setObjectName(QStringLiteral("shpFrameLabel"));
    selectCell_ = new QCheckBox(tr("Select cell"), this);
    selectCell_->setChecked(true);
    selectCell_->setToolTip(tr("Set the marquee to the frame's cell, so strokes stay inside it"));
    grid_ = new QCheckBox(tr("Grid"), this);
    grid_->setChecked(true);
    row->addWidget(prev_);
    row->addWidget(next_);
    row->addWidget(label_, 1);
    row->addWidget(selectCell_);
    row->addWidget(grid_);
    layout->addLayout(row);

    list_ = new QListWidget(this);
    list_->setObjectName(QStringLiteral("shpFrameList"));
    list_->setViewMode(QListView::IconMode);
    list_->setFlow(QListView::LeftToRight);
    list_->setWrapping(true);
    list_->setResizeMode(QListView::Adjust);
    list_->setMovement(QListView::Static);
    list_->setIconSize(QSize(kThumb, kThumb));
    list_->setUniformItemSizes(true);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(list_, 1);

    connect(list_, &QListWidget::itemClicked, this,
            [this](QListWidgetItem* item) { focusFrame(list_->row(item)); });
    connect(prev_, &QToolButton::clicked, this, [this] { step(-1); });
    connect(next_, &QToolButton::clicked, this, [this] { step(1); });
    connect(grid_, &QCheckBox::toggled, this, [this] { syncGrid(); });
    refresh();
}

void ShpFramesPanel::setView(PictureView* view, ImageView* canvas)
{
    if (view_ != view) {
        current_ = -1;
        builtRevision_ = 0;
        builtGeneration_ = 0;
        builtLayout_.clear();
    }
    view_ = view;
    canvas_ = canvas;
    refresh();
}

int ShpFramesPanel::frameCount() const
{
    return list_->count();
}

QRect ShpFramesPanel::cellRect(int index) const
{
    if (!view_) {
        return {};
    }
    const ShpInfo info = shp_info(*view_);
    if (!info.valid || index < 0 || index >= info.frames || info.cols <= 0) {
        return {};
    }
    return QRect((index % info.cols) * (info.width + info.gutter),
                 (index / info.cols) * (info.height + info.gutter), info.width, info.height);
}

bool ShpFramesPanel::selectsCell() const
{
    return selectCell_->isChecked();
}

void ShpFramesPanel::setSelectsCell(bool on)
{
    selectCell_->setChecked(on);
}

bool ShpFramesPanel::gridVisible() const
{
    return grid_->isChecked();
}

void ShpFramesPanel::refresh()
{
    const bool shp = view_ && shp_info(*view_).valid;
    if (!shp) {
        list_->clear();
        current_ = -1;
        builtLayout_.clear();
        label_->setText(tr("Not an SHP document"));
    } else {
        const ShpInfo info = shp_info(*view_);
        const QString layout = QStringLiteral("%1x%2x%3x%4x%5")
                                   .arg(info.width)
                                   .arg(info.height)
                                   .arg(info.frames)
                                   .arg(info.cols)
                                   .arg(info.gutter);
        const quint64 revision = view_->canvas_revision();
        const quint64 generation = view_->document_generation();
        if (layout != builtLayout_ || revision != builtRevision_ || generation != builtGeneration_) {
            builtLayout_ = layout;
            builtRevision_ = revision;
            builtGeneration_ = generation;
            rebuild();
        }
        label_->setText(current_ >= 0 ? tr("Frame %1 of %2").arg(current_).arg(info.frames)
                                      : tr("%1 frames, %2x%3").arg(info.frames)
                                            .arg(info.width)
                                            .arg(info.height));
    }
    prev_->setEnabled(shp);
    next_->setEnabled(shp);
    selectCell_->setEnabled(shp);
    grid_->setEnabled(shp);
    syncGrid();
}

void ShpFramesPanel::rebuild()
{
    const ShpInfo info = shp_info(*view_);
    const int frames = info.frames;
    const bool reuse = list_->count() == frames;
    if (!reuse) {
        list_->clear();
    }
    // Integer scale for crisp pixels when the frame is small, else fit.
    const double fit = std::min(double(kThumb) / info.width, double(kThumb) / info.height);
    const double scale = fit >= 1.0 ? std::floor(fit) : fit;
    const QSize thumb(std::max(1, int(info.width * scale)), std::max(1, int(info.height * scale)));
    for (int i = 0; i < frames; ++i) {
        const QRect cell = cellRect(i);
        const QImage image =
            view_->display_image(0, cell.x(), cell.y(), cell.width(), cell.height());
        QPixmap pixmap(kThumb, kThumb);
        pixmap.fill(Qt::transparent);
        if (!image.isNull()) {
            const QImage scaled = image.scaled(thumb, Qt::KeepAspectRatio, Qt::FastTransformation);
            QPainter p(&pixmap);
            p.drawImage(QPoint((kThumb - scaled.width()) / 2, (kThumb - scaled.height()) / 2),
                        scaled);
        }
        if (reuse) {
            list_->item(i)->setIcon(QIcon(pixmap));
        } else {
            auto* item = new QListWidgetItem(QIcon(pixmap), QString::number(i), list_);
            item->setToolTip(tr("Frame %1").arg(i));
            item->setTextAlignment(Qt::AlignHCenter);
        }
    }
    if (current_ >= frames) {
        current_ = -1;
    }
    if (current_ >= 0) {
        list_->setCurrentRow(current_);
    }
}

void ShpFramesPanel::syncGrid()
{
    if (!canvas_) {
        return;
    }
    ImageView::ShpGrid grid;
    if (view_) {
        const ShpInfo info = shp_info(*view_);
        if (info.valid) {
            grid.cell = QSize(info.width, info.height);
            grid.cols = info.cols;
            grid.frames = info.frames;
            grid.gutter = info.gutter;
            grid.active = current_;
            grid.visible = grid_->isChecked();
        }
    }
    canvas_->setShpGrid(grid);
}

void ShpFramesPanel::focusFrame(int index)
{
    const QRect cell = cellRect(index);
    if (cell.isNull()) {
        return;
    }
    current_ = index;
    if (list_->currentRow() != index) {
        list_->setCurrentRow(index);
    }
    list_->scrollToItem(list_->item(index));
    if (canvas_) {
        // Zoom in only when the cell would be tiny, at an integer level so
        // pixels stay square; never zoom out from a closer look.
        const QSizeF room = QSizeF(canvas_->size()) * 0.8;
        const double fit = std::min(room.width() / cell.width(), room.height() / cell.height());
        if (fit >= 1.0 && canvas_->zoom() * 4.0 < fit) {
            canvas_->setZoom(std::floor(std::min(fit, 32.0)),
                             QPointF(canvas_->width() / 2.0, canvas_->height() / 2.0));
        }
        canvas_->centerOnImageRect(QRectF(cell));
    }
    if (view_ && selectCell_->isChecked()) {
        view_->select_rect(cell.x(), cell.y(), cell.width(), cell.height(), QStringLiteral("new"),
                           0.0);
    }
    if (view_) {
        label_->setText(tr("Frame %1 of %2").arg(index).arg(shp_info(*view_).frames));
    }
    syncGrid();
}

void ShpFramesPanel::step(int delta)
{
    const int count = frameCount();
    if (count <= 0) {
        return;
    }
    const int from = current_ < 0 ? (delta > 0 ? -1 : 0) : current_;
    focusFrame(((from + delta) % count + count) % count);
}

} // namespace pictura

#include "moc_shp_frames_panel.cpp"
