#include "frame_includes.h"

#include "api/document_api.h"
#include "color_picker_dialog.h"
#include "fill_layer_editor.h"
#include "command_ids.h"

#include "pictura_app/src/cxxqt_object/impl_layers/fill_content.cxxqt.h"
#include "fonts.h"
#include "panels/actions_engine_bridge.h"
#include "panels/actions_panel.h"
#include "pictura_app/src/cxxqt_object/actions_engine.cxxqt.h"
#include "pictura_app/src/cxxqt_object/layers_smart_filters.cxxqt.h"
#include "pictura_app/src/cxxqt_object/paint_tools.cxxqt.h"
#include "pictura_app/src/cxxqt_object/path_list.cxxqt.h"
#include "panels/brush_preset_picker.h"
#include "panels/numeric_field.h"
#include "shp_support.h"
#include "text_input.h"

#include <QtCore/QDebug>
#include <QtCore/QEvent>
#include <QtCore/QJsonDocument>
#include <QtCore/QTimer>
#include <QtGui/QFont>
#include <QtGui/QMouseEvent>
#include <QtWidgets/QApplication>
#include <QtWidgets/QFrame>
#include <QtWidgets/QStackedWidget>

namespace pictura {

QColor PicturaMainWindow::foregroundColor() const
{
    return colorState_ ? colorState_->foreground() : QColor(Qt::black);
}

QColor PicturaMainWindow::backgroundColor() const
{
    return colorState_ ? colorState_->background() : QColor(Qt::white);
}

QList<PanelGroup*> PicturaMainWindow::buildPanels(const SessionState& state)
{
    colorState_ = new ColorState(this);

    layersPanel_ = new LayersPanel(colorState_, this);
    layersPanel_->setObjectName(QStringLiteral("layersPanel"));
    connect(layersPanel_, &LayersPanel::editContentRequested, this, [this](const QString& path) {
        PictureView* view = activeView();
        // A content-thumbnail double-click may target a row that is not
        // current; make it current before the editor or Properties runs.
        if (layersPanel_ && !path.isEmpty() && view) {
            layersPanel_->selectPaths(QStringList{path}, path);
        }
        if (view && layer_is_editable_fill(*view, path)) {
            openFillLayerEditor(*view, path, this);
            return;
        }
        if (registry_) {
            registry_->dispatch(QString::fromLatin1(command_ids::LayerContentOptions));
        }
    });
    // Edit Smart Filter… on a filter row reopens that filter's own dialog, which
    // already prefills from the stored record and replaces the entry.
    connect(layersPanel_, &LayersPanel::smartFilterEditRequested, this,
            [this](const QString& path, int index) {
                PictureView* view = activeView();
                if (!view || !registry_) {
                    return;
                }
                // The filter's layer becomes current, so its dialog edits it.
                if (layersPanel_) {
                    layersPanel_->selectPaths(QStringList{path}, path);
                }
                switch (layer_smart_filter_edit_kind(*view, path, index)) {
                case 1:
                    registry_->dispatch(QString::fromLatin1(command_ids::FilterPicturaRaw));
                    break;
                case 2:
                    registry_->dispatch(commandIdForPath(
                        {QStringLiteral("Filter"), QStringLiteral("Lens Correction…")}));
                    break;
                case 3:
                case 4:
                    editSmartFilter(path, index);
                    break;
                default:
                    break;
                }
            });

    historyPanel_ = new HistoryPanel(this);
    historyPanel_->setObjectName(QStringLiteral("historyPanel"));

    navigatorPanel_ = new NavigatorPanel(this);
    navigatorPanel_->setObjectName(QStringLiteral("navigatorPanel"));

    colorPanel_ = new ColorPanel(colorState_, this);
    colorPanel_->setObjectName(QStringLiteral("colorPanel"));

    swatchesPanel_ = new SwatchesPanel(colorState_, this);
    swatchesPanel_->setObjectName(QStringLiteral("swatchesPanel"));
    ColorPickerDialog::setDefaultSwatchGrid(swatchesPanel_->grid());
    if (state.swatchesLoaded) {
        QList<Swatch> swatches;
        for (const QJsonValue& value : state.swatches) {
            const QJsonObject entry = value.toObject();
            const QColor color(entry.value(QStringLiteral("color")).toString());
            if (color.isValid()) {
                swatches.append({color, entry.value(QStringLiteral("name")).toString()});
            }
        }
        // An intentionally empty array clears the panel; a corrupt non-empty
        // array keeps the default palette rather than opening blank.
        if (!swatches.isEmpty() || state.swatches.isEmpty()) {
            swatchesPanel_->grid()->setSwatches(swatches);
        }
    }
    connect(swatchesPanel_->grid(), &SwatchGrid::countChanged, this, [this]() { saveSession(); });

    infoPanel_ = new InfoPanel(this);
    infoPanel_->setObjectName(QStringLiteral("infoPanel"));

    histogramPanel_ = new HistogramPanel(this);
    histogramPanel_->setObjectName(QStringLiteral("histogramPanel"));

    gradientsPanel_ = new PlaceholderPanel(QStringLiteral("Gradients"), QString(), this);
    gradientsPanel_->setObjectName(QStringLiteral("gradientsPanel"));

    patternsPanel_ = new PlaceholderPanel(QStringLiteral("Patterns"), QString(), this);
    patternsPanel_->setObjectName(QStringLiteral("patternsPanel"));

    notesPanel_ = new NotesPanel(this);
    notesPanel_->setObjectName(QStringLiteral("notesPanel"));

    propertiesPanel_ = new PropertiesPanel(this);
    propertiesPanel_->setObjectName(QStringLiteral("propertiesPanel"));
    connect(propertiesPanel_, &PropertiesPanel::modeRequested, this,
            &PicturaMainWindow::convertImageMode);
    connect(propertiesPanel_, &PropertiesPanel::depthRequested, this,
            &PicturaMainWindow::convertImageDepth);

    adjustmentsPanel_ = new AdjustmentsPanel(this);
    adjustmentsPanel_->setObjectName(QStringLiteral("adjustmentsPanel"));
    // The grid is a launcher: an adjustment icon creates the layer through the
    // panel-menu path, and the engine selects the created layer so the
    // Properties panel opens its controls.
    connect(adjustmentsPanel_, &AdjustmentsPanel::adjustmentRequested, this,
            [this](const QString& kind) {
        if (!layersPanel_ || !activeView()) {
            return;
        }
        layersPanel_->performPanelMenuAction(QStringLiteral("adjustment:") + kind);
    });
    // Fill icons are the quick-add path: create with defaults, then open the
    // type editor on the new layer.
    connect(adjustmentsPanel_, &AdjustmentsPanel::fillRequested, this,
            [this](const QString& type) {
        if (PictureView* view = activeView()) {
            createFillLayerFromDialog(*view, type, foregroundColor(), this, false);
        }
    });

    channelsPanel_ = new ChannelsPanel(this);
    channelsPanel_->setObjectName(QStringLiteral("channelsPanel"));
    // Channel visibility follows the active document; the panel resets it on a
    // document switch, so applying it to every window's canvas keeps them in
    // step.
    connect(channelsPanel_, &ChannelsPanel::channelMaskChanged, this, [this](int mask) {
        for (const DocWindow* window : windows_) {
            if (window->canvas) {
                window->canvas->setChannelMask(mask);
            }
        }
    });

    pathsPanel_ = new PathsPanel(this);
    pathsPanel_->setObjectName(QStringLiteral("pathsPanel"));

    actionsPanel_ = new ActionsPanel(this);
    actionsPanel_->setObjectName(QStringLiteral("actionsPanel"));
    actionsEngine_ = new ActionsEngineBridge(new ActionsEngine(this), this);
    set_active_actions_engine(actionsEngine_);
    actionsPanel_->setEngine(actionsEngine_);
    const bool libraryLoaded = actionsEngine_->loadLibrary();
    const QString libraryError = actionsEngine_->lastError();
    if (!libraryLoaded || !libraryError.isEmpty()) {
        const QString message =
            libraryError.isEmpty()
            ? tr("The Actions library could not be loaded.")
            : tr("The Actions library could not be loaded: %1").arg(libraryError);
        qWarning().noquote() << message;
        statusBar()->showMessage(message, 10000);
    }
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
        if (actionsEngine_) {
            actionsEngine_->saveLibrary();
        }
    });
    actionsPanel_->setRegistry(registry_);
    actionsPanel_->setPathSelectionProvider([this]() -> QString {
        if (!pathsPanel_ || !pathsPanel_->listForTest()) {
            return QString();
        }
        QListWidgetItem* item = pathsPanel_->listForTest()->currentItem();
        return item ? item->text() : QString();
    });
    actionsPanel_->setStopHandler([this](const QString& message, bool allowContinue) {
        QMessageBox box(QMessageBox::Information, tr("Stop"), message, QMessageBox::NoButton, this);
        QPushButton* proceed =
            box.addButton(allowContinue ? tr("Continue") : tr("OK"), QMessageBox::AcceptRole);
        box.addButton(tr("Cancel"), QMessageBox::RejectRole);
        box.setDefaultButton(proceed);
        runDialog(box, this);
        return box.clickedButton() == proceed;
    });
    actionsPanel_->setToolStepExecutor([this](const QJsonObject& payload) {
        PictureView* view = activeView();
        if (!view || !view->has_document()) {
            return false;
        }
        const QString json =
            QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact));
        return pictura::replay_tool_stroke(*view, json);
    });
    actionsPanel_->setDocumentFactsProvider([this]() {
        ActionsPanel::DocumentFacts facts;
        PictureView* view = activeView();
        if (!view || !view->has_document()) {
            return facts;
        }
        facts.present = true;
        facts.square = view->document_width() == view->document_height();
        facts.mode = document_api::documentMode(*view);
        facts.hasSelection = view->has_selection();
        return facts;
    });
    actionsPanel_->setPathStepExecutor([this](const QString& name) {
        PictureView* view = activeView();
        if (!view || !view->has_document()) {
            return false;
        }
        const int count = paths_saved_count(*view);
        for (int i = 0; i < count; ++i) {
            if (paths_saved_name(*view, i) == name) {
                return paths_set_active(*view, i);
            }
        }
        return false;
    });
    connect(actionsPanel_, &ActionsPanel::errorOccurred, this, [this](const QString& message) {
        statusBar()->showMessage(message, 8000);
    });
    connect(actionsPanel_, &ActionsPanel::functionKeyWarning, this,
            [this](const QString& message) { statusBar()->showMessage(message, 8000); });

    // `Styles` takes the former Gradients/Patterns tab slot in the default set.
    stylesPanel_ = new PlaceholderPanel(QStringLiteral("Styles"), QString(), this);
    stylesPanel_->setObjectName(QStringLiteral("stylesPanel"));

    brushPanel_ = new BrushPanel(this);
    brushPanel_->setObjectName(QStringLiteral("brushPanel"));

    cloneSourcePanel_ = new CloneSourcePanel(this);
    cloneSourcePanel_->setObjectName(QStringLiteral("cloneSourcePanel"));

    characterPanel_ = new CharacterPanel(this);
    characterPanel_->setObjectName(QStringLiteral("characterPanel"));
    characterPanel_->setShowAsianOptions(state.typeShowAsianOptions);
    paragraphPanel_ = new ParagraphPanel(this);
    paragraphPanel_->setObjectName(QStringLiteral("paragraphPanel"));
    paragraphPanel_->setMiddleEastern(state.typeMiddleEastern);

    glyphsPanel_ = new GlyphsPanel(this);
    glyphsPanel_->setObjectName(QStringLiteral("glyphsPanel"));

    paragraphStylesPanel_ = new ParagraphStylesPanel(this);
    paragraphStylesPanel_->setObjectName(QStringLiteral("paragraphStylesPanel"));
    characterStylesPanel_ = new CharacterStylesPanel(this);
    characterStylesPanel_->setObjectName(QStringLiteral("characterStylesPanel"));

    // Content widgets, not docks: the PanelColumn hosts them. Registering the
    // objectNames keeps the Window menu and the duplicate-name guard working.
    registerPanel(layersPanel_, Qt::RightDockWidgetArea);
    registerPanel(historyPanel_, Qt::RightDockWidgetArea);
    registerPanel(navigatorPanel_, Qt::RightDockWidgetArea);
    registerPanel(colorPanel_, Qt::RightDockWidgetArea);
    registerPanel(swatchesPanel_, Qt::RightDockWidgetArea);
    registerPanel(stylesPanel_, Qt::RightDockWidgetArea);
    registerPanel(infoPanel_, Qt::RightDockWidgetArea);
    registerPanel(histogramPanel_, Qt::RightDockWidgetArea);
    registerPanel(gradientsPanel_, Qt::RightDockWidgetArea);
    registerPanel(patternsPanel_, Qt::RightDockWidgetArea);
    registerPanel(notesPanel_, Qt::RightDockWidgetArea);
    registerPanel(propertiesPanel_, Qt::RightDockWidgetArea);
    registerPanel(adjustmentsPanel_, Qt::RightDockWidgetArea);
    registerPanel(channelsPanel_, Qt::RightDockWidgetArea);
    registerPanel(pathsPanel_, Qt::RightDockWidgetArea);
    registerPanel(actionsPanel_, Qt::RightDockWidgetArea);
    registerPanel(brushPanel_, Qt::RightDockWidgetArea);
    registerPanel(cloneSourcePanel_, Qt::RightDockWidgetArea);
    registerPanel(characterPanel_, Qt::RightDockWidgetArea);
    registerPanel(paragraphPanel_, Qt::RightDockWidgetArea);
    registerPanel(glyphsPanel_, Qt::RightDockWidgetArea);
    registerPanel(paragraphStylesPanel_, Qt::RightDockWidgetArea);
    registerPanel(characterStylesPanel_, Qt::RightDockWidgetArea);
    registerPanel(ShpSupport::install(this, colorState_)->dock(), Qt::BottomDockWidgetArea);

    // CS6 Essentials groups and order. The tab text is the panel title and the
    // tab icon reuses the panel's `window.panels.<name>` asset.
    auto addPanel = [](PanelGroup* group, QWidget* panel, const QString& title,
                       const QString& iconId) {
        group->addPanel(panel, title, pictura::icon(iconId));
    };
    // A fresh session is regrouped into the Essentials layout right after
    // (applyFreshSessionDefault), so its groups are staged rather than laid out
    // in the column only to be taken apart again.
    const bool freshLayout = !sessionHasPanelLayout(state);
    QList<PanelGroup*> staged;
    auto attachGroup = [this, freshLayout, &staged](PanelGroup* group) {
        if (freshLayout) {
            staged.append(group);
        } else {
            panelColumn_->addGroup(group);
        }
    };

    auto* colorGroup = new PanelGroup(this);
    addPanel(colorGroup, colorPanel_, tr("Color"), QString::fromLatin1(command_ids::WindowPanelsColor));
    addPanel(colorGroup, swatchesPanel_, tr("Swatches"),
             QString::fromLatin1(command_ids::WindowPanelsSwatches));
    addPanel(colorGroup, stylesPanel_, tr("Styles"), QStringLiteral("window.panels.styles"));
    attachGroup(colorGroup);

    // Properties folds into the Adjustments group; its tab starts hidden.
    auto* adjustmentsGroup = new PanelGroup(this);
    addPanel(adjustmentsGroup, adjustmentsPanel_, tr("Adjustments"),
             QString::fromLatin1(command_ids::WindowPanelsAdjustments));
    addPanel(adjustmentsGroup, propertiesPanel_, tr("Properties"),
             QString::fromLatin1(command_ids::WindowPanelsProperties));
    attachGroup(adjustmentsGroup);

    auto* layersGroup = new PanelGroup(this);
    addPanel(layersGroup, layersPanel_, tr("Layers"),
             QString::fromLatin1(command_ids::WindowPanelsLayers));
    addPanel(layersGroup, channelsPanel_, tr("Channels"),
             QString::fromLatin1(command_ids::WindowPanelsChannels));
    addPanel(layersGroup, pathsPanel_, tr("Paths"),
             QString::fromLatin1(command_ids::WindowPanelsPaths));
    attachGroup(layersGroup);

    auto* navigatorGroup = new PanelGroup(this);
    addPanel(navigatorGroup, navigatorPanel_, tr("Navigator"),
             QString::fromLatin1(command_ids::WindowPanelsNavigator));
    addPanel(navigatorGroup, histogramPanel_, tr("Histogram"),
             QString::fromLatin1(command_ids::WindowPanelsHistogram));
    addPanel(navigatorGroup, infoPanel_, tr("Info"),
             QString::fromLatin1(command_ids::WindowPanelsInfo));
    attachGroup(navigatorGroup);

    // Iconic single-panel groups.
    auto* historyGroup = new PanelGroup(this);
    addPanel(historyGroup, historyPanel_, tr("History"),
             QString::fromLatin1(command_ids::WindowPanelsHistory));
    attachGroup(historyGroup);

    auto* actionsGroup = new PanelGroup(this);
    addPanel(actionsGroup, actionsPanel_, tr("Actions"),
             QString::fromLatin1(command_ids::WindowPanelsActions));
    actionsGroup->setPanelMenuHook(QStringLiteral("actionsPanel"),
                                   [this](QMenu* menu) { actionsPanel_->updatePanelMenu(menu); });
    attachGroup(actionsGroup);
    actionsPanel_->applySessionState(state);

    // Kept registered and Window-menu reachable, but out of the default groups.
    auto* overflowGroup = new PanelGroup(this);
    addPanel(overflowGroup, gradientsPanel_, tr("Gradients"),
             QString::fromLatin1(command_ids::WindowPanelsGradients));
    addPanel(overflowGroup, patternsPanel_, tr("Patterns"),
             QString::fromLatin1(command_ids::WindowPanelsPatterns));
    addPanel(overflowGroup, notesPanel_, tr("Notes"),
             QString::fromLatin1(command_ids::WindowPanelsNotes));
    // The painting panels open from Window > Panels or an options-bar toggle.
    addPanel(overflowGroup, brushPanel_, tr("Brush"),
             QString::fromLatin1(command_ids::WindowPanelsBrush));
    addPanel(overflowGroup, cloneSourcePanel_, tr("Clone Source"),
             QString::fromLatin1(command_ids::WindowPanelsCloneSource));
    attachGroup(overflowGroup);

    // CS6 docks Character and Paragraph as one group, opened from the Window
    // or Type menu or the Type options bar's panel toggle; Glyphs (post-CS6)
    // joins them.
    auto* typeGroup = new PanelGroup(this);
    addPanel(typeGroup, characterPanel_, tr("Character"),
             QString::fromLatin1(command_ids::WindowPanelsCharacter));
    typeGroup->setPanelMenuHook(QStringLiteral("characterPanel"),
                                [this](QMenu* menu) { characterPanel_->updatePanelMenu(menu); });
    addPanel(typeGroup, paragraphPanel_, tr("Paragraph"),
             QString::fromLatin1(command_ids::WindowPanelsParagraph));
    typeGroup->setPanelMenuHook(QStringLiteral("paragraphPanel"),
                                [this](QMenu* menu) { paragraphPanel_->updatePanelMenu(menu); });
    addPanel(typeGroup, glyphsPanel_, tr("Glyphs"),
             QString::fromLatin1(command_ids::WindowPanelsGlyphs));
    addPanel(typeGroup, paragraphStylesPanel_, tr("Paragraph Styles"),
             QString::fromLatin1(command_ids::WindowPanelsParagraph));
    addPanel(typeGroup, characterStylesPanel_, tr("Character Styles"),
             QString::fromLatin1(command_ids::WindowPanelsCharacterStyles));
    attachGroup(typeGroup);

    // Default visibility matches the pre-M41 layout: Color/Swatches/Styles,
    // Adjustments, Layers/Channels/Paths and Navigator/Histogram/Info visible;
    // History, Actions and the overflow panels hidden.
    panelColumn_->showPanel(QStringLiteral("historyPanel"), false);
    panelColumn_->showPanel(QStringLiteral("actionsPanel"), false);
    panelColumn_->showPanel(QStringLiteral("propertiesPanel"), false);
    panelColumn_->showPanel(QStringLiteral("gradientsPanel"), false);
    panelColumn_->showPanel(QStringLiteral("patternsPanel"), false);
    panelColumn_->showPanel(QStringLiteral("notesPanel"), false);
    panelColumn_->showPanel(QStringLiteral("brushPanel"), false);
    panelColumn_->showPanel(QStringLiteral("cloneSourcePanel"), false);
    panelColumn_->showPanel(QStringLiteral("glyphsPanel"), false);
    panelColumn_->showPanel(QStringLiteral("characterPanel"), false);
    panelColumn_->showPanel(QStringLiteral("paragraphPanel"), false);
    panelColumn_->showPanel(QStringLiteral("paragraphStylesPanel"), false);
    panelColumn_->showPanel(QStringLiteral("characterStylesPanel"), false);

    // The Layers row menu's Edit / Replace / Export Contents actions run through
    // the frame, so this is wired here where the panel exists (registerHandlers
    // runs before buildPanels).
    wireSmartObjectRowMenu();
    wireMaskOverlay();

    // Switching between a layer's pixels and its mask swaps in that target's
    // own FG/BG pair, so each remembers its colours.
    connect(layersPanel_, &LayersPanel::editTargetChanged, this, [this](bool mask) {
        if (!colorState_) {
            return;
        }
        colorState_->setMaskActive(mask);
        if (tools_) {
            tools_->setMaskActive(mask);
        }
        const QColor foreground = colorState_->foreground();
        const QColor background = colorState_->background();
        colorState_->setForeground(otherForeground_);
        colorState_->setBackground(otherBackground_);
        otherForeground_ = foreground;
        otherBackground_ = background;
    });
    connect(layersPanel_, &LayersPanel::selectionChanged, this, [this] {
        if (propertiesPanel_) {
            propertiesPanel_->refresh();
        }
        updateAlignControls();
        if (registry_) {
            if (refreshingDock_) {
                registryRefreshPending_ = true;
            } else if (!panelsRefreshedByChanged_
                       || !layersPanel_->refreshInProgress()) {
                // A user-driven selection change must update labels and
                // enablement immediately, even right after an edit. A
                // refresh-induced emission after a notifying edit reuses the
                // edit's synchronous registry recomputation.
                registry_->refresh();
            }
        }
        // The Move tool's Show Transform Controls box follows the active layer.
        if (tools_) {
            tools_->syncTransformControls();
        }
        // The type panels follow the active layer; selection does not bump the
        // view's `changed`, so refresh them here rather than from the timer.
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
    });
    return staged;
}

