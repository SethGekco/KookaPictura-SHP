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
#include <QtWidgets/QSpinBox>
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
    windowLabel_ = new QLabel(tr("Frames loaded:"), this);
    windowFrames_ = new QSpinBox(this);
    windowFrames_->setObjectName(QStringLiteral("shpWindowFrames"));
    windowFrames_->setKeyboardTracking(false);
    windowFrames_->setToolTip(
        tr("How many frames of this sprite are held at once. More than fit the SHP size "
           "limit needs Edit > Preferences > Advanced."));
    windowLabel_->setBuddy(windowFrames_);
    windowLabel_->hide();
    windowFrames_->hide();
    row->addWidget(windowLabel_);
    row->addWidget(windowFrames_);
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
    connect(windowFrames_, &QSpinBox::valueChanged, this, [this](int count) {
        if (view_ && count != shp_info(*view_).span) {
            emit windowSizeRequested(count);
            // Refused or cancelled: show the window size there is.
            refresh();
        }
    });
    refresh();
}

void ShpFramesPanel::setView(PictureView* view, ImageView* canvas)
{
    if (view_ != view) {
        current_ = -1;
        builtRevision_ = 0;
        builtGeneration_ = 0;
        builtLayout_.clear();
        offThumbs_.clear();
        offPalette_.clear();
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
    const int cell = index - info.first;
    if (!info.valid || cell < 0 || cell >= info.frames || info.cols <= 0) {
        return {};
    }
    return QRect((cell % info.cols) * (info.width + info.gutter),
                 (cell / info.cols) * (info.height + info.gutter), info.width, info.height);
}

bool ShpFramesPanel::onSheet(int index) const
{
    return !cellRect(index).isNull();
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
        const QString layout = QStringLiteral("%1x%2x%3x%4x%5 %6/%7")
                                   .arg(info.width)
                                   .arg(info.height)
                                   .arg(info.frames)
                                   .arg(info.cols)
                                   .arg(info.gutter)
                                   .arg(info.first)
                                   .arg(info.total);
        const quint64 revision = view_->canvas_revision();
        const quint64 generation = view_->document_generation();
        if (layout != builtLayout_ || revision != builtRevision_ || generation != builtGeneration_) {
            builtLayout_ = layout;
            builtRevision_ = revision;
            builtGeneration_ = generation;
            rebuild();
        }
        updateLabel(info);
    }
    const ShpInfo info = shp ? shp_info(*view_) : ShpInfo{};
    const bool paged = shp && info.frames < info.total;
    windowLabel_->setVisible(paged);
    windowFrames_->setVisible(paged);
    if (paged) {
        const QSignalBlocker block(windowFrames_);
        windowFrames_->setRange(1, info.total);
        windowFrames_->setValue(info.span);
    }
    prev_->setEnabled(shp);
    next_->setEnabled(shp);
    selectCell_->setEnabled(shp);
    grid_->setEnabled(shp);
    syncGrid();
}

void ShpFramesPanel::updateLabel(const ShpInfo& info)
{
    QString text = current_ >= 0 ? tr("Frame %1 of %2").arg(current_).arg(info.total)
                                 : tr("%1 frames, %2x%3").arg(info.total)
                                       .arg(info.width)
                                       .arg(info.height);
    if (info.frames < info.total) {
        text += tr(" (%1-%2 loaded)").arg(info.first).arg(info.first + info.frames - 1);
    }
    label_->setText(text);
    label_->setToolTip(info.frames < info.total
                           ? tr("This sprite is too big to hold all at once, so Kooka loads "
                                "%1 frames at a time. Picking another frame loads its "
                                "frames; that starts a new undo history.")
                                 .arg(info.span)
                           : QString());
}

