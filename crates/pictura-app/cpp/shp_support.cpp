// SPDX-License-Identifier: GPL-3.0-or-later
// KookaPictura-SHP fork: the SHP dock, frame stepping and the save gate.

#include "shp_support.h"

#include "panels/shp_frames_panel.h"
#include "panels/shp_palette_panel.h"

#include <QtCore/QFileInfo>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeySequence>
#include <QtGui/QShortcut>
#include <QtWidgets/QDockWidget>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QStatusBar>

#include "pictura_app/src/cxxqt_object.cxxqt.h"
#include "pictura_app/src/cxxqt_object/shp.cxxqt.h"

namespace pictura {

namespace {

int g_gateInteractive = -1; // -1: follow the platform

QString frameList(const ::rust::Vec<int32_t>& frames)
{
    QStringList names;
    for (const int32_t f : frames) {
        if (names.size() == 12) {
            names << QStringLiteral("…");
            break;
        }
        names << QString::number(f);
    }
    return names.join(QStringLiteral(", "));
}

} // namespace

bool isShpPath(const QString& path)
{
    return QFileInfo(path).suffix().compare(QStringLiteral("shp"), Qt::CaseInsensitive) == 0;
}

ShpSupport::ShpSupport(QMainWindow* frame, ColorState* colors)
    : QObject(frame)
    , frame_(frame)
{
    setObjectName(QStringLiteral("shpSupport"));
    dock_ = new QDockWidget(tr("SHP Frames & Palette"), frame);
    dock_->setObjectName(QStringLiteral("shpDock"));
    auto* split = new QSplitter(Qt::Horizontal, dock_);
    frames_ = new ShpFramesPanel(split);
    palette_ = new ShpPalettePanel(colors, split);
    split->addWidget(frames_);
    split->addWidget(palette_);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 1);
    dock_->setWidget(split);
    dock_->hide();
    connect(palette_, &ShpPalettePanel::message, this, &ShpSupport::message);

    prevKey_ = new QShortcut(QKeySequence(Qt::Key_Comma), frame);
    nextKey_ = new QShortcut(QKeySequence(Qt::Key_Period), frame);
    prevKey_->setEnabled(false);
    nextKey_->setEnabled(false);
    connect(prevKey_, &QShortcut::activated, this, [this] { frames_->step(-1); });
    connect(nextKey_, &QShortcut::activated, this, [this] { frames_->step(1); });
}

ShpSupport* ShpSupport::install(QMainWindow* frame, ColorState* colors)
{
    if (ShpSupport* existing = of(frame)) {
        return existing;
    }
    return new ShpSupport(frame, colors);
}

ShpSupport* ShpSupport::of(const QObject* frame)
{
    return frame ? frame->findChild<ShpSupport*>(QStringLiteral("shpSupport"),
                                                 Qt::FindDirectChildrenOnly)
                 : nullptr;
}

void ShpSupport::setView(PictureView* view, ImageView* canvas)
{
    view_ = view;
    const bool shp = view && shp_info(*view).valid;
    frames_->setView(view, canvas);
    palette_->setView(view);
    prevKey_->setEnabled(shp);
    nextKey_->setEnabled(shp);
    if (shp && !wasShp_) {
        dock_->show();
    }
    wasShp_ = shp;
}

void ShpSupport::refresh()
{
    // A document can become an SHP in place (Revert) or stop being one.
    const bool shp = view_ && shp_info(*view_).valid;
    prevKey_->setEnabled(shp);
    nextKey_->setEnabled(shp);
    if (!dock_->isVisible() && !(shp && !wasShp_)) {
        wasShp_ = shp;
        return;
    }
    if (shp && !wasShp_) {
        dock_->show();
    }
    wasShp_ = shp;
    frames_->refresh();
    palette_->refresh();
}

void ShpSupport::message(const QString& text)
{
    if (frame_ && frame_->statusBar()) {
        frame_->statusBar()->showMessage(text, 8000);
    }
}

void ShpSupport::setGateInteractive(int interactive)
{
    g_gateInteractive = interactive;
}