void PicturaMainWindow::togglePanel(const QString& objectName)
{
    PanelColumn* owner = columnForPanel(objectName);
    if (!owner) {
        owner = panelColumn_;
    }
    if (owner) {
        owner->showPanel(objectName, !owner->isPanelVisible(objectName));
    }
}

void PicturaMainWindow::buildTools(int toolsColumns, bool useShiftKeyForToolSwitch,
                                   const QJsonObject& toolOptions)
{
    tools_ = new ToolController(this);
    applyToolOptionsJson(*tools_, toolOptions);
    tools_->setViewProvider([this]() { return activeView(); });
    // Every picker opened without its own sampler (the toolbox and Color panel
    // swatches, options bars, filter dialogs) samples the active document's
    // composite through this pair, as CS6's picker eyedropper does. The window
    // is held weakly, so a destroyed window leaves the functions inert.
    {
        QPointer<PicturaMainWindow> self(this);
        ColorPickerDialog::setDefaultCanvasSampler(
            [self]() -> ToolController* { return self ? self->tools_ : nullptr; },
            [self](const QPoint& pos) -> QColor {
                PictureView* view = self ? self->activeView() : nullptr;
                if (!view) {
                    return {};
                }
                const quint32 argb = view->sample_argb(pos.x(), pos.y());
                return argb != 0 ? QColor::fromRgb(argb) : QColor();
            });
    }
    connect(tools_, &ToolController::commandRequested, this, [this](const QString& id) {
        if (registry_) {
            registry_->dispatch(id);
        }
    });
    pathsPanel_->setToolServices(tools_);

    // A tool commit (marquee release, wand click, lasso close) reaches the
    // picture view without a `changed` emission, so refresh the overlay directly.
    connect(tools_, &ToolController::selectionCommitted, this,
            &PicturaMainWindow::refreshSelectionOverlay);
    // A locked pixel edit is refused silently at the bridge; report it so the
    // user learns why nothing was painted.
    connect(tools_, &ToolController::pixelEditRefused, this, [this](const QString& message) {
        statusBar()->showMessage(message, 4000);
    });

    if (colorState_) {
        tools_->setForeground(colorState_->foreground());
        tools_->setBackground(colorState_->background());
        connect(colorState_, &ColorState::foregroundChanged, tools_,
                &ToolController::setForeground);
        connect(colorState_, &ColorState::backgroundChanged, tools_,
                &ToolController::setBackground);
        // The tool hint carries the foreground colour, so it follows it.
        connect(colorState_, &ColorState::foregroundChanged, this,
                [this](const QColor&) { updateToolHint(); });
        // A tool or preset that changes a colour (e.g. a brush preset) reaches
        // the controller, not ColorState; mirror it back so the toolbox swatch
        // and the Color panel follow.
        connect(tools_, &ToolController::colorsChanged, this, [this]() {
            if (!colorState_) {
                return;
            }
            if (colorState_->foreground() != tools_->foreground()) {
                colorState_->setForeground(tools_->foreground());
            }
            if (colorState_->background() != tools_->background()) {
                colorState_->setBackground(tools_->background());
            }
        });
    }

    auto* toolbox = new Toolbox(tools_, colorState_, this);
    toolbox_ = toolbox;
    toolbox->setShiftKeyForToolSwitch(useShiftKeyForToolSwitch);

    // D1: the Tools panel is a tabless, atomic `PanelColumn` hosting the tool
    // content as one plain child in the central splitter, default left at index
    // 0. Its header toggle reflows the tool grid one<->two columns (D2), not the
    // normal/iconic rail mode.
    toolsColumn_ = new PanelColumn(this);
    toolsColumn_->setObjectName(QStringLiteral("toolsColumn"));
    wirePanelColumn(toolsColumn_);
    centerSplitter_->insertWidget(0, toolsColumn_);
    toolsColumn_->setToolsContent(
        toolbox, [toolbox]() { return toolbox->columns(); },
        [this]() {
            if (toolbox_) {
                toolbox_->setColumns(toolbox_->columns() == 1 ? 2 : 1);
            }
        });
    // The screen-mode button opens a menu of the same three commands `F` and
    // `Shift+F` cycle, sharing the registry actions so checks and handlers agree.
    toolbox->setScreenModeActions({
        registry_->action(QString::fromLatin1(command_ids::ViewScreenModeStandard)),
        registry_->action(QString::fromLatin1(command_ids::ViewScreenModeFullWithMenuBar)),
        registry_->action(QString::fromLatin1(command_ids::ViewScreenModeFull)),
    });
    toolbox->setActiveScreenMode(static_cast<int>(screenMode_));
    // CommandRegistry::refresh() rewrites every action's text from its spec,
    // which would drop the toolbox's display-only `F` hint. Pin the hinted text
    // with label providers so the shared action keeps it in both menus.
    for (const char* id : {command_ids::ViewScreenModeStandard,
                           command_ids::ViewScreenModeFullWithMenuBar,
                           command_ids::ViewScreenModeFull}) {
        const QString commandId = QString::fromLatin1(id);
        if (QAction* action = registry_->action(commandId)) {
            const QString label = action->text();
            registry_->setLabelProvider(commandId, [label]() { return label; });
        }
    }
    connect(toolbox, &Toolbox::columnsChanged, this, [this](int) {
        if (toolsColumn_) {
            toolsColumn_->refreshToolsWidth();
        }
        saveSession();
    });
    toolbox->setColumns(toolsColumns);
    toolsColumn_->refreshToolsWidth();
    reapplyColumnStretch();
    toolsColumn_->setVisible(!panelsHidden_);

    rebuildToolShortcuts();

    auto brushActive = [this]() {
        return tools_ && isBrushTool(tools_->activeTool());
    };
    // With the Magnetic Lasso, `[` / `]` step the detection width instead.
    auto magneticActive = [this]() {
        return tools_ && tools_->activeTool() == ToolId::MagneticLasso;
    };
    connect(new QShortcut(QKeySequence(Qt::Key_BracketLeft), this), &QShortcut::activated, this,
            [this, brushActive, magneticActive]() {
                if (brushActive()) {
                    tools_->adjustBrushSize(-1);
                } else if (magneticActive()) {
                    if (SelectionOptions* selection =
                            tools_->optionsFor<SelectionOptions>(ToolId::MagneticLasso)) {
                        selection->setMagneticWidth(selection->magneticWidth() - 1);
                    }
                }
            });
    connect(new QShortcut(QKeySequence(Qt::Key_BracketRight), this), &QShortcut::activated, this,
            [this, brushActive, magneticActive]() {
                if (brushActive()) {
                    tools_->adjustBrushSize(+1);
                } else if (magneticActive()) {
                    if (SelectionOptions* selection =
                            tools_->optionsFor<SelectionOptions>(ToolId::MagneticLasso)) {
                        selection->setMagneticWidth(selection->magneticWidth() + 1);
                    }
                }
            });
    connect(new QShortcut(QKeySequence(Qt::SHIFT | Qt::Key_BracketLeft), this),
            &QShortcut::activated, this, [this, brushActive]() {
                if (brushActive()) {
                    tools_->adjustBrushHardness(-5);
                }
            });
    connect(new QShortcut(QKeySequence(Qt::SHIFT | Qt::Key_BracketRight), this),
            &QShortcut::activated, this, [this, brushActive]() {
                if (brushActive()) {
                    tools_->adjustBrushHardness(+5);
                }
            });

    // CS6 foreground/background swap. `X` is unassigned in the tool letter
    // catalogue, so it does not collide with the tool-switch shortcuts.
    connect(new QShortcut(QKeySequence(Qt::Key_X), this), &QShortcut::activated, this,
            [this]() {
                if (toolbox_) {
                    toolbox_->swapForegroundBackground();
                }
            });

    // M43: `D` resets fg/bg to the active target's CS6 defaults (black/white
    // for layer pixels, white/black for a mask). `D` is also unassigned in the
    // tool letter catalogue (the engine tool catalogue, `pictura_script::tool_catalogue`).
    connect(new QShortcut(QKeySequence(Qt::Key_D), this), &QShortcut::activated, this,
            [this]() {
                if (toolbox_) {
                    toolbox_->resetForegroundBackground();
                }
            });

    // CS6's Delete / Backspace act from anywhere in the workspace: a pixel
    // selection clears, otherwise the selected layer(s) delete. Auto-repeat is
    // off so holding the key cannot delete layer after layer; the type
    // session's shortcut override, an open dialog, and a tool that consumes
    // the key all keep it.
    QShortcut* deleteShortcuts[2] = {nullptr, nullptr};
    int deleteShortcutCount = 0;
    for (const Qt::Key key : {Qt::Key_Delete, Qt::Key_Backspace}) {
        auto* shortcut = new QShortcut(QKeySequence(key), this);
        shortcut->setAutoRepeat(false);
        shortcut->setEnabled(!isTextInput(QApplication::focusWidget()));
        connect(shortcut, &QShortcut::activated, this, [this]() { handleDeleteKey(); });
        deleteShortcuts[deleteShortcutCount++] = shortcut;
    }
    // A focused editable field owns Delete / Backspace: while one has focus the
    // shortcuts are off, so the key reaches the field instead of being
    // swallowed by the shortcut map.
    deleteShortcutsFocusConn_ = connect(qApp, &QApplication::focusChanged, this,
            [deleteShortcuts](QWidget*, QWidget* now) {
                const bool field = isTextInput(now);
                for (QShortcut* shortcut : deleteShortcuts) {
                    if (shortcut) {
                        shortcut->setEnabled(!field);
                    }
                }
            });

    // A Space pan is a key-held override; if the key-up is missed because the
    // canvas loses focus or the window deactivates, it would otherwise stick and
    // route every later press to pan. End it and restore the tool on that edge.
    spacePanFocusConn_ = connect(qApp, &QApplication::focusChanged, this,
                                 [this](QWidget* now, QWidget* old) {
        ImageView* canvas = imageView();
        if (!canvas || !canvas->spacePanForTest()) {
            return;
        }
        const bool wasCanvas = old && (old == canvas || canvas->isAncestorOf(old));
        const bool isCanvas = now && (now == canvas || canvas->isAncestorOf(now));
        if (now == nullptr || (wasCanvas && !isCanvas)) {
            canvas->setSpacePan(false);
            canvas->setPanEnabled(tools_ && tools_->activeTool() == ToolId::Hand);
            if (tools_) {
                tools_->refreshCursor();
            }
        }
    });

    registerPanel(toolbox, Qt::LeftDockWidgetArea);

    optionsBar_ = new OptionsBar(tools_, this);
    optionsBar_->setObjectName(QStringLiteral("optionsBar"));
    addToolBar(optionsBar_);
    // The options bar spans the full window width, but its content sits inside
    // the central band's 3 px frame; inset it so no control touches the line.
    // The larger left inset leaves breathing room to the right of the toolbar's
    // drag grip.
    optionsBar_->setContentsMargins(9, 0, 3, 0);
    connect(optionsBar_, &OptionsBar::panelToggleRequested, this, &PicturaMainWindow::togglePanel);
    connect(optionsBar_, &OptionsBar::alignRequested, this,
            [this](int edge) {
                const MoveGroup* move = tools_ ? tools_->optionsFor<MoveGroup>(ToolId::Move) : nullptr;
                alignSelectedLayers(edge, true, move ? move->get().alignTo : 0);
            });
    connect(optionsBar_, &OptionsBar::distributeRequested, this,
            &PicturaMainWindow::distributeSelectedLayers);
    connect(tools_, &ToolController::moveOptionsChanged, this,
            &PicturaMainWindow::updateAlignControls);
    connect(optionsBar_, &OptionsBar::refineEdgeRequested, this, [this]() {
        registry_->dispatch(
            commandIdForPath({QStringLiteral("Select"), QStringLiteral("Refine Edge…")}));
    });
    // While new type is being set the Character panel edits that session.
    characterPanel_->setTextSession(
        [this]() { return tools_ ? tools_->textSession() : nullptr; });
    connect(tools_, &ToolController::textEditingChanged, characterPanel_,
            &CharacterPanel::refresh);
    connect(optionsBar_, &OptionsBar::warpTextRequested, this, [this]() {
        registry_->dispatch(
            commandIdForPath({QStringLiteral("Type"), QStringLiteral("Warp Text…")}));
    });
    brushPanel_->setController(tools_);
    cloneSourcePanel_->setController(tools_);
    glyphsPanel_->setController(tools_);

    // The Info panel follows the Ruler tool: its A/L block and W/H readout come
    // from the ruler, and a new measuring line re-reads them.
    connect(tools_, &ToolController::rulerChanged, this, [this]() {
        if (infoPanel_) {
            infoPanel_->refresh();
        }
    });

    connect(tools_, &ToolController::activeToolChanged, this, [this](ToolId id) {
        if (optionsBar_) {
            optionsBar_->showTool(id);
        }
        if (infoPanel_) {
            infoPanel_->setRulerMode(id == ToolId::Ruler);
        }
        updateToolHint();
        syncBrushTipPreview();
    });
    connect(tools_, &ToolController::brushTipChanged, this,
            [this]() { syncBrushTipPreview(); });
    // A modal tool session (Crop) recorded or discarded a step; re-evaluate the
    // Undo/Redo commands so Ctrl+Z reaches the session.
    connect(tools_, &ToolController::toolSessionChanged, this, [this]() {
        if (registry_) {
            registry_->refresh();
        }
    });
    // Crop options (mode, grid, ratio, delete pixels) persist across restarts.
    connect(tools_, &ToolController::cropOptionsChanged, this, [this]() { saveSession(); });
    // Every tool's options persist too: saved on close and quit, and on these
    // changes so a crash keeps the last tool switch or bar edit.
    for (auto signal : {&ToolController::moveOptionsChanged, &ToolController::typeOptionsChanged,
                        &ToolController::shapeOptionsChanged}) {
        connect(tools_, signal, this, [this]() { saveSession(); });
    }
    connect(tools_, &ToolController::activeToolChanged, this, [this]() { saveSession(); });
    // The Note tool opens a note in the Notes panel; the panel's
    // previous/next/delete hand the current note back to the controller.
    connect(tools_, &ToolController::layerCreated, this, &PicturaMainWindow::selectLayerPath);
    // The Move tool's Auto-Select hit asks for an existing layer's row.
    connect(tools_, &ToolController::layerSelectionRequested, this,
            &PicturaMainWindow::selectLayerPath);
    connect(tools_, &ToolController::noteActivated, this, [this](int index) {
        if (!notesPanel_) {
            return;
        }
        notesPanel_->showNote(index);
        if (index >= 0 && panelColumn_) {
            panelColumn_->showPanel(QStringLiteral("notesPanel"), true);
        }
    });
    if (notesPanel_) {
        connect(notesPanel_, &NotesPanel::noteRequested, tools_, &ToolController::setCurrentNote);
    }
    connect(tools_, &ToolController::foregroundSampled, this, [this](const QColor& color) {
        if (colorState_) {
            colorState_->setForeground(color);
        }
    });

    // Canvas context-menu requests: the shared widget menus the controller
    // cannot reach (the toolbox Zoom preset menu, the options-bar Brush Preset
    // picker), isolation mode, and activation of the clicked document so its
    // menu acts on the right document.
    connect(tools_, &ToolController::zoomMenuRequested, this, [this](const QPoint& pos) {
        if (toolbox_ && toolbox_->zoomMenu()) {
            toolbox_->zoomMenu()->popup(pos);
        }
    });
    connect(tools_, &ToolController::toolZoomed, this, &PicturaMainWindow::onToolZoomed);
    connect(tools_, &ToolController::brushPickerRequested, this, [this](const QPoint& pos) {
        if (optionsBar_) {
            optionsBar_->brushPicker()->popUpAt(pos);
        }
    });
    connect(tools_, &ToolController::isolationModeRequested, this, [this]() {
        if (layersPanel_) {
            layersPanel_->enterIsolation();
        }
    });
    tools_->setCanvasActivationHandler(
        [this](ImageView* canvas) { activateDocWindow(windowOfCanvas(canvas)); });

    if (optionsBar_) {
        optionsBar_->showTool(tools_->activeTool());
    }
    if (infoPanel_) {
        infoPanel_->setRulerMode(tools_->activeTool() == ToolId::Ruler);
    }
}

