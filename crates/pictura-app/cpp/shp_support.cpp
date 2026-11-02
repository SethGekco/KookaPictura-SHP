// SPDX-License-Identifier: GPL-3.0-or-later
// KookaPictura-SHP fork: the SHP dock, frame stepping and the save gate.

#include "shp_support.h"

#include "commands.h"
#include "command_ids.h"
#include "frame.h"
#include "shp_authoring.h"
#include "shp_palettes.h"
#include "panels/shp_frames_panel.h"
#include "panels/shp_palette_panel.h"
#include "panels/shp_playback_panel.h"

#include <QtCore/QFileInfo>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeySequence>
#include <QtGui/QShortcut>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QDockWidget>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QStatusBar>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <array>

#include "pictura_app/src/cxxqt_object.cxxqt.h"
#include "pictura_app/src/cxxqt_object/shp.cxxqt.h"

namespace pictura {

namespace {

int g_gateInteractive = -1; // -1: follow the platform
int g_quiet = 0;             // open ShpSupport::Quiet scopes
QString g_quietRefusal;      // the last size-limit refusal inside one

const char* const kAllowanceProperty = "shpAllowance";
const char* const kStageProperty = "shpConfirmedStage";

QString mib(quint64 bytes)
{
    return QStringLiteral("%1 MiB").arg(double(bytes) / double(1 << 20), 0, 'f', 0);
}

QString memory(quint64 bytes)
{
    return bytes >= (quint64(1) << 30)
               ? QStringLiteral("%1 GiB").arg(double(bytes) / double(1 << 30), 0, 'f', 1)
               : mib(bytes);
}

// How far over the limit, in whole percent rounded up (1% for anything over).
int percentOver(const ShpPlan& plan)
{
    if (plan.limit == 0 || plan.needed <= plan.limit) {
        return 0;
    }
    return int((plan.needed - plan.limit) * 100 / plan.limit
               + ((plan.needed - plan.limit) * 100 % plan.limit ? 1 : 0));
}

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
    playback_ = new ShpPlaybackPanel(split);
    palette_ = new ShpPalettePanel(colors, split);
    split->addWidget(frames_);
    split->addWidget(playback_);
    split->addWidget(palette_);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 1);
    split->setStretchFactor(2, 1);
    dock_->setWidget(split);
    dock_->hide();
    connect(palette_, &ShpPalettePanel::message, this, &ShpSupport::message);
    connect(frames_, &ShpFramesPanel::pageRequested, this, [this](int frame) { pageTo(frame); });
    connect(frames_, &ShpFramesPanel::windowSizeRequested, this,
            [this](int count) { setWindowFrames(count); });
    connect(frames_, &ShpFramesPanel::frameOpRequested, this,
            [this](const QString& op, int first, int count, int to) {
                frameOp(op, first, count, to);
            });

    prevKey_ = new QShortcut(QKeySequence(Qt::Key_Comma), frame);
    nextKey_ = new QShortcut(QKeySequence(Qt::Key_Period), frame);
    prevKey_->setEnabled(false);
    nextKey_->setEnabled(false);
    connect(prevKey_, &QShortcut::activated, this, [this] { frames_->step(-1); });
    connect(nextKey_, &QShortcut::activated, this, [this] { frames_->step(1); });
    installMenus();
}

void ShpSupport::installMenus()
{
    newShpAction_ = new QAction(tr("New SHP…"), this);
    newShpAction_->setObjectName(QStringLiteral("shpNewAction"));
    connect(newShpAction_, &QAction::triggered, this, [this] { newShpWithDialog(); });
    auto* window = qobject_cast<PicturaMainWindow*>(frame_);
    QAction* fileNew = window && window->registry()
        ? window->registry()->action(QLatin1String(command_ids::FileNew))
        : nullptr;
    auto* menu = fileNew ? qobject_cast<QMenu*>(fileNew->parent()) : nullptr;
    if (!menu) {
        return;
    }
    const QList<QAction*> actions = menu->actions();
    const qsizetype at = actions.indexOf(fileNew);
    menu->insertAction(actions.value(at + 1), newShpAction_);
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
    playback_->setView(view);
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
    playback_->refresh();
    palette_->refresh();
}

void ShpSupport::message(const QString& text)
{
    if (frame_ && frame_->statusBar()) {
        frame_->statusBar()->showMessage(text, 8000);
    }
}