void ShpFramesPanel::rebuild()
{
    const ShpInfo info = shp_info(*view_);
    const int frames = info.total;
    const bool reuse = list_->count() == frames;
    if (!reuse) {
        list_->clear();
    }
    const QString palette = info.palette_path + QLatin1Char('|') + info.palette_name;
    if (palette != offPalette_) {
        offThumbs_.clear();
        offPalette_ = palette;
    }
    // Integer scale for crisp pixels when the frame is small, else fit.
    const double fit = std::min(double(kThumb) / info.width, double(kThumb) / info.height);
    const double scale = fit >= 1.0 ? std::floor(fit) : fit;
    const QSize thumb(std::max(1, int(info.width * scale)), std::max(1, int(info.height * scale)));
    for (int i = 0; i < frames; ++i) {
        const QRect cell = cellRect(i);
        QPixmap pixmap(kThumb, kThumb);
        pixmap.fill(Qt::transparent);
        if (!cell.isNull()) {
            offThumbs_.remove(i);
            const QImage image =
                view_->display_image(0, cell.x(), cell.y(), cell.width(), cell.height());
            if (!image.isNull()) {
                const QImage scaled =
                    image.scaled(thumb, Qt::KeepAspectRatio, Qt::FastTransformation);
                QPainter p(&pixmap);
                p.drawImage(QPoint((kThumb - scaled.width()) / 2,
                                   (kThumb - scaled.height()) / 2),
                            scaled);
            }
        } else {
            pixmap = offSheetThumbnail(i);
        }
        if (reuse) {
            list_->item(i)->setIcon(QIcon(pixmap));
        } else {
            auto* item = new QListWidgetItem(QIcon(pixmap), QString::number(i), list_);
            item->setTextAlignment(Qt::AlignHCenter);
        }
        list_->item(i)->setToolTip(cell.isNull() ? tr("Frame %1 (click to load it)").arg(i)
                                                 : tr("Frame %1").arg(i));
    }
    if (current_ >= frames) {
        current_ = -1;
    }
    if (current_ >= 0) {
        list_->setCurrentRow(current_);
    }
}

QPixmap ShpFramesPanel::offSheetThumbnail(int index)
{
    const quint64 stamp = shp_frame_stamp(*view_, index);
    const auto cached = offThumbs_.constFind(index);
    if (cached != offThumbs_.cend() && cached->first == stamp) {
        return cached->second;
    }
    QPixmap pixmap(kThumb, kThumb);
    pixmap.fill(Qt::transparent);
    const ShpThumb t = shp_frame_thumbnail(*view_, index, kThumb);
    if (t.width > 0 && t.height > 0 && t.argb.size() == size_t(t.width) * size_t(t.height)) {
        QImage image(t.width, t.height, QImage::Format_ARGB32);
        for (int y = 0; y < t.height; ++y) {
            std::copy_n(t.argb.data() + size_t(y) * size_t(t.width), t.width,
                        reinterpret_cast<uint32_t*>(image.scanLine(y)));
        }
        // Small frames scale up by a whole number, as on the sheet.
        const double fit = std::min(double(kThumb) / t.width, double(kThumb) / t.height);
        const QImage scaled =
            fit >= 2.0 ? image.scaled(t.width * int(fit), t.height * int(fit), Qt::KeepAspectRatio,
                                      Qt::FastTransformation)
                       : image;
        QPainter p(&pixmap);
        p.setOpacity(0.75);
        p.drawImage(QPoint((kThumb - scaled.width()) / 2, (kThumb - scaled.height()) / 2), scaled);
    }
    offThumbs_.insert(index, {stamp, pixmap});
    return pixmap;
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
            grid.active = onSheet(current_) ? current_ - info.first : -1;
            grid.visible = grid_->isChecked();
        }
    }
    canvas_->setShpGrid(grid);
}

void ShpFramesPanel::focusFrame(int index)
{
    const QRect cell = cellRect(index);
    if (cell.isNull()) {
        if (view_ && index >= 0 && index < shp_info(*view_).total) {
            emit pageRequested(index);
        }
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
        updateLabel(shp_info(*view_));
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
