#include "frame_includes.h"
#include "pictura_app/src/cxxqt_object/layer_target_bridge.cxxqt.h"

#include "api/document_api.h"
#include "api/selection_api.h"
#include "api/type_api.h"
#include "busy_job.h"
#include "file_drop_router.h"
#include "fonts.h"
#include "frame_canvas.h"
#include "gpu_probe.h"
#include "panels/numeric_field.h"
#include "pictura_debug_timing.h"
#include "screen_mode_message.h"
#include "shp_support.h"
#include "text_input.h"
#include "pictura_app/src/cxxqt_object/color_management.cxxqt.h"
#include "pictura_app/src/cxxqt_object/image_adjust/image_mode.cxxqt.h"
#include "pictura_app/src/cxxqt_object/impl_history/display.cxxqt.h"
#include "pictura_app/src/cxxqt_object/pdf_export.cxxqt.h"

#include <QtCore/QHash>
#include <QtCore/QSize>
#include <QtCore/QTemporaryDir>
#include <QtGui/QFontMetrics>
#include <QtWidgets/QApplication>
#include <QtWidgets/QFrame>
#include <QtWidgets/QTabBar>

namespace pictura {
namespace {

// Save may route through Save As; only the origin command records the step.
thread_local bool saveActiveOrigin = false;

struct SaveActiveOrigin {
    SaveActiveOrigin() { saveActiveOrigin = true; }
    ~SaveActiveOrigin() { saveActiveOrigin = false; }
};
// The document pane keeps a minimum width even with no document open, so the
// widget columns can never absorb the whole workspace (and the splitter keeps a
// grabbable handle on each side of it). ponytail: chosen, not a sourced CS6
// metric.
constexpr int kWorkspaceMinWidth = 160;

} // namespace

bool launchCreatesScratchDocument(bool selfTest, bool codecLoaded)
{
    return selfTest && !codecLoaded;
}

bool saveActiveInProgress() { return saveActiveOrigin; }

PicturaMainWindow::PicturaMainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    tabs_ = new QTabWidget(this);
    tabs_->setObjectName(QStringLiteral("documentTabs"));
    tabs_->tabBar()->setObjectName(QStringLiteral("documentTabBar"));
    // Every tab gets a custom close button (or spinner) tab button, so the
    // close glyph is part of the tab's size hint and never clipped.
    tabs_->setTabsClosable(false);
    tabs_->setMovable(true);
    tabs_->tabBar()->setElideMode(Qt::ElideRight);
    // Middle-click on a loading tab cancels it; normal tabs are untouched.
    tabs_->tabBar()->installEventFilter(this);
    // The tab windows lead windows_ in visual order, so a dragged tab moves
    // its window there; documents keep their index. QTabBar::tabMoved(from,
    // to) mirrors QList::move(from, to), and Qt keeps the current tab current.
    connect(tabs_->tabBar(), &QTabBar::tabMoved, this, [this](int from, int to) {
        if (from >= 0 && from < tabs_->count() && to >= 0 && to < tabs_->count()
            && tabs_->count() <= windows_.size()) {
            windows_.move(from, to);
        }
    });
    tabs_->setDocumentMode(true);
    buildDocumentTabs();
    // The document area and the panel columns share the central widget through
    // a splitter; the columns are the only host for the panels. M43: the
    // splitter is an ordered set of left columns, the document area, then right
    // columns, with the document area keeping the stretch.
    documentArea_ = new QStackedWidget(this);
    documentArea_->setObjectName(QStringLiteral("documentArea"));
    documentArea_->addWidget(tabs_);
    panelColumn_ = new PanelColumn(this);
    // The splitter itself is the central widget, so drop-target geometry keeps
    // its existing meaning. It reserves the 3 px band on each side; 0-width
    // handles leave no resize seam between panes.
    centerSplitter_ = new QSplitter(Qt::Horizontal, this);
    centerSplitter_->setObjectName(QStringLiteral("centerSplitter"));
    centerSplitter_->setHandleWidth(0);
    centerSplitter_->setContentsMargins(kWorkspaceFrameWidth, 0, kWorkspaceFrameWidth, 0);
    centerSplitter_->addWidget(documentArea_);
    centerSplitter_->addWidget(panelColumn_);
    documentArea_->setMinimumWidth(kWorkspaceMinWidth);
    centerSplitter_->setStretchFactor(0, 1);
    centerSplitter_->setStretchFactor(1, 0);
    centerSplitter_->setChildrenCollapsible(false);
    setCentralWidget(centerSplitter_);
    setDockOptions(QMainWindow::AnimatedDocks | QMainWindow::AllowTabbedDocks);

    // Double-clicking the empty workspace (no documents open) opens the Open
    // dialog. The blank pane is the tab widget's internal stacked widget.
    installWorkspaceOpenGesture();

    registry_ = new CommandRegistry(this);
    addDefaultCommands(*registry_);

