// SPDX-License-Identifier: GPL-3.0-or-later
// KookaPictura-SHP fork: the frame grid of an SHP sheet, drawn over the
// canvas, the house-color preview, the onion skin, the shadow underlay and
// the off-palette marks. View-only: none of it
// touches the document or a sampled color.

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
        && g.onion == grid.onion && g.onionOpacity == grid.onionOpacity
        && g.offPalette == grid.offPalette && g.first == grid.first
        && g.shadowHalf == grid.shadowHalf && g.shadowUnderlay == grid.shadowUnderlay
        && g.activeShadow.cacheKey() == grid.activeShadow.cacheKey()
        && g.remapFrom == grid.remapFrom && g.remapTo == grid.remapTo
        && g.tileOutlines == grid.tileOutlines && g.extraOutlines == grid.extraOutlines
        && g.activeTile == grid.activeTile) {
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
    const auto cellImage = [this](int i) { return shpCellImage(i); };
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

// Cell `cell` of the sheet as the composite shows it.
QImage ImageView::shpCellImage(int cell) const
{
    const ShpGrid& g = shpGrid_;
    if (cell < 0 || cell >= g.frames || g.cols <= 0) {
        return {};
    }
    const QRect r(QPoint((cell % g.cols) * (g.cell.width() + g.gutter),
                         (cell / g.cols) * (g.cell.height() + g.gutter)),
                  g.cell);
    if (!image_.isNull()) {
        return image_.copy(r);
    }
    return levelProvider_.crop ? levelProvider_.crop(0, r.x(), r.y(), r.width(), r.height())
                               : QImage();
}

// Shadow underlay: each art cell on screen shows its shadow frame (any
// non-transparent pixel of it) as a half-transparent black under the art's
// own pixels. View-only like the onion skin. A shadow frame off the sheet is
// shown only for the active frame (`activeShadow`); at most
// kShadowBudget pixels are drawn per paint, the active cell first.
namespace {
constexpr qint64 kShadowBudget = 4 * 1024 * 1024;
} // namespace

void ImageView::paintShpShadow(QPainter& painter)
{
    const ShpGrid& g = shpGrid_;
    if (!g.shadowUnderlay || g.shadowHalf <= 0 || g.cell.isEmpty() || g.cols <= 0) {
        return;
    }
    const qint64 cellPixels = qint64(g.cell.width()) * g.cell.height();
    if (cellPixels > kOnionMaxPixels) {
        return;
    }
    const auto origin = [&](int i) {
        return QPoint((i % g.cols) * (g.cell.width() + g.gutter),
                      (i / g.cols) * (g.cell.height() + g.gutter));
    };
    // The visible part of the document, in sheet pixels.
    const QRectF visible = viewRect();
    const QRectF docVisible((visible.left() - offset_.x()) / zoom_,
                            (visible.top() - offset_.y()) / zoom_, visible.width() / zoom_,
                            visible.height() / zoom_);
    QList<int> cells;
    if (g.active >= 0 && g.active < g.frames) {
        cells << g.active;
    }
    for (int c = 0; c < g.frames; ++c) {
        if (c != g.active && QRectF(origin(c), g.cell).intersects(docVisible)) {
            cells << c;
        }
    }
    painter.save();
    painter.setTransform(viewRotation());
    painter.setClipRect(viewRect());
    painter.translate(offset_);
    painter.scale(zoom_, zoom_);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    qint64 budget = kShadowBudget;
    for (const int c : std::as_const(cells)) {
        const int art = g.first + c;
        if (art >= g.shadowHalf || budget < cellPixels) {
            continue;
        }
        const int shadowCell = art + g.shadowHalf - g.first;
        QImage shadow;
        if (shadowCell >= 0 && shadowCell < g.frames) {
            shadow = shpCellImage(shadowCell);
        } else if (c == g.active) {
            shadow = g.activeShadow;
        }
        if (shadow.isNull()) {
            continue;
        }
        budget -= cellPixels;
        QImage shade = shadow.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        {
            QPainter p(&shade);
            p.setCompositionMode(QPainter::CompositionMode_SourceIn);
            p.fillRect(shade.rect(), QColor(0, 0, 0, 128));
            const QImage artPixels = shpCellImage(c);
            if (!artPixels.isNull()) {
                p.setCompositionMode(QPainter::CompositionMode_DestinationOut);
                p.drawImage(0, 0, artPixels);
            }
        }
        painter.drawImage(QPointF(origin(c)), shade);
    }
    painter.restore();
}

// House-color preview: the visible pixels whose color is one of the remap
// indices' sheet colors are drawn again in the preview ramp. View-only. At
// most kHouseBudget pixels are recolored per change (zoomed far out on a
// big sheet only the active cell is); the result is kept until the
// pixels, the region or the colors change.
namespace {
constexpr qint64 kHouseBudget = 8 * 1024 * 1024;
} // namespace

void ImageView::paintShpHouse(QPainter& painter)
{
    const ShpGrid& g = shpGrid_;
    if (g.remapFrom.isEmpty() || g.remapFrom.size() != g.remapTo.size() || docSize_.isEmpty()
        || g.cell.isEmpty() || g.cols <= 0) {
        return;
    }
    const QRectF visible = viewRect();
    QRect region = QRectF((visible.left() - offset_.x()) / zoom_,
                          (visible.top() - offset_.y()) / zoom_, visible.width() / zoom_,
                          visible.height() / zoom_)
                       .toAlignedRect()
                   & QRect(QPoint(), docSize_);
    if (qint64(region.width()) * region.height() > kHouseBudget) {
        if (g.active < 0 || g.active >= g.frames) {
            return;
        }
        const QRect cell(QPoint((g.active % g.cols) * (g.cell.width() + g.gutter),
                                (g.active / g.cols) * (g.cell.height() + g.gutter)),
                         g.cell);
        region &= cell;
        if (qint64(region.width()) * region.height() > kHouseBudget) {
            return;
        }
    }
    if (region.isEmpty()) {
        return;
    }
    const quint64 revision = levelProvider_.canvasRevision ? levelProvider_.canvasRevision()
                                                           : quint64(image_.cacheKey());
    ShpHouseCache& cache = shpHouseCache_;
    if (cache.image.isNull() || cache.revision != revision || cache.region != region
        || cache.from != g.remapFrom || cache.to != g.remapTo) {
        QImage source = !image_.isNull() ? image_.copy(region)
                        : levelProvider_.crop
                            ? levelProvider_.crop(0, region.x(), region.y(), region.width(),
                                                  region.height())
                            : QImage();
        if (source.isNull()) {
            return;
        }
        source = source.convertToFormat(QImage::Format_ARGB32);
        QImage out(region.size(), QImage::Format_ARGB32_Premultiplied);
        out.fill(Qt::transparent);
        for (int y = 0; y < source.height(); ++y) {
            const auto* in = reinterpret_cast<const QRgb*>(source.constScanLine(y));
            auto* dst = reinterpret_cast<QRgb*>(out.scanLine(y));
            for (int x = 0; x < source.width(); ++x) {
                if (qAlpha(in[x]) != 255) {
                    continue;
                }
                const qsizetype k = g.remapFrom.indexOf(in[x] | 0xFF000000u);
                if (k >= 0) {
                    dst[x] = g.remapTo[k] | 0xFF000000u;
                }
            }
        }
        cache = {revision, region, g.remapFrom, g.remapTo, out};
    }
    painter.save();
    painter.setTransform(viewRotation());
    painter.setClipRect(viewRect());
    painter.translate(offset_);
    painter.scale(zoom_, zoom_);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, smoothSamplingForZoom(zoom_));
    painter.drawImage(QPointF(region.topLeft()), cache.image);
    painter.restore();
}

