// SPDX-License-Identifier: GPL-3.0-or-later
// KookaPictura-SHP fork: File > New SHP and the Save As SHP options.

#include "shp_authoring.h"

#include <QtCore/QFileInfo>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>

namespace pictura {

namespace {

// The SHP format's limits (pictura_shp::MAX_SIDE, MAX_FRAMES).
constexpr int kMaxSide = 4096;
constexpr int kMaxFrames = 4096;

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

} // namespace

ShpPaletteField::ShpPaletteField(const QString& path, QWidget* parent)
    : QWidget(parent)
{
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    edit_ = new QLineEdit(path, this);
    edit_->setObjectName(QStringLiteral("shpPalettePath"));
    edit_->setPlaceholderText(tr("Built-in grey palette"));
    auto* browse = new QPushButton(tr("Browse…"), this);
    row->addWidget(edit_, 1);
    row->addWidget(browse);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString dir = edit_->text().isEmpty() ? QString() : QFileInfo(edit_->text()).path();
        const QString chosen = QFileDialog::getOpenFileName(
            this, tr("Palette"), dir, tr("Palettes (*.pal *.PAL);;All Files (*)"));
        if (!chosen.isEmpty()) {
            edit_->setText(chosen);
        }
    });
}

QString ShpPaletteField::path() const
{
    return edit_->text().trimmed();
}

ShpNewDialog::ShpNewDialog(const QString& palette, QWidget* parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("shpNewDialog"));
    setWindowTitle(tr("New SHP"));
    auto* form = new QFormLayout;
    width_ = spin(this, "shpNewWidth", 1, kMaxSide, 60);
    height_ = spin(this, "shpNewHeight", 1, kMaxSide, 48);
    frames_ = spin(this, "shpNewFrames", 1, kMaxFrames, 8);
    palette_ = new ShpPaletteField(palette, this);
    size_ = new QLabel(this);
    form->addRow(tr("Frame width:"), width_);
    form->addRow(tr("Frame height:"), height_);
    form->addRow(tr("Frames:"), frames_);
    form->addRow(tr("Palette:"), palette_);
    form->addRow(QString(), size_);
    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(okCancel(this));
    for (QSpinBox* box : {width_, height_, frames_}) {
        connect(box, &QSpinBox::valueChanged, this, &ShpNewDialog::updateSize);
    }
    updateSize();
}

int ShpNewDialog::frameWidth() const
{
    return width_->value();
}

int ShpNewDialog::frameHeight() const
{
    return height_->value();
}

int ShpNewDialog::frames() const
{
    return frames_->value();
}

void ShpNewDialog::updateSize()
{
    size_->setText(tr("Index 0 is transparent; every frame starts empty."));
}

