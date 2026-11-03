// SPDX-License-Identifier: GPL-3.0-or-later
// KookaPictura-SHP fork: the Palette panel of an SHP document.

#include "panels/shp_palette_panel.h"

#include "api/document_api.h"
#include "panels/color_panel.h"
#include "shp_palettes.h"

#include <QtCore/QFileInfo>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMenu>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>

#include <cmath>

#include "pictura_app/src/cxxqt_object.cxxqt.h"
#include "pictura_app/src/cxxqt_object/shp.cxxqt.h"

namespace pictura {

namespace {

constexpr int kSide = 16;
const char* const kKindProperty = "shpPaletteKind";

QString hex(QRgb c)
{
    return QStringLiteral("#%1").arg(c & 0xFFFFFFu, 6, 16, QLatin1Char('0')).toUpper();
}

} // namespace

ShpPaletteGrid::ShpPaletteGrid(QWidget* parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
}

void ShpPaletteGrid::setColours(const QList<QRgb>& colours)
{
    colours_ = colours;
    update();
}

void ShpPaletteGrid::setMarked(int index)
{
    if (marked_ != index) {
        marked_ = index;
        update();
    }
}

int ShpPaletteGrid::cell() const
{
    return std::max(6, std::min(width(), 16 * 18) / kSide);
}

QRect ShpPaletteGrid::cellRect(int index) const
{
    const int c = cell();
    return QRect((index % kSide) * c, (index / kSide) * c, c, c);
}

int ShpPaletteGrid::indexAt(const QPoint& pos) const
{
    const int c = cell();
    if (pos.x() < 0 || pos.y() < 0 || pos.x() >= c * kSide || pos.y() >= c * kSide) {
        return -1;
    }
    const int index = (pos.y() / c) * kSide + pos.x() / c;
    return index < colours_.size() ? index : -1;
}

QSize ShpPaletteGrid::sizeHint() const
{
    return QSize(16 * 14, 16 * 14);
}

QSize ShpPaletteGrid::minimumSizeHint() const
{
    return QSize(16 * 8, 16 * 8);
}

void ShpPaletteGrid::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    const int c = cell();
    for (int i = 0; i < colours_.size(); ++i) {
        const QRect r = cellRect(i);
        if (i == 0) {
            // Transparency: a two-tone checker from the theme.
            p.fillRect(r, palette().base());
            const int h = std::max(1, c / 2);
            p.fillRect(QRect(r.x(), r.y(), h, h), palette().mid());
            p.fillRect(QRect(r.x() + h, r.y() + h, c - h, c - h), palette().mid());
            continue;
        }
        p.fillRect(r, QColor::fromRgb(colours_.at(i)));
    }
    if (marked_ >= 0 && marked_ < colours_.size()) {
        const QRect r = cellRect(marked_).adjusted(0, 0, -1, -1);
        p.setPen(QPen(palette().highlight(), 2));
        p.drawRect(r.adjusted(1, 1, -1, -1));
        p.setPen(QPen(palette().base(), 1));
        p.drawRect(r);
    }
    if (hover_ >= 0 && hover_ != marked_) {
        p.setPen(QPen(palette().text(), 1));
        p.drawRect(cellRect(hover_).adjusted(0, 0, -1, -1));
    }
}

void ShpPaletteGrid::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    setMinimumHeight(cell() * kSide);
}

void ShpPaletteGrid::mousePressEvent(QMouseEvent* event)
{
    const int index = indexAt(event->position().toPoint());
    if (index >= 0 && event->button() == Qt::LeftButton) {
        emit picked(index, event->modifiers().testFlag(Qt::AltModifier));
    }
}

void ShpPaletteGrid::mouseMoveEvent(QMouseEvent* event)
{
    const int index = indexAt(event->position().toPoint());
    if (index != hover_) {
        hover_ = index;
        update();
        emit hovered(index);
    }
}

void ShpPaletteGrid::leaveEvent(QEvent*)
{
    if (hover_ != -1) {
        hover_ = -1;
        update();
        emit hovered(-1);
    }
}