    const SessionState state = pictura::loadSession();
    recent_ = state.recent;
    setFileDialogRecentPaths(recent_);
    gpuCompute_ = state.gpuCompute;
    colorPolicy_ = state.colorPolicy;
    cmykPolicy_ = state.cmykPolicy;
    workingCmyk_ = state.workingCmyk;
    renderIntent_ = state.renderIntent;
    blackPointCompensation_ = state.blackPointCompensation;
    useDither_ = state.useDither;
    askMismatchOpen_ = state.askMismatchOpen;
    askMissingOpen_ = state.askMissingOpen;
    pickerChoice_ = state.pickerChoice;
    ColorPickerDialog::setPickerPreference(pickerChoice_);
    applyTextGamma(state.textGammaEnabled, state.textGamma);
    typeShowAsianOptions_ = state.typeShowAsianOptions; typeMiddleEastern_ = state.typeMiddleEastern;
    typeMissingGlyphProtection_ = state.typeMissingGlyphProtection;
    pictura::type_api::setMissingGlyphProtection(typeMissingGlyphProtection_);
    useShiftKeyForToolSwitch_ = state.useShiftKeyForToolSwitch;
    openDocumentsAsTabs_ = state.openDocumentsAsTabs;
    floatingDocumentDocking_ = state.enableFloatingDocumentDocking;
    zoomResizesWindows_ = state.zoomResizesWindows;
    applyShortcutOverrides(state);
    autoSaveRecovery_ = state.autoSaveRecovery;
    autoSaveMinutes_ = state.autoSaveMinutes;
    loadGuideSession(state);
    saveFormatOptions_ = state.saveFormatOptions;
    // The family drives the theme chrome built below, so resolve it before the menu bar (and its theme toggle icon) is built.
    themeFamily_ = state.themeFamily == 1 ? 1 : 0;
    // The application font and every chrome metric built below read the scale,
    // so resolve it before the menu bar, panels, tools, and status bar exist.
    uiScalePercent_ = Theme::normalizedUiScalePercent(state.uiScalePercent);
    Theme::setUiScalePercent(uiScalePercent_);
    applyBundledUiFont();
    // The window predates the application font, so it keeps the default face
    // until told otherwise; the menu bar is created below and inherits it.
    setFont(QApplication::font());
    // Apply the restored theme before the menus, panels, and tools exist, so
    // they are built under it instead of re-polished by a later application.
    // Its style sheet reads the scale and the application font set above.
    brightnessLevel_ = Theme::clampLevel(state.brightnessLevel);
    Theme::apply(themeFamily_ == 1 ? Theme::ThemeFamily::Light : Theme::ThemeFamily::Dark,
                 brightnessLevel_);
    // Probe the adapter once so the toggle can be offered without a document.
    // The marker makes a parent crash during init a one-launch fallback; the
    // cache records a clean failure so the next launch advances.
    {
        PictureView probe;
        const bool marked = beginDeviceInit();
        gpuAvailable_ = probe.gpu_available();
        if (marked) {
            endDeviceInit(gpuAvailable_);
        }
    }
    gpuCandidatesAvailable_ = gpuAvailable_ || gpuChainHasResetPath();
    registerHandlers();
    const auto saveToRecordedPath = [this](const CommandParams& params) {
        return saveActiveAs(params.values.value(QStringLiteral("in")).toString());
    };
    registry_->setExecutor(command_ids::FileSave, saveToRecordedPath);
    registry_->setExecutor(command_ids::FileSaveAs, saveToRecordedPath);
    buildMenus();
    const QList<PanelGroup*> stagedGroups = buildPanels(state);
    buildTools(state.toolsColumns, state.useShiftKeyForToolSwitch, state.toolOptions);
    if (tools_) {
        tools_->setTypePlaceholderText(state.typePlaceholderText);
        tools_->applyCropOptions(state.cropClassicMode, state.cropDeletePixels, state.cropRatio,
                                 state.cropGridOverlay);
    }
    buildStatusBar();
    // The two 1 px inner frame lines span the whole central band (options bar
    // included); the outer 2 px is the window `${surface}` showing through the
    // reserved margin. They are raised above the chrome in layoutWorkspaceFrame.
    workspaceFrameLeft_ = new QWidget(this);
    workspaceFrameRight_ = new QWidget(this);
    for (QWidget* line : {workspaceFrameLeft_, workspaceFrameRight_}) {
        line->setObjectName(QStringLiteral("workspaceFrameLine"));
        line->setAttribute(Qt::WA_StyledBackground, true);
        line->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    }
    // Route OS file drops by target: a document canvas places into the active
    // document, every other target opens a new tab. Enabling drops is what makes
    // each widget a drop target; per-canvas install happens in addDocument.
    fileDropRouter_ = new FileDropRouter(this);
    for (QWidget* target :
         {static_cast<QWidget*>(tabs_), static_cast<QWidget*>(tabs_->tabBar()),
          static_cast<QWidget*>(menuBar()), static_cast<QWidget*>(optionsBar_),
          static_cast<QWidget*>(this)}) {
        target->setAcceptDrops(true);
        target->installEventFilter(fileDropRouter_);
    }
    applyBrightness(state.brightnessLevel);
    applyPanelSession(state);
    applyFreshSessionDefault(state, stagedGroups);
    const bool sessionHadLayout = !state.panelColumns.isEmpty() || !state.panelGroups.isEmpty();
    initWorkspaces(sessionHadLayout);
    // Persist every column change through the same path as the Window toggles,
    // and route the tab menu's `Interface Options…` to the Interface pane.
    wirePanelColumn(panelColumn_);
    if (!state.layout.isEmpty()) {
        restoreStoredLayout(state.layout, state.layoutRevision);
    }

