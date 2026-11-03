// SPDX-License-Identifier: GPL-3.0-or-later
// KookaPictura-SHP fork: the dialogs that turn one Westwood format into
// another -- an image into a new TMP terrain template (File > New TMP from
// Image, Save As TMP of an image), and a template into an SHP (Save As SHP
// of a template) -- and the Tiles panel's cell and extra-block dialogs.
#pragma once

#include <QtWidgets/QDialog>

#include <functional>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QTableWidget;

namespace pictura {

class ShpPaletteField;

// The options of a new TMP template made from an image: the game (its tile
// size), the template grid in cells, which cells hold tiles (by default
// every cell whose diamond covers a pixel that is not transparent), the
// height, land and ramp of every tile (flat clear ground by default), the
// theater and palette, how colours become palette indices, the z-data, and
// whether pixels outside the diamonds become extra blocks.
class TmpFromImageDialog : public QDialog {
    Q_OBJECT

public:
    // The default cells of a cx x cy tile grid of blocks x by over the image.
    using CellsFn = std::function<QList<bool>(int cx, int cy, int bx, int by)>;
    // The palette a template of the theater `suffix` would open with.
    using PaletteFn = std::function<QString(const QString& suffix)>;

    // `theater`: the suffix the template will be saved under (the theater
    // box is then fixed), or empty to choose one.
    TmpFromImageDialog(int imageWidth, int imageHeight, CellsFn cells, PaletteFn palette,
                       const QString& theater, QWidget* parent);

    // Tile width and height, blocks x and y, height, land, ramp, nearest
    // (0/1), z-data (0 none, 1 flat), extras (0/1): what tmp_from_image
    // takes.
    QList<int> params() const;
    // One 0/1 per slot, row by row.
    QList<int> cells() const;
    QString palette() const;
    QString theater() const;

    QComboBox* gameBox() const { return game_; }
    QSpinBox* colsBox() const { return cols_; }
    QSpinBox* rowsBox() const { return rows_; }
    QTableWidget* cellTable() const { return cellTable_; }
    QPushButton* resetCellsButton() const { return resetCells_; }
    QSpinBox* heightBox() const { return height_; }
    QSpinBox* landBox() const { return land_; }
    QSpinBox* rampBox() const { return ramp_; }
    QComboBox* theaterBox() const { return theater_; }
    QComboBox* coloursBox() const { return colours_; }
    QComboBox* zBox() const { return z_; }
    QCheckBox* extrasBox() const { return extras_; }
    ShpPaletteField* paletteField() const { return palette_; }
    QString summaryForTest() const;

private:
    int tileWidth() const;
    int tileHeight() const;
    // Rebuild the cell table for the grid, checked as the image suggests.
    void resetCells();
    void updateSummary();
    void updateNames();

    int imageWidth_ = 0;
    int imageHeight_ = 0;
    CellsFn cellsFn_;
    PaletteFn paletteFn_;
    QComboBox* game_ = nullptr;
    QSpinBox* cols_ = nullptr;
    QSpinBox* rows_ = nullptr;
    QTableWidget* cellTable_ = nullptr;
    QPushButton* resetCells_ = nullptr;
    QSpinBox* height_ = nullptr;
    QSpinBox* land_ = nullptr;
    QSpinBox* ramp_ = nullptr;
    QLabel* landName_ = nullptr;
    QLabel* rampName_ = nullptr;
    QComboBox* theater_ = nullptr;
    QComboBox* colours_ = nullptr;
    QComboBox* z_ = nullptr;
    QCheckBox* extras_ = nullptr;
    ShpPaletteField* palette_ = nullptr;
    QLabel* summary_ = nullptr;
};

// Save As SHP of a TMP template: one frame per tile (each the tile's
// diamond, the same place in every frame) or one frame of the whole
// template, and whether extra blocks are drawn over the diamonds. A note
// says what an SHP cannot hold: the tile headers and the z-data.
class TmpToShpDialog : public QDialog {
    Q_OBJECT

public:
    TmpToShpDialog(int tiles, int tileWidth, int tileHeight, int width, int height,
                   const QString& palette, QWidget* parent);
    bool perTile() const;
    bool extras() const;

    QComboBox* framesBox() const { return frames_; }
    QCheckBox* extrasBox() const { return extras_; }
    QString noteForTest() const;

private:
    QComboBox* frames_ = nullptr;
    QCheckBox* extras_ = nullptr;
    QLabel* note_ = nullptr;
};

// The Tiles panel's Move and Add: a cell of the template grid. Cells that
// hold a tile are refused (OK is disabled); the first free one is offered.
class TmpCellDialog : public QDialog {
    Q_OBJECT

public:
    // `taken`: one flag per slot, row by row, true where a tile is.
    TmpCellDialog(const QString& title, const QString& text, int blocksX, int blocksY,
                  const QList<bool>& taken, QWidget* parent);
    int column() const;
    int row() const;

    QSpinBox* columnBox() const { return col_; }
    QSpinBox* rowBox() const { return row_; }
    QPushButton* okButton() const { return ok_; }

private:
    void update();

    int blocksX_ = 1;
    QList<bool> taken_;
    QSpinBox* col_ = nullptr;
    QSpinBox* row_ = nullptr;
    QLabel* state_ = nullptr;
    QPushButton* ok_ = nullptr;
};

// The Tiles panel's Add Extra Block: its offset from the tile and its size
// (by default a tile-sized block right above the tile). The block starts
// transparent and without z-data.
class TmpExtraDialog : public QDialog {
    Q_OBJECT

public:
    TmpExtraDialog(int slot, int tileWidth, int tileHeight, QWidget* parent);
    // x and y offset from the tile, width, height.
    QList<int> values() const;

    QSpinBox* xBox() const { return x_; }
    QSpinBox* yBox() const { return y_; }
    QSpinBox* widthBox() const { return w_; }
    QSpinBox* heightBox() const { return h_; }

private:
    QSpinBox* x_ = nullptr;
    QSpinBox* y_ = nullptr;
    QSpinBox* w_ = nullptr;
    QSpinBox* h_ = nullptr;
};

} // namespace pictura