ShpConvertDialog::ShpConvertDialog(int imageWidth, int imageHeight, const QString& palette,
                                   QWidget* parent)
    : QDialog(parent)
    , imageWidth_(std::max(1, imageWidth))
    , imageHeight_(std::max(1, imageHeight))
{
    setObjectName(QStringLiteral("shpConvertDialog"));
    setWindowTitle(tr("Save As SHP"));
    auto* intro = new QLabel(
        tr("This image is not an SHP sheet. Choose how to cut its %1 x %2 pixels into frames; "
           "the SHP opens as a new document and is saved from there. This image is not "
           "changed.")
            .arg(imageWidth_)
            .arg(imageHeight_),
        this);
    intro->setWordWrap(true);
    width_ = spin(this, "shpConvertWidth", 1, std::min(kMaxSide, imageWidth_), imageWidth_);
    height_ = spin(this, "shpConvertHeight", 1, std::min(kMaxSide, imageHeight_), imageHeight_);
    cols_ = spin(this, "shpConvertCols", 1, imageWidth_, 1);
    rows_ = spin(this, "shpConvertRows", 1, imageHeight_, 1);
    gutter_ = spin(this, "shpConvertGutter", 0, 64, 0);
    frames_ = spin(this, "shpConvertFrames", 1, kMaxFrames, 1);
    colours_ = new QComboBox(this);
    colours_->setObjectName(QStringLiteral("shpConvertColours"));
    colours_->addItem(tr("Nearest palette colour (under half opaque: transparent)"));
    colours_->addItem(tr("Exact palette colours only (Save reports the rest)"));
    palette_ = new ShpPaletteField(palette, this);
    summary_ = new QLabel(this);
    summary_->setObjectName(QStringLiteral("shpConvertSummary"));
    auto* form = new QFormLayout;
    form->addRow(tr("Frame width:"), width_);
    form->addRow(tr("Frame height:"), height_);
    form->addRow(tr("Columns:"), cols_);
    form->addRow(tr("Rows:"), rows_);
    form->addRow(tr("Gap between cells:"), gutter_);
    form->addRow(tr("Frames:"), frames_);
    form->addRow(tr("Palette:"), palette_);
    form->addRow(tr("Other colours:"), colours_);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(intro);
    layout->addLayout(form);
    layout->addWidget(summary_);
    layout->addWidget(okCancel(this));
    connect(width_, &QSpinBox::valueChanged, this, &ShpConvertDialog::fromFrameSize);
    connect(height_, &QSpinBox::valueChanged, this, &ShpConvertDialog::fromFrameSize);
    connect(gutter_, &QSpinBox::valueChanged, this, &ShpConvertDialog::fromFrameSize);
    connect(cols_, &QSpinBox::valueChanged, this, &ShpConvertDialog::fromGrid);
    connect(rows_, &QSpinBox::valueChanged, this, &ShpConvertDialog::fromGrid);
    connect(frames_, &QSpinBox::valueChanged, this, &ShpConvertDialog::sync);
    fromFrameSize();
}

void ShpConvertDialog::fromFrameSize()
{
    if (syncing_) {
        return;
    }
    syncing_ = true;
    const int g = gutter_->value();
    cols_->setValue(std::max(1, (imageWidth_ + g) / (width_->value() + g)));
    rows_->setValue(std::max(1, (imageHeight_ + g) / (height_->value() + g)));
    frames_->setValue(cols_->value() * rows_->value());
    syncing_ = false;
    sync();
}

void ShpConvertDialog::fromGrid()
{
    if (syncing_) {
        return;
    }
    syncing_ = true;
    const int g = gutter_->value();
    const int cols = cols_->value();
    const int rows = rows_->value();
    width_->setValue(std::max(1, (imageWidth_ - (cols - 1) * g) / cols));
    height_->setValue(std::max(1, (imageHeight_ - (rows - 1) * g) / rows));
    frames_->setValue(cols * rows);
    syncing_ = false;
    sync();
}

void ShpConvertDialog::sync()
{
    const int cells = cols_->value() * rows_->value();
    const int g = gutter_->value();
    const bool fits = cols_->value() * width_->value() + (cols_->value() - 1) * g <= imageWidth_
        && rows_->value() * height_->value() + (rows_->value() - 1) * g <= imageHeight_
        && frames_->value() <= cells;
    summary_->setText(fits ? tr("%1 frames of %2 x %3.")
                                 .arg(frames_->value())
                                 .arg(width_->value())
                                 .arg(height_->value())
                           : tr("That grid does not fit the image."));
    if (auto* buttons = findChild<QDialogButtonBox*>()) {
        buttons->button(QDialogButtonBox::Ok)->setEnabled(fits);
    }
}

QList<int> ShpConvertDialog::cut() const
{
    return {width_->value(), height_->value(), cols_->value(),
            rows_->value(),  gutter_->value(), frames_->value()};
}

bool ShpConvertDialog::nearest() const
{
    return colours_->currentIndex() == 0;
}

} // namespace pictura

#include "moc_shp_authoring.cpp"
