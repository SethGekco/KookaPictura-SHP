// SPDX-License-Identifier: GPL-3.0-or-later
// KookaPictura-SHP fork: the Palette panel of an SHP document.
#pragma once

#include <QtCore/QList>
#include <QtGui/QRgb>
#include <QtWidgets/QWidget>

class QCheckBox;
class QLabel;
class QToolButton;

namespace pictura {

class ColorState;
class PictureView;

// The 256 palette entries as a fixed 16 x 16 grid. Each cell shows the
// colour the sheet draws that index with; index 0 (transparency) shows a
// checker. The foreground's index is outlined.
class ShpPaletteGrid : public QWidget {
    Q_OBJECT

public:
    explicit ShpPaletteGrid(QWidget* parent = nullptr);

    void setColours(const QList<QRgb>& colours);
    void setMarked(int index);
    int indexAt(const QPoint& pos) const;
    QRect cellRect(int index) const;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void picked(int index, bool background);
    void hovered(int index);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    int cell() const;

    QList<QRgb> colours_;
    int marked_ = -1;
    int hover_ = -1;
};

// Click an entry for the foreground, Alt-click for the background: the
// colour is set exactly (the sheet's own RGB), so a Pencil stroke with it
// saves that index. Hovering reads out the index, its colour in the .pal
// and what the game does with it. Load Palette rebinds the document by
// index as one undoable "Change Palette" step.
class ShpPalettePanel : public QWidget {
    Q_OBJECT

public:
    explicit ShpPalettePanel(ColorState* colors, QWidget* parent = nullptr);

    void setView(PictureView* view);
    void refresh();

    // Rebind the document to the palette at `path`; remembers it for the
    // document's folder when `forFolder`. False (with the reason shown in the
    // readout) when it is not a palette.
    bool loadPalette(const QString& path, bool forFolder);

    ShpPaletteGrid* grid() const { return grid_; }
    QString readoutForTest() const;
    QString foregroundReadoutForTest() const;
    // The canvas pixel under the cursor (sheet coordinates, from
    // ImageView::mouseMoved): its frame, palette index, colour and role.
    void showCanvasHover(const QPointF& imagePos);
    QString canvasHoverForTest() const;

signals:
    void message(const QString& text);

private:
    void pick(int index, bool background);
    void showHover(int index);
    void showForeground();
    void choosePalette();
    void showMenu();

    ColorState* colors_ = nullptr;
    PictureView* view_ = nullptr;
    QList<QRgb> unique_;
    QList<QRgb> source_;
    QString paletteName_;
    ShpPaletteGrid* grid_ = nullptr;
    QLabel* name_ = nullptr;
    QLabel* readout_ = nullptr;
    QLabel* foreground_ = nullptr;
    QLabel* canvasHover_ = nullptr;
    QToolButton* load_ = nullptr;
    QToolButton* more_ = nullptr;
    QCheckBox* forFolder_ = nullptr;
};

} // namespace pictura
