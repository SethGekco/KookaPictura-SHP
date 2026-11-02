// SPDX-License-Identifier: GPL-3.0-or-later
// KookaPictura-SHP fork: Undo and Redo bring the frame they changed into
// view.

#include "shp_undo_focus.h"

#include "panels/shp_frames_panel.h"

#include <QtCore/QHashFunctions>
#include <QtCore/QTimer>
#include <QtGui/QImage>

#include "pictura_app/src/cxxqt_object.cxxqt.h"
#include "pictura_app/src/cxxqt_object/shp.cxxqt.h"

namespace pictura {

ShpUndoFocus::ShpUndoFocus(ShpFramesPanel* frames, QObject* parent)
    : QObject(parent)
    , frames_(frames)
{
}

void ShpUndoFocus::setView(PictureView* view)
{
    if (view == view_) {
        refresh();
        return;
    }
    for (const auto& c : std::as_const(connections_)) {
        disconnect(c);
    }
    connections_.clear();
    view_ = view;
    hashes_.clear();
    layout_.clear();
    damage_ = {};
    lastChanged_.clear();
    if (view) {
        // After an edit, once control is back in the event loop: a restore
        // reports its damage and `changed` before `historyRestored`, and
        // must compare with the fingerprints from before it.
        const auto queue = [this] {
            if (!refreshQueued_) {
                refreshQueued_ = true;
                QTimer::singleShot(0, this, [this] {
                    refreshQueued_ = false;
                    refresh();
                });
            }
        };
        connections_ << connect(view, &PictureView::regionDamaged, this,
                                [this, queue](int x, int y, int w, int h) {
                                    damage_ |= QRect(x, y, w, h);
                                    queue();
                                });
        connections_ << connect(view, &PictureView::changed, this, queue);
        connections_ << connect(view, &PictureView::historyRestored, this,
                                &ShpUndoFocus::restored);
    }
    refresh();
}

bool ShpUndoFocus::syncLayout()
{
    const ShpInfo info = view_ ? shp_info(*view_) : ShpInfo{};
    if (!info.valid) {
        hashes_.clear();
        layout_.clear();
        return false;
    }
    // Cells are compared by position in frame order, so a frame-count (and
    // column) change keeps the fingerprints; a new cell size, window or
    // document does not.
    const QString layout = QStringLiteral("%1x%2+%3@%4#%5")
                               .arg(info.width)
                               .arg(info.height)
                               .arg(info.gutter)
                               .arg(info.first)
                               .arg(view_->document_generation());
    if (layout != layout_) {
        layout_ = layout;
        hashes_.fill(0, info.frames);
        return true;
    }
    return false;
}

QVector<int> ShpUndoFocus::rescan(const QRect& area)
{
    QVector<int> changed;
    const ShpInfo info = shp_info(*view_);
    if (!info.valid || info.cols <= 0) {
        return changed;
    }
    const int before = int(hashes_.size());
    hashes_.resize(info.frames);
    const qint64 sheetBytes = qint64(view_->document_width()) * view_->document_height() * 4;
    if (area.isNull() && sheetBytes > kScanBytes) {
        hashes_.fill(0);
        return changed;
    }
    for (int i = 0; i < info.frames; ++i) {
        const QRect cell((i % info.cols) * (info.width + info.gutter),
                         (i / info.cols) * (info.height + info.gutter), info.width, info.height);
        if (!area.isNull() && !area.intersects(cell)) {
            continue;
        }
        const QImage image = view_->display_image(0, cell.x(), cell.y(), cell.width(),
                                                  cell.height());
        const quint64 hash =
            quint64(qHashBits(image.constBits(), size_t(image.sizeInBytes()), 0x5348u)) | 1u;
        const quint64 old = hashes_[i];
        if ((old != 0 && old != hash) || (old == 0 && i >= before)) {
            changed << info.first + i;
        }
        hashes_[i] = hash;
    }
    return changed;
}

void ShpUndoFocus::refresh()
{
    if (!view_ || !shp_info(*view_).valid) {
        hashes_.clear();
        layout_.clear();
        damage_ = {};
        return;
    }
    const bool fresh = syncLayout();
    const quint64 revision = view_->canvas_revision();
    if (fresh || (damage_.isNull() && revision != revision_)) {
        rescan(QRect());
    } else if (!damage_.isNull()) {
        rescan(damage_);
    }
    damage_ = {};
    revision_ = revision;
}

void ShpUndoFocus::restored()
{
    lastChanged_.clear();
    if (!view_ || !shp_info(*view_).valid) {
        return;
    }
    if (syncLayout()) {
        // Another window or document: nothing to compare with.
        rescan(QRect());
        damage_ = {};
        revision_ = view_->canvas_revision();
        return;
    }
    lastChanged_ = rescan(damage_);
    damage_ = {};
    revision_ = view_->canvas_revision();
    if (lastChanged_.isEmpty() || !frames_) {
        return;
    }
    const int current = frames_->current();
    const int target = lastChanged_.contains(current) ? current : lastChanged_.first();
    frames_->refresh();
    // View-only: setting the marquee here would be a new history step and
    // drop the redo stack.
    const bool selects = frames_->selectsCell();
    frames_->setSelectsCell(false);
    frames_->focusFrame(target);
    frames_->setSelectsCell(selects);
}

} // namespace pictura

#include "moc_shp_undo_focus.cpp"
