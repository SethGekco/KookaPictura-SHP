// SPDX-License-Identifier: GPL-3.0-or-later
// KookaPictura-SHP fork: the Frames panel of an SHP document.
#pragma once

#include <QtCore/QPointer>
#include <QtCore/QRect>
#include <QtWidgets/QWidget>

class QCheckBox;
class QLabel;
class QListWidget;
class QToolButton;

namespace pictura {

class ImageView;
class PictureView;

// Thumbnails of every frame of the sheet. Clicking one (or stepping with
// , and .) brings its cell into view and, with "Select cell" on, sets the
// marquee to the cell so strokes cannot bleed into a neighbour. Navigation
// is view-only; only the selection is a (normal) document change.
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
    // Frame `index`'s cell in sheet pixels; null past the last frame.
    QRect cellRect(int index) const;

    QListWidget* list() const { return list_; }
    bool selectsCell() const;
    void setSelectsCell(bool on);
    bool gridVisible() const;

private:
    void rebuild();
    void syncGrid();

    QPointer<PictureView> view_;
    QPointer<ImageView> canvas_;
    int current_ = -1;
    quint64 builtRevision_ = 0;
    quint64 builtGeneration_ = 0;
    QString builtLayout_;
    QListWidget* list_ = nullptr;
    QLabel* label_ = nullptr;
    QToolButton* prev_ = nullptr;
    QToolButton* next_ = nullptr;
    QCheckBox* selectCell_ = nullptr;
    QCheckBox* grid_ = nullptr;
};

} // namespace pictura
