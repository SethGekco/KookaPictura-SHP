#include "actions_panel.h"
#include "actions_panel_internal.h"

#include "actions_button_view.h"
#include "actions_dialogs.h"
#include "commands.h"
#include "icons.h"
#include "session.h"

#include <QtCore/QDataStream>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QItemSelectionModel>
#include <QtCore/QJsonValue>
#include <QtCore/QMimeData>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtGui/QAction>
#include <QtGui/QDragEnterEvent>
#include <QtGui/QDropEvent>
#include <QtGui/QKeyEvent>
#include <QtGui/QShortcut>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QStyle>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>


namespace pictura {

using namespace actions_internal;

bool ActionsPanel::playSteps(const QStringList& stepIds)
{
    if (!engine_ || playing_ || engine_->isRecording() || stepIds.isEmpty()) {
        return stepIds.isEmpty() && engine_ != nullptr;
    }
    if (engine_->isPlaying()) {
        return false;
    }
    updateDocumentFacts();
    if (!engine_->startPlayback(stepIds)) {
        reportError(engine_->lastError());
        return false;
    }
    playing_ = true;
    playQueue_ = stepIds;
    playIndex_ = 0;
    playDepth_ = 0;
    lastError_.clear();
    updateToolbar();
    schedulePlayback();
    return true;
}

bool ActionsPanel::playSelection(bool singleStep)
{
    if (!engine_ || engine_->isRecording() || playing_ || engine_->isPlaying()) {
        return false;
    }
    const QStringList ids = selectedNodeIds();
    if (ids.isEmpty()) {
        return false;
    }
    bool hasSteps = false;
    QString error;
    const QStringList queue = buildPlayQueue(ids, singleStep, &hasSteps, &error);
    if (!error.isEmpty()) {
        reportError(error);
        return false;
    }
    if (!hasSteps) {
        return true;
    }
    return playSteps(queue);
}

bool ActionsPanel::playAction(const QString& actionId, const QString& fromStepId,
                              bool singleStep)
{
    if (!engine_ || engine_->isRecording() || playing_ || engine_->isPlaying()) {
        return false;
    }
    QStringList steps = includedStepsForAction(actionId);
    int start = fromStepId.isEmpty() ? 0 : steps.indexOf(fromStepId);
    if (!fromStepId.isEmpty() && start < 0) {
        const QStringList all = model_->stepIdsForAction(actionId);
        const int position = all.indexOf(fromStepId);
        for (int i = position + 1; i >= 0 && i < all.size(); ++i) {
            const int candidate = steps.indexOf(all.at(i));
            if (candidate >= 0) {
                start = candidate;
                break;
            }
        }
    }
    if (start < 0) {
        start = 0;
    }
    steps = steps.mid(start);
    if (singleStep && !steps.isEmpty()) {
        steps = steps.mid(0, 1);
    }
    if (steps.isEmpty()) {
        return true;
    }
    return playSteps(steps);
}

bool ActionsPanel::playActionForTest(const QString& actionId)
{
    return playAction(actionId, QString(), false);
}

void ActionsPanel::schedulePlayback()
{
    if (!playing_ || !engine_) {
        return;
    }
    int interval = 0;
    if (engine_->playbackMode() == 1) {
        interval = 300;
    } else if (engine_->playbackMode() == 2) {
        interval = qMax(1, qRound(engine_->playbackPauseSeconds() * 1000.0));
    }
    playbackTimer_->start(interval);
}

bool ActionsPanel::effectiveModal(const ActionNodeRow& step)
{
    if (step.dialogOptions == 1) {
        return true;
    }
    if (step.dialogOptions == 2) {
        return false;
    }
    if (step.dialogOptions == 3) {
        return true;
    }
    return step.modal;
}

bool ActionsPanel::refuseStep(const ActionNodeRow& step)
{
    const QString reason = stepGate_ && !step.commandId.isEmpty() ? stepGate_(step.commandId)
                                                                   : QString();
    if (reason.isEmpty()) {
        return false;
    }
    reportError(tr("Step %1 (%2) was not run: %3").arg(step.name, step.commandId, reason));
    return true;
}

bool ActionsPanel::executeStep(const ActionNodeRow& step, QJsonObject* params, bool modal)
{
    // KookaPictura-SHP fork: Record Again and step edits run through here too.
    if (refuseStep(step)) {
        return false;
    }
    if (!step.commandId.isEmpty()) {
        if (stepExecutor_) {
            return stepExecutor_(step, params, modal);
        }
        if (registry_) {
            CommandParams commandParams;
            commandParams.values = params ? *params : step.params;
            const bool ok = registry_->executeRecorded(
                step.commandId, &commandParams,
                modal ? CommandMode::Modal : CommandMode::Replay);
            if (ok && params) {
                *params = commandParams.values;
            }
            return ok;
        }
    }
    return false;
}

void ActionsPanel::advancePlayback()
{
    if (!playing_ || !engine_) {
        return;
    }
    const ActionNodeRow step = engine_->playbackNext();
    if (step.stepKind == QLatin1String("finished")
        || (step.id.isEmpty() && step.commandId.isEmpty() && step.stepKind.isEmpty())) {
        finishPlayback();
        return;
    }
    if (step.stepKind == QLatin1String("error")) {
        reportError(step.message.isEmpty() ? tr("Playback failed.") : step.message);
        finishPlayback();
        return;
    }
    if (!step.id.isEmpty()) {
        const QModelIndex current = model_->indexForId(step.id);
        if (current.isValid()) {
            tree_->setCurrentIndex(current);
        }
    }
    // KookaPictura-SHP fork: a command the active document refuses stops the
    // playback with the reason instead of running.
    if (refuseStep(step)) {
        finishPlayback();
        return;
    }
    bool ok = true;
    if (step.stepKind == QLatin1String("stop")) {
        if (stopHandler_) {
            ok = stopHandler_(step.message, step.allowContinue);
        } else {
            QMessageBox::information(this, tr("Stop"), step.message);
            ok = step.allowContinue;
        }
        if (ok && step.allowContinue) {
            engine_->playbackContinue();
        } else if (!ok) {
            finishPlayback();
            return;
        }
    } else if (step.stepKind == QLatin1String("waiting")) {
        return;
    } else if (step.stepKind == QLatin1String("path")) {
        ok = pathStepExecutor_ ? pathStepExecutor_(step.path)
                               : engine_->applyPathStep(step.id);
    } else if (step.stepKind == QLatin1String("menuItem")) {
        ok = registry_ && registry_->dispatch(step.commandId);
    } else if (step.stepKind == QLatin1String("tool")) {
        if (!toolStepExecutor_) {
            reportError(tr("Tool step %1 has no executor.").arg(step.id));
            finishPlayback();
            return;
        }
        ok = toolStepExecutor_(step.params);
    } else if (step.stepKind == QLatin1String("conditional")) {
        const QString branch = engine_->conditionalBranch(step.id);
        if (branch.isEmpty()) {
            reportError(tr("Conditional step %1 has no branch.").arg(step.name));
            finishPlayback();
            return;
        }
    } else if (!step.commandId.isEmpty()) {
        QJsonObject params = step.params;
        const bool modal = effectiveModal(step);
        ok = executeStep(step, &params, modal);
        if (ok && modal && params != step.params) {
            engine_->editStep(step.id, params);
        }
    }
    if (!ok) {
        reportError(tr("Step %1 (%2) failed.").arg(step.name, step.commandId));
        finishPlayback();
        return;
    }
    schedulePlayback();
}

void ActionsPanel::advancePlaybackForTest()
{
    advancePlayback();
}

void ActionsPanel::finishPlayback()
{
    if (!playing_) {
        return;
    }
    playing_ = false;
    playbackTimer_->stop();
    playQueue_.clear();
    playIndex_ = 0;
    playDepth_ = 0;
    if (engine_ && engine_->isPlaying()) {
        engine_->cancelPlayback();
    }
    updateToolbar();
    emit changed();
}

void ActionsPanel::cancelPlaybackForTest()
{
    finishPlayback();
}

void ActionsPanel::setButtonMode(bool on)
{
    setButtonModeInternal(on);
}

void ActionsPanel::setButtonModeInternal(bool on)
{
    const bool modeChanged = buttonMode_ != on;
    buttonMode_ = on;
    stack_->setCurrentWidget(on ? static_cast<QWidget*>(buttonView_)
                                : static_cast<QWidget*>(tree_));
    if (!modeChanged) {
        return;
    }
    if (on) {
        updateButtonModel();
    }
    updateToolbar();
    schedulePersist();
    emit changed();
}

void ActionsPanel::updateDocumentFacts()
{
    if (!engine_) {
        return;
    }
    DocumentFacts facts;
    if (documentFactsProvider_) {
        facts = documentFactsProvider_();
    }
    engine_->setDocumentFacts(facts.present, facts.square, facts.mode, facts.hasSelection);
}

void ActionsPanel::setAllowToolRecording(bool on)
{
    allowToolRecording_ = on;
    if (engine_) {
        engine_->setAllowToolRecording(on);
    }
    schedulePersist();
    emit changed();
}


} // namespace pictura