    // Expensive panel work (layer thumbnails, histogram) is coalesced off the
    // paint-dab/commit path so the canvas can repaint first.
    panelRefreshTimer_ = new QTimer(this);
    panelRefreshTimer_->setSingleShot(true);
    panelRefreshTimer_->setInterval(120);
    connect(panelRefreshTimer_, &QTimer::timeout, this, &PicturaMainWindow::refreshPanels);

    // A splitter drag has no other save trigger; coalesce the column-width
    // changes into one write instead of saving on every pixel of the drag.
    sessionSaveTimer_ = new QTimer(this);
    sessionSaveTimer_->setSingleShot(true);
    sessionSaveTimer_->setInterval(400);
    connect(sessionSaveTimer_, &QTimer::timeout, this, &PicturaMainWindow::saveSession);
    connect(centerSplitter_, &QSplitter::splitterMoved, this,
            [this](int pos, int index) { onCenterSplitterMoved(pos, index); });

    connect(tabs_, &QTabWidget::currentChanged, this,
            [this](int tab) { onTabCurrentChanged(tab); });
    connect(tabs_->tabBar(), &QTabBar::tabBarClicked, this,
            [this](int tab) { onTabBarClicked(tab); });
    auto* hidePanels = new QShortcut(QKeySequence(Qt::Key_Tab), this);
    connect(hidePanels, &QShortcut::activated, this, [this]() { togglePanelsHidden(); });
    auto* hidePanelsBack = new QShortcut(QKeySequence(Qt::SHIFT | Qt::Key_Tab), this);
    connect(hidePanelsBack, &QShortcut::activated, this, [this]() { togglePanelsHidden(); });
    // A focused field owns Tab / Shift+Tab: it commits and moves focus to the
    // next field instead of hiding the panels (#1028). Each connection's context
    // is its shortcut, so teardown disconnects it with the shortcut.
    for (QShortcut* shortcut : {hidePanels, hidePanelsBack}) {
        shortcut->setEnabled(!isTextInput(QApplication::focusWidget()));
        connect(qApp, &QApplication::focusChanged, shortcut,
                [shortcut](QWidget*, QWidget* now) { shortcut->setEnabled(!isTextInput(now)); });
    }
    wireDocumentCycling();
    wireDocumentMenus();
    wireWindowArrange();
    auto* brightnessDown = new QShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F1), this);
    connect(brightnessDown, &QShortcut::activated, this,
            [this]() { setBrightnessLevel(brightnessLevel_ - 1); });
    auto* brightnessUp = new QShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F2), this);
    connect(brightnessUp, &QShortcut::activated, this,
            [this]() { setBrightnessLevel(brightnessLevel_ + 1); });

    resize(1100, 700);
    restoreSessionWindowGeometry(state);
    // A normal launch opens no document; refresh() is the only writer of the
    // tab pane's visibility, so hide the empty ghost canvas at startup.
    refresh();
    setWindowTitle(QStringLiteral("Kooka Pictura"));
    // Make the splitter handles inert from the start: the widget columns' own
    // grips are the only width-drag affordance on a workspace-facing edge.
    reapplyColumnStretch();
    layoutWorkspaceFrame();
}

PicturaMainWindow::~PicturaMainWindow()
{
    shutdownRecovery();
    // Detach the focus-loss Space guard before the child widgets are torn down;
    // a child's clearFocus() emits focusChanged while tabs_ is already gone.
    QObject::disconnect(spacePanFocusConn_);
    QObject::disconnect(deleteShortcutsFocusConn_);
    for (const SmartObjectEditSession& session : editSessions_) {
        delete session.temp;
    }
    editSessions_.clear();
    // The registry is plain data; stop the tab signals from reaching it while
    // the child widgets are torn down.
    QObject::disconnect(tabs_, nullptr, this, nullptr);
    QObject::disconnect(tabs_->tabBar(), nullptr, this, nullptr);
    activeWindow_ = nullptr;
    windows_.clear();
    for (DocEntry* doc : docs_) {
        qDeleteAll(doc->windows);
    }
    qDeleteAll(docs_);
    docs_.clear();
}

QStringList PicturaMainWindow::topLevelMenuTitles() const
{
    return registry_->topLevelTitles();
}

ToolId PicturaMainWindow::activeTool() const
{
    return tools_ ? tools_->activeTool() : ToolId::Move;
}

void PicturaMainWindow::setActiveTool(ToolId id)
{
    if (tools_) {
        tools_->setActiveTool(id);
    }
}

bool PicturaMainWindow::hasPendingCrop() const
{
    return tools_ && tools_->hasPendingCrop();
}

bool PicturaMainWindow::commitCrop()
{
    CropCommitSession* session = tools_ ? tools_->cropCommitSession() : nullptr;
    if (!session || !session->commitCrop()) {
        return false;
    }
    refresh();
    return true;
}

bool PicturaMainWindow::registerPanel(QWidget* panel, Qt::DockWidgetArea area)
{
    if (!panel || panelNames_.contains(panel->objectName())) {
        return false;
    }
    panelNames_.insert(panel->objectName());
    if (auto* dock = qobject_cast<QDockWidget*>(panel)) {
        addDockWidget(area, dock);
    }
    return true;
}