void PicturaMainWindow::buildDocumentTabs()
{
    // The document tab strip is flat chrome (no base line above the tabs) and
    // carries the shared tab font on the bar itself, so the label weight
    // does not depend on the QSS subcontrol rule and QTabBar's elision metrics
    // match the painted label.
    tabs_->tabBar()->setDrawBase(false);
    applyTabBarFont(tabs_->tabBar());
}

void PicturaMainWindow::buildStatusBar()
{
    QStatusBar* bar = statusBar();
    bar->setSizeGripEnabled(false);
    bar->setContentsMargins(6, 0, 0, 0);
    bar->setFixedHeight(Theme::scaled(28));

    // Footer text runs 2px under the app default, matching the tab chrome. A
    // point-sized default font (pixelSize() < 0) falls back to 12px at 96 DPI.
    int baseFontPx = QApplication::font().pixelSize();
    if (baseFontPx <= 0) {
        baseFontPx = 12;
    }
    QFont footerFont = bar->font();
    footerFont.setPixelSize(baseFontPx - 2);
    bar->setFont(footerFont);

    NumericFieldConfig zoomConfig;
    zoomConfig.minimum = 1;
    zoomConfig.maximum = 3200;
    zoomConfig.decimals = 0;
    zoomConfig.suffix = QStringLiteral("%");
    zoomConfig.namePrefix = QStringLiteral("statusZoom");
    zoomConfig.objectName = QStringLiteral("statusZoomField");
    zoomField_ = new NumericField(QString(), zoomConfig, bar);
    // The value hugs its content; leftover status-bar width belongs to the
    // empty message area, keeping the readouts left-aligned.
    zoomField_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    sizeLabel_ = new QLabel(QStringLiteral("—"), bar);
    sizeLabel_->setObjectName(QStringLiteral("statusSizeLabel"));

    // The options button sits with the resolution readout it controls, between
    // Resolution and GPU. Built in visual order so the widget tree matches.
    statusOptionsButton_ = new QToolButton(bar);
    statusOptionsButton_->setObjectName(QStringLiteral("statusOptionsButton"));
    statusOptionsButton_->setArrowType(Qt::DownArrow);
    statusOptionsButton_->setPopupMode(QToolButton::InstantPopup);
    auto* optionsMenu = new QMenu(statusOptionsButton_);
    auto* group = new QActionGroup(optionsMenu);
    for (const QString& key : {QStringLiteral("sizes"), QStringLiteral("dimensions")}) {
        QAction* option = optionsMenu->addAction(
            key == QStringLiteral("sizes") ? QStringLiteral("Document Sizes")
                                           : QStringLiteral("Document Dimensions"));
        option->setCheckable(true);
        option->setData(key);
        option->setChecked(key == QStringLiteral("sizes"));
        group->addAction(option);
        connect(option, &QAction::triggered, this, [this, key]() {
            statusReadout_ = key;
            updateStatus();
        });
    }
    statusOptionsButton_->setMenu(optionsMenu);

    backendLabel_ = new QLabel(QStringLiteral("—"), bar);
    backendLabel_->setObjectName(QStringLiteral("statusBackendLabel"));
    actionLabel_ = new QLabel(bar);
    actionLabel_->setObjectName(QStringLiteral("statusActionLabel"));
    actionLabel_->hide();
    hintBar_ = new ToolHintBar(bar);
    // A ~28px status strip: cap the readouts and hint so the bar trims 4px
    // without clipping. (The theme already dropped the bar's top border.)
    const int kStatusItemHeight = Theme::scaled(22);
    zoomField_->setFixedHeight(kStatusItemHeight);
    sizeLabel_->setFixedHeight(kStatusItemHeight);
    statusOptionsButton_->setFixedHeight(kStatusItemHeight);
    backendLabel_->setFixedHeight(kStatusItemHeight);
    actionLabel_->setFixedHeight(kStatusItemHeight);
    hintBar_->setFixedHeight(kStatusItemHeight);

    // A 1px themable rule between the footer groups. Mouse-transparent so it
    // never intercepts the status bar's own clicks.
    const auto makeSeparator = [bar]() {
        auto* separator = new QFrame(bar);
        separator->setObjectName(QStringLiteral("statusSeparator"));
        separator->setFrameShape(QFrame::VLine);
        separator->setFrameShadow(QFrame::Sunken);
        separator->setFixedWidth(1);
        separator->setFixedHeight(Theme::scaled(16));
        separator->setAttribute(Qt::WA_TransparentForMouseEvents);
        return separator;
    };

    // Left to right: Zoom | Resolution [options] | GPU | Hints.
    bar->addWidget(zoomField_);
    bar->addWidget(makeSeparator());
    bar->addWidget(sizeLabel_);
    bar->addWidget(statusOptionsButton_);
    bar->addWidget(makeSeparator());
    bar->addWidget(backendLabel_);
    // The action readout ("Skew in progress") travels with its rule.
    actionSeparator_ = makeSeparator();
    bar->addWidget(actionSeparator_);
    bar->addWidget(actionLabel_);
    // The rule before the hint strip travels with the strip's visibility.
    hintSeparator_ = makeSeparator();
    bar->addWidget(hintSeparator_);
    bar->addWidget(hintBar_);
    // Right-aligned: the Help > Search keycap, following its live binding.
    buildSearchHint(bar);
    searchHint_->setFixedHeight(kStatusItemHeight);

    connect(zoomField_, &NumericField::valueCommitted, this, [this](double percent) {
        if (ImageView* canvas = imageView()) {
            canvas->setZoom(percent / 100.0,
                            QPointF(canvas->width() / 2.0, canvas->height() / 2.0));
        }
    });
    if (tools_) {
        connect(tools_, &ToolController::transformChanged, this, [this]() { updateStatus(); });
        installTransformLock();
    }
    updateToolHint();
}

