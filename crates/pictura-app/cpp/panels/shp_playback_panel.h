// SPDX-License-Identifier: GPL-3.0-or-later
// KookaPictura-SHP fork: the playback preview of an SHP document.
#pragma once

#include <QtCore/QCache>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtGui/QImage>
#include <QtWidgets/QWidget>

class QCheckBox;
class QLabel;
class QSpinBox;
class QToolButton;

namespace pictura {

class PictureView;

// Plays the frames `from`..`to` at `fps`, once or in a loop, in its own
// preview; it never moves the editor's current frame or pages a big sprite.
// Frames on the sheet are read from the live composite (edits show at
// once); frames off a paged window are decoded from the frame store or the
// file into a cache bounded by kCacheBytes.
class ShpPlaybackPanel : public QWidget {
    Q_OBJECT

public:
    // The decoded off-sheet frames kept for playback, in bytes.
    static constexpr qint64 kCacheBytes = 16 * 1024 * 1024;

    explicit ShpPlaybackPanel(QWidget* parent = nullptr);

    void setView(PictureView* view);
    // Follow the document: frame count, edits, palette.
    void refresh();

    bool playing() const;
    void setPlaying(bool on);
    // Show the next frame of the range (what one tick of the timer does).
    void advance();
    int frame() const { return frame_; }
    void setFrame(int frame);
    void setRange(int from, int to);
    int from() const;
    int to() const;
    void setFps(int fps);
    int fps() const;
    void setLoop(bool on);
    int intervalMs() const { return timer_.interval(); }
    // Frame `index` as the preview shows it (unscaled); null when there is
    // none.
    QImage frameImage(int index);
    qint64 cachedBytes() const { return cache_.totalCost(); }

private:
    void showFrame(int frame);
    void syncControls();

    QPointer<PictureView> view_;
    int frame_ = 0;
    int total_ = 0;
    quint64 generation_ = 0;
    QString palette_;
    struct Cached {
        quint64 stamp = 0;
        QImage image;
    };
    QCache<int, Cached> cache_;
    QTimer timer_;
    QLabel* screen_ = nullptr;
    QLabel* label_ = nullptr;
    QToolButton* play_ = nullptr;
    QSpinBox* fps_ = nullptr;
    QSpinBox* from_ = nullptr;
    QSpinBox* to_ = nullptr;
    QCheckBox* loop_ = nullptr;
};

} // namespace pictura