ImageView* PicturaMainWindow::imageView() const
{
    return canvasAt(activeDocumentIndex());
}

PictureView* PicturaMainWindow::activeView() const
{
    return viewAt(activeDocumentIndex());
}

int PicturaMainWindow::activeDocumentIndex() const
{
    return activeWindow_ ? indexOfDoc(activeWindow_->doc) : -1;
}

void PicturaMainWindow::setActiveDocumentIndex(int index)
{
    if (index < 0 || index >= docs_.size()) {
        return;
    }
    const DocEntry* doc = docs_.at(index);
    if (activeWindow_ && activeWindow_->doc == doc) {
        activateDocWindow(activeWindow_);
    } else if (doc->lastActive) {
        activateDocWindow(doc->lastActive);
    } else if (!doc->windows.isEmpty()) {
        activateDocWindow(doc->windows.first());
    }
}

PictureView* PicturaMainWindow::viewAt(int index) const
{
    if (index < 0 || index >= docs_.size()) {
        return nullptr;
    }
    return docs_.at(index)->view;
}

ImageView* PicturaMainWindow::canvasAt(int index) const
{
    if (index < 0 || index >= docs_.size()) {
        return nullptr;
    }
    const DocEntry* doc = docs_.at(index);
    if (activeWindow_ && activeWindow_->doc == doc) {
        return activeWindow_->canvas;
    }
    if (doc->lastActive) {
        return doc->lastActive->canvas;
    }
    return doc->windows.isEmpty() ? nullptr : doc->windows.first()->canvas;
}

QString PicturaMainWindow::documentPath(int index) const
{
    if (index < 0 || index >= docs_.size()) {
        return QString();
    }
    return docs_.at(index)->path;
}

QString PicturaMainWindow::documentName(int index) const
{
    if (index < 0 || index >= docs_.size()) {
        return QStringLiteral("Untitled");
    }
    return documentDisplayName(*docs_.at(index));
}

bool PicturaMainWindow::isDocumentDirty(int index) const
{
    PictureView* view = viewAt(index);
    return view && document_api::isDirty(*view);
}

QString PicturaMainWindow::activeFilePath() const
{
    return documentPath(activeDocumentIndex());
}

QString PicturaMainWindow::activeDocumentName() const
{
    return documentName(activeDocumentIndex());
}

bool PicturaMainWindow::isActiveDirty() const
{
    return isDocumentDirty(activeDocumentIndex());
}

int PicturaMainWindow::addDocument(PictureView* view, const QString& path, bool asTab)
{
    if (!view) {
        return -1;
    }
    // Apply the persisted preference to every newly created/opened tab.
    if (view->gpu_compute() != gpuCompute_) {
        view->set_gpu_compute(gpuCompute_);
    }
    applyColorSettings(view);
    gpuAvailable_ = view->gpu_available();
    auto* entry = new DocEntry;
    entry->view = view;
    if (path.isEmpty()) {
        entry->untitledNumber = ++untitledCounter_;
    } else {
        entry->path = path;
    }
    auto* window = new DocWindow;
    window->doc = entry;
    entry->windows.append(window);
    setupCanvas(*window);
    window->page = window->host;
    wireDocument(*entry);
    wireCanvas(*window);

    docs_.append(entry);
    const int index = docs_.size() - 1;
    addTabWindow(window);
    installCloseButton(window);
    activateDocWindow(window);
    // A tile layout takes in even a window asked for as a tab, which would
    // otherwise sit in the hidden strip.
    if (!asTab || tileHost_) {
        hostReadyWindow(window, true);
    }
    if (!path.isEmpty()) {
        rememberRecent(path);
    }
    refresh();
    panelRefreshTimer_->stop();
    refreshPanels();
    return index;
}

bool PicturaMainWindow::newDocument(const QString& name, int width, int height,
                                    const QString& mode, int depth, const QString& background)
{
    // `name` is accepted for the frozen API; DocEntry carries no name field, so
    // the tab always shows the generated Untitled-<n>.
    Q_UNUSED(name);
    auto* view = new PictureView(this);
    if (!document_api::newDocument(*view, width, height, mode, depth, background)) {
        delete view;
        return false;
    }
    addDocument(view, QString());
    return true;
}

bool PicturaMainWindow::editSmartObjectContents(const QString& layerPath)
{
    PictureView* origin = activeView();
    if (!origin || layerPath.isEmpty()
        || !origin->layer_can_edit_smart_object_contents(layerPath)) {
        return false;
    }
    auto* temp = new QTemporaryDir();
    if (!temp->isValid()) {
        delete temp;
        return false;
    }
    const QString filename = QStringLiteral("contents.psd");
    const QString file = temp->filePath(filename);
    if (!origin->export_smart_object_contents(layerPath, file)) {
        delete temp;
        return false;
    }
    auto* editor = new PictureView(this);
    if (!document_api::open(*editor, file)) {
        delete editor;
        delete temp;
        return false;
    }
    addDocument(editor, QString());
    editSessions_.append(SmartObjectEditSession{editor, origin, layerPath, filename, temp});
    return true;
}

