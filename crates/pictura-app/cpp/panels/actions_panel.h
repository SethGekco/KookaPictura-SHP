#pragma once

#include "actions_tree_model.h"

#include <QtCore/QHash>
#include <QtCore/QJsonObject>
#include <QtGui/QKeySequence>
#include <QtCore/QList>
#include <QtCore/QPair>
#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtWidgets/QWidget>

#include <functional>
#include <utility>

class QEvent;
class QMenu;
class QStackedWidget;
class QTimer;
class QToolButton;
class QShortcut;

namespace pictura {

class ActionsButtonView;
class CommandRegistry;
struct SessionState;

class ActionsPanel : public QWidget {
    Q_OBJECT

public:
    using StepExecutor = std::function<bool(const ActionNodeRow& step, QJsonObject* params,
                                            bool modal)>;
    using StopHandler = std::function<bool(const QString& message, bool allowContinue)>;
    using ConfirmHandler = std::function<bool(const QString& title, const QString& text)>;
    using ShortcutProvider = std::function<QString(const QKeySequence& shortcut)>;
    using FileDialogProvider =
        std::function<QString(bool save, const QString& title, const QString& filter)>;
    using PathProvider = std::function<QString()>;
    using PathStepExecutor = std::function<bool(const QString&)>;
    using ToolStepExecutor = std::function<bool(const QJsonObject&)>;
    // KookaPictura-SHP fork: why the active document refuses the command a
    // step would run, or empty when it may run.
    using StepGate = std::function<QString(const QString& commandId)>;
    struct DocumentFacts {
        bool present = false;
        bool square = false;
        QString mode;
        bool hasSelection = false;
    };
    using DocumentFactsProvider = std::function<DocumentFacts()>;

    explicit ActionsPanel(QWidget* parent = nullptr);
    ~ActionsPanel() override;

    void setEngine(ActionsEngineApi* engine);
    ActionsEngineApi* engineForTest() const { return engine_.data(); }
    void setRegistry(CommandRegistry* registry);

    void refresh();
    void updatePanelMenu(QMenu* menu);
    bool performPanelMenuAction(const QString& actionId);

    void applySessionState(const SessionState& state);
    void writeSessionState(SessionState* state) const;

    void setPresetsRoot(const QString& root);
    QString presetsRoot() const { return presetsRoot_; }
    QStringList installedSetNamesForTest() const;
    QSet<QString> shadowedCommandIds() const;

    bool buttonMode() const { return buttonMode_; }
    void setButtonMode(bool on);
    bool allowToolRecording() const { return allowToolRecording_; }
    void setAllowToolRecording(bool on);
    bool isRecording() const;
    bool isPlaying() const { return playing_; }

    QString lastErrorForTest() const { return lastError_; }
    QString lastWarningForTest() const { return lastWarning_; }

    ActionsTreeModel* modelForTest() const { return model_; }
    ActionsTreeView* treeForTest() const { return tree_; }
    ActionsButtonView* buttonViewForTest() const { return buttonView_; }
    QList<QToolButton*> toolbarButtonsForTest() const;
    QToolButton* toolbarButtonForTest(const QString& objectName) const;

    QStringList selectedNodeIds() const;
    void selectNodesForTest(const QStringList& ids);
    void expandAllForTest();
    void collapseAllForTest();

    bool createSetForTest(const QString& name);
    bool renameSetForTest(const QString& setId, const QString& name);
    bool createActionForTest(const QString& name, const QString& setId, int functionKey,
                             bool shift, bool ctrl, int color, bool record);
    bool applyActionOptionsForTest(const QString& actionId, const QString& name,
                                   const QString& setId, int functionKey, bool shift, bool ctrl,
                                   int color);
    bool setPlaybackOptionsForTest(int mode, double pauseSeconds);
    bool insertStopForTest(const QString& message, bool allowContinue);
    bool insertMenuItemForTest(const QString& commandId);
    bool insertPathForTest(const QString& path);
    bool insertConditionalForTest(const QString& condition, const QString& thenActionId,
                                  const QString& elseActionId, QString* error);
    bool deleteSelectionForTest(bool skipConfirmation);
    bool moveNodeForTest(const QString& id, const QString& parentId, int row, bool copy);
    bool saveSetForTest(const QString& setId, const QString& path, bool textDump);
    bool loadForTest(const QString& path, bool replace);
    bool resetForTest(bool append);
    bool clearAllForTest();
    bool assignFunctionKeyForTest(const QString& actionId, int key, bool shift, bool ctrl,
                                  QString* error);
    bool handleFunctionKeyForTest(int key, int modifiers);
    bool recordToolStrokeForTest(const QString& toolId, const QJsonObject& params,
                                 int payloadBytes);
    bool overwriteStepForTest(const QString& stepId);

    bool playSelection(bool singleStep = false);
    bool playActionForTest(const QString& actionId);
    void advancePlaybackForTest();
    void cancelPlaybackForTest();