QString ShpSupport::lastSaveError() const
{
    const QString error = shp_last_error();
    return error.isEmpty() ? tr("save failed") : error;
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

bool ShpSupport::saveGate(PictureView* view, bool dialogs, const QString& path)
{
    savedCopy_ = false;
    if (view && !shp_info(*view).valid && !path.isEmpty()) {
        return convertForSave(view, path, dialogs);
    }
    return cleanGate(view, dialogs, false);
}

bool ShpSupport::newShp(int width, int height, int frames, const QString& palette)
{
    gateMessage_.clear();
    auto* window = qobject_cast<PicturaMainWindow*>(frame_);
    const ShpPlan plan = shp_plan_sheet(width, height, frames);
    if (!window || !plan.valid) {
        gateMessage_ = tr("An SHP holds 1 to 4096 frames of 1 to 4096 pixels a side.");
        message(gateMessage_);
        return false;
    }
    auto* view = new PictureView(window);
    switch (confirmLimit(plan, 0, tr("A new SHP of %1 frames").arg(frames), tr("Create"),
                         QString())) {
    case Verdict::Go:
        grant(view, plan.needed, plan.stage);
        break;
    case Verdict::Refused:
        delete view;
        gateMessage_ = limitMessage_;
        return refuseLimit();
    default:
        delete view;
        return false;
    }
    if (!shp_new_document(*view, tr("Untitled"), width, height, frames, palette,
                          allowanceOf(view))) {
        delete view;
        gateMessage_ = shp_last_error();
        message(gateMessage_);
        return false;
    }
    window->addDocument(view, QString());
    return true;
}

void ShpSupport::newShpWithDialog()
{
    ShpNewDialog dialog(defaultShpPalette(), frame_);
    if (dialog.exec() == QDialog::Accepted) {
        newShp(dialog.frameWidth(), dialog.frameHeight(), dialog.frames(), dialog.palette());
    }
}

bool ShpSupport::convertForSave(PictureView* source, const QString& path, bool dialogs)
{
    gateMessage_.clear();
    savedCopy_ = false;
    auto* window = qobject_cast<PicturaMainWindow*>(frame_);
    if (!window || !source || !dialogs || !asksNow()) {
        gateMessage_ = tr("This image is not an SHP sheet. Saving it as SHP converts it with an "
                          "options dialog (frame size, palette), which needs an interactive "
                          "session.");
        message(gateMessage_);
        return false;
    }
    ShpConvertDialog dialog(source->document_width(), source->document_height(),
                            resolveShpPalette(path), frame_);
    if (dialog.exec() != QDialog::Accepted) {
        gateMessage_ = tr("Save As SHP was cancelled.");
        return false;
    }
    const QList<int> cut = dialog.cut();
    const ShpPlan plan = shp_plan_sheet(cut[0], cut[1], cut[5]);
    if (!plan.valid) {
        gateMessage_ = tr("An SHP holds 1 to 4096 frames of 1 to 4096 pixels a side.");
        QMessageBox::warning(frame_, tr("Save As SHP"), gateMessage_);
        return false;
    }
    auto* target = new PictureView(window);
    switch (confirmLimit(plan, 0, tr("Converting to SHP"), tr("Convert"), QString())) {
    case Verdict::Go:
        grant(target, plan.needed, plan.stage);
        break;
    case Verdict::Refused:
        delete target;
        gateMessage_ = limitMessage_;
        return refuseLimit();
    default:
        delete target;
        return false;
    }
    const std::array<int32_t, 6> values = {cut[0], cut[1], cut[2], cut[3], cut[4], cut[5]};
    if (!shp_convert(*target, *source, QFileInfo(path).completeBaseName(),
                     ::rust::Slice<const int32_t>(values.data(), values.size()),
                     dialog.palette(), dialog.nearest(), allowanceOf(target))) {
        delete target;
        gateMessage_ = shp_last_error();
        QMessageBox::warning(frame_, tr("Save As SHP"), gateMessage_);
        return false;
    }
    window->addDocument(target, QString());
    // The new document is active now: the normal save, gate included.
    savedCopy_ = window->saveActiveAs(path);
    if (savedCopy_) {
        gateMessage_ = tr("Saved %1 from a new SHP document; the original image is unchanged.")
                           .arg(QFileInfo(path).fileName());
        message(gateMessage_);
    }
    return false;
}

void ShpSupport::refreshFrame()
{
    if (auto* window = qobject_cast<PicturaMainWindow*>(frame_)) {
        window->refresh();
    } else {
        refresh();
    }
}

bool ShpSupport::pageTo(int frame, int span)
{
    gateMessage_.clear();
    PictureView* view = view_;
    if (!view || !shp_info(*view).valid) {
        gateMessage_ = tr("no SHP document");
        return false;
    }
    if (!cleanGate(view, true, true)) {
        return false;
    }
    if (!shp_show_frames(*view, frame, span, allowanceOf(view))) {
        gateMessage_ = shp_last_error();
        message(gateMessage_);
        if (interactive()) {
            QMessageBox::warning(frame_, tr("Change Frames"), gateMessage_);
        }
        return false;
    }
    refreshFrame();
    const ShpInfo info = shp_info(*view);
    frames_->focusFrame(frame);
    message(tr("Frames %1-%2 of %3 loaded. Loading other frames starts a new undo history.")
                .arg(info.first)
                .arg(info.first + info.frames - 1)
                .arg(info.total));
    return true;
}

bool ShpSupport::frameOp(const QString& op, int first, int count, int to)
{
    gateMessage_.clear();
    PictureView* view = view_;
    if (!view || !shp_info(*view).valid) {
        gateMessage_ = tr("no SHP document");
        return false;
    }
    const bool paged = [&] {
        const ShpInfo info = shp_info(*view);
        return info.frames < info.total;
    }();
    const auto refuse = [this](const QString& text) {
        gateMessage_ = text;
        message(text);
        if (asksNow()) {
            QMessageBox::warning(frame_, tr("Frames"), text);
        }
        return false;
    };
    const ShpPlan plan = shp_plan_frames(*view, op, first, count, to);
    if (!plan.valid) {
        return refuse(tr("That frame operation does not apply here."));
    }
    switch (confirmLimit(plan, view->property(kStageProperty).toInt(),
                         tr("A sheet of %1 frames").arg(plan.count), tr("Continue"), QString())) {
    case Verdict::Go:
        grant(view, plan.needed, plan.stage);
        break;
    case Verdict::Refused:
        gateMessage_ = limitMessage_;
        return refuseLimit();
    default:
        gateMessage_ = limitMessage_;
        return false;
    }
    if (!cleanGate(view, g_quiet == 0, true, paged)) {
        return false;
    }
    const int focus = shp_frame_op(*view, op, first, count, to, allowanceOf(view));
    if (focus < 0) {
        return refuse(shp_last_error());
    }
    refreshFrame();
    frames_->focusFrame(focus);
    if (paged) {
        const ShpInfo info = shp_info(*view);
        message(tr("Frames %1-%2 of %3 loaded. Changing the frames of a sprite this big starts "
                   "a new undo history.")
                    .arg(info.first)
                    .arg(info.first + info.frames - 1)
                    .arg(info.total));
    }
    return true;
}

// --- The SHP size limit -----------------------------------------------------

ShpSupport::Quiet::Quiet()
{
    if (g_quiet++ == 0) {
        g_quietRefusal.clear();
    }
}

ShpSupport::Quiet::~Quiet()
{
    --g_quiet;
}

QString ShpSupport::Quiet::refusal() const
{
    return g_quietRefusal;
}

void ShpSupport::setAllowOverLimit(bool on)
{
    if (allowOverLimit_ != on) {
        allowOverLimit_ = on;
        emit limitSettingsChanged();
    }
}

void ShpSupport::setRemoveCeiling(bool on)
{
    if (removeCeiling_ != on) {
        removeCeiling_ = on;
        emit limitSettingsChanged();
    }
}

QWidget* ShpSupport::advancedPage(QWidget* parent)
{
    auto* page = new QWidget(parent);
    page->setObjectName(QStringLiteral("shpAdvancedPage"));
    auto* layout = new QVBoxLayout(page);
    auto* heading = new QLabel(tr("<b>SHP size limit</b>"), page);
    auto* about = new QLabel(
        tr("An SHP sprite whose frames would take more than %1 of image memory opens as a "
           "window of frames, so Kooka does not run this computer out of memory. For a rare "
           "file that needs more at once, the limit can be lifted here. Every time it is "
           "used you are asked to confirm, and Kooka still refuses anything the memory "
           "available now cannot hold. These settings are not saved: they are off again "
           "the next time Kooka starts.")
            .arg(mib(shp_size_limit())),
        page);
    about->setWordWrap(true);
    auto* allow = new QCheckBox(tr("Allow SHP files over the size limit (until Kooka closes)"),
                                page);
    allow->setObjectName(QStringLiteral("shpAllowOverLimit"));
    auto* ceiling = new QCheckBox(
        tr("Remove the 10% ceiling on that allowance (until Kooka closes)"), page);
    ceiling->setObjectName(QStringLiteral("shpRemoveCeiling"));
    ceiling->setToolTip(tr("Without this, the allowance goes up to 10% over the limit."));
    layout->addWidget(heading);
    layout->addWidget(about);
    layout->addWidget(allow);
    layout->addWidget(ceiling);
    layout->addStretch(1);

    const auto sync = [this, allow, ceiling] {
        const QSignalBlocker blockAllow(allow);
        const QSignalBlocker blockCeiling(ceiling);
        allow->setChecked(allowOverLimit_);
        ceiling->setChecked(removeCeiling_);
        ceiling->setEnabled(allowOverLimit_);
    };
    sync();
    connect(this, &ShpSupport::limitSettingsChanged, page, sync);
    connect(allow, &QCheckBox::toggled, this, [this](bool on) { setAllowOverLimit(on); });
    connect(ceiling, &QCheckBox::toggled, this, [this](bool on) { setRemoveCeiling(on); });
    return page;
}

quint64 ShpSupport::allowanceOf(const PictureView* view)
{
    return view ? view->property(kAllowanceProperty).toULongLong() : 0;
}

void ShpSupport::grant(PictureView* view, quint64 bytes, int stage)
{
    if (!view || stage <= 0) {
        return;
    }
    const quint64 allowance = std::max(allowanceOf(view), bytes);
    view->setProperty(kAllowanceProperty, allowance);
    view->setProperty(kStageProperty, std::max(view->property(kStageProperty).toInt(), stage));
    shp_set_open_allowance(*view, allowance);
}

bool ShpSupport::asksNow() const
{
    return g_quiet == 0 && interactive();
}

bool ShpSupport::refuseLimit()
{
    message(limitMessage_);
    if (g_quiet > 0) {
        g_quietRefusal = limitMessage_;
    } else if (asksNow()) {
        QMessageBox box(QMessageBox::Warning, tr("SHP Size Limit"), limitMessage_,
                        QMessageBox::Ok, frame_);
        box.setObjectName(QStringLiteral("shpLimitRefused"));
        box.exec();
    }
    return false;
}

ShpSupport::Verdict ShpSupport::confirmLimit(const ShpPlan& plan, int confirmedStage,
                                             const QString& what, const QString& go,
                                             const QString& alternative)
{
    limitMessage_.clear();
    if (plan.stage <= 0) {
        return Verdict::Go;
    }
    const QString over = tr("%1 needs %2 of image memory, %3% over the %4 SHP size limit.")
                             .arg(what, mib(plan.needed))
                             .arg(percentOver(plan))
                             .arg(mib(plan.limit));
    if (!allowOverLimit_) {
        limitMessage_ = over
            + tr(" To go over it until Kooka closes, turn on \"Allow SHP files over the size "
                 "limit\" in Edit > Preferences > Advanced.");
        return Verdict::Refused;
    }
    if (plan.stage >= 2 && !removeCeiling_) {
        limitMessage_ = over
            + tr(" The allowance goes up to 10% over the limit. To go further until Kooka "
                 "closes, also turn on \"Remove the 10% ceiling\" in Edit > Preferences > "
                 "Advanced.");
        return Verdict::Refused;
    }
    if (!plan.floor_ok) {
        limitMessage_ = over
            + tr(" Kooka would need about %1 of memory for it and %2 is available now. No "
                 "setting allows that.")
                  .arg(memory(plan.estimate), memory(plan.available));
        return Verdict::Refused;
    }
    if (plan.stage <= confirmedStage) {
        return Verdict::Go;
    }
    if (!asksNow()) {
        limitMessage_ = over
            + tr(" Going over the limit has to be confirmed in a dialog, so it is refused "
                 "here.");
        return Verdict::Refused;
    }
    for (int stage = std::max(confirmedStage + 1, 1); stage <= plan.stage; ++stage) {
        const Verdict verdict = limitDialog(plan, stage, what, go, alternative);
        if (verdict != Verdict::Go) {
            limitMessage_ = verdict == Verdict::Cancelled ? over : QString();
            return verdict;
        }
    }
    return Verdict::Go;
}

ShpSupport::Verdict ShpSupport::limitDialog(const ShpPlan& plan, int stage, const QString& what,
                                            const QString& go, const QString& alternative)
{
    QDialog dialog(frame_);
    dialog.setObjectName(stage == 1 ? QStringLiteral("shpLimitDialog")
                                    : QStringLiteral("shpLimitCeilingDialog"));
    dialog.setWindowTitle(stage == 1 ? tr("Over the SHP Size Limit")
                                     : tr("Past the 10% Ceiling"));
    auto* layout = new QVBoxLayout(&dialog);
    const QString headline =
        stage == 1 ? tr("%1 breaks the SHP size limit.").arg(what)
                   : tr("%1 is %2% over the SHP size limit, past the 10% ceiling.")
                         .arg(what)
                         .arg(percentOver(plan));
    auto* text = new QLabel(&dialog);
    text->setObjectName(QStringLiteral("shpLimitText"));
    text->setTextFormat(Qt::PlainText);
    text->setWordWrap(true);
    text->setText(
        headline + QStringLiteral("\n\n")
        + tr("Image memory needed: %1\nSize limit: %2 (%3% over)\nEstimated memory use: about "
             "%4\nMemory available now: %5")
              .arg(mib(plan.needed), mib(plan.limit))
              .arg(percentOver(plan))
              .arg(memory(plan.estimate), memory(plan.available))
        + QStringLiteral("\n\n")
        + (stage == 1
               ? tr("The limit is a safety check that keeps Kooka from running this computer "
                    "out of memory. Going over it applies to this file only.")
               : tr("This is a second safety check. The memory estimate is measured, not "
                    "exact, and this far over the limit other programs may slow down or be "
                    "closed by the system.")));
    auto* understand = new QCheckBox(
        stage == 1 ? tr("I understand this switches off a safety check for this file, and Kooka "
                        "could run this computer out of memory.")
                   : tr("I understand this goes past the 10% ceiling as well."),
        &dialog);
    understand->setObjectName(QStringLiteral("shpLimitUnderstand"));
    auto* buttons = new QDialogButtonBox(&dialog);
    QPushButton* goButton = buttons->addButton(go, QDialogButtonBox::AcceptRole);
    goButton->setObjectName(QStringLiteral("shpLimitGo"));
    goButton->setEnabled(false);
    goButton->setAutoDefault(false);
    QPushButton* altButton = nullptr;
    if (!alternative.isEmpty()) {
        altButton = buttons->addButton(alternative, QDialogButtonBox::ActionRole);
        altButton->setObjectName(QStringLiteral("shpLimitAlternative"));
        altButton->setAutoDefault(false);
    }
    QPushButton* cancel = buttons->addButton(QDialogButtonBox::Cancel);
    cancel->setObjectName(QStringLiteral("shpLimitCancel"));
    cancel->setDefault(true);
    layout->addWidget(text);
    layout->addWidget(understand);
    layout->addWidget(buttons);
    connect(understand, &QCheckBox::toggled, goButton, &QPushButton::setEnabled);
    QAbstractButton* clicked = nullptr;
    connect(buttons, &QDialogButtonBox::clicked, &dialog, [&](QAbstractButton* button) {
        clicked = button;
        if (button == goButton || (altButton && button == altButton)) {
            dialog.accept();
        } else {
            dialog.reject();
        }
    });
    dialog.exec();
    if (clicked == goButton && understand->isChecked()) {
        return Verdict::Go;
    }
    if (altButton && clicked == altButton) {
        return Verdict::Alternative;
    }
    return Verdict::Cancelled;
}

bool ShpSupport::confirmOpen(PictureView* view, const QString& path)
{
    limitMessage_.clear();
    const ShpPlan window = shp_plan_file(path, 0);
    if (!view || !window.valid) {
        return true; // not an SHP after all: the decoder answers
    }
    const ShpPlan whole =
        window.count == window.total ? window : shp_plan_file(path, window.total);
    if (whole.stage <= 0) {
        return true; // within the limit: never asks
    }
    const QString name = QFileInfo(path).fileName();
    if (window.stage <= 0) {
        // A window fits: that is how it opens, unless the user lifts the limit
        // for the whole sprite.
        const QString windowNote =
            tr("%1 is over the %2 SHP size limit, so it opens %3 frames at a time.")
                .arg(name, mib(whole.limit))
                .arg(window.count);
        if (!allowOverLimit_) {
            message(windowNote);
            return true;
        }
        switch (confirmLimit(whole, 0, tr("Opening all %1 frames of %2").arg(whole.total).arg(name),
                             tr("Open Whole"), tr("Open as Window"))) {
        case Verdict::Go:
            grant(view, whole.needed, whole.stage);
            return true;
        case Verdict::Alternative:
            message(windowNote);
            return true;
        case Verdict::Refused:
            message(windowNote + QLatin1Char(' ') + limitMessage_);
            return true;
        case Verdict::Cancelled:
            limitMessage_ = tr("Opening %1 was cancelled.").arg(name);
            return false;
        }
    }
    // Not even one frame fits within the limit.
    switch (confirmLimit(window, 0, tr("Opening %1").arg(name), tr("Open"), QString())) {
    case Verdict::Go:
        grant(view, window.needed, window.stage);
        return true;
    case Verdict::Refused:
        return refuseLimit();
    default:
        limitMessage_ = tr("Opening %1 was cancelled.").arg(name);
        return false;
    }
}

bool ShpSupport::allowSize(PictureView* view, int width, int height)
{
    limitMessage_.clear();
    if (!view || !shp_info(*view).valid) {
        return true;
    }
    const ShpPlan plan = shp_plan_size(*view, width, height);
    if (!plan.valid || plan.stage <= 0) {
        return true;
    }
    switch (confirmLimit(plan, view->property(kStageProperty).toInt(),
                         tr("Resizing to %1 x %2").arg(width).arg(height), tr("Resize"),
                         QString())) {
    case Verdict::Go:
        grant(view, plan.needed, plan.stage);
        return true;
    case Verdict::Refused:
        return refuseLimit();
    default:
        return false;
    }
}

bool ShpSupport::setWindowFrames(int count)
{
    limitMessage_.clear();
    PictureView* view = view_;
    if (!view || !shp_info(*view).valid || count < 1) {
        return false;
    }
    const int frame = std::max(frames_->current(), shp_info(*view).first);
    const ShpPlan plan = shp_plan_window(*view, frame, count);
    if (!plan.valid) {
        return false;
    }
    switch (confirmLimit(plan, view->property(kStageProperty).toInt(),
                         tr("Loading %1 frames").arg(plan.count), tr("Load Frames"),
                         QString())) {
    case Verdict::Go:
        grant(view, plan.needed, plan.stage);
        break;
    case Verdict::Refused:
        return refuseLimit();
    default:
        return false;
    }
    return pageTo(frame, count);
}

bool ShpSupport::cleanGate(PictureView* view, bool dialogs, bool paging, bool palette)
{
    gateMessage_.clear();
    const QString title = paging ? tr("Change Frames") : tr("Save as SHP");
    const bool asks = dialogs && interactive();
    if (!view) {
        gateMessage_ = tr("no active document");
        return false;
    }
    ShpCheck check = shp_check(*view);
    const auto refuse = [this, asks, title](const QString& text) {
        gateMessage_ = text;
        message(text);
        if (asks) {
            QMessageBox::warning(frame_, title, text);
        }
        return false;
    };
    if (!check.error.isEmpty()) {
        return refuse(check.error);
    }
    if (palette && check.off_palette > 0) {
        const QString text = tr("%n pixel(s) are not palette colours (frames %1).", nullptr,
                                check.off_palette)
                                 .arg(frameList(check.off_frames));
        if (!asks) {
            return refuse(text
                          + (paging ? tr(" Snap them to the palette before changing frames.")
                                    : tr(" Snap them to the palette before saving.")));
        }
        QMessageBox box(QMessageBox::Warning, title, text, QMessageBox::NoButton, frame_);
        box.setInformativeText(
            tr("Snap maps each to the nearest palette index (the house-colour ramp loses "
               "ties; anything mostly transparent becomes index 0). It is recorded in "
               "History, so what is saved is what you see."));
        QPushButton* snap = box.addButton(
            paging ? tr("Snap to Palette and Continue") : tr("Snap to Palette and Save"),
            QMessageBox::AcceptRole);
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
        if (!asks) {
            return refuse(text
                          + (paging ? tr(" Clear the frame gutters before changing frames.")
                                    : tr(" Clear the frame gutters before saving.")));
        }
        QMessageBox box(QMessageBox::Warning, title, text, QMessageBox::NoButton, frame_);
        box.setInformativeText(tr("Clearing them is recorded in History."));
        QPushButton* clear = box.addButton(paging ? tr("Clear and Continue") : tr("Clear and Save"),
                                           QMessageBox::AcceptRole);
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