void PicturaMainWindow::updateToolHint()
{
    if (!hintBar_) {
        return;
    }
    QString fallback = tools_ ? QString::fromLatin1(toolInfo(tools_->activeTool()).hint)
                              : QStringLiteral("Ready");
    const QColor foreground = foregroundColor();
    if (foreground.isValid()) {
        fallback += QStringLiteral("  ·  Foreground %1").arg(foreground.name());
    }
    QList<ToolHint> hints;
    if (tools_) {
        hints = toolHintEntries(tools_->activeTool());
        for (ToolHint& hint : hints) {
            if (!hint.commandId || !registry_) {
                continue;
            }
            if (QAction* action = registry_->action(QString::fromLatin1(hint.commandId))) {
                const QString key = action->shortcut().toString(QKeySequence::NativeText);
                if (!key.isEmpty()) {
                    hint.key = key;
                }
            }
        }
    }
    hintBar_->setHints(hints, fallback);
    if (infoPanel_) {
        QStringList hintTexts;
        for (const ToolHint& hint : hints) {
            hintTexts << QStringLiteral("%1 %2").arg(hint.key, hint.text);
        }
        const QString name = tools_ ? QString::fromLatin1(toolInfo(tools_->activeTool()).name)
                                    : QString();
        infoPanel_->setToolInfo(name, hintTexts);
    }
}