    void setStepExecutorForTest(StepExecutor executor) { stepExecutor_ = std::move(executor); }
    void setStopHandler(StopHandler handler) { stopHandler_ = std::move(handler); }
    void setStopHandlerForTest(StopHandler handler) { stopHandler_ = std::move(handler); }
    void setConfirmHandlerForTest(ConfirmHandler handler) { confirmHandler_ = std::move(handler); }
    void setShortcutProviderForTest(ShortcutProvider provider)
    {
        shortcutProvider_ = std::move(provider);
    }
    void setFileDialogProviderForTest(FileDialogProvider provider)
    {
        fileDialogProvider_ = std::move(provider);
    }
    void setPathSelectionProvider(PathProvider provider)
    {
        pathProvider_ = std::move(provider);
    }
    void setPathSelectionProviderForTest(PathProvider provider)
    {
        setPathSelectionProvider(std::move(provider));
    }
    void setPathStepExecutor(PathStepExecutor executor)
    {
        pathStepExecutor_ = std::move(executor);
    }
    void setToolStepExecutor(ToolStepExecutor executor)
    {
        toolStepExecutor_ = std::move(executor);
    }
    // KookaPictura-SHP fork: a step the gate refuses is not run; playback
    // stops with the reason.
    void setStepGate(StepGate gate) { stepGate_ = std::move(gate); }
    void setDocumentFactsProvider(DocumentFactsProvider provider)
    {
        documentFactsProvider_ = std::move(provider);
    }
    void setWindowsPlatformForTest(bool windows)
    {
        windowsPlatform_ = windows;
        if (engine_) {
            engine_->setWindowsPlatform(windows);
        }
    }

    void setPersistEnabledForTest(bool enabled) { persistEnabled_ = enabled; }
    void persistNowForTest();
    void saveLibraryNowForTest();
    void loadLibraryNowForTest();

    static bool windowsKeyRefused(int key, int modifiers, bool windowsPlatform);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

signals:
    void errorOccurred(const QString& message);
    void functionKeyWarning(const QString& message);
    void changed();

private:
    void buildUi();
    void connectModel();
    void connectEngine();
    void updateToolbar();
    void updateButtonModel();
    void updateActionShortcuts();
    void schedulePersist();
    void persistNow();
    void reportError(const QString& message);
    // KookaPictura-SHP fork: true (and the reason reported) when the step gate
    // refuses `step`.
    bool refuseStep(const ActionNodeRow& step);
    void setButtonModeInternal(bool on);

    QList<ActionNodeRow> selectedRows() const;
    bool selectedIsSet() const;
    bool selectedIsAction() const;
    bool selectedIsStep() const;
    QString selectedSetId() const;
    QString selectedActionId() const;
    QString selectedStepId() const;

    bool editingAllowed() const;
    bool startRecordingSelection();
    void startRecordingOnAction(const QString& actionId, int stepIndex);
    void stopRecording();
    void recordAgain();

    QStringList includedStepsForAction(const QString& actionId) const;
    QStringList buildPlayQueue(const QStringList& ids, bool singleStep, bool* hasSteps,
                               QString* error) const;
    bool playAction(const QString& actionId, const QString& fromStepId, bool singleStep);
    bool playSteps(const QStringList& stepIds);
    void advancePlayback();
    void finishPlayback();
    void schedulePlayback();
    bool executeStep(const ActionNodeRow& step, QJsonObject* params, bool modal);
    void updateDocumentFacts();
    static bool effectiveModal(const ActionNodeRow& step);

    void showNewAction();
    void showNewSet();
    void showOptions();
    void showPlaybackOptions();
    void showInsertStop();
    void showInsertMenuItem();
    void showInsertPath();
    void showInsertConditional();

    bool applyFunctionKey(const QString& actionId, int key, bool shift, bool ctrl,
                          QString* error, int previousSequence = -1);
    int actionKeySequence(const QString& actionId) const;
    void refreshShadowedCommands();
    void restoreShadowedSequence(int sequence);
    QString resolvedPresetsRoot() const;
    QStringList installedSetPaths() const;
    QString fileDialog(bool save, const QString& title, const QString& filter);
    bool confirm(const QString& title, const QString& text);

    QPointer<ActionsEngineApi> engine_;
    CommandRegistry* registry_ = nullptr;
    ActionsTreeModel* model_ = nullptr;
    ActionsTreeView* tree_ = nullptr;
    ActionsButtonView* buttonView_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    QWidget* toolbar_ = nullptr;
    QToolButton* stopButton_ = nullptr;
    QToolButton* recordButton_ = nullptr;
    QToolButton* playButton_ = nullptr;
    QToolButton* newSetButton_ = nullptr;
    QToolButton* newActionButton_ = nullptr;
    QToolButton* deleteButton_ = nullptr;
    QTimer* playbackTimer_ = nullptr;
    QTimer* persistTimer_ = nullptr;
    QList<QShortcut*> actionShortcuts_;

    QString presetsRoot_;
    bool buttonMode_ = false;
    bool allowToolRecording_ = false;
    bool playing_ = false;
    bool persistEnabled_ = true;
    bool applyingSession_ = false;
    bool restoringState_ = false;
    QStringList playQueue_;
    int playIndex_ = 0;
    int playDepth_ = 0;
    QSet<QString> expandedIds_;
    QStringList pendingExpanded_;
    QStringList pendingSelection_;
    QString lastError_;
    QString lastWarning_;

    StepExecutor stepExecutor_;
    StopHandler stopHandler_;
    ConfirmHandler confirmHandler_;
    ShortcutProvider shortcutProvider_;
    FileDialogProvider fileDialogProvider_;
    PathProvider pathProvider_;
    PathStepExecutor pathStepExecutor_;
    ToolStepExecutor toolStepExecutor_;
    StepGate stepGate_;
    DocumentFactsProvider documentFactsProvider_;

    bool windowsPlatform_ = false;
    QHash<int, QPair<QString, QList<QKeySequence>>> shadowedCommands_;
};

} // namespace pictura