// Drawn in the view frame so the lines stay one screen pixel wide at any
// zoom: each cell is outlined just outside its pixels (in the gutter), and
// the active cell gets a second, highlighted outline.
void ImageView::paintShpGrid(QPainter& painter)
{
    const ShpGrid& g = shpGrid_;
    if (!docSize_.isEmpty() && !g.tileOutlines.isEmpty()) {
        paintTmpOutlines(painter);
        return;
    }
    if (docSize_.isEmpty() || g.cell.isEmpty() || g.cols <= 0 || g.frames <= 0) {
        return;
    }
    paintShpHouse(painter);
    paintShpShadow(painter);
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
    paintShpOffPalette(painter);
}

// TMP terrain: each tile's diamond and each extra block (dashed), one
// screen pixel wide at any zoom, the active tile highlighted. View-only.
void ImageView::paintTmpOutlines(QPainter& painter)
{
    const ShpGrid& g = shpGrid_;
    painter.save();
    painter.setTransform(viewRotation());
    painter.setClipRect(viewRect());
    painter.setRenderHint(QPainter::Antialiasing, true);
    const auto toView = [&](const QPointF& p) {
        return QPointF(p.x() * zoom_ + offset_.x(), p.y() * zoom_ + offset_.y());
    };
    const auto polygon = [&](const QPolygonF& image) {
        QPolygonF out;
        out.reserve(image.size());
        for (const QPointF& p : image) {
            out.append(toView(p));
        }
        return out;
    };
    const QRectF visible = viewRect();
    if (g.visible) {
        QColor line = palette().color(QPalette::Mid);
        line.setAlpha(190);
        painter.setPen(QPen(line, 0));
        painter.setBrush(Qt::NoBrush);
        for (const QPolygonF& tile : g.tileOutlines) {
            const QPolygonF v = polygon(tile);
            if (v.boundingRect().intersects(visible)) {
                painter.drawPolygon(v);
            }
        }
        QPen dashed(line, 0, Qt::DashLine);
        painter.setPen(dashed);
        for (const QRectF& extra : g.extraOutlines) {
            const QRectF v(toView(extra.topLeft()), toView(extra.bottomRight()));
            if (v.intersects(visible)) {
                painter.drawRect(v);
            }
        }
    }
    if (g.activeTile >= 0 && g.activeTile < g.tileOutlines.size()) {
        painter.setPen(QPen(palette().color(QPalette::Highlight), 2));
        painter.drawPolygon(polygon(g.tileOutlines.at(g.activeTile)));
    }
    painter.restore();
    paintShpOffPalette(painter);
}

