// SPDX-License-Identifier: GPL-3.0-or-later
// KookaPictura-SHP fork: the frame grid of an SHP sheet, drawn over the
// canvas. View-only: it never touches the document or a sampled colour.

#include "image_view.h"

#include <QtGui/QPainter>

#include <algorithm>
#include <cmath>

namespace pictura {

void ImageView::setShpGrid(const ShpGrid& grid)
{
    const ShpGrid& g = shpGrid_;
    if (g.cell == grid.cell && g.cols == grid.cols && g.frames == grid.frames
        && g.gutter == grid.gutter && g.active == grid.active && g.visible == grid.visible
        && g.onion == grid.onion && g.onionOpacity == grid.onionOpacity) {
        return;
    }
    shpGrid_ = grid;
    update();
}

// Onion skin: the frames before and after the active one, tinted (red
// before, blue after) and ghosted into its cell only where the active frame
// is transparent, so its own pixels read as they are. Drawn over the
// composite: nothing reaches the document, the sampled colors or the saved
// bytes. Only cells on the sheet are ghosted: the first frame of a paged
// window shows no "before" (and the last no "after"); frame 0 and the last
// frame do not wrap. Very large cells (over kOnionMaxPixels) are skipped so
// a paint never copies megabytes.
namespace {
constexpr qint64 kOnionMaxPixels = 1024 * 1024;
} // namespace

void ImageView::paintShpOnion(QPainter& painter)
{
    const ShpGrid& g = shpGrid_;
    if (!g.onion || g.active < 0 || g.active >= g.frames
        || qint64(g.cell.width()) * g.cell.height() > kOnionMaxPixels) {
        return;
    }
    const auto origin = [&](int i) {
        return QPoint((i % g.cols) * (g.cell.width() + g.gutter),
                      (i / g.cols) * (g.cell.height() + g.gutter));
    };
    const auto cellImage = [&](int i) {
        const QRect r(origin(i), g.cell);
        if (!image_.isNull()) {
            return image_.copy(r);
        }
        return levelProvider_.crop ? levelProvider_.crop(0, r.x(), r.y(), r.width(), r.height())
                                   : QImage();
    };
    QImage ghost(g.cell, QImage::Format_ARGB32_Premultiplied);
    ghost.fill(Qt::transparent);
    bool any = false;
    {
        QPainter p(&ghost);
        const struct {
            int frame;
            QColor tint;
        } sides[] = {{g.active - 1, QColor(220, 40, 40)}, {g.active + 1, QColor(40, 90, 230)}};
        for (const auto& side : sides) {
            if (side.frame < 0 || side.frame >= g.frames) {
                continue;
            }
            const QImage cell = cellImage(side.frame);
            if (cell.isNull()) {
                continue;
            }
            QImage tinted = cell.convertToFormat(QImage::Format_ARGB32_Premultiplied);
            {
                QPainter t(&tinted);
                t.setCompositionMode(QPainter::CompositionMode_SourceAtop);
                t.setOpacity(0.45);
                t.fillRect(tinted.rect(), side.tint);
            }
            p.setOpacity(std::clamp(g.onionOpacity, 0, 100) / 100.0);
            p.drawImage(0, 0, tinted);
            any = true;
        }
        const QImage active = cellImage(g.active);
        if (any && !active.isNull()) {
            p.setOpacity(1.0);
            p.setCompositionMode(QPainter::CompositionMode_DestinationOut);
            p.drawImage(0, 0, active);
        }
    }
    if (!any) {
        return;
    }
    painter.save();
    painter.setTransform(viewRotation());
    painter.setClipRect(viewRect());
    painter.translate(offset_);
    painter.scale(zoom_, zoom_);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter.drawImage(QPointF(origin(g.active)), ghost);
    painter.restore();
}

// Drawn in the view frame so the lines stay one screen pixel wide at any
// zoom: each cell is outlined just outside its pixels (in the gutter), and
// the active cell gets a second, highlighted outline.
void ImageView::paintShpGrid(QPainter& painter)
{
    const ShpGrid& g = shpGrid_;
    if (docSize_.isEmpty() || g.cell.isEmpty() || g.cols <= 0 || g.frames <= 0) {
        return;
    }
    paintShpOnion(painter);
    painter.save();
    painter.setTransform(viewRotation());
    painter.setClipRect(viewRect());
    painter.setRenderHint(QPainter::Antialiasing, false);
    const auto cellRect = [&](int i) {
        const double x = (i % g.cols) * (g.cell.width() + g.gutter);
        const double y = (i / g.cols) * (g.cell.height() + g.gutter);
        const double left = std::floor(x * zoom_ + offset_.x()) - 0.5;
        const double top = std::floor(y * zoom_ + offset_.y()) - 0.5;
        const double right = std::ceil((x + g.cell.width()) * zoom_ + offset_.x()) + 0.5;
        const double bottom = std::ceil((y + g.cell.height()) * zoom_ + offset_.y()) + 0.5;
        return QRectF(QPointF(left, top), QPointF(right, bottom));
    };
    if (g.visible) {
        const QRectF visible = viewRect();
        QColor line = palette().color(QPalette::Mid);
        line.setAlpha(170);
        painter.setPen(QPen(line, 0));
        painter.setBrush(Qt::NoBrush);
        for (int i = 0; i < g.frames; ++i) {
            const QRectF r = cellRect(i);
            if (r.intersects(visible)) {
                painter.drawRect(r);
            }
        }
    }
    if (g.active >= 0 && g.active < g.frames) {
        painter.setPen(QPen(palette().color(QPalette::Highlight), 0));
        painter.drawRect(cellRect(g.active).adjusted(-1, -1, 1, 1));
    }
    painter.restore();
}

} // namespace pictura
