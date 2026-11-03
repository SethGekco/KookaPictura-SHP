// SPDX-License-Identifier: GPL-3.0-or-later
// KookaPictura-SHP fork: New TMP from Image and the conversions between the
// Westwood formats.

#include "tmp_authoring.h"

#include "shp_authoring.h"
#include "shp_palettes.h"

#include <QtCore/QSignalBlocker>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "pictura_app/src/cxxqt_object.cxxqt.h"
#include "pictura_app/src/cxxqt_object/shp.cxxqt.h"

namespace pictura {

namespace {

// pictura_shp::tmp::MAX_BLOCKS.
constexpr int kMaxBlocks = 256;
// The largest grid the cell table shows; a bigger one keeps the image's
// default cells.
constexpr int kMaxTableCells = 64 * 64;

QSpinBox* spin(QWidget* parent, const char* name, int low, int high, int value)
{
    auto* box = new QSpinBox(parent);
    box->setObjectName(QLatin1String(name));
    box->setRange(low, high);
    box->setValue(std::clamp(value, low, high));
    return box;
}

QDialogButtonBox* okCancel(QDialog* dialog)
{
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    return buttons;
}

// A spin box with the game's name for its value beside it.
QWidget* named(QWidget* parent, QSpinBox* box, QLabel* name)
{
    auto* row = new QWidget(parent);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(box);
    layout->addWidget(name, 1);
    return row;
}

} // namespace

TmpFromImageDialog::TmpFromImageDialog(int imageWidth, int imageHeight, CellsFn cells,
                                       PaletteFn palette, const QString& theater,
                                       QWidget* parent)
    : QDialog(parent)
    , imageWidth_(std::max(1, imageWidth))
    , imageHeight_(std::max(1, imageHeight))
    , cellsFn_(std::move(cells))
    , paletteFn_(std::move(palette))
{
    setObjectName(QStringLiteral("tmpFromImageDialog"));
    setWindowTitle(tr("New TMP from Image"));
    auto* intro = new QLabel(
        tr("The %1 x %2 image is laid over a template grid, its top-left on the grid's "
           "top-left. Each checked cell becomes a tile holding the pixels inside its diamond. "
           "The template opens as a new document; this image is not changed.")
            .arg(imageWidth_)
            .arg(imageHeight_),
        this);
    intro->setWordWrap(true);

    game_ = new QComboBox(this);
    game_->setObjectName(QStringLiteral("tmpImageGame"));
    game_->addItem(tr("Red Alert 2 / Yuri's Revenge (60 x 30 tiles)"));
    game_->addItem(tr("Tiberian Sun (48 x 24 tiles)"));
    // A guess at the grid: the image's width spans blocks x + blocks y half
    // tiles.
    const int span = std::max(2, int(std::lround(2.0 * imageWidth_ / 60.0)));
    cols_ = spin(this, "tmpImageCols", 1, kMaxBlocks, (span + 1) / 2);
    rows_ = spin(this, "tmpImageRows", 1, kMaxBlocks, std::max(1, span - (span + 1) / 2));
    cellTable_ = new QTableWidget(this);
    cellTable_->setObjectName(QStringLiteral("tmpImageCells"));
    cellTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    cellTable_->setToolTip(tr("Checked cells hold a tile (column, row of the template grid)."));
    resetCells_ = new QPushButton(tr("Cells with Pixels"), this);
    resetCells_->setObjectName(QStringLiteral("tmpImageResetCells"));
    resetCells_->setToolTip(tr("Check exactly the cells whose diamond covers a pixel that is not "
                               "transparent."));

    height_ = spin(this, "tmpImageHeight", 0, 255, 0);
    land_ = spin(this, "tmpImageLand", 0, 255, 0);
    ramp_ = spin(this, "tmpImageRamp", 0, 255, 0);
    landName_ = new QLabel(this);
    rampName_ = new QLabel(this);

    theater_ = new QComboBox(this);
    theater_->setObjectName(QStringLiteral("tmpImageTheater"));
    const QStringList names = {tr("Temperate"), tr("Snow"), tr("Urban"),
                               tr("New urban"), tr("Desert"), tr("Lunar")};
    for (int i = 0; i < theaterSuffixes().size() && i < names.size(); ++i) {
        theater_->addItem(QStringLiteral("%1 (.%2)").arg(names[i], theaterSuffixes()[i]),
                          theaterSuffixes()[i]);
    }
    if (!theater.isEmpty()) {
        const int at = theater_->findData(theater.toLower());
        if (at >= 0) {
            theater_->setCurrentIndex(at);
        }
        theater_->setEnabled(false);
    }
    palette_ = new ShpPaletteField(paletteFn_ ? paletteFn_(this->theater()) : QString(), this);

    colours_ = new QComboBox(this);
    colours_->setObjectName(QStringLiteral("tmpImageColours"));
    colours_->addItem(tr("Nearest palette colour (under half opaque: transparent)"));
    colours_->addItem(tr("Exact palette colours only (Save reports the rest)"));
    z_ = new QComboBox(this);
    z_->setObjectName(QStringLiteral("tmpImageZ"));
    z_->addItem(tr("None"));
    z_->addItem(tr("Flat ground (the depths the game's flat tiles hold)"));
    extras_ = new QCheckBox(tr("Keep pixels outside the tiles as extra blocks"), this);
    extras_->setObjectName(QStringLiteral("tmpImageExtras"));
    extras_->setToolTip(tr("Each pixel outside every diamond goes to the extra block of the tile "
                           "whose diamond is nearest. Unchecked, Save offers to clip them."));
    summary_ = new QLabel(this);
    summary_->setObjectName(QStringLiteral("tmpImageSummary"));
    summary_->setWordWrap(true);

    auto* grid = new QWidget(this);
    auto* gridRow = new QHBoxLayout(grid);
    gridRow->setContentsMargins(0, 0, 0, 0);
    gridRow->addWidget(cols_);
    gridRow->addWidget(new QLabel(tr("x"), grid));
    gridRow->addWidget(rows_);
    gridRow->addStretch(1);
    gridRow->addWidget(resetCells_);

    auto* form = new QFormLayout;
    form->addRow(tr("Game:"), game_);
    form->addRow(tr("Template (cells):"), grid);
    form->addRow(tr("Tiles:"), cellTable_);
    form->addRow(tr("Height:"), height_);
    form->addRow(tr("Land type:"), named(this, land_, landName_));
    form->addRow(tr("Ramp type:"), named(this, ramp_, rampName_));
    form->addRow(tr("Theater:"), theater_);
    form->addRow(tr("Palette:"), palette_);
    form->addRow(tr("Other colours:"), colours_);
    form->addRow(tr("Z-data:"), z_);
    form->addRow(QString(), extras_);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(intro);
    layout->addLayout(form);
    layout->addWidget(summary_);
    layout->addWidget(okCancel(this));

    connect(game_, &QComboBox::currentIndexChanged, this, &TmpFromImageDialog::resetCells);
    connect(cols_, &QSpinBox::valueChanged, this, &TmpFromImageDialog::resetCells);
    connect(rows_, &QSpinBox::valueChanged, this, &TmpFromImageDialog::resetCells);
    connect(resetCells_, &QPushButton::clicked, this, &TmpFromImageDialog::resetCells);
    connect(cellTable_, &QTableWidget::itemChanged, this, &TmpFromImageDialog::updateSummary);
    connect(land_, &QSpinBox::valueChanged, this, &TmpFromImageDialog::updateNames);
    connect(ramp_, &QSpinBox::valueChanged, this, &TmpFromImageDialog::updateNames);
    connect(theater_, &QComboBox::currentIndexChanged, this, [this] {
        if (paletteFn_) {
            palette_->edit()->setText(paletteFn_(this->theater()));
        }
    });
    updateNames();
    resetCells();
}

int TmpFromImageDialog::tileWidth() const
{
    return game_->currentIndex() == 1 ? 48 : 60;
}

int TmpFromImageDialog::tileHeight() const
{
    return tileWidth() / 2;
}

void TmpFromImageDialog::resetCells()
{
    const int bx = cols_->value();
    const int by = rows_->value();
    QList<bool> on = cellsFn_ ? cellsFn_(tileWidth(), tileHeight(), bx, by) : QList<bool>();
    if (on.size() != bx * by) {
        on = QList<bool>(bx * by, true);
    }
    const QSignalBlocker block(cellTable_);
    const bool shown = bx * by <= kMaxTableCells;
    cellTable_->clear();
    cellTable_->setColumnCount(shown ? bx : 0);
    cellTable_->setRowCount(shown ? by : 0);
    cellTable_->setProperty("defaults", QVariant::fromValue(on));
    for (int row = 0; shown && row < by; ++row) {
        for (int col = 0; col < bx; ++col) {
            auto* item = new QTableWidgetItem(QString::number(row * bx + col));
            item->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
            item->setCheckState(on[row * bx + col] ? Qt::Checked : Qt::Unchecked);
            cellTable_->setItem(row, col, item);
        }
    }
    updateSummary();
}

QList<int> TmpFromImageDialog::cells() const
{
    const int bx = cols_->value();
    const int by = rows_->value();
    QList<int> out;
    if (cellTable_->columnCount() != bx || cellTable_->rowCount() != by) {
        for (const bool on : cellTable_->property("defaults").value<QList<bool>>()) {
            out << (on ? 1 : 0);
        }
        return out;
    }
    for (int row = 0; row < by; ++row) {
        for (int col = 0; col < bx; ++col) {
            const QTableWidgetItem* item = cellTable_->item(row, col);
            out << (item && item->checkState() == Qt::Checked ? 1 : 0);
        }
    }
    return out;
}

void TmpFromImageDialog::updateSummary()
{
    const QList<int> on = cells();
    const int tiles = int(std::count(on.begin(), on.end(), 1));
    const int span = cols_->value() + rows_->value();
    const int width = span * tileWidth() / 2;
    const int height = span * tileHeight() / 2;
    QString text = tr("%n tile(s) of %1 x %2. The grid covers %3 x %4 px of the image.", nullptr,
                      tiles)
                       .arg(tileWidth())
                       .arg(tileHeight())
                       .arg(width)
                       .arg(height);
    if (cellTable_->columnCount() == 0) {
        text += tr(" The grid is too large to list; the cells with pixels are used.");
    }
    summary_->setText(text);
    if (auto* buttons = findChild<QDialogButtonBox*>()) {
        buttons->button(QDialogButtonBox::Ok)->setEnabled(tiles > 0);
    }
}

void TmpFromImageDialog::updateNames()
{
    landName_->setText(QString(tmp_land_label(land_->value())));
    rampName_->setText(QString(tmp_ramp_label(ramp_->value())));
}

QList<int> TmpFromImageDialog::params() const
{
    return {tileWidth(),
            tileHeight(),
            cols_->value(),
            rows_->value(),
            height_->value(),
            land_->value(),
            ramp_->value(),
            colours_->currentIndex() == 0 ? 1 : 0,
            z_->currentIndex(),
            extras_->isChecked() ? 1 : 0};
}

QString TmpFromImageDialog::palette() const
{
    return palette_->path();
}

QString TmpFromImageDialog::theater() const
{
    return theater_->currentData().toString();
}

QString TmpFromImageDialog::summaryForTest() const
{
    return summary_->text();
}

TmpToShpDialog::TmpToShpDialog(int tiles, int tileWidth, int tileHeight, int width, int height,
                               const QString& palette, QWidget* parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("tmpToShpDialog"));
    setWindowTitle(tr("Save As SHP"));
    auto* intro = new QLabel(tr("This is a TMP terrain template. Saving it as SHP converts its "
                                "pixels into a new SHP document in the same palette (%1), which "
                                "is then saved; the template is not changed.")
                                 .arg(palette.isEmpty() ? tr("the built-in grey") : palette),
                             this);
    intro->setWordWrap(true);
    frames_ = new QComboBox(this);
    frames_->setObjectName(QStringLiteral("tmpToShpFrames"));
    frames_->addItem(tr("One frame per tile (%n frame(s) of %1 x %2 or more)", nullptr, tiles)
                         .arg(tileWidth)
                         .arg(tileHeight));
    frames_->addItem(tr("One frame for the whole template (%1 x %2)").arg(width).arg(height));
    extras_ = new QCheckBox(tr("Draw the extra blocks over the tiles"), this);
    extras_->setObjectName(QStringLiteral("tmpToShpExtras"));
    extras_->setChecked(true);
    note_ = new QLabel(tr("Not carried over: an SHP has no place for the tile headers (cell, "
                          "position, height, land and ramp type, radar colors) or the z-data. "
                          "Per tile, every frame holds its tile at the same place, grown to "
                          "hold the furthest extra block."),
                       this);
    note_->setObjectName(QStringLiteral("tmpToShpNote"));
    note_->setWordWrap(true);
    auto* form = new QFormLayout;
    form->addRow(tr("Frames:"), frames_);
    form->addRow(QString(), extras_);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(intro);
    layout->addLayout(form);
    layout->addWidget(note_);
    layout->addWidget(okCancel(this));
}

bool TmpToShpDialog::perTile() const
{
    return frames_->currentIndex() == 0;
}

bool TmpToShpDialog::extras() const
{
    return extras_->isChecked();
}

QString TmpToShpDialog::noteForTest() const
{
    return note_->text();
}

} // namespace pictura

#include "moc_tmp_authoring.cpp"
