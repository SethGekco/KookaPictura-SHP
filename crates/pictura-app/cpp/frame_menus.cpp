#include "frame_includes.h"

#include "fill_layer_editor.h"
#include "gpu_probe.h"

#include "pictura_app/src/cxxqt_object/impl_layers/fill_content.cxxqt.h"
#include "pictura_app/src/cxxqt_object/layer_target_bridge.cxxqt.h"
#include "api/type_api.h"

#include "design_system_dialog.h"

#include "api/layers_api.h"

#include "api/document_api.h"
#include "pictura_app/src/cxxqt_object/impl_layers/layers_surface.cxxqt.h"
#include "pictura_app/src/cxxqt_object/layers_smart_filters.cxxqt.h"
#include "pictura_app/src/cxxqt_object/layer_arrange.cxxqt.h"

namespace pictura {

const QList<PanelToggle>& panelToggles()
{
    static const QList<PanelToggle> toggles = {
        {command_ids::WindowPanelsLayers, "layersPanel"},
        {command_ids::WindowPanelsNavigator, "navigatorPanel"},
        {command_ids::WindowPanelsHistory, "historyPanel"},
        {command_ids::WindowPanelsColor, "colorPanel"},
        {command_ids::WindowPanelsSwatches, "swatchesPanel"},
        {command_ids::WindowPanelsInfo, "infoPanel"},
        {command_ids::WindowPanelsHistogram, "histogramPanel"},
        {command_ids::WindowPanelsGradients, "gradientsPanel"},
        {command_ids::WindowPanelsPatterns, "patternsPanel"},
        {command_ids::WindowPanelsNotes, "notesPanel"},
        {command_ids::WindowPanelsProperties, "propertiesPanel"},
        {command_ids::WindowPanelsAdjustments, "adjustmentsPanel"},
        {command_ids::WindowPanelsChannels, "channelsPanel"},
        {command_ids::WindowPanelsPaths, "pathsPanel"},
        {command_ids::WindowPanelsActions, "actionsPanel"},
        {command_ids::WindowPanelsBrush, "brushPanel"},
        {command_ids::WindowPanelsCloneSource, "cloneSourcePanel"},
        {command_ids::WindowPanelsCharacter, "characterPanel"},
        {command_ids::WindowPanelsParagraph, "paragraphPanel"},
        {command_ids::WindowPanelsGlyphs, "glyphsPanel"},
        {command_ids::WindowPanelsParagraphStyles, "paragraphStylesPanel"},
        {command_ids::WindowPanelsCharacterStyles, "characterStylesPanel"},
        {command_ids::TypePanelsCharacter, "characterPanel"},
        {command_ids::TypePanelsParagraph, "paragraphPanel"},
        {command_ids::TypePanelsParagraphStyles, "paragraphStylesPanel"},
        {command_ids::TypePanelsCharacterStyles, "characterStylesPanel"},
    };
    return toggles;
}
void PicturaMainWindow::buildMenus()
{
    registry_->buildMenuBar(menuBar());
    buildUiSizeMenu();
    // The History panel's iconic flyout is a separate top-level Qt::Popup.
    if (QAction* undo = registry_->action(QLatin1String(command_ids::EditUndo))) {
        undo->setShortcutContext(Qt::ApplicationShortcut);
    }
    installMenuCornerButtons();
    if (QMenu* recent = recentMenu()) {
        connect(recent, &QMenu::aboutToShow, this, [this, recent]() { refreshRecentMenu(recent); });
    }
    // Icon for every implemented command; ids without an asset are skipped.
    static const char* const kIconCommands[] = {
        command_ids::FileNew,
        command_ids::FileOpen,
        command_ids::FileSave,
        command_ids::FileSaveAs,
        command_ids::FileRevert,
        command_ids::FileClose,
        command_ids::FileCloseAll,
        command_ids::FileExit,
        command_ids::EditUndo,
        command_ids::EditRedo,
        command_ids::EditStepForward,
        command_ids::EditStepBackward,
        command_ids::ImageRotate90Cw,
        command_ids::ImageRotate90Ccw,
        command_ids::ImageRotate180,
        command_ids::ImageFlipHorizontal,
        command_ids::ImageFlipVertical,
        command_ids::ImageCrop,
        command_ids::SelectAll,
        command_ids::SelectDeselect,
        command_ids::ViewZoomIn,
        command_ids::ViewZoomOut,
        command_ids::ViewFitOnScreen,
        command_ids::ViewActualPixels,
        command_ids::ViewScreenModeStandard,
        command_ids::ViewScreenModeFullWithMenuBar,
        command_ids::ViewScreenModeFull,
        command_ids::ViewOptions,
        command_ids::WindowPanelsLayers,
        command_ids::WindowPanelsTools,
        command_ids::HelpAbout,
    };
    for (const char* id : kIconCommands) {
        const QString commandId = QString::fromLatin1(id);
        if (QAction* action = registry_->action(commandId)) {
            action->setIcon(icon(commandId));
        }
    }
    wireWorkspaceCommands();
}
bool PicturaMainWindow::saveAsWithDialog()
{
    PictureView* view = activeView();
    if (!view || !view->has_document()) {
        return false;
    }
    const QStringList& filters = saveFileFilters();
    const int sourceIndex = defaultSaveFilterIndex(view);
    QString selected = filters.at(sourceIndex);
    QString path = getSaveFileName(this, tr("Save As"), activeFilePath(), filters, &selected);
    if (path.isEmpty()) {
        return false;
    }
    // The chosen filter owns the extension; All Formats keeps a typed suffix.
    const int chosen = filters.indexOf(selected);
    const int filterIndex = chosen >= 0 ? chosen : sourceIndex;
    path = pathForSaveFilter(path, filterIndex, sourceIndex);
    if (!isWritableSavePath(path)) {
        QMessageBox::warning(this, tr("Save As"),
                             tr("Kooka Pictura cannot save .%1 files. Choose one of the listed "
                                "formats.").arg(QFileInfo(path).suffix()));
        return false;
    }
    if (isPdfPath(path)) {
        const bool saved = savePdfWithDialog(path);
        if (saved && !saveActiveInProgress()) {
            registry_->recordCommand(
                command_ids::FileSaveAs,
                CommandParams{QJsonObject{
                    {QStringLiteral("as"), path.section(QLatin1Char('.'), -1).toUpper()},
                    {QStringLiteral("in"), path}}});
        }
        return saved;
    }
    return saveActiveAsWithOptions(path);
}

void PicturaMainWindow::registerHandlers()
{
    registry_->setHandler(command_ids::FileNew, [this]() { showNewDocumentDialog(); });
    registry_->setHandler(command_ids::FileOpen, [this]() { showOpenDialog(); });
    registry_->setHandler(command_ids::FileOpenAs, [this]() { showOpenAsDialog(); });
    registry_->setHandler(command_ids::FileOpenAsSmartObject, [this]() {
        const QString path = getOpenFileName(this, tr("Open As Smart Object"),
                                             documentDirectory(activeFilePath()),
                                             openFileFilters());
        if (!path.isEmpty()) {
            startOpenAsSmartObjectPath(path);
        }
    });
    registry_->setHandler(command_ids::FilePlace, [this]() {
        PictureView* view = activeView();
        if (!view) {
            return;
        }
        const QString path = getOpenFileName(this, tr("Place"),
                                             documentDirectory(activeFilePath()), openFileFilters());
        if (path.isEmpty()) {
            return;
        }
        const bool psd = PicturaMainWindow::isNativeDocumentPath(path);
        applyColorSettings(view);
        if (!psd && !confirmProfileForOpen(view, path)) {
            return;
        }
        const QString created =
            psd ? view->place_smart_object(path) : view->place_image(path);
        if (!created.isEmpty()) {
            beginFreeTransform(created);
            selectLayerPath(created);
        }
    });

    registry_->setHandler(command_ids::FileInfo, [this]() { showFileInfo(); });

    registry_->setHandler(command_ids::FileSave, [this]() { saveActive(); });
    registry_->setHandler(command_ids::FileSaveAs, [this]() { saveAsWithDialog(); });
    wirePdfCommands();
    registry_->setHandler(command_ids::FileExportAs,
                          [this]() { exportAsFromView(this, activeView()); });
    // File > Print… and Print One Copy (WF-013).
    for (const char* leaf : {"Print…", "Print One Copy"}) {
        const QString id = commandIdForPath({QStringLiteral("File"), QString::fromUtf8(leaf)});
        const bool once = QByteArray(leaf) == "Print One Copy";
        registry_->setImplemented(id, true);
        registry_->setHandler(id, [this, once]() {
            once ? printOneCopyFromView(this, activeView()) : printFromView(this, activeView());
        });
        registry_->setEnabledProvider(
            id, [this]() { return activeView() && activeView()->has_document(); });
    }
    registry_->setHandler(command_ids::FileSaveForWeb, [this]() {
        saveForWebFromView(this, activeView(), documentName(activeDocumentIndex()));
    });
    registry_->setHandler(command_ids::FileQuickExportPng,
                          [this]() { quickExportPngFromView(this, activeView()); });
    registry_->setHandler(command_ids::FileRevert, [this]() {
        const int index = activeDocumentIndex();
        PictureView* view = activeView();
        if (index < 0 || !view || document_api::filePath(*view).isEmpty()) {
            return;
        }
        if (isDocumentDirty(index)) {
            switch (askUnsaved(this, documentName(index))) {
            case UnsavedChoice::Cancel:
                return;
            case UnsavedChoice::Save:
                setActiveDocumentIndex(index);
                if (!saveActive()) {
                    return;
                }
                break;
            case UnsavedChoice::Discard:
                break;
            }
        }
        revertActive();
    });
    registry_->setHandler(command_ids::FileClose, [this]() { closeWindow(activeWindow_, true); });
    registry_->setHandler(command_ids::FileCloseAll, [this]() {
        for (int i = docs_.size() - 1; i >= 0; --i) {
            if (!closeDocument(i, true)) {
                break;
            }
        }
    });
    registry_->setHandler(command_ids::FileExit, [this]() {
        for (int i = docs_.size() - 1; i >= 0; --i) {
            if (!closeDocument(i, true)) {
                break;
            }
        }
        if (documentCount() == 0) {
            qApp->quit();
        }
    });

    auto hasDocument = [this]() { return documentCount() > 0; };
    for (const char* id : {command_ids::FileSave, command_ids::FileSaveAs, command_ids::FileExportAs,
                           command_ids::FileQuickExportPng, command_ids::FileSaveForWeb,
                           command_ids::FileClose,
                           command_ids::FileCloseAll, command_ids::FilePlace,
                           command_ids::FileInfo}) {
        registry_->setEnabledProvider(id, hasDocument);
    }
    registry_->setEnabledProvider(command_ids::FileRevert, [this]() {
        PictureView* view = activeView();
        // A raster/smart-object import keeps the source path only on the tab;
        // its document path stays empty (Save must still ask where to write).
        return view && !document_api::filePath(*view).isEmpty();
    });

    wireEditHistory();
    wireColorManagementMenu();

    wireTransformMenu();

    const auto recordRotation = [this](const char* id, int angle) {
        CommandParams command;
        command.values.insert(QStringLiteral("angle"), angle);
        registry_->recordCommand(QLatin1String(id), command);
    };
    const auto recordFlip = [this](const char* id, const char* axis) {
        CommandParams command;
        command.values.insert(QStringLiteral("axis"), QString::fromLatin1(axis));
        registry_->recordCommand(QLatin1String(id), command);
    };

    registry_->setHandler(command_ids::ImageRotate90Cw, [this, recordRotation]() {
        PictureView* view = activeView();
        if (view && view->has_document() && view->rotate_doc(1)) {
            recordRotation(command_ids::ImageRotate90Cw, 90);
        }
    });
    registry_->setExecutor(command_ids::ImageRotate90Cw, [this](const CommandParams&) {
        PictureView* view = activeView();
        return view && view->has_document() && view->rotate_doc(1);
    });
    registry_->setHandler(command_ids::ImageRotate90Ccw, [this, recordRotation]() {
        PictureView* view = activeView();
        if (view && view->has_document() && view->rotate_doc(3)) {
            recordRotation(command_ids::ImageRotate90Ccw, -90);
        }
    });
    registry_->setExecutor(command_ids::ImageRotate90Ccw, [this](const CommandParams&) {
        PictureView* view = activeView();
        return view && view->has_document() && view->rotate_doc(3);
    });
    registry_->setHandler(command_ids::ImageRotate180, [this, recordRotation]() {
        PictureView* view = activeView();
        if (view && view->has_document() && view->rotate_doc(2)) {
            recordRotation(command_ids::ImageRotate180, 180);
        }
    });
    registry_->setExecutor(command_ids::ImageRotate180, [this](const CommandParams&) {
        PictureView* view = activeView();
        return view && view->has_document() && view->rotate_doc(2);
    });
    registry_->setHandler(command_ids::ImageFlipHorizontal, [this, recordFlip]() {
        PictureView* view = activeView();
        if (view && view->has_document() && view->flip_doc(true)) {
            recordFlip(command_ids::ImageFlipHorizontal, "horizontal");
        }
    });
    registry_->setExecutor(command_ids::ImageFlipHorizontal, [this](const CommandParams&) {
        PictureView* view = activeView();
        return view && view->has_document() && view->flip_doc(true);
    });
    registry_->setHandler(command_ids::ImageFlipVertical, [this, recordFlip]() {
        PictureView* view = activeView();
        if (view && view->has_document() && view->flip_doc(false)) {
            recordFlip(command_ids::ImageFlipVertical, "vertical");
        }
    });
    registry_->setExecutor(command_ids::ImageFlipVertical, [this](const CommandParams&) {
        PictureView* view = activeView();
        return view && view->has_document() && view->flip_doc(false);
    });
    for (const char* id : {command_ids::ImageRotate90Cw, command_ids::ImageRotate90Ccw,
                           command_ids::ImageRotate180, command_ids::ImageFlipHorizontal,
                           command_ids::ImageFlipVertical}) {
        registry_->setEnabledProvider(id,
                                      [this]() { return activeView() && activeView()->has_document(); });
    }

    wireImageMenu();

    registry_->setHandler(command_ids::SelectAll, [this]() {
        if (PictureView* view = activeView()) {
            view->select_all();
        }
    });
    registry_->setEnabledProvider(command_ids::SelectAll,
                                  [this]() { return activeView() && activeView()->has_document(); });
    registry_->setHandler(command_ids::SelectDeselect, [this]() {
        if (PictureView* view = activeView()) {
            view->deselect();
        }
    });
    registry_->setEnabledProvider(command_ids::SelectDeselect,
                                  [this]() { return activeView() && activeView()->has_document(); });

    registerSelectHandlers();
    registerEditHandlers();
    wireFilterMenu();
    wireAlignMenu();
    wireTypeMenu();
    wireLayerStyleMenu();
    wireImageAdjustments();
    wireLayerAdjustments();
    wireLayerContentOptions();
    wireLayerArrangeStamp();

    // M37: layer creation and grouping. Layer…/Group… open the modal dialog,
    // whose accept step places the node above the selection; Group from Layers…
    // wraps the panel selection. Duplicate acts on every selected path like
    // the panel's row menu; every command reads the Rust layer target, not the
    // panel's tree current row.
    registry_->setHandler(command_ids::LayerNewLayer, [this]() {
        if (layersPanel_) {
            layersPanel_->openNewLayerDialog();
        }
    });
    registry_->setHandler(command_ids::LayerNewGroup, [this]() {
        if (layersPanel_) {
            layersPanel_->openNewGroupDialog();
        }
    });
    registry_->setHandler(command_ids::LayerNewGroupFromLayers, [this]() {
        if (layersPanel_) {
            layersPanel_->openGroupFromLayersDialog();
        }
    });
    registry_->setHandler(command_ids::LayerDuplicateLayer, [this]() {
        PictureView* view = activeView();
        if (!layersPanel_ || !view) {
            return;
        }
        const int before = view->layer_count();
        layersPanel_->duplicateSelection();
        if (view->layer_count() > before) {
            registry_->recordCommand(command_ids::LayerDuplicateLayer, CommandParams{});
        }
    });
    registry_->setHandler(command_ids::LayerGroupLayers, [this]() {
        if (layersPanel_) {
            layersPanel_->groupSelection();
        }
    });
    registry_->setHandler(command_ids::LayerUngroupLayers, [this]() {
        if (layersPanel_) {
            layersPanel_->ungroupSelection();
        }
    });
    for (const char* id : {command_ids::LayerNewLayer, command_ids::LayerNewGroup,
                           command_ids::LayerGroupLayers,
                           command_ids::LayerUngroupLayers}) {
        registry_->setEnabledProvider(id,
                                      [this]() { return activeView() && activeView()->has_document(); });
    }
    registry_->setEnabledProvider(command_ids::LayerDuplicateLayer, [this]() {
        return activeView() && activeView()->has_document() && !selectedLayerPaths().isEmpty();
    });
    registry_->setEnabledProvider(command_ids::LayerNewGroupFromLayers, [this]() {
        return activeView() && activeView()->has_document() && layersPanel_
            && !selectedLayerPaths().isEmpty();
    });

    // Merge and flatten. Merge Down and Merge Layers share one id (design D4):
    // one selected layer merges down, two or more merge the selection, and the
    // label follows the selection.
    registry_->setHandler(command_ids::LayerMergeLayers, [this]() {
        PictureView* view = activeView();
        const QStringList paths = selectedLayerPaths();
        if (!view || paths.isEmpty()) {
            return;
        }
        const QString merged = view->merge_layers(paths);
        if (!merged.isEmpty()) {
            layersPanel_->refresh();
            refresh();
        }
    });
    registry_->setEnabledProvider(command_ids::LayerMergeLayers, [this]() {
        PictureView* view = activeView();
        return view && view->has_document() && layersPanel_
            && view->can_merge_layers(selectedLayerPaths());
    });
    registry_->setLabelProvider(command_ids::LayerMergeLayers, [this]() {
        const bool one = layersPanel_ && selectedLayerPaths().size() == 1;
        return one ? QStringLiteral("Merge Down") : QStringLiteral("Merge Layers");
    });

    registry_->setHandler(command_ids::LayerMergeVisible, [this]() {
        PictureView* view = activeView();
        const QString path = layerAnchor();
        if (!view || path.isEmpty()) {
            return;
        }
        const QString merged = view->merge_visible(path);
        if (!merged.isEmpty()) {
            layersPanel_->refresh();
            refresh();
        }
    });
    registry_->setEnabledProvider(command_ids::LayerMergeVisible, [this]() {
        PictureView* view = activeView();
        const QString path = layerAnchor();
        if (!view || !view->has_document() || path.isEmpty()) {
            return false;
        }
        const auto row = pictura::layers_api::rowForPath(*view, path);
        return row.path == path && row.visible;
    });

    // Create / Release Clipping Mask act on every selected layer.
    const auto clippingPaths = [this]() {
        return selectedLayerPaths();
    };
    registry_->setHandler(command_ids::LayerCreateClippingMask, [this, clippingPaths]() {
        PictureView* view = activeView();
        if (view) {
            clipping_create(*view, clippingPaths());
        }
    });
    registry_->setEnabledProvider(command_ids::LayerCreateClippingMask, [this, clippingPaths]() {
        PictureView* view = activeView();
        return view && view->has_document() && clipping_can_create(*view, clippingPaths());
    });
    registry_->setHandler(command_ids::LayerReleaseClippingMask, [this, clippingPaths]() {
        PictureView* view = activeView();
        if (view) {
            clipping_release(*view, clippingPaths());
        }
    });
    registry_->setEnabledProvider(command_ids::LayerReleaseClippingMask, [this, clippingPaths]() {
        PictureView* view = activeView();
        return view && view->has_document() && clipping_can_release(*view, clippingPaths());
    });

    registry_->setHandler(command_ids::LayerMergeClippingMask, [this]() {
        PictureView* view = activeView();
        const QString path = layerAnchor();
        if (view && !path.isEmpty()) {
            view->merge_clipping_mask(path);
        }
    });
    registry_->setEnabledProvider(command_ids::LayerMergeClippingMask, [this]() {
        PictureView* view = activeView();
        const QString path = layerAnchor();
        return view && view->has_document() && !path.isEmpty()
            && view->can_merge_clipping_mask(path);
    });

    // Layer Mask authoring; the bridge acts on the active layer.
    const auto addLayerMask = [this](const QString& kind) {
        PictureView* view = activeView();
        if (view) {
            layer_mask_add(*view, kind);
        }
    };
    const auto enableWhenNoMask = [this](const char* id) {
        registry_->setEnabledProvider(id, [this]() {
            PictureView* view = activeView();
            return view && view->has_document() && !layer_mask_present(*view);
        });
    };
    const auto enableWhenMask = [this](const char* id) {
        registry_->setEnabledProvider(id, [this]() {
            PictureView* view = activeView();
            return view && view->has_document() && layer_mask_present(*view);
        });
    };
    registry_->setHandler(command_ids::LayerMaskRevealAll,
                          [addLayerMask]() { addLayerMask(QStringLiteral("reveal-all")); });
    registry_->setHandler(command_ids::LayerMaskHideAll,
                          [addLayerMask]() { addLayerMask(QStringLiteral("hide-all")); });
    registry_->setHandler(command_ids::LayerMaskRevealSelection,
                          [addLayerMask]() { addLayerMask(QStringLiteral("reveal-selection")); });
    registry_->setHandler(command_ids::LayerMaskHideSelection,
                          [addLayerMask]() { addLayerMask(QStringLiteral("hide-selection")); });
    registry_->setHandler(command_ids::LayerMaskFromTransparency, [addLayerMask]() {
        addLayerMask(QStringLiteral("from-transparency"));
    });
    enableWhenNoMask(command_ids::LayerMaskRevealAll);
    enableWhenNoMask(command_ids::LayerMaskHideAll);
    enableWhenNoMask(command_ids::LayerMaskFromTransparency);
    registry_->setEnabledProvider(command_ids::LayerMaskRevealSelection, [this]() {
        PictureView* view = activeView();
        return view && view->has_document() && view->has_selection()
            && !layer_mask_present(*view);
    });
    registry_->setEnabledProvider(command_ids::LayerMaskHideSelection, [this]() {
        PictureView* view = activeView();
        return view && view->has_document() && view->has_selection()
            && !layer_mask_present(*view);
    });
    const auto simpleMaskAction = [this, enableWhenMask](const char* id, auto action) {
        registry_->setHandler(id, [this, action]() {
            PictureView* view = activeView();
            if (view) {
                action(*view);
            }
        });
        enableWhenMask(id);
    };
    simpleMaskAction(command_ids::LayerMaskDelete,
                     [](PictureView& view) { return layer_mask_delete(view); });
    simpleMaskAction(command_ids::LayerMaskApply,
                     [](PictureView& view) { return layer_mask_apply(view); });
    simpleMaskAction(command_ids::LayerMaskEnable,
                     [](PictureView& view) { return layer_mask_set_enabled(view, true); });
    simpleMaskAction(command_ids::LayerMaskDisable,
                     [](PictureView& view) { return layer_mask_set_enabled(view, false); });
    simpleMaskAction(command_ids::LayerMaskLink,
                     [](PictureView& view) { return layer_mask_set_linked(view, true); });
    simpleMaskAction(command_ids::LayerMaskUnlink,
                     [](PictureView& view) { return layer_mask_set_linked(view, false); });

    registry_->setHandler(command_ids::LayerFlattenImage, [this]() {
        PictureView* view = activeView();
        if (view && view->has_document() && view->flatten_image() > 0) {
            registry_->recordCommand(command_ids::LayerFlattenImage, CommandParams{});
        }
    });
    registry_->setEnabledProvider(command_ids::LayerFlattenImage,
                                  [this]() { return activeView() && activeView()->has_document(); });

    // Background conversion. The applicable kind gates each command: "Layer
    // from Background…" needs the Background; "Background From Layer" needs a
    // raster pixel layer and a document with no Background yet (groups,
    // adjustment/fill layers, and a second Background are refused).
    auto currentLayerKind = [this]() -> QString {
        PictureView* view = activeView();
        const QString path = layerAnchor();
        if (!view || path.isEmpty()) {
            return QString();
        }
        return pictura::layers_api::rowForPath(*view, path).kind;
    };
    registry_->setHandler(command_ids::LayerNewLayerFromBackground, [this]() {
        PictureView* view = activeView();
        const QString path = layerAnchor();
        if (view && !path.isEmpty()) {
            view->layer_from_background(path);
        }
    });
    registry_->setEnabledProvider(command_ids::LayerNewLayerFromBackground,
                                  [currentLayerKind]() {
                                      return currentLayerKind() == QStringLiteral("background");
                                  });
    registry_->setHandler(command_ids::LayerNewBackgroundFromLayer, [this]() {
        PictureView* view = activeView();
        const QString path = layerAnchor();
        if (view && !path.isEmpty()) {
            view->background_from_layer(path);
        }
    });
    registry_->setEnabledProvider(command_ids::LayerNewBackgroundFromLayer, [this]() {
        PictureView* view = activeView();
        const QString path = layerAnchor();
        return view && !path.isEmpty() && pictura::can_background_from_layer(*view, path);
    });

    // Layer via Copy / Cut. Both need an active selection and an active layer
    // (the panel's current row, which the bridge resolves).
    const auto layerVia = [this](bool cut) {
        PictureView* view = activeView();
        const QString path = layerAnchor();
        if (!view || path.isEmpty()) {
            return;
        }
        cut ? view->layer_via_cut(path) : view->layer_via_copy(path);
    };
    registry_->setHandler(command_ids::LayerNewLayerViaCopy, [layerVia]() { layerVia(false); });
    registry_->setHandler(command_ids::LayerNewLayerViaCut, [layerVia]() { layerVia(true); });
    for (const char* id : {command_ids::LayerNewLayerViaCopy, command_ids::LayerNewLayerViaCut}) {
        registry_->setEnabledProvider(id, [this]() {
            return activeView() && activeView()->has_document() && activeView()->has_selection()
                && layersPanel_ && !layerAnchor().isEmpty();
        });
    }

    // Select Similar/Linked and Link/Unlink. The engine sets the target and
    // notifies the view; Link and Unlink only touch the transient link sets.
    // None records history.
    registry_->setHandler(command_ids::LayerSelectSimilar, [this]() {
        PictureView* view = activeView();
        const QString path = layerAnchor();
        if (!view || path.isEmpty()) {
            return;
        }
        view->select_similar(path);
        if (layersPanel_) {
            layersPanel_->refresh();
        }
        refresh();
    });
    registry_->setEnabledProvider(command_ids::LayerSelectSimilar, [this]() {
        return activeView() && activeView()->has_document() && layersPanel_
            && !layerAnchor().isEmpty();
    });

    registry_->setHandler(command_ids::LayerSelectLinked, [this]() {
        PictureView* view = activeView();
        const QString path = layerAnchor();
        if (!view || path.isEmpty()) {
            return;
        }
        view->select_linked(path);
        if (layersPanel_) {
            layersPanel_->refresh();
        }
        refresh();
    });
    registry_->setEnabledProvider(command_ids::LayerSelectLinked, [this]() {
        return activeView() && activeView()->has_document() && layersPanel_
            && !layerAnchor().isEmpty();
    });

    registry_->setHandler(command_ids::LayerLinkLayers, [this]() {
        PictureView* view = activeView();
        const QStringList paths = selectedLayerPaths();
        if (view && !paths.isEmpty()) {
            view->link_layers(paths, true);
        }
    });
    registry_->setHandler(command_ids::LayerUnlinkLayers, [this]() {
        PictureView* view = activeView();
        const QStringList paths = selectedLayerPaths();
        if (view && !paths.isEmpty()) {
            view->link_layers(paths, false);
        }
    });
    for (const char* id : {command_ids::LayerLinkLayers, command_ids::LayerUnlinkLayers}) {
        registry_->setEnabledProvider(id, [this]() {
            return activeView() && activeView()->has_document() && layersPanel_
                && !selectedLayerPaths().isEmpty();
        });
    }

    // Delete Layer removes the Layers-panel selection, as the panel's own
    // delete does, and is unavailable when that would remove nothing (the
    // Background, locked layers, the document's last layer). Delete Hidden
    // Layers needs only a document (it records nothing when nothing is
    // hidden); Hide Layers needs a selection.
    registry_->setHandler(command_ids::LayerDeleteLayer, [this]() {
        if (layersPanel_) {
            layersPanel_->deleteSelection();
        }
    });
    registry_->setEnabledProvider(command_ids::LayerDeleteLayer, [this]() {
        return activeView() && activeView()->has_document() && layersPanel_
            && layers_can_delete(*activeView(), selectedLayerPaths());
    });
    registry_->setHandler(command_ids::LayerDeleteHiddenLayers, [this]() {
        PictureView* view = activeView();
        if (view) {
            view->delete_hidden_layers();
        }
    });
    registry_->setEnabledProvider(command_ids::LayerDeleteHiddenLayers,
                                  [this]() { return activeView() && activeView()->has_document(); });

    registry_->setHandler(command_ids::LayerHideLayers, [this]() {
        PictureView* view = activeView();
        const QStringList paths = selectedLayerPaths();
        if (view && !paths.isEmpty() && view->hide_layers(paths) > 0) {
            registry_->recordCommand(command_ids::LayerHideLayers, CommandParams{});
        }
    });
    registry_->setEnabledProvider(command_ids::LayerHideLayers, [this]() {
        return activeView() && activeView()->has_document() && layersPanel_
            && !selectedLayerPaths().isEmpty();
    });

    // Fill creation: New Layer dialog -> the type editor -> one creation.
    registry_->setHandler(command_ids::LayerNewFillSolidColor, [this]() {
        if (PictureView* view = activeView()) {
            createFillLayerFromDialog(*view, QStringLiteral("solid-color"), foregroundColor(),
                                      this, true);
        }
    });
    registry_->setEnabledProvider(command_ids::LayerNewFillSolidColor,
                                  [this]() { return activeView() && activeView()->has_document(); });

    registry_->setHandler(command_ids::LayerNewFillGradient, [this]() {
        if (PictureView* view = activeView()) {
            createFillLayerFromDialog(*view, QStringLiteral("gradient"), foregroundColor(), this,
                                      true);
        }
    });
    registry_->setEnabledProvider(command_ids::LayerNewFillGradient,
                                  [this]() { return activeView() && activeView()->has_document(); });

    registry_->setHandler(command_ids::LayerNewFillPattern, [this]() {
        if (PictureView* view = activeView()) {
            createFillLayerFromDialog(*view, QStringLiteral("pattern"), foregroundColor(), this,
                                      true);
        }
    });
    registry_->setEnabledProvider(command_ids::LayerNewFillPattern, [this]() {
        PictureView* view = activeView();
        return view && view->has_document() && layer_has_patterns(*view);
    });

    // Rasterize. Fill Content and Type each need a current layer of that kind;
    // Layer tries type then fill; All Layers needs a document. Shape/Vector
    // Mask/Layer Style/Video/3D have no handler and stay disabled.
    const auto currentFillPath = [this]() -> QString {
        PictureView* view = activeView();
        const QString path = layerAnchor();
        if (!view || path.isEmpty() || !view->layer_is_fill_content(path)) {
            return QString();
        }
        return path;
    };
    const auto currentTypePath = [this]() -> QString {
        PictureView* view = activeView();
        const QString path = layerAnchor();
        if (!view || path.isEmpty() || !pictura::type_api::readLayer(*view, path).isType) {
            return QString();
        }
        return path;
    };
    registry_->setHandler(command_ids::LayerRasterizeFillContent, [this, currentFillPath]() {
        if (PictureView* view = activeView()) {
            const QString path = currentFillPath();
            if (!path.isEmpty()) {
                view->rasterize_fill_content(path);
            }
        }
    });
    registry_->setEnabledProvider(command_ids::LayerRasterizeFillContent,
                                  [currentFillPath]() { return !currentFillPath().isEmpty(); });
    registry_->setHandler(command_ids::LayerRasterizeLayer, [this, currentFillPath]() {
        if (PictureView* view = activeView()) {
            const QString path = currentFillPath();
            if (!path.isEmpty()) {
                view->rasterize_layer(path);
            }
        }
    });
    registry_->setEnabledProvider(command_ids::LayerRasterizeLayer,
                                  [currentFillPath]() { return !currentFillPath().isEmpty(); });
    registry_->setHandler(command_ids::LayerRasterizeType, [this, currentTypePath]() {
        if (PictureView* view = activeView()) {
            const QString path = currentTypePath();
            if (!path.isEmpty()) {
                pictura::type_api::rasterize(*view, path);
            }
        }
    });
    registry_->setEnabledProvider(command_ids::LayerRasterizeType,
                                  [currentTypePath]() { return !currentTypePath().isEmpty(); });
    registry_->setHandler(command_ids::LayerRasterizeAllLayers, [this]() {
        PictureView* view = activeView();
        if (view) {
            view->rasterize_all_layers();
        }
    });
    registry_->setEnabledProvider(command_ids::LayerRasterizeAllLayers, [this]() {
        PictureView* view = activeView();
        if (!view || !view->has_document()) {
            return false;
        }
        for (const auto& row : pictura::layers_api::rows(*view)) {
            if (view->layer_is_fill_content(row.path)) {
                return true;
            }
        }
        return false;
    });

    wireShapeLayerActions();
    wireVectorMaskActions();
    wireMattingActions();
    wireAdvancedSmartObjectActions();

    // Smart Objects. Convert to Smart Object needs the current layer to be a
    // convertible raster pixel layer or Background (not a group, adjustment, or
    // already a smart object); a Background is unlocked first.
    const auto currentSmartPath = [this]() -> QString {
        PictureView* view = activeView();
        const QString path = layerAnchor();
        if (!view || path.isEmpty() || !view->layer_can_convert_to_smart_object(path)) {
            return QString();
        }
        return path;
    };
    registry_->setHandler(command_ids::LayerSmartObjectConvertTo, [this, currentSmartPath]() {
        if (PictureView* view = activeView()) {
            const QString path = currentSmartPath();
            if (!path.isEmpty()) {
                view->convert_to_smart_object(path);
            }
        }
    });
    registry_->setEnabledProvider(command_ids::LayerSmartObjectConvertTo,
                                  [currentSmartPath]() { return !currentSmartPath().isEmpty(); });

    // Convert for Smart Filters shares the convertible-raster eligibility and
    // records its own state label so the Filter menu command is distinct.
    registry_->setHandler(command_ids::FilterConvertForSmartFilters,
                          [this, currentSmartPath]() {
                              if (PictureView* view = activeView()) {
                                  const QString path = currentSmartPath();
                                  if (!path.isEmpty()) {
                                      convert_for_smart_filters(*view, path);
                                  }
                              }
                          });
    registry_->setEnabledProvider(command_ids::FilterConvertForSmartFilters,
                                  [currentSmartPath]() { return !currentSmartPath().isEmpty(); });

    wireSmartFilterActions();

    const auto currentRasterizableSmartPath = [this]() -> QString {
        PictureView* view = activeView();
        const QString path = layerAnchor();
        if (!view || path.isEmpty() || !view->layer_can_rasterize_smart_object(path)) {
            return QString();
        }
        return path;
    };
    registry_->setHandler(command_ids::LayerRasterizeSmartObject,
                          [this, currentRasterizableSmartPath]() {
                              if (PictureView* view = activeView()) {
                                  const QString path = currentRasterizableSmartPath();
                                  if (!path.isEmpty()) {
                                      view->rasterize_smart_object(path);
                                  }
                              }
                          });
    registry_->setEnabledProvider(command_ids::LayerRasterizeSmartObject,
                                  [currentRasterizableSmartPath]() {
                                      return !currentRasterizableSmartPath().isEmpty();
                                  });

    // Replace Contents needs the current layer to be a replaceable smart object.
    const auto currentReplaceableSmartPath = [this]() -> QString {
        PictureView* view = activeView();
        const QString path = layerAnchor();
        if (!view || path.isEmpty() || !view->layer_can_replace_smart_object_contents(path)) {
            return QString();
        }
        return path;
    };
    registry_->setHandler(command_ids::LayerSmartObjectReplaceContents,
                          [this, currentReplaceableSmartPath]() {
                              const QString path = currentReplaceableSmartPath();
                              if (!path.isEmpty()) {
                                  replaceSmartObjectContents(path);
                              }
                          });
    registry_->setEnabledProvider(command_ids::LayerSmartObjectReplaceContents,
                                  [currentReplaceableSmartPath]() {
                                      return !currentReplaceableSmartPath().isEmpty();
                                  });

    // Export Contents needs the current layer to carry a non-empty embedded
    // payload. Export only reads the document, so nothing is refreshed.
    const auto currentExportableSmartPath = [this]() -> QString {
        PictureView* view = activeView();
        const QString path = layerAnchor();
        if (!view || path.isEmpty() || !view->layer_can_export_smart_object_contents(path)) {
            return QString();
        }
        return path;
    };
    registry_->setHandler(command_ids::LayerSmartObjectExportContents,
                          [this, currentExportableSmartPath]() {
                              const QString path = currentExportableSmartPath();
                              if (!path.isEmpty()) {
                                  exportSmartObjectContents(path);
                              }
                          });
    registry_->setEnabledProvider(command_ids::LayerSmartObjectExportContents,
                                  [currentExportableSmartPath]() {
                                      return !currentExportableSmartPath().isEmpty();
                                  });

    // Edit Contents needs the current layer to be an embedded PSD/PSB source,
    // so it can open as an ordinary document in an untitled editor tab.
    const auto currentEditableSmartPath = [this]() -> QString {
        PictureView* view = activeView();
        const QString path = layerAnchor();
        if (!view || path.isEmpty() || !view->layer_can_edit_smart_object_contents(path)) {
            return QString();
        }
        return path;
    };
    registry_->setHandler(command_ids::LayerSmartObjectEditContents,
                          [this, currentEditableSmartPath]() {
                              const QString path = currentEditableSmartPath();
                              if (!path.isEmpty() && editSmartObjectContents(path)) {
                                  refresh();
                              }
                          });
    registry_->setEnabledProvider(command_ids::LayerSmartObjectEditContents,
                                  [currentEditableSmartPath]() {
                                      return !currentEditableSmartPath().isEmpty();
                                  });

    // Filter > Pictura Raw…: prefill the dialog from the layer's stored
    // Pictura Raw settings (empty when none), then apply in one bridge call /
    // one history state — destructively on a plain raster layer, as a retained
    // camera-raw smart filter on an embedded smart object.
    registry_->setHandler(command_ids::FilterPicturaRaw, [this]() {
        PictureView* view = activeView();
        const QString path = layerAnchor();
        if (!view || path.isEmpty()) {
            return;
        }
        const QString initial = view->layer_pictura_raw_settings(path);
        QString settings;
        if (!PicturaRawDialog::get(this, view, path, initial, &settings)) {
            return;
        }
        if (view->apply_pictura_raw_filter(path, settings)) {
            refresh();
        }
    });
    registry_->setEnabledProvider(command_ids::FilterPicturaRaw, [this]() {
        PictureView* view = activeView();
        const QString path = layerAnchor();
        return view && view->has_document() && !path.isEmpty()
            && view->layer_can_apply_pictura_raw(path);
    });

    registry_->setHandler(command_ids::ViewZoomIn, [this]() {
        if (ImageView* canvas = imageView()) {
            canvas->zoomIn();
            if (zoomResizesWindows_) {
                resizeWindowToFit(windowOfCanvas(canvas));
            }
        }
    });
    registry_->setHandler(command_ids::ViewZoomOut, [this]() {
        if (ImageView* canvas = imageView()) {
            canvas->zoomOut();
            if (zoomResizesWindows_) {
                resizeWindowToFit(windowOfCanvas(canvas));
            }
        }
    });
    registry_->setHandler(command_ids::ViewFitOnScreen, [this]() {
        if (ImageView* canvas = imageView()) {
            canvas->fitOnScreen();
        }
    });
    registry_->setHandler(command_ids::ViewActualPixels, [this]() {
        if (ImageView* canvas = imageView()) {
            canvas->actualPixels();
        }
    });

    registry_->setHandler(command_ids::ViewScreenModeStandard,
                          [this]() { requestScreenMode(ScreenMode::Standard); });
    registry_->setHandler(command_ids::ViewScreenModeFullWithMenuBar,
                          [this]() { requestScreenMode(ScreenMode::FullWithMenuBar); });
    registry_->setHandler(command_ids::ViewScreenModeFull,
                          [this]() { requestScreenMode(ScreenMode::Full); });
    registry_->setCheckedProvider(command_ids::ViewScreenModeStandard,
                                  [this]() { return screenMode_ == ScreenMode::Standard; });
    registry_->setCheckedProvider(command_ids::ViewScreenModeFullWithMenuBar, [this]() {
        return screenMode_ == ScreenMode::FullWithMenuBar;
    });
    registry_->setCheckedProvider(command_ids::ViewScreenModeFull,
                                  [this]() { return screenMode_ == ScreenMode::Full; });

    registry_->setHandler(command_ids::ViewOptions, [this]() {
        QAction* action = registry_->action(command_ids::ViewOptions);
        if (optionsBar_ && action) {
            optionsBar_->setVisible(action->isChecked());
            layoutWorkspaceFrame();
        }
    });
    registry_->setCheckedProvider(command_ids::ViewOptions,
                                  [this]() { return optionsBar_ && optionsBar_->isVisible(); });

    registry_->setHandler(command_ids::ViewToolHints, [this]() {
        const QAction* action = registry_->action(command_ids::ViewToolHints);
        hintsVisible_ = action && action->isChecked();
        updateStatus();
    });
    registry_->setCheckedProvider(command_ids::ViewToolHints, [this]() { return hintsVisible_; });

    registry_->setHandler(command_ids::ViewFreePan, [this]() {
        freePan_ = !freePan_;
        for (const DocWindow* window : windows_) {
            if (window->canvas) {
                window->canvas->setFreePan(freePan_);
            }
        }
        saveSession();
        registry_->refresh();
    });
    registry_->setCheckedProvider(command_ids::ViewFreePan, [this]() { return freePan_; });

    registry_->setHandler(command_ids::ViewSnapCanvasToWorkspace, [this]() {
        snapCanvasToWorkspace_ = !snapCanvasToWorkspace_;
        for (const DocWindow* window : windows_) {
            if (window->canvas) {
                window->canvas->setSnapToWorkspace(snapCanvasToWorkspace_);
            }
        }
        saveSession();
        registry_->refresh();
    });
    registry_->setCheckedProvider(command_ids::ViewSnapCanvasToWorkspace,
                                  [this]() { return snapCanvasToWorkspace_; });

    registry_->setHandler(command_ids::ViewShowSelectionEdges, [this]() {
        const QAction* action = registry_->action(command_ids::ViewShowSelectionEdges);
        if (ImageView* canvas = imageView()) {
            canvas->setSelectionEdgesVisible(action && action->isChecked());
        }
    });
    registry_->setEnabledProvider(command_ids::ViewShowSelectionEdges,
                                  [this]() { return documentCount() > 0; });
    registry_->setCheckedProvider(command_ids::ViewShowSelectionEdges, [this]() {
        ImageView* canvas = imageView();
        return canvas ? canvas->selectionEdgesVisible() : true;
    });

    registry_->setHandler(command_ids::WindowPanelsTools, [this]() {
        QAction* action = registry_->action(command_ids::WindowPanelsTools);
        if (toolsColumn_ && action) {
            toolsColumn_->setVisible(action->isChecked());
        }
    });
    registry_->setCheckedProvider(command_ids::WindowPanelsTools,
                                  [this]() { return toolsColumn_ && toolsColumn_->isVisible(); });

    const auto stepUiScale = [this](int direction) {
        const QVector<int> choices = Theme::uiScaleChoices();
        int index = choices.indexOf(uiScalePercent_);
        if (index < 0) {
            index = choices.indexOf(Theme::kDefaultUiScalePercent);
        }
        setUiScalePercent(choices.at(qBound(0, index + direction, choices.size() - 1)));
    };
    registry_->setHandler(command_ids::WindowUiEnlarge, [stepUiScale]() { stepUiScale(1); });
    registry_->setHandler(command_ids::WindowUiDecrease, [stepUiScale]() { stepUiScale(-1); });
    registry_->setHandler(command_ids::WindowUiReset,
                          [this]() { setUiScalePercent(Theme::kDefaultUiScalePercent); });
    registry_->setEnabledProvider(command_ids::WindowUiEnlarge,
                                  [this]() { return uiScalePercent_ < Theme::kUiScaleMaxPercent; });
    registry_->setEnabledProvider(command_ids::WindowUiDecrease,
                                  [this]() { return uiScalePercent_ > Theme::kUiScaleMinPercent; });
    registry_->setEnabledProvider(command_ids::WindowUiReset,
                                  [this]() { return uiScalePercent_ != Theme::kDefaultUiScalePercent; });
    registry_->setLabelProvider(command_ids::WindowUiSize,
                                [this]() { return tr("UI Size: %1%").arg(uiScalePercent_); });

    registerPreferenceHandlers();
    registerShortcutHandlers();
    registerGuideHandlers();
    registerSearchHandlers();

    // Every other panel is a content widget hosted by the PanelColumn; the
    // Window > Panels toggles drive and reflect the column's registry rather
    // than a dock's visibility.
    for (const PanelToggle& toggle : panelToggles()) {
        const QString command = QString::fromLatin1(toggle.command);
        const QString panel = QString::fromLatin1(toggle.panel);
        registry_->setHandler(command, [this, command, panel]() {
            QAction* action = registry_->action(command);
            PanelColumn* owner = columnForPanel(panel);
            if (!owner) {
                owner = panelColumn_;
            }
            if (owner && action) {
                owner->showPanel(panel, action->isChecked());
            }
        });
        registry_->setCheckedProvider(command, [this, panel]() {
            PanelColumn* owner = columnForPanel(panel);
            if (!owner) {
                owner = panelColumn_;
            }
            return owner && owner->isPanelVisible(panel);
        });
    }

    registry_->setHandler(command_ids::HelpSystemInfo, [this]() { showSystemInfo(); });
    registry_->setHandler(command_ids::HelpSubmitIssue, [this]() { submitIssue(); });

    registry_->setHandler(command_ids::HelpAbout, [this]() {
        QMessageBox::about(
            this, tr("About Kooka Pictura"),
            tr("Kooka Pictura — an open-source layered image editor for Linux, written in Rust "
               "and Qt.\n\n"
               "Licensed under the GNU GPL v3.0 or later. Built with Qt 6 (LGPLv3) and "
               "open-source Rust crates; see THIRD-PARTY-LICENSES.\n\n"
               "%1\n\n"
                "Adobe, Photoshop, and Camera Raw are trademarks of Adobe Inc.; "
                "Kooka Pictura is not affiliated with or endorsed by Adobe.")
                .arg(aboutText(activeBackendLabel(gpuCompute_))));
    });

    // Help > Developer > Design System…: the in-app storybook over the real UI.
    registry_->setHandler(command_ids::HelpDeveloperDesignSystem,
                          [this]() { showDesignSystemDialog(); });
}

void PicturaMainWindow::showDesignSystemDialog()
{
    if (!designSystemDialog_) {
        designSystemDialog_ = new DesignSystemDialog(this);
    }
    designSystemDialog_->show();
    designSystemDialog_->raise();
    designSystemDialog_->activateWindow();
}

bool PicturaMainWindow::beginFreeTransform(const QString& path)
{
    return tools_ && tools_->beginFreeTransform(path);
}

bool PicturaMainWindow::beginTransformMode(const QString& path, const QString& mode)
{
    return tools_ && tools_->beginTransformMode(path, mode);
}

void PicturaMainWindow::selectLayerPath(const QString& path)
{
    PictureView* view = activeView();
    if (!view || path.isEmpty()) {
        return;
    }
    select_layer_path(*view, path);
    if (layersPanel_) {
        layersPanel_->refresh();
    }
}

} // namespace pictura