bool PicturaMainWindow::saveActive()
{
    SaveActiveOrigin origin;
    const auto record = [this] {
        registry_->recordCommand(command_ids::FileSave,
            CommandParams{QJsonObject{{QStringLiteral("in"), activeFilePath()}}});
    };
    const int index = activeDocumentIndex();
    if (index < 0) {
        return false;
    }
    PictureView* view = viewAt(index);
    for (const SmartObjectEditSession& session : editSessions_) {
        if (session.editor != view) {
            continue;
        }
        const QString file = session.temp->filePath(session.filename);
        const QString name = QFileInfo(file).fileName();
        const bool saved = saveRunsOnWorker(file)
            ? runBusyJob(this, name, tr("Saving %1…").arg(name),
                         [view, file] { return document_api::saveStart(*view, file); },
                         [view] { return document_api::saveRunning(*view); },
                         [view] { return document_api::saveFinish(*view); })
            : document_api::save(*view, file);
        if (!saved
            || !session.origin->commit_smart_object_edit(session.layerPath, file)) {
            return false;
        }
        refresh();
        updateTabTitle(index);
        updateWindowTitle();
        // The editor tab has no file path of its own; the step would replay an
        // empty save target, so a smart-object commit records nothing here.
        return true;
    }
    QString path = docs_.at(index)->path;
    if (!path.isEmpty() && isPdfPath(path)) {
        const bool saved = savePdfWithDialog(path);
        if (saved) {
            record();
        }
        return saved;
    }
    if (path.isEmpty()
        || savePathNeedsFormatDialog(path, view, isNativeDocumentPath(path))) {
        const bool saved = saveAsWithDialog();
        if (saved) {
            record();
        }
        return saved;
    }
    const bool saved = saveActiveAs(path);
    if (saved) {
        record();
    }
    return saved;
}

bool PicturaMainWindow::saveActiveAs(const QString& path)
{
    const int index = activeDocumentIndex();
    PictureView* view = viewAt(index);
    // A suffix no writer handles is refused here, before any worker starts.
    if (!view || path.isEmpty() || !isWritableSavePath(path)) {
        return false;
    }
    // KookaPictura-SHP fork: off-palette and gutter pixels are fixed or refused first,
    // and an image that is not an SHP sheet is converted into a new one.
    if (isShpPath(path)
        && !(ShpSupport::of(this) && ShpSupport::of(this)->saveGate(view, true, path))) {
        return false;
    }
    const QString name = QFileInfo(path).fileName();
    const bool saved = saveRunsOnWorker(path)
        ? runBusyJob(this, name, tr("Saving %1…").arg(name),
                     [view, path] { return document_api::saveStart(*view, path); },
                     [view] { return document_api::saveRunning(*view); },
                     [view] { return document_api::saveFinish(*view); })
        : document_api::save(*view, path);
    if (!saved) {
        return false;
    }
    docs_[index]->path = path;
    // A stale save leaves the document modified; keep the recovery snapshot so
    // an edit made during the save is still recoverable.
    if (!document_api::isDirty(*view)) {
        dropRecoverySnapshot(view);
    }
    updateTabTitle(index);
    rememberRecent(path);
    updateWindowTitle();
    if (!saveActiveOrigin) {
        registry_->recordCommand(command_ids::FileSaveAs,
            CommandParams{QJsonObject{{QStringLiteral("as"), path.section(QLatin1Char('.'), -1).toUpper()},
                                      {QStringLiteral("in"), path}}});
    }
    return true;
}

bool PicturaMainWindow::revertActive()
{
    const int index = activeDocumentIndex();
    PictureView* view = viewAt(index);
    if (!view) {
        return false;
    }
    const QString path = document_api::filePath(*view);
    if (path.isEmpty()) {
        return false;
    }
    // Reload in place through the Rust edge: decode first, preserve history and
    // file identity, then append one undoable Revert state.
    const bool reloaded = document_api::revert(*view, path);
    if (!reloaded) {
        return false;
    }
    updateTabTitle(index);
    refresh();
    return true;
}

bool PicturaMainWindow::closeDocument(int index, bool interactive)
{
    if (index < 0 || index >= docs_.size()) {
        return false;
    }
    // Closing a loading tab cancels its open; there is no document to save.
    if (docs_.at(index)->loading) {
        cancelOpen(index);
        return true;
    }
    if (interactive && isDocumentDirty(index)) {
        switch (askUnsaved(this, documentName(index))) {
        case UnsavedChoice::Cancel:
            return false;
        case UnsavedChoice::Save:
            setActiveDocumentIndex(index);
            if (!saveActive()) {
                return false;
            }
            break;
        case UnsavedChoice::Discard:
            break;
        }
    }
    removeDocument(index);
    return true;
}

bool PicturaMainWindow::closeActiveDocument(bool interactive)
{
    return closeDocument(activeDocumentIndex(), interactive);
}

