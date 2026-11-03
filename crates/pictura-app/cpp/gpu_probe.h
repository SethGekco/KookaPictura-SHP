#pragma once

#include <functional>

#include <QtCore/QHash>
#include <QtCore/QString>
#include <QtCore/QStringList>

namespace pictura {

// The per-backend outcome persisted next to state.json.
enum class GpuBackendState { Unknown, Ok, Failed, Crashed };

struct GpuBackendEntry {
    GpuBackendState state = GpuBackendState::Unknown;
    QString updatedAt;
    QString detail;
};

struct GpuBackendCache {
    QString appVersion;
    QHash<QString, GpuBackendEntry> backends;
};

#ifdef PICTURA_VERSION
inline constexpr const char* kGpuCacheAppVersion = PICTURA_VERSION;
#else
inline constexpr const char* kGpuCacheAppVersion = "unknown";
#endif

// $XDG_STATE_HOME/kooka-pictura/gpu-backend.json, beside state.json.
QString gpuBackendFilePath();
// $XDG_STATE_HOME/kooka-pictura/gpu-init.marker, beside state.json.
QString gpuMarkerPath();

// A missing, corrupt, oversized, or foreign-version store loads as all
// unknown. `appVersion` is a parameter so tests can drive a version bump.
GpuBackendCache loadGpuBackendCache(const QString& appVersion = QString());
bool saveGpuBackendCache(const GpuBackendCache& cache);
void clearGpuBackendCache();

// 0 = ok, 1 = failed, 2 = crashed (validation failure, crash, or timeout).
// Anything else is a clean failure.
GpuBackendState gpuProbeOutcomeToState(int outcome);
GpuBackendState gpuBackendState(const GpuBackendCache& cache, const QString& backend);

// Promote a marker left by a previous launch to `crashed`. A marker whose PID
// is still alive belongs to a concurrent instance and is left alone.
void recoverGpuInitMarker(GpuBackendCache& cache);

// Marker lifecycle. The backend is the one selected by prepareGpuBackend.
bool beginDeviceInit();
bool beginDeviceInit(const QString& backend);
void endDeviceInit(bool ok);
void endDeviceInit(bool ok, const QString& backend);

// The outcome of probing one backend: 0 = ok, 1 = failed, 2 = crashed
// (validation failure, crash, or timeout), plus its diagnostic detail.
struct GpuProbeResult {
    int code = 1;
    QString detail;
};

// Run the chain for this launch. `probe` returns the outcome and detail for one
// backend. Returns the chosen backend id, or an empty string for CPU.
QString selectGpuBackend(GpuBackendCache& cache,
                         const std::function<GpuProbeResult(const QString&)>& probe);

// Windows: recover the marker, select and pin a backend, and set safe mode on
// exhaustion. Linux: a no-op (the direct Vulkan path stays).
void prepareGpuBackend();

// The backend selected for this launch, or an empty string on Linux/CPU.
QString selectedGpuBackend();

// True when the chain ended in safe mode, so Preferences keeps the reset path.
bool gpuChainHasResetPath();

// The ordered candidate backend ids for this launch (`dx12`, `vulkan`).
QStringList gpuCandidateNames();

// The five-state label for the status bar and About.
QString activeBackendLabel(bool gpuCompute);
QString aboutText(const QString& backendLabel);

// The message about a GPU failure at run time (out of video memory, a lost
// device) that turned the GPU off for this session, or empty. Taken once.
QString takeGpuFailureNotice();

// Child mode: `--gpu-probe <dx12|vulkan>`. Suppresses the Windows dialog and
// calls the Rust probe. Returns 0 on success, 1 when no adapter exists, 2 when
// a device exists but fails validation.
int runGpuProbeChild(const char* backend);

} // namespace pictura
