// The Edit > Preferences leaves and the dialog they open, lifted out of
// frame.cpp / frame_menus.cpp to keep them within their size budgets.

#include "frame_includes.h"
#include "agent_control.h"
#include "api/type_api.h"
#include "preferences_agent_page.h"
#include "shp_support.h"

namespace pictura {

// The implemented Preferences leaves; the other CS6 panes stay disabled and
// are no-ops (their command-tree enablement is unchanged).
void PicturaMainWindow::registerPreferenceHandlers()
{
    registry_->setHandler(command_ids::EditPreferencesGeneral,
                          [this]() { showPreferences(PreferencesDialog::kGeneral); });
    registry_->setHandler(command_ids::EditPreferencesInterface,
                          [this]() { showPreferences(PreferencesDialog::kInterface); });
    registry_->setHandler(command_ids::EditPreferencesPerformance,
                          [this]() { showPreferences(PreferencesDialog::kPerformance); });
    registry_->setHandler(command_ids::EditPreferencesUnits,
                          [this]() { showPreferences(PreferencesDialog::kUnits); });
    registry_->setHandler(command_ids::EditPreferencesGuides,
                          [this]() { showPreferences(PreferencesDialog::kGuides); });
    registry_->setHandler(command_ids::EditPreferencesType,
                          [this]() { showPreferences(PreferencesDialog::kType); });
    registry_->setHandler(command_ids::EditPreferencesAgentControl,
                          [this]() { showPreferences(PreferencesDialog::kAgentControl); });
    // Only an interactive window has one.
    registry_->setEnabledProvider(command_ids::EditPreferencesAgentControl,
                                  [this]() { return findChild<AgentControl*>() != nullptr; });
}

void PicturaMainWindow::showPreferences(const QString& page)
{
    if (!preferencesDialog_) {
        preferencesDialog_ = new PreferencesDialog(this);
        preferencesDialog_->setShiftKeyForToolSwitch(useShiftKeyForToolSwitch_);
        preferencesDialog_->setOpenDocumentsAsTabs(openDocumentsAsTabs_);
        preferencesDialog_->setFloatingDocumentDocking(floatingDocumentDocking_);
        preferencesDialog_->setZoomResizesWindows(zoomResizesWindows_);
        preferencesDialog_->setAutoCollapseIconic(panelColumn_->autoCollapseIconic());
        preferencesDialog_->setAutoShowHidden(panelColumn_->autoShowHidden());
        preferencesDialog_->setBrightnessLevel(brightnessLevel_);
        preferencesDialog_->setThemeFamily(themeFamily_);
        preferencesDialog_->setUiScalePercent(uiScalePercent_);
        preferencesDialog_->setPickerChoice(pickerChoice_);
        preferencesDialog_->setGpuCompute(gpuCompute_);
        preferencesDialog_->setGpuComputeEnabled(gpuCandidatesAvailable_);
        preferencesDialog_->setTypePlaceholderText(!tools_ || tools_->typePlaceholderText());
        preferencesDialog_->setTypeShowAsianOptions(typeShowAsianOptions_);
        preferencesDialog_->setTypeMiddleEastern(typeMiddleEastern_);
        preferencesDialog_->setTypeMissingGlyphProtection(typeMissingGlyphProtection_);
        if (auto* agent = findChild<AgentControl*>()) {
            preferencesDialog_->addPage(PreferencesDialog::kAgentControl,
                                        new AgentControlPage(agent));
        }
        // KookaPictura-SHP fork: Advanced (the SHP size limit, until Kooka closes).
        if (auto* shp = ShpSupport::of(this)) {
            preferencesDialog_->addPage(tr("Advanced"), shp->advancedPage(preferencesDialog_));
        }
        connectRecoveryPreferences(preferencesDialog_);
        connectGuidePreferences(preferencesDialog_);
        connect(preferencesDialog_, &PreferencesDialog::useShiftKeyForToolSwitchChanged,
                this, [this](bool on) {
                    useShiftKeyForToolSwitch_ = on;
                    if (toolbox_) {
                        toolbox_->setShiftKeyForToolSwitch(on);
                    }
                    saveSession();
                });
        connect(preferencesDialog_, &PreferencesDialog::openDocumentsAsTabsChanged, this,
                [this](bool on) {
                    openDocumentsAsTabs_ = on;
                    saveSession();
                });
        connect(preferencesDialog_, &PreferencesDialog::floatingDocumentDockingChanged, this,
                [this](bool on) {
                    floatingDocumentDocking_ = on;
                    saveSession();
                });
        connect(preferencesDialog_, &PreferencesDialog::zoomResizesWindowsChanged, this,
                [this](bool on) {
                    zoomResizesWindows_ = on;
                    saveSession();
                });
        connect(preferencesDialog_, &PreferencesDialog::autoCollapseIconicChanged, this,
                [this](bool on) { panelColumn_->setAutoCollapseIconic(on); });
        connect(preferencesDialog_, &PreferencesDialog::autoShowHiddenChanged, this,
                [this](bool on) { panelColumn_->setAutoShowHidden(on); });
        connect(preferencesDialog_, &PreferencesDialog::brightnessLevelChanged, this,
                [this](int level) {
                    setBrightnessLevel(level);
                    saveSession();
                });
        connect(preferencesDialog_, &PreferencesDialog::themeFamilyChanged, this,
                [this](int family) { setThemeFamily(family); });
        connect(preferencesDialog_, &PreferencesDialog::uiScalePercentChanged, this,
                [this](int percent) { setUiScalePercent(percent); });
        connect(preferencesDialog_, &PreferencesDialog::pickerChoiceChanged, this,
                [this](int choice) {
                    pickerChoice_ = choice == 1 ? 1 : 0;
                    ColorPickerDialog::setPickerPreference(pickerChoice_);
                    for (const DocEntry* entry : docs_) {
                        if (entry && entry->view) {
                            pictura::set_picker_choice(*entry->view, pickerChoice_);
                        }
                    }
                    saveSession();
                });
        connect(preferencesDialog_, &PreferencesDialog::gpuComputeChanged, this,
                &PicturaMainWindow::applyGpuComputePreference);
        connect(preferencesDialog_, &PreferencesDialog::typePlaceholderTextChanged, this,
                [this](bool on) {
                    if (tools_) {
                        tools_->setTypePlaceholderText(on);
                    }
                    saveSession();
                });
        connect(preferencesDialog_, &PreferencesDialog::typeShowAsianOptionsChanged, this,
                [this](bool on) {
                    typeShowAsianOptions_ = on;
                    if (characterPanel_) {
                        characterPanel_->setShowAsianOptions(on);
                    }
                    saveSession();
                });
        connect(preferencesDialog_, &PreferencesDialog::typeMiddleEasternChanged, this,
                [this](bool on) {
                    typeMiddleEastern_ = on;
                    if (paragraphPanel_) {
                        paragraphPanel_->setMiddleEastern(on);
                    }
                    saveSession();
                });
        connect(preferencesDialog_, &PreferencesDialog::typeMissingGlyphProtectionChanged, this,
                [this](bool on) {
                    typeMissingGlyphProtection_ = on;
                    pictura::type_api::setMissingGlyphProtection(on);
                    for (const DocEntry* entry : docs_) {
                        if (entry && entry->view) {
                            pictura::type_api::refreshMissingGlyphProtection(*entry->view);
                        }
                    }
                    saveSession();
                });
    }
    preferencesDialog_->openOn(page);
}

} // namespace pictura