// Off-palette pixels: outlined in magenta when a pixel is at least four
// screen pixels wide (its own colour stays visible inside), filled below
// that. View-only, like the rest of this file.
void ImageView::paintShpOffPalette(QPainter& painter)
{
    const ShpGrid& g = shpGrid_;
    if (g.offPalette.isEmpty()) {
        return;
    }
    painter.save();
    painter.setTransform(viewRotation());
    painter.setClipRect(viewRect());
    painter.setRenderHint(QPainter::Antialiasing, false);
    const QRectF visible = viewRect();
    const QColor mark(255, 0, 255);
    const bool outline = zoom_ >= 4.0;
    painter.setPen(outline ? QPen(mark, 0) : Qt::NoPen);
    painter.setBrush(outline ? QBrush(Qt::NoBrush) : QBrush(mark));
    for (const QPoint& p : g.offPalette) {
        const QPointF tl(p.x() * zoom_ + offset_.x(), p.y() * zoom_ + offset_.y());
        const QRectF r(tl, QSizeF(std::max(zoom_, 1.0), std::max(zoom_, 1.0)));
        if (!r.intersects(visible)) {
            continue;
        }
        painter.drawRect(outline ? r.adjusted(0.5, 0.5, -0.5, -0.5) : r);
    }
    painter.restore();
}

} // namespace pictura
