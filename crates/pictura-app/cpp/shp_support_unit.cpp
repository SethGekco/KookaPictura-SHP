// SPDX-License-Identifier: GPL-3.0-or-later
// KookaPictura-SHP fork: the unit tools of the SHP dock (the shadow half
// of a unit sprite) and their save warning.

#include "shp_support.h"

#include "panels/shp_frames_panel.h"
#include "panels/shp_palette_panel.h"
#include "panels/shp_unit_panel.h"

#include <QtWidgets/QMainWindow>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>

#include <vector>

#include "pictura_app/src/cxxqt_object.cxxqt.h"
#include "pictura_app/src/cxxqt_object/shp.cxxqt.h"

namespace pictura {

namespace {

::rust::Slice<const int32_t> slice(const std::vector<int32_t>& v)
{
    return ::rust::Slice<const int32_t>(v.data(), v.size());
}

std::vector<int32_t> toVector(const QList<int>& list)
{
    return std::vector<int32_t>(list.cbegin(), list.cend());
}

QString frameNames(const ::rust::Vec<int32_t>& frames)
{
    QStringList names;
    for (const int32_t f : frames) {
        if (names.size() == 12) {
            names << QStringLiteral("...");
            break;
        }
        names << QString::number(f);
    }
    return names.join(QStringLiteral(", "));
}

} // namespace

int ShpSupport::unitRefused(const QString& text)
{
    gateMessage_ = text;
    message(text);
    if (asksNow()) {
        QMessageBox::warning(frame_, tr("Shadows"), text);
    }
    return -1;
}

int ShpSupport::generateShadows(const QList<int>& art, int dx, int dy, double skew)
{
    gateMessage_.clear();
    PictureView* view = view_;
    if (!view || !shp_info(*view).valid) {
        gateMessage_ = tr("no SHP document");
        return -1;
    }
    if (!ShpUnitPanel::kindHasShadows(view)) {
        return unitRefused(tr("A %1 palette has no shadow index; set the palette kind to Unit "
                              "or Animation in the Palette panel.")
                               .arg(QString(shp_kind(ShpPalettePanel::kindOf(view)).label)));
    }
    const ShpInfo info = shp_info(*view);
    const bool paged = info.frames < info.total;
    const int half = ShpUnitPanel::shadowHalfOf(view);
    if (half == 0) {
        // Adding the shadow half grows the sprite like Insert Frames.
        const ShpPlan plan = shp_plan_shadow_half(*view);
        if (!plan.valid) {
            return unitRefused(tr("Twice as many frames do not fit one SHP."));
        }
        switch (confirmLimit(plan, view->property("shpConfirmedStage").toInt(),
                             tr("A sheet of %1 frames").arg(plan.count), tr("Continue"),
                             QString())) {
        case Verdict::Go:
            grant(view, plan.needed, plan.stage);
            break;
        case Verdict::Refused:
            gateMessage_ = limitMessage_;
            refuseLimit();
            return -1;
        default:
            gateMessage_ = limitMessage_;
            return -1;
        }
    }
    // A paged window is rebuilt, and adding frames reflows the sheet: both
    // need the window clean first.
    if ((paged || half == 0) && !cleanGate(view, asksNow(), true, paged)) {
        return -1;
    }
    const std::vector<int32_t> frames = toVector(art);
    const int done =
        shp_generate_shadows(*view, half, slice(frames), dx, dy, skew, allowanceOf(view));
    if (done < 0) {
        return unitRefused(shp_last_error());
    }
    if (half == 0) {
        ShpUnitPanel::setShadowsOf(view, true);
    }
    refreshFrame();
    const int shown = ShpUnitPanel::shadowHalfOf(view);
    message(paged ? tr("Generated %n shadow(s); frames %1-%2 are loaded. Changing the shadows "
                       "of a sprite this big starts a new undo history.",
                       nullptr, done)
                        .arg(shp_info(*view).first)
                        .arg(shp_info(*view).first + shp_info(*view).frames - 1)
                  : tr("Generated %n shadow(s) in frames %1-%2.", nullptr, done)
                        .arg(shown)
                        .arg(2 * shown - 1));
    return done;
}

int ShpSupport::clearShadows(const QList<int>& frames)
{
    gateMessage_.clear();
    PictureView* view = view_;
    if (!view || !shp_info(*view).valid) {
        gateMessage_ = tr("no SHP document");
        return -1;
    }
    const int half = ShpUnitPanel::shadowHalfOf(view);
    if (half == 0) {
        return unitRefused(tr("This sprite has no shadow frames."));
    }
    const ShpInfo info = shp_info(*view);
    const bool paged = info.frames < info.total;
    if (paged && !cleanGate(view, asksNow(), true, true)) {
        return -1;
    }
    const std::vector<int32_t> list = toVector(frames);
    const int done = shp_clear_shadows(*view, half, slice(list));
    if (done < 0) {
        return unitRefused(shp_last_error());
    }
    refreshFrame();
    message(tr("Cleared %n shadow frame(s).", nullptr, done));
    return done;
}

bool ShpSupport::unitSaveCheck(PictureView* view, bool asks)
{
    const int half = ShpUnitPanel::shadowHalfOf(view);
    if (half <= 0) {
        return true;
    }
    const ::rust::Vec<int32_t> bad = shp_shadow_violations(*view, half);
    if (bad.empty()) {
        return true;
    }
    const QString text =
        tr("Shadow frame(s) %1 hold colors other than index 0 and 1. The game draws a "
           "shadow frame as shadow, so those colors do not show as drawn.")
            .arg(frameNames(bad));
    if (!asks) {
        // A warning, never a refusal: the file is still a valid SHP.
        gateMessage_ = text;
        message(text);
        return true;
    }
    QMessageBox box(QMessageBox::Warning, tr("Save as SHP"), text, QMessageBox::NoButton, frame_);
    box.setObjectName(QStringLiteral("shpGate"));
    QPushButton* save = box.addButton(tr("Save Anyway"), QMessageBox::AcceptRole);
    save->setObjectName(QStringLiteral("shpGateSaveAnyway"));
    QPushButton* show = box.addButton(tr("Show Me"), QMessageBox::ActionRole);
    show->setObjectName(QStringLiteral("shpGateShow"));
    QPushButton* cancel = box.addButton(QMessageBox::Cancel);
    cancel->setObjectName(QStringLiteral("shpGateCancel"));
    box.setDefaultButton(save);
    box.setEscapeButton(cancel);
    box.exec();
    if (box.clickedButton() == save) {
        return true;
    }
    gateMessage_ = text;
    if (box.clickedButton() == show) {
        frames_->focusFrame(bad[0]);
    }
    return false;
}

} // namespace pictura
