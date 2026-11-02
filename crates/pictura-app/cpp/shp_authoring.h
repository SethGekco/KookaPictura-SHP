// SPDX-License-Identifier: GPL-3.0-or-later
// KookaPictura-SHP fork: the dialogs that make new SHP documents -- File >
// New SHP, and the options Save As SHP asks for when the image is not an
// SHP sheet.
#pragma once

#include <QtWidgets/QDialog>

class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;

namespace pictura {

// A .pal path with a Browse button; empty means the built-in grey palette.
class ShpPaletteField : public QWidget {
    Q_OBJECT

public:
    ShpPaletteField(const QString& path, QWidget* parent);
    QString path() const;
    QLineEdit* edit() const { return edit_; }

private:
    QLineEdit* edit_ = nullptr;
};

// File > New SHP: frame size, frame count, palette.
class ShpNewDialog : public QDialog {
    Q_OBJECT

public:
    ShpNewDialog(const QString& palette, QWidget* parent);
    int frameWidth() const;
    int frameHeight() const;
    int frames() const;
    QString palette() const { return palette_->path(); }

    QSpinBox* widthBox() const { return width_; }
    QSpinBox* heightBox() const { return height_; }
    QSpinBox* framesBox() const { return frames_; }
    ShpPaletteField* paletteField() const { return palette_; }

private:
    void updateSize();

    QSpinBox* width_ = nullptr;
    QSpinBox* height_ = nullptr;
    QSpinBox* frames_ = nullptr;
    ShpPaletteField* palette_ = nullptr;
    QLabel* size_ = nullptr;
};

// Save As SHP for an image that is not an SHP sheet: how to cut it into
// frames (frame size, or columns x rows; a gutter between cells; how many
// frames, read row by row), the palette, and how colours that are not
// palette colours are mapped.
class ShpConvertDialog : public QDialog {
    Q_OBJECT

public:
    ShpConvertDialog(int imageWidth, int imageHeight, const QString& palette, QWidget* parent);
    // Frame width, height, columns, rows, gutter, frame count.
    QList<int> cut() const;
    QString palette() const { return palette_->path(); }
    // Nearest palette colour (alpha under 128: transparent), or exact
    // colours only (the save gate reports the rest).
    bool nearest() const;

    QSpinBox* widthBox() const { return width_; }
    QSpinBox* heightBox() const { return height_; }
    QSpinBox* colsBox() const { return cols_; }
    QSpinBox* rowsBox() const { return rows_; }
    QSpinBox* gutterBox() const { return gutter_; }
    QSpinBox* framesBox() const { return frames_; }
    QComboBox* coloursBox() const { return colours_; }
    ShpPaletteField* paletteField() const { return palette_; }

private:
    // Keep the grid and the frame size consistent with the image.
    void fromFrameSize();
    void fromGrid();
    void sync();

    int imageWidth_ = 0;
    int imageHeight_ = 0;
    bool syncing_ = false;
    QSpinBox* width_ = nullptr;
    QSpinBox* height_ = nullptr;
    QSpinBox* cols_ = nullptr;
    QSpinBox* rows_ = nullptr;
    QSpinBox* gutter_ = nullptr;
    QSpinBox* frames_ = nullptr;
    QComboBox* colours_ = nullptr;
    ShpPaletteField* palette_ = nullptr;
    QLabel* summary_ = nullptr;
};

} // namespace pictura
