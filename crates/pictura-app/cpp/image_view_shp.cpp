// SPDX-License-Identifier: GPL-3.0-or-later
// KookaPictura-SHP fork: the frame grid of an SHP sheet, drawn over the
// canvas. View-only: it never touches the document or a sampled colour.

#include "image_view.h"

#include <QtGui/QPainter>

#include <cmath>

namespace pictura {

void ImageView::setShpGrid(const ShpGrid& grid)
{
    const ShpGrid& g = shpGrid_;
    if (g.cell == grid.cell && g.cols == grid.cols && g.frames == grid.frames
        && g.gutter == grid.gutter && g.active == grid.active && g.visible == grid.visible) {
        return;
    }
    shpGrid_ = grid;
    update();
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
