// SPDX-License-Identifier: GPL-3.0-or-later
// KookaPictura-SHP fork: the Unit panel of an SHP document.

#include "panels/shp_unit_panel.h"

#include "panels/shp_frames_panel.h"

#include <QtGui/QAction>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QColorDialog>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>

#include "pictura_app/src/cxxqt_object.cxxqt.h"
#include "pictura_app/src/cxxqt_object/shp.cxxqt.h"

namespace pictura {

namespace {

const char* const kShadowsProperty = "shpShadows";

} // namespace

ShpUnitPanel::ShpUnitPanel(ShpFramesPanel* frames, QWidget* parent)
    : QWidget(parent)
    , frames_(frames)
{
    setObjectName(QStringLiteral("shpUnitPanel"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(4);

    auto* box = new QGroupBox(tr("Shadows"), this);
    auto* form = new QFormLayout(box);
    shadows_ = new QCheckBox(tr("Second half is shadow frames"), box);
    shadows_->setObjectName(QStringLiteral("shpShadowFrames"));
    shadows_->setToolTip(
        tr("TS/RA2 unit sprites keep a shadow for each frame in the second half of the "
           "frames, using only index 0 and index 1. Found from the pixels when the sprite "
           "opens."));
    readout_ = new QLabel(box);
    readout_->setObjectName(QStringLiteral("shpShadowReadout"));
    underlay_ = new QCheckBox(tr("Show shadows under the art"), box);
    underlay_->setObjectName(QStringLiteral("shpShadowUnderlay"));
    underlay_->setToolTip(tr("Draw each frame's shadow under it. View only: colors you pick "
                             "and the saved file are unchanged."));
    form->addRow(shadows_);
    form->addRow(readout_);
    form->addRow(underlay_);

    auto* offset = new QHBoxLayout;
    dx_ = new QSpinBox(box);
    dx_->setObjectName(QStringLiteral("shpShadowDx"));
    dy_ = new QSpinBox(box);
    dy_->setObjectName(QStringLiteral("shpShadowDy"));
    for (QSpinBox* spin : {dx_, dy_}) {
        spin->setRange(-4096, 4096);
        spin->setSuffix(tr(" px"));
        offset->addWidget(spin);
    }
    dx_->setToolTip(tr("Move the shadow right (negative: left)"));
    dy_->setToolTip(tr("Move the shadow down (negative: up)"));
    form->addRow(tr("Offset:"), offset);
    skew_ = new QDoubleSpinBox(box);
    skew_->setObjectName(QStringLiteral("shpShadowSkew"));
    skew_->setRange(-8.0, 8.0);
    skew_->setDecimals(2);
    skew_->setSingleStep(0.25);
    skew_->setSuffix(tr(" px/row"));
    skew_->setToolTip(tr("Lean the shadow: pixels to the right per row above the art's lowest "
                         "row (negative leans left). The base stays where the art stands."));
    form->addRow(tr("Lean:"), skew_);

    const struct {
        const char* name;
        QString text;
        QString tip;
    } specs[] = {
        {"generate", tr("Generate"),
         tr("Generate the shadows of the selected frames from their art (index 1). Adds the "
            "shadow frames when the sprite has none. One undoable step.")},
        {"generateAll", tr("Generate All"), tr("Generate every frame's shadow from its art")},
        {"clear", tr("Clear"), tr("Clear the shadows of the selected frames")},
        {"clearAll", tr("Clear All"), tr("Clear every shadow frame")},
    };
    auto* buttons = new QHBoxLayout;
    for (const auto& spec : specs) {
        auto* action = new QAction(spec.text, this);
        const QString name = QLatin1String(spec.name);
        action->setObjectName(QStringLiteral("shpShadow_%1").arg(name));
        action->setToolTip(spec.tip);
        actions_.insert(name, action);
        auto* button = new QToolButton(box);
        button->setDefaultAction(action);
        buttons->addWidget(button);
    }
    buttons->addStretch(1);
    form->addRow(buttons);
    layout->addWidget(box);

    auto* houseBox = new QGroupBox(tr("House Color"), this);
    auto* houseForm = new QFormLayout(houseBox);
    house_ = new QCheckBox(tr("Preview house color (approximate)"), houseBox);
    house_->setObjectName(QStringLiteral("shpHousePreview"));
    house_->setToolTip(
        tr("Show the house-color remap indices (16-31 of a unit palette) in a player's "
           "color. Approximate: the ramp follows the palette's own shading, not the game's "
           "formula. View only: colors you pick and the saved file are unchanged."));
    houseColors_ = new QComboBox(houseBox);
    houseColors_->setObjectName(QStringLiteral("shpHouseColor"));
    for (const auto& [name, color] : housePresets()) {
        houseColors_->addItem(name, color);
    }
    houseColors_->addItem(tr("Custom"));
    houseSwatch_ = new QLabel(houseBox);
    houseSwatch_->setObjectName(QStringLiteral("shpHouseSwatch"));
    houseSwatch_->setFixedSize(28, 18);
    auto* custom = new QToolButton(houseBox);
    custom->setObjectName(QStringLiteral("shpHouseCustom"));
    custom->setText(tr("Pick..."));
    custom->setToolTip(tr("Preview a color of your own"));
    auto* colorRow = new QHBoxLayout;
    colorRow->addWidget(houseColors_, 1);
    colorRow->addWidget(houseSwatch_);
    colorRow->addWidget(custom);
    houseForm->addRow(house_);
    houseForm->addRow(tr("Color:"), colorRow);
    layout->addWidget(houseBox);
    layout->addStretch(1);
    setHouseColor(housePresets().first().second);
    connect(house_, &QCheckBox::toggled, this, [this] { frames_->syncGrid(); });
    connect(houseColors_, &QComboBox::activated, this, [this](int index) {
        const QVariant color = houseColors_->itemData(index);
        if (color.isValid()) {
            setHouseColor(color.value<QColor>());
        }
    });
    connect(custom, &QToolButton::clicked, this, [this] {
        const QColor color = QColorDialog::getColor(houseColor_, this, tr("House Color"));
        if (color.isValid()) {
            setHouseColor(color);
        }
    });

    connect(shadows_, &QCheckBox::toggled, this, [this](bool on) {
        if (view_) {
            setShadowsOf(view_, on);
        }
        sync();
    });
    connect(underlay_, &QCheckBox::toggled, this, [this] { frames_->syncGrid(); });
    const auto generate = [this](bool all) {
        const QList<int> art = artFrames(all);
        if (art.isEmpty()) {
            emit message(tr("Pick the frames whose shadows to generate."));
            return;
        }
        emit generateRequested(art, dx_->value(), dy_->value(), skew_->value());
    };
    const auto clear = [this](bool all) {
        const QList<int> frames = shadowFrames(all);
        if (frames.isEmpty()) {
            emit message(shadowHalf() > 0 ? tr("Pick the frames whose shadows to clear.")
                                          : tr("This sprite has no shadow frames."));
            return;
        }
        emit clearRequested(frames);
    };
    connect(actions_.value(QStringLiteral("generate")), &QAction::triggered, this,
            [generate] { generate(false); });
    connect(actions_.value(QStringLiteral("generateAll")), &QAction::triggered, this,
            [generate] { generate(true); });
    connect(actions_.value(QStringLiteral("clear")), &QAction::triggered, this,
            [clear] { clear(false); });
    connect(actions_.value(QStringLiteral("clearAll")), &QAction::triggered, this,
            [clear] { clear(true); });
    frames_->setGridDecorator([this](ImageView::ShpGrid& grid) { decorate(grid); });
    refresh();
}

QList<QPair<QString, QColor>> ShpUnitPanel::housePresets()
{
    // Stock rulesmd.ini [Colors] of Yuri's Revenge, Westwood HSV (each 0-255),
    // for the eight multiplayer colors.
    const struct {
        const char* name;
        int h, s, v;
    } stock[] = {
        {QT_TR_NOOP("Gold"), 43, 239, 255},     {QT_TR_NOOP("Red"), 0, 230, 255},
        {QT_TR_NOOP("Blue"), 153, 214, 212},    {QT_TR_NOOP("Green"), 81, 200, 210},
        {QT_TR_NOOP("Orange"), 25, 230, 255},   {QT_TR_NOOP("Sky Blue"), 131, 200, 230},
        {QT_TR_NOOP("Purple"), 201, 201, 189},  {QT_TR_NOOP("Pink"), 221, 102, 255},
    };
    QList<QPair<QString, QColor>> out;
    for (const auto& c : stock) {
        out.append({tr(c.name), QColor::fromHsv(c.h * 360 / 256, c.s, c.v)});
    }
    return out;
}

void ShpUnitPanel::setHouseColor(const QColor& color)
{
    houseColor_ = color.toRgb();
    int index = houseColors_->count() - 1; // Custom
    for (int i = 0; i + 1 < houseColors_->count(); ++i) {
        if (houseColors_->itemData(i).value<QColor>().rgb() == houseColor_.rgb()) {
            index = i;
            break;
        }
    }
    houseColors_->setCurrentIndex(index);
    houseSwatch_->setStyleSheet(QStringLiteral("background: %1; border: 1px solid palette(mid);")
                                    .arg(houseColor_.name()));
    houseSwatch_->setToolTip(houseColor_.name());
    frames_->syncGrid();
}

bool ShpUnitPanel::shadowsOf(PictureView* view)
{
    if (!view || !shp_info(*view).valid) {
        return false;
    }
    QVariant on = view->property(kShadowsProperty);
    if (!on.isValid()) {
        on = shp_shadow_detect(*view) > 0;
        view->setProperty(kShadowsProperty, on);
    }
    return on.toBool();
}

void ShpUnitPanel::setShadowsOf(PictureView* view, bool on)
{
    if (view) {
        view->setProperty(kShadowsProperty, on);
    }
}

int ShpUnitPanel::shadowHalfOf(PictureView* view)
{
    if (!shadowsOf(view)) {
        return 0;
    }
    const int total = shp_info(*view).total;
    return total >= 2 && total % 2 == 0 ? total / 2 : 0;
}

int ShpUnitPanel::shadowHalf() const
{
    return shadowHalfOf(view_);
}

void ShpUnitPanel::setView(PictureView* view)
{
    if (view_ != view) {
        shadowKey_ = {-1, 0};
        shadowImage_ = QImage();
    }
    view_ = view;
    refresh();
}

void ShpUnitPanel::refresh()
{
    const bool shp = view_ && shp_info(*view_).valid;
    {
        const QSignalBlocker block(shadows_);
        shadows_->setChecked(shp && shadowsOf(view_));
    }
    shadows_->setEnabled(shp);
    sync();
}

void ShpUnitPanel::sync()
{
    const bool shp = view_ && shp_info(*view_).valid;
    const int half = shp ? shadowHalf() : 0;
    const int total = shp ? shp_info(*view_).total : 0;
    if (!shp) {
        readout_->setText(QString());
    } else if (half > 0) {
        readout_->setText(tr("Shadows: frames %1-%2 (art %3-%4)")
                              .arg(half)
                              .arg(total - 1)
                              .arg(0)
                              .arg(half - 1));
    } else if (shadows_->isChecked()) {
        readout_->setText(tr("Shadows: none (%1 frames is an odd count)").arg(total));
    } else {
        readout_->setText(tr("Shadows: none. Generate adds a shadow frame for every frame."));
    }
    underlay_->setEnabled(half > 0);
    house_->setEnabled(shp);
    for (QAction* action : std::as_const(actions_)) {
        action->setEnabled(shp);
    }
    for (const char* name : {"clear", "clearAll"}) {
        actions_.value(QLatin1String(name))->setEnabled(half > 0);
    }
    frames_->setShadowHalf(half);
    frames_->syncGrid();
}

QList<int> ShpUnitPanel::artFrames(bool all) const
{
    QList<int> out;
    if (!view_ || !shp_info(*view_).valid) {
        return out;
    }
    const int half = shadowHalf();
    const int artCount = half > 0 ? half : shp_info(*view_).total;
    if (all) {
        for (int i = 0; i < artCount; ++i) {
            out << i;
        }
        return out;
    }
    const auto [first, count] = frames_->selectedRange();
    for (int f = first; f < first + count; ++f) {
        const int art = f >= artCount ? f - artCount : f;
        if (art >= 0 && art < artCount && !out.contains(art)) {
            out << art;
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

QList<int> ShpUnitPanel::shadowFrames(bool all) const
{
    QList<int> out;
    const int half = shadowHalf();
    if (half <= 0) {
        return out;
    }
    if (all) {
        for (int i = half; i < 2 * half; ++i) {
            out << i;
        }
        return out;
    }
    const auto [first, count] = frames_->selectedRange();
    for (int f = first; f < first + count; ++f) {
        const int shadow = f < half ? f + half : f;
        if (shadow >= half && shadow < 2 * half && !out.contains(shadow)) {
            out << shadow;
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

void ShpUnitPanel::decorate(ImageView::ShpGrid& grid)
{
    if (!view_) {
        return;
    }
    const int half = shadowHalf();
    grid.shadowHalf = half;
    grid.shadowUnderlay = half > 0 && underlay_->isChecked();
    const int active = grid.active >= 0 ? grid.first + grid.active : -1;
    const int shadow = active >= 0 && active < half ? active + half : -1;
    if (grid.shadowUnderlay && shadow >= 0 && !frames_->onSheet(shadow)) {
        grid.activeShadow = offSheetShadow(shadow);
    }
    if (house_->isChecked()) {
        constexpr int kFirst = 16;
        constexpr int kLast = 31;
        const ::rust::Vec<uint32_t> unique = shp_palette_unique(*view_);
        const ::rust::Vec<uint32_t> ramp =
            shp_house_ramp(*view_, houseColor_.rgb() & 0xFFFFFFu, kFirst, kLast);
        if (unique.size() == 256 && ramp.size() == size_t(kLast - kFirst + 1)) {
            for (int i = kFirst; i <= kLast; ++i) {
                grid.remapFrom.append(unique[size_t(i)] | 0xFF000000u);
                grid.remapTo.append(ramp[size_t(i - kFirst)]);
            }
        }
    }
}

// The full-size picture of shadow frame `frame` off a paged sheet, decoded
// from the frame store or the file, kept while its stamp holds.
QImage ShpUnitPanel::offSheetShadow(int frame)
{
    const quint64 stamp = shp_frame_stamp(*view_, frame);
    if (shadowKey_ == qMakePair(frame, stamp)) {
        return shadowImage_;
    }
    const ShpInfo info = shp_info(*view_);
    const ShpThumb t = shp_frame_thumbnail(*view_, frame, std::max(info.width, info.height));
    QImage image;
    if (t.width == info.width && t.height == info.height
        && t.argb.size() == size_t(t.width) * size_t(t.height)) {
        image = QImage(t.width, t.height, QImage::Format_ARGB32);
        for (int y = 0; y < t.height; ++y) {
            std::copy_n(t.argb.data() + size_t(y) * size_t(t.width), t.width,
                        reinterpret_cast<uint32_t*>(image.scanLine(y)));
        }
    }
    shadowKey_ = qMakePair(frame, stamp);
    shadowImage_ = image;
    return image;
}

} // namespace pictura

#include "moc_shp_unit_panel.cpp"