void PicturaMainWindow::removeDocument(int index)
{
    if (index < 0 || index >= docs_.size()) {
        return;
    }
    DocEntry* entry = docs_.takeAt(index);
    // Replace Color is non-modal and holds the raw view; cancel it before the
    // view is deleted, or its preview teardown would touch freed memory.
    if (replaceColorDialog_ && replaceColorDialog_->view() == entry->view) {
        replaceColorDialog_->reject();
    }
    for (int i = editSessions_.size() - 1; i >= 0; --i) {
        const SmartObjectEditSession& session = editSessions_.at(i);
        if (session.editor == entry->view || session.origin == entry->view) {
            delete session.temp;
            editSessions_.removeAt(i);
        }
    }
    const bool removedWasActive = entry->view && activeWindow_ && activeWindow_->doc == entry;
    const bool removedWasBound = entry->view && entry->view == dockView_;
    detachWindows(entry->windows);
    forgetRecovery(entry->view);
    if (entry->view) {
        openWired_.remove(entry->view);
        pdfProbes_.remove(entry->view);
        entry->view->cancel_transform();
        if (tools_) {
            tools_->forgetView(entry->view);
        }
    }
    openSourcePaths_.remove(entry->token);
    openWarnings_.remove(entry->token);
    for (DocWindow* window : entry->windows) {
        destroyWindowWidgets(window);
    }
    qDeleteAll(entry->windows);
    // Unbind the panels before the active or bound view is freed; closing an
    // unbound, inactive document leaves the surface untouched.
    if (removedWasActive || removedWasBound) {
        retargetDock();
    }
    delete entry->view;
    delete entry;
    refresh();
    panelRefreshTimer_->stop();
    refreshPanels();
}

void PicturaMainWindow::showNewDocumentDialog()
{
    const QString suggested = QStringLiteral("Untitled-%1").arg(untitledCounter_ + 1);
    NewDocumentSpec spec;
    if (!NewDocumentDialog::get(this, suggested,
                                tools_ ? tools_->background() : QColor(Qt::white), &spec)) {
        return;
    }
    auto* view = new PictureView(this);
    if (!create_document(*view, spec.width, spec.height, spec.mode, spec.depth,
                         spec.fill.rgb() & 0xffffffu, spec.transparent, spec.ppi, spec.perCm)) {
        delete view;
        statusBar()->showMessage(tr("Could not create the document."));
        return;
    }
    const int index = addDocument(view, QString());
    // A name the user typed replaces the generated Untitled-n.
    if (index >= 0 && spec.name.trimmed() != documentName(index) && !spec.name.trimmed().isEmpty()) {
        docs_[index]->displayName = spec.name.trimmed();
        updateTabTitle(index);
    }
}

void PicturaMainWindow::showFileInfo()
{
    PictureView* view = activeView();
    if (!view) {
        return;
    }
    FileInfoDialog dialog(view->exif_rows(), view->xmp_rows(), view->iptc_edit_fields(),
                          view->iptc_rows(), view->xmp_packet(), this);
    dialog.onExportTemplate = [this, &dialog, view] {
        const QString filter = QStringLiteral("XMP files (*.xmp)");
        QString dest = getSaveFileName(this, tr("Export Metadata Template"),
                                       documentDirectory(activeFilePath()),
                                       {filter}, nullptr);
        if (dest.isEmpty()) {
            return;
        }
        // ponytail: the static getSaveFileName has no setDefaultSuffix; append it when omitted.
        if (!dest.endsWith(QStringLiteral(".xmp"), Qt::CaseInsensitive)) {
            dest += QStringLiteral(".xmp");
        }
        if (!view->export_metadata_template(dest)) {
            QMessageBox::warning(&dialog, tr("Export Metadata Template"),
                                 tr("Could not write the template."));
        }
    };
    dialog.onApplyTemplate = [this, &dialog, view](int mode) {
        const QString filter = QStringLiteral("XMP files (*.xmp)");
        const QString path =
            getOpenFileName(&dialog, tr("Apply Metadata Template"),
                            documentDirectory(activeFilePath()), {filter});
        if (path.isEmpty()) {
            return;
        }
        if (!view->apply_metadata_template(path, mode)) {
            QMessageBox::warning(&dialog, tr("Apply Metadata Template"),
                                 tr("Could not apply the template."));
        }
    };
    if (runDialog(dialog, this) == QDialog::Accepted) {
        view->apply_metadata_edits(dialog.edits());
    }
}

void PicturaMainWindow::showOpenDialog()
{
    const QStringList paths = getOpenFileNames(this, tr("Open"),
                                               documentDirectory(activeFilePath()),
                                               openFileFilters());
    // Start every chosen path; a failure on one must not skip the rest.
    for (const QString& path : paths) {
        startOpenDocumentAtPath(path);
    }
}