bool ShpSupport::interactive() const
{
    return g_gateInteractive < 0
               ? QGuiApplication::platformName() != QLatin1String("offscreen")
               : g_gateInteractive == 1;
}

void ShpSupport::showOffPalette(PictureView* view)
{
    const ShpCheck check = shp_check(*view);
    if (check.off_frames.empty()) {
        return;
    }
    dock_->show();
    const bool selects = frames_->selectsCell();
    frames_->setSelectsCell(false);
    frames_->focusFrame(check.off_frames[0]);
    frames_->setSelectsCell(selects);
    if (check.first_off.size() == 4) {
        view->select_rect(check.first_off[0], check.first_off[1], check.first_off[2],
                          check.first_off[3], QStringLiteral("new"), 0.0);
    }
}

bool ShpSupport::saveGate(PictureView* view)
{
    gateMessage_.clear();
    if (!view) {
        return false;
    }
    ShpCheck check = shp_check(*view);
    const auto refuse = [this](const QString& text) {
        gateMessage_ = text;
        message(text);
        if (interactive()) {
            QMessageBox::warning(frame_, tr("Save as SHP"), text);
        }
        return false;
    };
    if (!check.error.isEmpty()) {
        return refuse(check.error);
    }
    if (check.off_palette > 0) {
        const QString text = tr("%n pixel(s) are not palette colours (frames %1).", nullptr,
                                check.off_palette)
                                 .arg(frameList(check.off_frames));
        if (!interactive()) {
            return refuse(text + tr(" Snap them to the palette before saving."));
        }
        QMessageBox box(QMessageBox::Warning, tr("Save as SHP"), text, QMessageBox::NoButton,
                        frame_);
        box.setInformativeText(
            tr("Snap maps each to the nearest palette index (the house-colour ramp loses "
               "ties; anything mostly transparent becomes index 0). It is recorded in "
               "History, so what is saved is what you see."));
        QPushButton* snap = box.addButton(tr("Snap to Palette and Save"), QMessageBox::AcceptRole);
        snap->setObjectName(QStringLiteral("shpGateSnap"));
        QPushButton* show = box.addButton(tr("Show Me"), QMessageBox::ActionRole);
        show->setObjectName(QStringLiteral("shpGateShow"));
        QPushButton* cancel = box.addButton(QMessageBox::Cancel);
        cancel->setObjectName(QStringLiteral("shpGateCancel"));
        box.setDefaultButton(cancel);
        box.setEscapeButton(cancel);
        box.setObjectName(QStringLiteral("shpGate"));
        box.exec();
        if (box.clickedButton() == show) {
            showOffPalette(view);
            gateMessage_ = text;
            return false;
        }
        if (box.clickedButton() != snap) {
            gateMessage_ = text;
            return false;
        }
        if (shp_snap(*view) < 0) {
            return refuse(shp_last_error());
        }
        check = shp_check(*view);
    }
    if (check.gutter > 0) {
        const QString text =
            tr("%n pixel(s) lie in the gaps between frames and would be lost.", nullptr,
               check.gutter);
        if (!interactive()) {
            return refuse(text + tr(" Clear the frame gutters before saving."));
        }
        QMessageBox box(QMessageBox::Warning, tr("Save as SHP"), text, QMessageBox::NoButton,
                        frame_);
        box.setInformativeText(tr("Clearing them is recorded in History."));
        QPushButton* clear = box.addButton(tr("Clear and Save"), QMessageBox::AcceptRole);
        clear->setObjectName(QStringLiteral("shpGateClear"));
        QPushButton* cancel = box.addButton(QMessageBox::Cancel);
        cancel->setObjectName(QStringLiteral("shpGateCancel"));
        box.setDefaultButton(cancel);
        box.setEscapeButton(cancel);
        box.setObjectName(QStringLiteral("shpGate"));
        box.exec();
        if (box.clickedButton() != clear) {
            gateMessage_ = text;
            return false;
        }
        if (shp_clear_gutter(*view) < 0) {
            return refuse(shp_last_error());
        }
    }
    return true;
}

} // namespace pictura

#include "moc_shp_support.cpp"
