// SPDX-License-Identifier: GPL-3.0-or-later
// KookaPictura-SHP fork: Undo and Redo bring the frame they changed into
// view.
#pragma once

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QRect>
#include <QtCore/QVector>

namespace pictura {

class PictureView;
class ShpFramesPanel;

// Keeps a fingerprint of every cell on the sheet. After a history restore
// (Undo, Redo, a History panel jump), the cells whose fingerprint changed
// are the frames that step touched; the first of them is focused in the
// Frames panel, view-only (scroll and outline, never the selection, so the
// redo stack is untouched). The current frame wins when it is among them.
//
// The fingerprints follow every edit from the event loop after it (and the
// dock's refresh). Only cells the canvas reported damaged are fingerprinted
// again, except after a change with no bounded region (a frame operation,
// Change Palette), which rescans the sheet; a sheet over kScanBytes is
// never rescanned whole, and a restore with no bounded region there
// focuses nothing.
class ShpUndoFocus : public QObject {
    Q_OBJECT

public:
    static constexpr qint64 kScanBytes = 64 * 1024 * 1024;

    ShpUndoFocus(ShpFramesPanel* frames, QObject* parent);

    void setView(PictureView* view);
    // After any document change (the dock's debounced refresh): bring the
    // fingerprints up to date without focusing anything.
    void refresh();
    // The frames the last restore changed (file frame numbers).
    QVector<int> lastChanged() const { return lastChanged_; }

private:
    void restored();
    // Fingerprint the cells meeting `area` (all cells when null) and return
    // those that changed.
    QVector<int> rescan(const QRect& area);
    bool syncLayout();

    QPointer<PictureView> view_;
    ShpFramesPanel* frames_ = nullptr;
    QVector<QMetaObject::Connection> connections_;
    QVector<quint64> hashes_; // per sheet cell; 0 = unknown
    QString layout_;
    QRect damage_;
    quint64 revision_ = 0;
    bool refreshQueued_ = false;
    QVector<int> lastChanged_;
};

} // namespace pictura