void PicturaMainWindow::setScreenMode(ScreenMode mode)
{
    const ScreenMode previous = screenMode_;
    screenMode_ = mode;
    auto setCanvasColor = [this](const QColor& color) {
        for (const DocWindow* window : windows_) {
            if (window->canvas) {
                window->canvas->setCanvasColor(color);
            }
        }
    };
    switch (mode) {
    case ScreenMode::Standard: {
        Qt::WindowStates state = windowState();
        state.setFlag(Qt::WindowFullScreen, false);
        setWindowState(state);
        menuBar()->setVisible(true);
        statusBar()->setVisible(true);
        setPanelColumnsVisible(!panelsHidden_);
        setCanvasColor(canvasColorForIndex(canvasColorIndex_));
        break;
    }
    case ScreenMode::FullWithMenuBar:
        fullScreenChromeShown_ = false;
        setWindowState(Qt::WindowFullScreen);
        menuBar()->setVisible(true);
        statusBar()->setVisible(false);
        // CS6 keeps the Tools panel, Options bar, and panels in this mode; only
        // the document-window chrome (tab, scroll bars, status bar) goes away.
        setPanelColumnsVisible(!panelsHidden_);
        setCanvasColor(QColor(128, 128, 128));
        break;
    case ScreenMode::Full:
        if (previous != ScreenMode::Full) {
            fullScreenChromeShown_ = false;
        }
        setWindowState(Qt::WindowFullScreen);
        menuBar()->setVisible(fullScreenChromeShown_);
        statusBar()->setVisible(false);
        setPanelColumnsVisible(fullScreenChromeShown_);
        setCanvasColor(Qt::black);
        break;
    }
    syncScreenModeWindows();
    registry_->refresh();
    if (toolbox_) {
        toolbox_->setActiveScreenMode(static_cast<int>(mode));
    }
    layoutWorkspaceFrame();
}

PicturaMainWindow::ScreenMode PicturaMainWindow::nextScreenMode(bool forward) const
{
    int index = static_cast<int>(screenMode_);
    index += forward ? 1 : -1;
    index = (index % 3 + 3) % 3;
    return static_cast<ScreenMode>(index);
}

void PicturaMainWindow::cycleScreenMode(bool forward)
{
    setScreenMode(nextScreenMode(forward));
}

void PicturaMainWindow::requestScreenMode(ScreenMode mode)
{
    if (mode != ScreenMode::Standard && screenMode_ == ScreenMode::Standard
        && !confirmFullScreenMode(this)) {
        return;
    }
    setScreenMode(mode);
}

void PicturaMainWindow::setPanelColumnsVisible(bool visible)
{
    for (PanelColumn* column : panelColumns()) {
        column->setVisible(visible);
    }
}

void PicturaMainWindow::togglePanelsHidden()
{
    if (screenMode_ != ScreenMode::Full) {
        // Standard and Full Screen With Menu Bar share the persisted Tab state;
        // CS6 toggles the same chrome in both.
        setPanelsHidden(!panelsHidden_);
        return;
    }
    // Full Screen Mode hides the panels and the menu bar; Tab reveals them
    // transiently, and the next Tab hides them again.
    fullScreenChromeShown_ = !fullScreenChromeShown_;
    menuBar()->setVisible(fullScreenChromeShown_);
    setPanelColumnsVisible(fullScreenChromeShown_);
    layoutWorkspaceFrame();
}

void PicturaMainWindow::refresh()
{
    pictura::ScopedTimer refreshTimer("cxx_frame_refresh (total)");
    const int index = activeDocumentIndex();
    PictureView* view = activeView();
    ImageView* canvas = canvasAt(index);

    // Empty workspace: keep the document pane (it holds the splitter stretch and
    // the minimum width, so the widget columns can never absorb the workspace),
    // but hide the empty tab strip. This is the single writer of the pane's
    // visibility so every document-set change routes here.
    if (tabs_) {
        // A full-screen float's page may hold the document area instead.
        tabs_->setVisible(documentArea_->currentWidget() == tabs_);
        if (tabs_->tabBar()) {
            tabs_->tabBar()->setVisible(!docs_.isEmpty());
        }
    }

    presentWindows();
    if (tools_) {
        tools_->bindCanvas(canvas);
    }
    syncBrushTipPreview();
    if (activeWindow_) {
        forEachCanvas(*activeWindow_->doc,
                      [this, view](ImageView* each) { syncMaskOverlay(view, each); });
    }
    syncSiblingGuides();

    panelRefreshTimer_->start();
    updateStatus();
    // A session can end without a transformChanged (a document switch).
    syncTransformLock();
    if (registry_) {
        pictura::ScopedTimer registryTimer("cxx_registry_refresh");
        registry_->refresh();
    }
    registryRefreshPending_ = false;
    updateAlignControls();
    updateTabTitle(index);
    updateWindowTitle();
    refreshSelectionOverlay();
}

void PicturaMainWindow::refreshSelectionOverlay()
{
    PictureView* view = activeView();
    if (!view || !activeWindow_) {
        return;
    }
    if (view->has_selection()) {
        const selection_api::SelectionContour contour = selection_api::contour(*view);
        forEachCanvas(*activeWindow_->doc,
                      [&contour](ImageView* canvas) { canvas->setSelectionContour(contour); });
    } else {
        forEachCanvas(*activeWindow_->doc,
                      [](ImageView* canvas) { canvas->clearSelectionContour(); });
    }
}

void PicturaMainWindow::syncBrushTipPreview()
{
    const PaintOptions* paint =
        tools_ ? tools_->optionsFor<PaintOptions>(ToolId::Brush) : nullptr;
    const bool paintTool = tools_
        && (tools_->activeTool() == ToolId::Brush || tools_->activeTool() == ToolId::Pencil);
    const bool penTip = paint && paint->penTip();
    for (const DocWindow* window : windows_) {
        BrushTipPreview* preview = window->tip;
        if (!preview) {
            continue;
        }
        ImageView* canvas = window->canvas;
        const bool show = window == activeWindow_ && paintTool && penTip && canvas
                          && canvas->hasDocument();
        preview->setVisible(show);
        if (show) {
            preview->raise();
        }
    }
}