ShpPalettePanel::ShpPalettePanel(ColorState* colors, QWidget* parent)
    : QWidget(parent)
    , colors_(colors)
{
    setObjectName(QStringLiteral("shpPalettePanel"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(4);

    auto* top = new QHBoxLayout;
    name_ = new QLabel(this);
    name_->setObjectName(QStringLiteral("shpPaletteName"));
    load_ = new QToolButton(this);
    load_->setText(tr("Load Palette…"));
    load_->setToolTip(tr("Rebind the sprite to another .pal by index (undoable)"));
    more_ = new QToolButton(this);
    more_->setText(tr("More"));
    more_->setPopupMode(QToolButton::InstantPopup);
    top->addWidget(name_, 1);
    top->addWidget(load_);
    top->addWidget(more_);
    layout->addLayout(top);

    auto* kindRow = new QHBoxLayout;
    auto* kindLabel = new QLabel(tr("Kind:"), this);
    kind_ = new QComboBox(this);
    kind_->setObjectName(QStringLiteral("shpPaletteKind"));
    kind_->setToolTip(
        tr("Which indices the game reserves: unit (0 transparent, 1 shadow, 16-31 house "
           "color, 240-254 unlit), terrain and cameo (0 only), animation (0 and the 1 "
           "shadow). Auto goes by the palette's name. The shadow and house-color tools "
           "follow it."));
    kind_->addItem(QString(), -1);
    kind_->addItem(tr("Unit"), 0);
    kind_->addItem(tr("Terrain (iso)"), 1);
    kind_->addItem(tr("Animation"), 2);
    kind_->addItem(tr("Cameo"), 3);
    kindLabel->setBuddy(kind_);
    kindRow->addWidget(kindLabel);
    kindRow->addWidget(kind_, 1);
    layout->addLayout(kindRow);
    connect(kind_, &QComboBox::activated, this, [this](int row) {
        if (!view_) {
            return;
        }
        setKindOf(view_, kind_->itemData(row).toInt());
        showHover(-1);
        emit kindChanged();
    });

    forFolder_ = new QCheckBox(tr("Use for this folder"), this);
    forFolder_->setChecked(true);
    forFolder_->setToolTip(tr("Open other SHPs in this folder with the loaded palette"));
    layout->addWidget(forFolder_);

    grid_ = new ShpPaletteGrid(this);
    layout->addWidget(grid_);

    readout_ = new QLabel(this);
    readout_->setObjectName(QStringLiteral("shpPaletteReadout"));
    readout_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    foreground_ = new QLabel(this);
    foreground_->setObjectName(QStringLiteral("shpPaletteForeground"));
    canvasHover_ = new QLabel(this);
    canvasHover_->setObjectName(QStringLiteral("shpCanvasHover"));
    canvasHover_->setToolTip(tr("The palette index of the canvas pixel under the cursor"));
    layout->addWidget(readout_);
    layout->addWidget(foreground_);
    layout->addWidget(canvasHover_);
    layout->addStretch(1);

    connect(grid_, &ShpPaletteGrid::picked, this, &ShpPalettePanel::pick);
    connect(grid_, &ShpPaletteGrid::hovered, this, &ShpPalettePanel::showHover);
    connect(load_, &QToolButton::clicked, this, &ShpPalettePanel::choosePalette);
    connect(more_, &QToolButton::clicked, this, &ShpPalettePanel::showMenu);
    if (colors_) {
        connect(colors_, &ColorState::foregroundChanged, this,
                [this](const QColor&) { showForeground(); });
    }
    refresh();
}

int ShpPalettePanel::kindOf(PictureView* view)
{
    if (!view) {
        return -1;
    }
    const int named = shp_palette_kind(*view);
    if (named < 0) {
        return -1;
    }
    const QVariant own = view->property(kKindProperty);
    return own.isValid() ? own.toInt() : named;
}

void ShpPalettePanel::setKindOf(PictureView* view, int kind)
{
    if (view) {
        view->setProperty(kKindProperty, kind < 0 ? QVariant() : QVariant(kind));
    }
}

// The palette of an SHP sheet or a TMP template (`valid` false for any
// other document).
ShpPalettePanel::DocumentPalette ShpPalettePanel::paletteOf(PictureView* view)
{
    if (!view) {
        return {};
    }
    if (const ShpInfo info = shp_info(*view); info.valid) {
        return {true, false, info.palette_name, info.palette_path};
    }
    if (const TmpInfo info = tmp_info(*view); info.valid) {
        return {true, true, info.palette_name, info.palette_path};
    }
    return {};
}

void ShpPalettePanel::setView(PictureView* view)
{
    view_ = view;
    refresh();
}

void ShpPalettePanel::refresh()
{
    unique_.clear();
    source_.clear();
    paletteName_.clear();
    const DocumentPalette info = paletteOf(view_);
    if (info.valid) {
        for (const uint32_t c : shp_palette_unique(*view_)) {
            unique_.append(QRgb(c));
        }
        for (const uint32_t c : shp_palette_source(*view_)) {
            source_.append(QRgb(c));
        }
        paletteName_ = info.name;
    }
    const bool shp = !unique_.isEmpty();
    grid_->setColours(unique_);
    name_->setText(shp ? tr("Palette: %1").arg(paletteName_) : tr("Not an SHP document"));
    name_->setToolTip(shp ? info.path : QString());
    load_->setEnabled(shp);
    more_->setEnabled(true);
    forFolder_->setEnabled(shp);
    {
        const QSignalBlocker block(kind_);
        const int named = view_ && shp ? shp_palette_kind(*view_) : -1;
        kind_->setItemText(0, named < 0 ? tr("Auto")
                                        : tr("Auto (%1)").arg(QString(shp_kind(named).label)));
        const QVariant own = view_ ? view_->property(kKindProperty) : QVariant();
        kind_->setCurrentIndex(own.isValid() ? kind_->findData(own.toInt()) : 0);
        kind_->setEnabled(shp);
    }
    showHover(-1);
    showForeground();
}

void ShpPalettePanel::pick(int index, bool background)
{
    if (!colors_ || index < 0 || index >= unique_.size()) {
        return;
    }
    if (index == 0) {
        emit message(tr("Index 0 is transparency: paint it with the Eraser in Pencil mode."));
        return;
    }
    // Exact RGB: QColor::fromRgb keeps the 8-bit channels as given.
    const QColor colour = QColor::fromRgb(unique_.at(index) | 0xFF000000u);
    if (background) {
        colors_->setBackground(colour);
    } else {
        colors_->setForeground(colour);
    }
}

void ShpPalettePanel::showHover(int index)
{
    if (index < 0 || index >= source_.size() || !view_) {
        readout_->setText(unique_.isEmpty() ? QString() : tr("Hover an entry for its index"));
        return;
    }
    const QString role = shp_kind_role(kindOf(view_), index);
    QString text = tr("Index %1  %2").arg(index).arg(hex(source_.at(index)));
    if (!role.isEmpty()) {
        text += QStringLiteral("  ") + role;
    }
    readout_->setText(text);
}

// Read from the composite, so it is what the eyedropper would sample and
// what Save would write for that pixel.
void ShpPalettePanel::showCanvasHover(const QPointF& imagePos)
{
    const ShpInfo info = view_ ? shp_info(*view_) : ShpInfo{};
    const int x = int(std::floor(imagePos.x()));
    const int y = int(std::floor(imagePos.y()));
    if (view_ && !info.valid && tmp_info(*view_).valid) {
        showTileHover(x, y);
        return;
    }
    if (!info.valid || info.cols <= 0 || x < 0 || y < 0 || x >= view_->document_width()
        || y >= view_->document_height()) {
        canvasHover_->clear();
        return;
    }
    const int col = x / (info.width + info.gutter);
    const int row = y / (info.height + info.gutter);
    const int cell = row * info.cols + col;
    const bool inCell = x % (info.width + info.gutter) < info.width
                        && y % (info.height + info.gutter) < info.height && cell < info.frames;
    if (!inCell) {
        canvasHover_->setText(tr("Canvas: gutter (%1, %2)").arg(x).arg(y));
        return;
    }
    const int frame = info.first + cell;
    const int fx = x - col * (info.width + info.gutter);
    const int fy = y - row * (info.height + info.gutter);
    const quint32 argb = view_->composite_argb(x, y);
    const int alpha = int(argb >> 24);
    const int index = alpha == 0 ? 0 : alpha == 255 ? shp_index_of(*view_, argb & 0xFFFFFFu) : -1;
    QString text = tr("Frame %1 (%2, %3): ").arg(frame).arg(fx).arg(fy);
    if (index < 0) {
        text += tr("off-palette %1, alpha %2").arg(hex(argb)).arg(alpha);
    } else {
        text += tr("index %1").arg(index);
        if (index > 0 && index < source_.size()) {
            text += QStringLiteral("  ") + hex(source_.at(index));
        }
        const QString role = shp_kind_role(kindOf(view_), index);
        if (!role.isEmpty()) {
            text += QStringLiteral("  ") + role;
        }
    }
    canvasHover_->setText(text);
}

// A TMP template: the tile under the cursor and the index of the active
// layer's pixel there (the Tiles and Extras layers hold colors; a Z layer
// reads as its depth).
void ShpPalettePanel::showTileHover(int x, int y)
{
    if (x < 0 || y < 0 || x >= view_->document_width() || y >= view_->document_height()) {
        canvasHover_->clear();
        return;
    }
    const int slot = tmp_tile_at(*view_, x, y);
    QString text = slot >= 0 ? tr("Tile %1 (%2, %3): ").arg(slot).arg(x).arg(y)
                             : tr("Canvas (%1, %2), no tile: ").arg(x).arg(y);
    const quint32 argb = view_->composite_argb(x, y);
    const int alpha = int(argb >> 24);
    const int index = alpha == 0 ? 0 : alpha == 255 ? shp_index_of(*view_, argb & 0xFFFFFFu) : -1;
    if (index < 0) {
        text += tr("off-palette %1, alpha %2").arg(hex(argb)).arg(alpha);
    } else {
        text += tr("index %1").arg(index);
        if (index > 0 && index < source_.size()) {
            text += QStringLiteral("  ") + hex(source_.at(index));
        }
    }
    canvasHover_->setText(text);
}

QString ShpPalettePanel::canvasHoverForTest() const
{
    return canvasHover_->text();
}

void ShpPalettePanel::showForeground()
{
    if (!view_ || unique_.isEmpty() || !colors_) {
        foreground_->clear();
        grid_->setMarked(-1);
        return;
    }
    const QColor fg = colors_->foreground();
    const int index = shp_index_of(*view_, fg.rgb() & 0xFFFFFFu);
    grid_->setMarked(index);
    foreground_->setText(index >= 0 ? tr("Foreground: index %1").arg(index)
                                    : tr("Foreground: off-palette (%1)").arg(hex(fg.rgb())));
}

bool ShpPalettePanel::loadPalette(const QString& path, bool forFolder)
{
    if (!view_ || path.isEmpty()) {
        return false;
    }
    if (!shp_rebind_palette(*view_, path)) {
        const QString why = shp_last_error();
        readout_->setText(why);
        emit message(why);
        return false;
    }
    const QString document = document_api::filePath(*view_);
    if (forFolder && !document.isEmpty()) {
        rememberFolderPalette(document, path, paletteOf(view_).terrain);
    }
    refresh();
    return true;
}

void ShpPalettePanel::choosePalette()
{
    if (!view_) {
        return;
    }
    QString dir = QFileInfo(paletteOf(view_).path).absolutePath();
    if (paletteOf(view_).path.isEmpty()) {
        const QStringList folders = shpPaletteFolders();
        dir = folders.isEmpty() ? QFileInfo(document_api::filePath(*view_)).absolutePath()
                                : folders.first();
    }
    const QString path = QFileDialog::getOpenFileName(this, tr("Load Palette"), dir,
                                                      tr("Palettes (*.pal *.PAL);;All files (*)"));
    if (!path.isEmpty()) {
        loadPalette(path, forFolder_->isChecked());
    }
}

void ShpPalettePanel::showMenu()
{
    QMenu menu(this);
    const QString current = paletteOf(view_).path;
    const bool terrain = paletteOf(view_).terrain;
    QAction* makeDefault = menu.addAction(tr("Use as Default Palette"));
    makeDefault->setEnabled(!current.isEmpty());
    connect(makeDefault, &QAction::triggered, this, [current] { setDefaultShpPalette(current); });
    QAction* addFolder = menu.addAction(tr("Add Palette Folder…"));
    connect(addFolder, &QAction::triggered, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Palette Folder"));
        if (!dir.isEmpty()) {
            QStringList folders = shpPaletteFolders();
            if (!folders.contains(dir)) {
                folders.append(dir);
                setShpPaletteFolders(folders);
            }
        }
    });
    QAction* clearFolders = menu.addAction(tr("Forget Palette Folders"));
    clearFolders->setEnabled(!shpPaletteFolders().isEmpty());
    connect(clearFolders, &QAction::triggered, this, [] { setShpPaletteFolders({}); });
    const QString document = view_ ? document_api::filePath(*view_) : QString();
    QAction* forget = menu.addAction(tr("Forget This Folder's Palette"));
    forget->setEnabled(!document.isEmpty());
    connect(forget, &QAction::triggered, this,
            [document, terrain] { forgetFolderPalette(document, terrain); });
    menu.exec(more_->mapToGlobal(QPoint(0, more_->height())));
}

QString ShpPalettePanel::readoutForTest() const
{
    return readout_->text();
}

QString ShpPalettePanel::foregroundReadoutForTest() const
{
    return foreground_->text();
}

} // namespace pictura

#include "moc_shp_palette_panel.cpp"
