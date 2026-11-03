// SPDX-License-Identifier: GPL-3.0-or-later
// KookaPictura-SHP fork: the Tiles panel of a TMP terrain document.
#pragma once

#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtWidgets/QWidget>

class QCheckBox;
class QLabel;
class QPushButton;
class QTableWidget;

namespace pictura {

class ImageView;
class PictureView;

// A TMP template opens with its tiles at their template positions on the
// Tiles layer, extra blocks on the Extras layers and z-data on the hidden Z
// layers. This panel lists the tiles: slot, cell in the template grid,
// canvas position, height, land and ramp type (number and the game's name),
// whether it has an extra block (and on which Extras layer) and z-data, and
// its radar colors. Clicking a row brings the tile into view and highlights
// its outline; the canvas outlines every diamond, and every extra block
// dashed, like the SHP frame grid. Height, land, ramp and radar colors are
// edited in place (double-click or F2), each change one undoable step. The
// buttons below the list reshape the template, each one undoable step too:
// move the selected tile to another cell (its header position follows the
// grid, and it is redrawn there with its extra block), add a blank tile in
// a free cell, remove the selected tile, and add or remove its extra block
// (an added block starts transparent, without z-data). The canvas grows or
// shrinks to fit, checked against the size limit when it grows.
class TmpTilesPanel : public QWidget {
    Q_OBJECT

public:
    explicit TmpTilesPanel(QWidget* parent = nullptr);

    void setView(PictureView* view, ImageView* canvas);
    // Rebuild the rows when the document changed since the last build.
    void refresh();

    int tileCount() const;
    // The slot of the selected row, or -1.
    int currentSlot() const;
    // Select the row of `slot`, bring the tile into view, highlight it.
    void focusSlot(int slot);
    // Bring canvas pixel `point` into view (zooming in on a small canvas).
    void focusPoint(const QPoint& point);

    QTableWidget* table() const { return table_; }
    QCheckBox* outlines() const { return outlines_; }
    QString summaryForTest() const;

    // Apply the fields typed into `row` (height, land, ramp, radar) to its
    // tile as one undoable "Tile Fields" step; false (and a message) when
    // a value does not parse or is out of range.
    bool applyRow(int row);

    // The reshaping edits (false and a message when refused). Cells are
    // column and row of the template grid; an extra block's x and y are
    // offsets from its tile.
    bool moveTile(int slot, int column, int row);
    bool addTile(int column, int row);
    bool removeTile(int slot);
    bool addExtra(int slot, int x, int y, int width, int height);
    bool removeExtra(int slot);
    // The buttons' dialogs, then the edit; false when cancelled or refused.
    bool askMove();
    bool askAdd();
    bool askExtra();

    QPushButton* moveButton() const { return move_; }
    QPushButton* addButton() const { return add_; }
    QPushButton* removeButton() const { return remove_; }
    QPushButton* addExtraButton() const { return addExtra_; }
    QPushButton* removeExtraButton() const { return removeExtra_; }

signals:
    void message(const QString& text);

private:
    void rebuild();
    void syncGrid();
    int rowOfSlot(int slot) const;
    // One tmp_edit_tiles call; selects `focus` afterwards when it is a tile.
    bool reshape(int op, int slot, const QList<int>& args, const QString& done, int focus);
    void syncButtons();
    // One flag per slot: true where a tile is.
    QList<bool> takenCells() const;

    QPointer<PictureView> view_;
    QPointer<ImageView> canvas_;
    QLabel* summary_ = nullptr;
    QTableWidget* table_ = nullptr;
    QCheckBox* outlines_ = nullptr;
    QPushButton* move_ = nullptr;
    QPushButton* add_ = nullptr;
    QPushButton* remove_ = nullptr;
    QPushButton* addExtra_ = nullptr;
    QPushButton* removeExtra_ = nullptr;
    quint64 builtRevision_ = 0;
    quint64 builtGeneration_ = 0;
    int builtHistory_ = -1;
    bool built_ = false;
};

} // namespace pictura