bool PicturaMainWindow::isLoadingDocument(int index) const
{
    return index >= 0 && index < docs_.size() && docs_.at(index)->loading;
}

void PicturaMainWindow::updateTabTitle(int index)
{
    if (!tabs_ || index < 0 || index >= docs_.size()) {
        return;
    }
    const DocEntry* doc = docs_.at(index);
    // Keep the close button (or loading spinner) inside the tab: elide the text
    // rather than letting the tab grow past the bar. The dirty star survives.
    const QFontMetrics metrics(tabs_->tabBar()->font());
    const bool dirty = !doc->loading && isDocumentDirty(index);
    for (const DocWindow* window : doc->windows) {
        const QString label = fullLabel(window);
        if (window->floating) {
            window->floating->setLabel(label, dirty);
        }
        if (window->tile) {
            window->tile->setLabel(label, dirty);
        }
        const int tab = tabOf(window);
        if (tab >= 0) {
            tabs_->setTabText(tab, documentTabText(label, dirty, metrics));
            tabs_->setTabToolTip(tab, label);
        }
    }
}

void PicturaMainWindow::updateWindowTitle()
{
    const int index = activeDocumentIndex();
    const QString name = index >= 0 ? documentName(index) : QStringLiteral("Untitled");
    setWindowTitle(QStringLiteral("%1 — Kooka Pictura").arg(name));
}

void PicturaMainWindow::closeEvent(QCloseEvent* event)
{
    cancelAllOpens();
    for (int i = 0; i < docs_.size(); ++i) {
        if (!isDocumentDirty(i)) {
            continue;
        }
        switch (askUnsaved(this, documentName(i))) {
        case UnsavedChoice::Cancel:
            event->ignore();
            return;
        case UnsavedChoice::Save:
            setActiveDocumentIndex(i);
            if (!saveActive()) {
                event->ignore();
                return;
            }
            break;
        case UnsavedChoice::Discard:
            break;
        }
    }
    for (NumericField* field : findChildren<NumericField*>()) field->commitPendingEdit();
    saveSession();
    QMainWindow::closeEvent(event);
}
void PicturaMainWindow::updateStatus()
{
    // One owner for footer visibility: an empty workspace shows none of it.
    const bool hasDoc = !docs_.isEmpty();
    ImageView* canvas = imageView();
    if (zoomField_) {
        zoomField_->setVisible(hasDoc);
        if (canvas) {
            // Integer percent like the tab title: the readouts agree and the
            // value always fits the width the box reserves for its range.
            zoomField_->setValue(qRound(canvas->zoom() * 100.0));
        }
    }
    if (sizeLabel_) {
        sizeLabel_->setVisible(hasDoc);
        QString text = QStringLiteral("—");
        PictureView* view = activeView();
        if (view && view->has_document()) {
            const int width = view->document_width();
            const int height = view->document_height();
            if (statusReadout_ == QStringLiteral("dimensions")) {
                text = QStringLiteral("W %1  H %2").arg(width).arg(height);
            } else {
                // ponytail: 72 ppi is the PSD default; wire to document
                // resolution once the codec surfaces it.
                text = QStringLiteral("%1 px x %2 px (72 ppi)").arg(width).arg(height);
            }
        }
        sizeLabel_->setText(text);
    }
    if (backendLabel_) {
        backendLabel_->setVisible(hasDoc);
        PictureView* view = activeView();
        backendLabel_->setText(view ? activeBackendLabel(view->gpu_compute())
                                     : QStringLiteral("—"));
    }
    if (actionLabel_) {
        QString action;
        PictureView* view = activeView();
        if (hasDoc && view && tools_ && tools_->transformSessionActive()) {
            const QString mode = transform_session_mode(*view);
            if (mode == QStringLiteral("scale")) {
                action = QStringLiteral("Scale in progress");
            } else if (mode == QStringLiteral("rotate")) {
                action = QStringLiteral("Rotate in progress");
            } else if (mode == QStringLiteral("skew")) {
                action = QStringLiteral("Skew in progress");
            } else if (mode == QStringLiteral("distort")) {
                action = QStringLiteral("Distort in progress");
            } else if (mode == QStringLiteral("perspective")) {
                action = QStringLiteral("Perspective in progress");
            } else {
                action = QStringLiteral("Free Transform in progress");
            }
        }
        actionLabel_->setText(action);
        actionLabel_->setVisible(!action.isEmpty());
        if (actionSeparator_) {
            actionSeparator_->setVisible(!action.isEmpty());
        }
    }
    const bool showHints = hasDoc && hintsVisible_;
    if (hintBar_) {
        hintBar_->setVisible(showHints);
    }
    if (hintSeparator_) {
        hintSeparator_->setVisible(showHints);
    }
    if (statusOptionsButton_) {
        statusOptionsButton_->setVisible(hasDoc);
    }
}
QStringList PicturaMainWindow::selectedLayerPaths() const
{
    PictureView* view = activeView();
    return view ? selected_layer_paths(*view) : QStringList{};
}
QString PicturaMainWindow::layerAnchor() const
{
    PictureView* view = activeView();
    return view ? layer_selection_anchor(*view) : QString();
}
} // namespace pictura
#include "moc_frame.cpp"