void PicturaMainWindow::installWorkspaceOpenGesture()
{
    // The blank pane is the tab widget's internal stacked widget, so it, the
    // tab widget, and the document area holding them are filtered.
    documentArea_->installEventFilter(this);
    tabs_->installEventFilter(this);
    if (auto* workspace = tabs_->findChild<QStackedWidget*>()) {
        workspace->installEventFilter(this);
    }
}

bool PicturaMainWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (tabs_ && watched == tabs_->tabBar() && tearOutFilter(event)) {
        return true;
    }
    if (tabs_ && (watched == tabs_->tabBar() || watched == tabs_)
        && windowDropFilter(watched, event)) {
        return true;
    }
    // A press on a canvas activates its window, before the tool sees it.
    if (event->type() == QEvent::MouseButtonPress) {
        if (DocWindow* window = windowOfCanvas(qobject_cast<ImageView*>(watched));
            window && window != activeWindow_) {
            activateDocWindow(window);
        }
    }
    // The empty workspace (no documents open) accepts a left double-click as
    // "Open": the tab widget and its blank stacked page are the only widgets
    // there.
    if (event->type() == QEvent::MouseButtonDblClick && workspaceOpenArmed()) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton) {
            showOpenDialog();
            return true;
        }
    }
    // Middle-click on a loading tab cancels it; on a ready tab it closes it.
    if (tabs_ && watched == tabs_->tabBar() && event->type() == QEvent::MouseButtonRelease) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::MiddleButton) {
            DocWindow* window =
                windowAtTab(tabs_->tabBar()->tabAt(mouse->position().toPoint()));
            if (window) {
                const bool loading = window->doc->loading;
                const QPointer<PictureView> guard(window->doc->view);
                QTimer::singleShot(0, this, [this, window, guard, loading] {
                    if (!guard) {
                        return;
                    }
                    if (loading) {
                        cancelOpenForView(guard.data());
                    } else if (windows_.contains(window) && window->doc->view == guard) {
                        closeWindow(window, true);
                    }
                });
                return true;
            }
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

} // namespace pictura
