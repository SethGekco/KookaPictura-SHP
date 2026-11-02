// SPDX-License-Identifier: GPL-3.0-or-later
// KookaPictura-SHP fork: the Frames panel of an SHP document.
#pragma once

#include <QtCore/QHash>
#include <QtCore/QPair>
#include <QtCore/QPointer>
#include <QtCore/QRect>
#include <QtGui/QPixmap>
#include <QtWidgets/QWidget>

class QAction;
class QCheckBox;
class QLabel;
class QHBoxLayout;
class QListWidget;
class QSpinBox;
class QToolButton;

namespace pictura {

class ImageView;
class PictureView;
struct ShpInfo;

// Thumbnails of every frame of the file. Clicking one (or stepping with
// , and .) brings its cell into view and, with "Select cell" on, sets the
// marquee to the cell so strokes cannot bleed into a neighbour. Navigation
// is view-only; only the selection is a (normal) document change.
//
// A paged SHP holds a window of its frames on the sheet. The others are
// still listed (thumbnails decoded off the sheet and cached); picking one,
// or stepping past the window's edge, asks for its window with
// pageRequested().
class ShpFramesPanel : public QWidget {
    Q_OBJECT

public:
    explicit ShpFramesPanel(QWidget* parent = nullptr);

    void setView(PictureView* view, ImageView* canvas);
    // Rebuild the thumbnails when the pixels or the layout changed.
    void refresh();

    // Focus frame `index`: scroll (and if needed zoom) to its cell, outline
    // it, and select it when "Select cell" is on.
    void focusFrame(int index);
    void step(int delta);
    int current() const { return current_; }
    int frameCount() const;
    // Frame `index`'s cell in sheet pixels; null past the last frame or for
    // a frame off the sheet.
    QRect cellRect(int index) const;
    // Is file frame `index` on the sheet?
    bool onSheet(int index) const;

    QListWidget* list() const { return list_; }
    bool selectsCell() const;
    void setSelectsCell(bool on);
    bool gridVisible() const;
    // Onion skin: ghost the frames before and after the current one into its
    // cell (view-only; see ImageView::paintShpOnion).
    QCheckBox* onion() const { return onion_; }
    QSpinBox* onionOpacity() const { return onionOpacity_; }

    // "Frames loaded": shown for a paged SHP, holding its window size.
    QSpinBox* windowFrames() const { return windowFrames_; }

    // The frames selected in the list as one range (first, count): the
    // span from the lowest to the highest selected frame, or the current
    // frame alone; count 0 when there is none.
    QPair<int, int> selectedRange() const;
    // The frame-operation actions by name: insert, duplicate, delete,
    // earlier, later, reverse. Each asks with frameOpRequested().
    QAction* opAction(const QString& name) const;

signals:
    // File frame `frame` is off the sheet: its window should be shown.
    void pageRequested(int frame);
    // The user asked for windows of `count` frames.
    void windowSizeRequested(int count);
    // A frame operation (see shp_frame_op): insert, duplicate, delete,
    // move, reverse.
    void frameOpRequested(const QString& op, int first, int count, int to);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void buildOps(QHBoxLayout* row);
    void requestOp(const QString& name);
    void rebuild();
    void updateLabel(const ShpInfo& info);
    QPixmap offSheetThumbnail(int index);
    void syncGrid();

    QPointer<PictureView> view_;
    QPointer<ImageView> canvas_;
    int current_ = -1;
    quint64 builtRevision_ = 0;
    quint64 builtGeneration_ = 0;
    QString builtLayout_;
    // Thumbnails of frames off the sheet: frame -> (stamp, picture), valid
    // for `offPalette_`.
    QHash<int, QPair<quint64, QPixmap>> offThumbs_;
    QString offPalette_;
    QListWidget* list_ = nullptr;
    QLabel* label_ = nullptr;
    QToolButton* prev_ = nullptr;
    QToolButton* next_ = nullptr;
    QCheckBox* selectCell_ = nullptr;
    QCheckBox* grid_ = nullptr;
    QCheckBox* onion_ = nullptr;
    QSpinBox* onionOpacity_ = nullptr;
    QLabel* windowLabel_ = nullptr;
    QSpinBox* windowFrames_ = nullptr;
    QHash<QString, QAction*> ops_;
};

} // namespace pictura
