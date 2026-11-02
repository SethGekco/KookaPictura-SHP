#include <QtCore/QRect>
#include "frame_includes.h"

#include "gpu_probe.h"
#include "panels/actions_panel.h"
#include "shp_support.h"

#include "pictura_debug_timing.h"
#include "type_fonts.h"

#include <QtGui/QGuiApplication>
#include <QtGui/QScreen>

#include "pictura_app/src/cxxqt_object/filter_tools/jobs.cxxqt.h"

namespace pictura {

void PicturaMainWindow::setPanelsHidden(bool hidden)
{
    panelsHidden_ = hidden;
    if (hidden) {
        // Frame-parented flyouts are not panel-column children; hide them with
        // the rest of the panels.
        for (PanelColumn* column : allPanelColumns()) {
            if (column) {
                column->closeFlyout();
            }
        }
    }
    setPanelColumnsVisible(!hidden);
}

void PicturaMainWindow::retargetDock()
{
    pictura::ScopedTimer dockTimer("cxx_retargetDock (all panels)");
    PictureView* view = activeView();
    ImageView* canvas = canvasAt(activeDocumentIndex());

    if (layersPanel_) {
        pictura::ScopedTimer t("cxx_layersPanel_refresh");
        // setView ends with a refresh; the debounced pass must not refresh it
        // a second time.
        layersPanel_->setView(view);
    }
    if (historyPanel_) {
        pictura::ScopedTimer t("cxx_historyPanel_refresh");
        historyPanel_->setView(view);
    }
    if (navigatorPanel_) {
        pictura::ScopedTimer t("cxx_navigatorPanel_refresh");
        navigatorPanel_->setCanvas(canvas);
        navigatorPanel_->setView(view);
    }
    if (infoPanel_) {
        pictura::ScopedTimer t("cxx_infoPanel_refresh");
        infoPanel_->setView(view);
    }
    if (propertiesPanel_) {
        propertiesPanel_->setView(view);
    }
    if (adjustmentsPanel_) {
        adjustmentsPanel_->setView(view);
    }
    if (channelsPanel_) {
        channelsPanel_->setView(view);
    }
    if (characterPanel_) {
        characterPanel_->setView(view);
    }
    if (paragraphPanel_) {
        paragraphPanel_->setView(view);
    }
    if (paragraphStylesPanel_) {
        paragraphStylesPanel_->setView(view);
    }
    if (characterStylesPanel_) {
        characterStylesPanel_->setView(view);
    }
    if (notesPanel_) {
        notesPanel_->setView(view);
    }
    // Color Range works on one document; switching away cancels it.
    if (colorRangeDialog_ && colorRangeDialog_->view() != view) {
        colorRangeDialog_->reject();
    }
    // Replace Color holds the same raw view; retargeting must cancel it before
    // its view is left behind.
    if (replaceColorDialog_ && replaceColorDialog_->view() != view) {
        replaceColorDialog_->reject();
    }
    if (pathsPanel_) {
        pathsPanel_->setView(view);
    }
    if (histogramPanel_) {
        pictura::ScopedTimer t("cxx_histogramPanel_refresh");
        histogramPanel_->setView(view);
    }
    if (ShpSupport* shp = ShpSupport::of(this)) {
        shp->setView(view, canvas);
    }

    dockView_ = view;
    dockCanvas_ = canvas;
    dockGeneration_ = view ? view->document_generation() : 0;
    panelsRefreshedByChanged_ = false;
    panelsFreshlyBound_ = true;
    ++dockRetargetCount_;
}

bool PicturaMainWindow::dockNeedsRebind() const
{
    PictureView* view = activeView();
    ImageView* canvas = canvasAt(activeDocumentIndex());
    return view != dockView_ || canvas != dockCanvas_
           || (view && view->document_generation() != dockGeneration_);
}

void PicturaMainWindow::refreshDock(bool includeSelfRefreshing)
{
    if (layersPanel_ && includeSelfRefreshing) {
        layersPanel_->refresh();
    }
    if (historyPanel_ && includeSelfRefreshing) {
        historyPanel_->refresh();
    }
    if (navigatorPanel_) {
        navigatorPanel_->refresh();
    }
    if (infoPanel_) {
        infoPanel_->refresh();
    }
    if (propertiesPanel_) {
        propertiesPanel_->refresh();
    }
    if (adjustmentsPanel_) {
        adjustmentsPanel_->refresh();
    }
    if (channelsPanel_) {
        channelsPanel_->refresh();
    }
    if (characterPanel_) {
        characterPanel_->refresh();
    }
    if (paragraphPanel_) {
        paragraphPanel_->refresh();
    }
    if (paragraphStylesPanel_) {
        paragraphStylesPanel_->refresh();
    }
    if (characterStylesPanel_) {
        characterStylesPanel_->refresh();
    }
    if (notesPanel_) {
        notesPanel_->refresh();
    }
    if (pathsPanel_) {
        pathsPanel_->refresh();
    }
    if (histogramPanel_) {
        histogramPanel_->refresh();
    }
    if (ShpSupport* shp = ShpSupport::of(this)) {
        shp->refresh();
    }
}

void PicturaMainWindow::refreshPanels()
{
    pictura::ScopedTimer t("cxx_refreshPanels (debounced timer)");
    registerTypeFallbacks();
    // A filter commit holds the history on its worker; the commit's own
    // refresh restarts this timer once it lands.
    PictureView* view = activeView();
    if (view && filter_commit_running(*view)) {
        return;
    }
    if (view && view->is_painting()) {
        return;
    }
    if (dockNeedsRebind()) {
        retargetDock();
    } else if (panelsFreshlyBound_) {
        panelsFreshlyBound_ = false;
    } else {
        const bool includeSelfRefreshing = !panelsRefreshedByChanged_;
        panelsRefreshedByChanged_ = false;
        refreshingDock_ = true;
        refreshDock(includeSelfRefreshing);
        refreshingDock_ = false;
    }
    if (registryRefreshPending_) {
        registryRefreshPending_ = false;
        if (registry_) {
            registry_->refresh();
        }
    }
}

void PicturaMainWindow::applyGpuComputePreference(bool on)
{
    // Re-enabling is the user's explicit reset: clear a cached crash so the
    // next launch retries the candidates.
    if (on && !gpuCompute_) {
        clearGpuBackendCache();
    }
    gpuCompute_ = on;
    for (const DocEntry* entry : docs_) {
        if (entry->view) {
            entry->view->set_gpu_compute(on);
        }
    }
    saveSession();
    registry_->refresh();
    refresh();
}

void PicturaMainWindow::saveSession()
{
    // A session restore rebuilds the columns from the store; re-saving mid
    // rebuild would persist a partial layout.
    if (restoringPanelSession_) {
        return;
    }
    SessionState state = pictura::loadSession();
    state.layout = saveState();
    state.layoutRevision = kLayoutRevision;
    state.windowGeometry = saveGeometry();
    state.windowMaximized = isMaximized();
    state.brightnessLevel = brightnessLevel_;
    state.themeFamily = themeFamily_ == 1 ? 1 : 0;
    state.uiScalePercent = uiScalePercent_;
    state.gpuCompute = gpuCompute_;
    state.colorPolicy = colorPolicy_;
    state.cmykPolicy = cmykPolicy_;
    state.workingCmyk = workingCmyk_;
    state.renderIntent = renderIntent_;
    state.blackPointCompensation = blackPointCompensation_;
    state.useDither = useDither_;
    state.askMismatchOpen = askMismatchOpen_;
    state.askMissingOpen = askMissingOpen_;
    state.pickerChoice = pickerChoice_;
    state.textGammaEnabled = textGammaEnabled_;
    state.textGamma = textGamma_;
    state.toolsColumns = toolbox_ ? toolbox_->columns() : 1;
    state.useShiftKeyForToolSwitch = useShiftKeyForToolSwitch_;
    state.openDocumentsAsTabs = openDocumentsAsTabs_;
    state.enableFloatingDocumentDocking = floatingDocumentDocking_;
    state.zoomResizesWindows = zoomResizesWindows_;
    state.typePlaceholderText = tools_ ? tools_->typePlaceholderText() : true;
    state.typeShowAsianOptions = typeShowAsianOptions_;
    state.typeMiddleEastern = typeMiddleEastern_;
    state.typeMissingGlyphProtection = typeMissingGlyphProtection_;
    state.shortcuts.clear();
    const QHash<QString, QList<QKeySequence>> overrides = registry_->shortcutOverrides();
    const QSet<QString> shadowed =
        actionsPanel_ ? actionsPanel_->shadowedCommandIds() : QSet<QString>();
    for (auto it = overrides.cbegin(); it != overrides.cend(); ++it) {
        if (shadowed.contains(it.key())) {
            continue;
        }
        QStringList bindings;
        for (const QKeySequence& binding : it.value()) {
            bindings << binding.toString(QKeySequence::PortableText);
        }
        state.shortcuts.insert(it.key(), bindings);
    }
    state.toolShortcuts.clear();
    const QMap<ToolId, QChar> toolOverrides = toolShortcutOverrides();
    for (auto it = toolOverrides.cbegin(); it != toolOverrides.cend(); ++it) {
        state.toolShortcuts.insert(toolIdName(it.key()), it.value().isNull() ? QString()
                                                                             : QString(it.value()));
    }
    state.autoSaveRecovery = autoSaveRecovery_;
    state.autoSaveMinutes = autoSaveMinutes_;
    state.rulersVisible = rulersVisible_;
    state.rulerUnit = int(rulerUnit_);
    state.traditionalPoints = traditionalPoints_;
    state.guidesVisible = guidesVisible_;
    state.guidesLocked = guidesLocked_;
    state.smartGuidesVisible = smartGuidesVisible_;
    state.freePan = freePan_;
    state.snapCanvasToWorkspace = snapCanvasToWorkspace_;
    state.guideColor = guideColor_.name();
    state.guideDashed = guideDashed_;
    state.saveFormatOptions = saveFormatOptions_;
    if (tools_ && tools_->optionsFor<CropOptions>(ToolId::Crop)) {
        const CropOptions* crop = tools_->optionsFor<CropOptions>(ToolId::Crop);
        state.cropClassicMode = crop->classicMode();
        state.cropDeletePixels = crop->deletePixels();
        state.cropRatio = crop->ratio();
        state.cropGridOverlay = crop->gridOverlay();
    }
    if (tools_) {
        state.toolOptions = toolOptionsToJson(*tools_);
    }
    if (swatchesPanel_) {
        QJsonArray swatches;
        for (const Swatch& swatch : swatchesPanel_->grid()->swatches()) {
            QJsonObject entry;
            entry.insert(QStringLiteral("name"), swatch.name);
            entry.insert(QStringLiteral("color"), swatch.color.name());
            swatches.append(entry);
        }
        state.swatches = swatches;
        state.swatchesLoaded = true;
    }
    if (panelColumn_) {
        const bool iconic = panelColumn_->railMode();
        state.panelRailMode = iconic ? QStringLiteral("iconic") : QStringLiteral("normal");
        // Keep the last normal-mode width; an iconic column has no width of its
        // own, so the loaded value survives unchanged. The remembered width,
        // not the live width, so a mid-flip strip width cannot leak through.
        if (!iconic) {
            const int remembered = panelColumn_->persistedWidth();
            if (remembered > 0) {
                state.railWidth = remembered;
            }
        }
        state.autoCollapseIconic = panelColumn_->autoCollapseIconic();
        state.autoShowHidden = panelColumn_->autoShowHidden();
        // Legacy flat mirror of the primary column kept for older stores.
        state.panelGroups = panelColumn_->savePanelState();
    }
    // v9: the ordered per-column layout, in central-splitter order; shared with
    // the workspace store's arrangement shape (see serializeWorkspaceLayout).
    state.panelColumns = serializeWorkspaceLayout();
    state.schemaVersion = 11;
    state.recent = recent_;
    // A refused write means another instance wrote the store after this one
    // launched; leave its workspace snapshot alone too.
    if (pictura::saveSession(state)) {
        snapshotActiveWorkspace();
    }
}

void PicturaMainWindow::restoreSessionWindowGeometry(const SessionState& state)
{
    // A stored geometry (from the last close) overrides the default size; a
    // stale one that lands fully off-screen (a removed monitor) falls back to
    // the default so the window is never unreachable.
    if (!state.windowGeometry.isEmpty() && restoreGeometry(state.windowGeometry)) {
        bool onScreen = false;
        for (const QScreen* screen : QGuiApplication::screens()) {
            if (screen->availableGeometry().intersects(frameGeometry())) {
                onScreen = true;
                break;
            }
        }
        if (!onScreen) {
            resize(1100, 700);
        }
    }
    if (state.windowMaximized) {
        setWindowState(windowState() | Qt::WindowMaximized);
    }
}

void PicturaMainWindow::onCanvasZoomChanged()
{
    updateStatus();
    if (const DocWindow* window = windowOfCanvas(qobject_cast<ImageView*>(sender()))) {
        updateTabTitle(indexOfDoc(window->doc));
    }
}

QString PicturaMainWindow::documentTabTextForTest(int index) const
{
    if (index < 0 || index >= docs_.size() || docs_.at(index)->windows.isEmpty()) {
        return QString();
    }
    const int tab = tabOf(docs_.at(index)->windows.first());
    return tab >= 0 ? tabs_->tabText(tab) : QString();
}

bool PicturaMainWindow::restoreStoredLayout(const QByteArray& layout, int revision)
{
    if (layout.isEmpty() || revision != kLayoutRevision) {
        return false;
    }
    return restoreState(layout);
}

void PicturaMainWindow::applyPanelSession(const SessionState& state)
{
    if (!panelColumn_) {
        return;
    }
    restoringPanelSession_ = true;
    // A new session apply supersedes any float restore still waiting for show.
    pendingFloatRestores_.clear();

    if (!sessionHasColumnLayout(state)) {
        // No per-column layout: keep the legacy flat-state behaviour and reset
        // the tools column to its default left slot.
        // An empty layout carries no float state: dock every live group and
        // column overlay before applying the legacy flat state.
        const QList<PanelColumn*> live = allPanelColumns();
        for (PanelColumn* column : live) {
            if (column) {
                column->closeFlyout();
            }
        }
        for (PanelColumn* column : live) {
            if (column) {
                column->redockGroupFloats();
            }
        }
        for (PanelColumn* column : live) {
            if (!column || !column->isColumnFloating()) {
                continue;
            }
            column->cancelColumnFloat();
            if (centerSplitter_->indexOf(column) < 0) {
                centerSplitter_->insertWidget(centerSplitter_->count(), column);
            }
            column->setVisible(!panelsHidden_);
        }
        clearDynamicColumns();
        panelColumn_->restorePanelState(state.panelGroups);
        // The legacy reset path still restores the primary rail mode and width.
        panelColumn_->setRailMode(state.panelRailMode == QStringLiteral("iconic"));
        panelColumn_->setRestoredWidth(state.railWidth > 0
                                            ? state.railWidth
                                            : PanelColumn::defaultNormalWidth());
        // A test/reset apply should leave the primary column visible when any
        // group still has visible tabs.
        bool anyVisible = false;
        for (PanelGroup* group : panelColumn_->groups()) {
            if (!group) {
                continue;
            }
            const bool visible = !group->visibleTitles().isEmpty();
            group->setVisible(visible);
            anyVisible = anyVisible || visible;
        }
        if (anyVisible) {
            panelColumn_->setVisible(!panelsHidden_);
        }
        if (toolsColumn_ && centerSplitter_) {
            toolsColumn_->cancelColumnFloat();
            toolsColumn_->setParent(nullptr);
            centerSplitter_->insertWidget(0, toolsColumn_);
            toolsColumn_->setVisible(!panelsHidden_);
            reapplyColumnStretch();
        }
        for (PanelColumn* column : allPanelColumns()) {
            if (column) {
                column->setAutoCollapseIconic(state.autoCollapseIconic);
                column->setAutoShowHidden(state.autoShowHidden);
            }
        }
        restoringPanelSession_ = false;
        return;
    }

    // Normalise pre-v7/v8 entries before the one layout builder runs. The first
    // right-hand column is the primary: it inherits the legacy railWidth; every
    // other column gets the default normal width.
    QJsonArray layout = state.panelColumns;
    int primaryIndex = -1;
    int primaryOrder = (1 << 30);
    for (int i = 0; i < layout.size(); ++i) {
        const QJsonObject entry = layout.at(i).toObject();
        if (entry.value(QStringLiteral("tools")).toBool()
            || entry.value(QStringLiteral("side")).toString() == QStringLiteral("left")) {
            continue;
        }
        const int order = entry.value(QStringLiteral("order")).toInt(0);
        if (order < primaryOrder) {
            primaryOrder = order;
            primaryIndex = i;
        }
    }
    for (int i = 0; i < layout.size(); ++i) {
        QJsonObject entry = layout.at(i).toObject();
        if (entry.value(QStringLiteral("tools")).toBool()) {
            continue;
        }
        if (!entry.contains(QStringLiteral("railMode"))) {
            entry.insert(QStringLiteral("railMode"), state.panelRailMode);
        }
        if (entry.value(QStringLiteral("width")).toInt() <= 0) {
            entry.insert(QStringLiteral("width"),
                         i == primaryIndex && state.railWidth > 0
                             ? state.railWidth
                             : PanelColumn::defaultNormalWidth());
        }
        layout.replace(i, entry);
    }

    regroupAndApply(layout, false);

    // regroupAndApply creates fresh columns; re-apply the legacy global flags to
    // every docked and floating column.
    for (PanelColumn* column : allPanelColumns()) {
        if (!column) {
            continue;
        }
        column->setAutoCollapseIconic(state.autoCollapseIconic);
        column->setAutoShowHidden(state.autoShowHidden);
    }

    // The tools entry is skipped by regroupAndApply (workspace apply must leave
    // Tools untouched), so place it here and restore a floating Tools overlay
    // only on the session path.
    bool haveTools = false;
    bool toolsRight = false;
    bool toolsFloating = false;
    QRect toolsGeometry;
    for (const QJsonValue& value : state.panelColumns) {
        const QJsonObject entry = value.toObject();
        if (!entry.value(QStringLiteral("tools")).toBool()) {
            continue;
        }
        haveTools = true;
        toolsRight = entry.value(QStringLiteral("side")).toString() == QStringLiteral("right");
        toolsFloating = entry.value(QStringLiteral("floating")).toBool(false);
        const QJsonArray geometry = entry.value(QStringLiteral("floatGeometry")).toArray();
        if (geometry.size() == 4) {
            toolsGeometry = QRect(geometry.at(0).toInt(), geometry.at(1).toInt(),
                                  geometry.at(2).toInt(), geometry.at(3).toInt());
        }
        break;
    }
    if (toolsColumn_ && centerSplitter_) {
        toolsColumn_->cancelColumnFloat();
        toolsColumn_->setParent(nullptr);
        const int at = toolsRight ? centerSplitter_->count() : 0;
        centerSplitter_->insertWidget(at, toolsColumn_);
        toolsColumn_->setVisible(!panelsHidden_);
        if (toolsColumn_->toolsContentForTest()) {
            toolsColumn_->refreshToolsWidth();
        }
        reapplyColumnStretch();
        if (haveTools && toolsFloating && toolsGeometry.isValid()) {
            queueOrRestoreFloat(toolsColumn_, nullptr, toolsGeometry);
        }
    }

    restoringPanelSession_ = false;
}

} // namespace pictura
