#include "gpu_probe.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QSaveFile>

#include <cstddef>

#include "pictura_app/src/gpu_probe_bridge.cxxqt.h"
#include "session.h"

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#endif

namespace pictura {

namespace {

constexpr qint64 kMaxCacheBytes = 64 * 1024;
const QString kDx12 = QStringLiteral("dx12");
const QString kVulkan = QStringLiteral("vulkan");

QString gSelectedBackend;
bool gChainResetPath = false;

QString nowIso()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
}

QString stateDirectory()
{
    return QFileInfo(pictura::sessionFilePath()).absolutePath();
}

QString stateName(GpuBackendState state)
{
    switch (state) {
    case GpuBackendState::Ok:
        return QStringLiteral("ok");
    case GpuBackendState::Failed:
        return QStringLiteral("failed");
    case GpuBackendState::Crashed:
        return QStringLiteral("crashed");
    case GpuBackendState::Unknown:
        break;
    }
    return QStringLiteral("unknown");
}

GpuBackendState parseState(const QString& value)
{
    if (value == QStringLiteral("ok")) {
        return GpuBackendState::Ok;
    }
    if (value == QStringLiteral("failed")) {
        return GpuBackendState::Failed;
    }
    if (value == QStringLiteral("crashed")) {
        return GpuBackendState::Crashed;
    }
    return GpuBackendState::Unknown;
}

QString backendName(int code)
{
    switch (code) {
    case 0:
        return kVulkan;
    case 1:
        return kDx12;
    default:
        return QString();
    }
}

int backendCode(const QString& name)
{
    if (name == kVulkan) {
        return 0;
    }
    if (name == kDx12) {
        return 1;
    }
    return -1;
}

QJsonObject readObject(const QString& path, qint64 maxBytes = kMaxCacheBytes)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > maxBytes) {
        return {};
    }
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        return {};
    }
    return doc.object();
}

bool writeObject(const QString& path, const QJsonObject& object)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
    return file.commit();
}

bool pidAlive(qint64 pid)
{
    if (pid <= 0) {
        return false;
    }
#ifdef Q_OS_WIN
    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
    if (!handle) {
        return false;
    }
    DWORD code = 0;
    const bool alive = GetExitCodeProcess(handle, &code) && code == STILL_ACTIVE;
    CloseHandle(handle);
    return alive;
#else
    return ::kill(pid_t(pid), 0) == 0 || errno == EPERM;
#endif
}

QString executablePath()
{
#ifdef Q_OS_WIN
    wchar_t buffer[32768];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, 32768);
    return QString::fromWCharArray(buffer, int(length));
#else
    return QString();
#endif
}

} // namespace

QString gpuBackendFilePath()
{
    return stateDirectory() + QStringLiteral("/gpu-backend.json");
}

QString gpuMarkerPath()
{
    return stateDirectory() + QStringLiteral("/gpu-init.marker");
}

GpuBackendCache loadGpuBackendCache(const QString& appVersion)
{
    const QString version =
        appVersion.isEmpty() ? QString::fromLatin1(kGpuCacheAppVersion) : appVersion;
    GpuBackendCache cache;
    cache.appVersion = version;

    const QJsonObject object = readObject(gpuBackendFilePath());
    if (object.isEmpty()) {
        return cache;
    }
    if (object.value(QStringLiteral("schemaVersion")).toInt() > 1) {
        return cache;
    }
    if (object.value(QStringLiteral("appVersion")).toString() != version) {
        return cache;
    }

    const QJsonObject backends = object.value(QStringLiteral("backends")).toObject();
    for (auto it = backends.constBegin(); it != backends.constEnd(); ++it) {
        if (!it.value().isObject()) {
            continue;
        }
        const QJsonObject entry = it.value().toObject();
        GpuBackendEntry value;
        value.state = parseState(entry.value(QStringLiteral("state")).toString());
        value.updatedAt = entry.value(QStringLiteral("updatedAt")).toString();
        value.detail = entry.value(QStringLiteral("detail")).toString();
        cache.backends.insert(it.key(), value);
    }
    return cache;
}

bool saveGpuBackendCache(const GpuBackendCache& cache)
{
    QJsonObject backends;
    for (auto it = cache.backends.constBegin(); it != cache.backends.constEnd(); ++it) {
        QJsonObject entry;
        entry.insert(QStringLiteral("state"), stateName(it.value().state));
        if (!it.value().updatedAt.isEmpty()) {
            entry.insert(QStringLiteral("updatedAt"), it.value().updatedAt);
        }
        if (!it.value().detail.isEmpty()) {
            entry.insert(QStringLiteral("detail"), it.value().detail);
        }
        backends.insert(it.key(), entry);
    }
    QJsonObject object;
    object.insert(QStringLiteral("schemaVersion"), 1);
    object.insert(QStringLiteral("appVersion"),
                  cache.appVersion.isEmpty() ? QString::fromLatin1(kGpuCacheAppVersion)
                                             : cache.appVersion);
    object.insert(QStringLiteral("backends"), backends);
    return writeObject(gpuBackendFilePath(), object);
}

void clearGpuBackendCache()
{
    QFile::remove(gpuBackendFilePath());
}

GpuBackendState gpuProbeOutcomeToState(int outcome)
{
    switch (outcome) {
    case 0:
        return GpuBackendState::Ok;
    case 2:
        return GpuBackendState::Crashed;
    default:
        return GpuBackendState::Failed;
    }
}

GpuBackendState gpuBackendState(const GpuBackendCache& cache, const QString& backend)
{
    return cache.backends.value(backend).state;
}

void recoverGpuInitMarker(GpuBackendCache& cache)
{
    const QString path = gpuMarkerPath();
    if (!QFileInfo::exists(path)) {
        return;
    }
    const QJsonObject marker = readObject(path, 4096);
    if (marker.isEmpty()) {
        QFile::remove(path);
        return;
    }
    if (marker.value(QStringLiteral("schemaVersion")).toInt() > 1) {
        QFile::remove(path);
        return;
    }
    const QString version = QString::fromLatin1(kGpuCacheAppVersion);
    if (marker.value(QStringLiteral("appVersion")).toString() != version) {
        QFile::remove(path);
        return;
    }
    const QString backend = marker.value(QStringLiteral("backend")).toString();
    if (backendCode(backend) < 0) {
        QFile::remove(path);
        return;
    }
    const qint64 pid = marker.value(QStringLiteral("pid")).toVariant().toLongLong();
    if (pidAlive(pid)) {
        // A concurrent instance owns this marker; leave it for that process.
        return;
    }
    GpuBackendEntry& entry = cache.backends[backend];
    entry.state = GpuBackendState::Crashed;
    entry.updatedAt = nowIso();
    entry.detail = QStringLiteral("crashed during parent init");
    saveGpuBackendCache(cache);
    QFile::remove(path);
}

bool beginDeviceInit()
{
    return beginDeviceInit(gSelectedBackend);
}

bool beginDeviceInit(const QString& backend)
{
    if (backend.isEmpty()) {
        return false;
    }
    const QJsonObject existing = readObject(gpuMarkerPath(), 4096);
    if (!existing.isEmpty()) {
        const qint64 existingPid =
            existing.value(QStringLiteral("pid")).toVariant().toLongLong();
        if (existingPid > 0 && existingPid != QCoreApplication::applicationPid()
            && pidAlive(existingPid)) {
            // A live marker belongs to a concurrent instance; do not clobber
            // its crash record.
            return false;
        }
    }
    QJsonObject marker;
    marker.insert(QStringLiteral("schemaVersion"), 1);
    marker.insert(QStringLiteral("appVersion"), QString::fromLatin1(kGpuCacheAppVersion));
    marker.insert(QStringLiteral("backend"), backend);
    marker.insert(QStringLiteral("pid"), double(QCoreApplication::applicationPid()));
    return writeObject(gpuMarkerPath(), marker);
}

void endDeviceInit(bool ok)
{
    endDeviceInit(ok, gSelectedBackend);
}

void endDeviceInit(bool ok, const QString& backend)
{
    if (backend.isEmpty()) {
        return;
    }
    GpuBackendCache cache = loadGpuBackendCache();
    GpuBackendEntry& entry = cache.backends[backend];
    entry.state = ok ? GpuBackendState::Ok : GpuBackendState::Failed;
    entry.updatedAt = nowIso();
    if (!ok) {
        entry.detail = QStringLiteral("parent device init failed");
    }
    saveGpuBackendCache(cache);
    QFile::remove(gpuMarkerPath());
}

QString selectGpuBackend(GpuBackendCache& cache,
                         const std::function<GpuProbeResult(const QString&)>& probe)
{
    for (const int code : pictura::gpu_candidates()) {
        const QString backend = backendName(code);
        if (backend.isEmpty()) {
            continue;
        }
        const GpuBackendState state = gpuBackendState(cache, backend);
        if (state == GpuBackendState::Ok) {
            return backend;
        }
        if (state == GpuBackendState::Crashed) {
            continue;
        }
        const GpuProbeResult result = probe(backend);
        const GpuBackendState outcome = gpuProbeOutcomeToState(result.code);
        GpuBackendEntry& entry = cache.backends[backend];
        entry.state = outcome;
        entry.updatedAt = nowIso();
        entry.detail = result.detail.left(512);
        saveGpuBackendCache(cache);
        if (outcome == GpuBackendState::Ok) {
            return backend;
        }
    }
    return QString();
}

void prepareGpuBackend()
{
#ifndef Q_OS_WIN
    // Linux keeps the direct Vulkan path: no child probe, cache, or marker.
    return;
#else
    GpuBackendCache cache = loadGpuBackendCache();
    recoverGpuInitMarker(cache);

    const QString executable = executablePath();
    const QByteArray executableUtf8 = executable.toUtf8();
    const QString chosen = selectGpuBackend(cache, [&](const QString& backend) {
        const int code = backendCode(backend);
        if (code < 0) {
            return GpuProbeResult{1, QStringLiteral("unknown backend")};
        }
        const auto reply = pictura::gpu_probe(
            code, rust::Str(executableUtf8.constData(), size_t(executableUtf8.size())));
        return GpuProbeResult{
            reply.code, QString::fromUtf8(reply.detail.data(), int(reply.detail.size()))};
    });

    if (!chosen.isEmpty() && pictura::gpu_set_backend(backendCode(chosen))) {
        gSelectedBackend = chosen;
        gChainResetPath = false;
        pictura::gpu_set_safe_mode(false);
        return;
    }

    // Nothing was pinned: fall to the CPU. Keep the reset path only when a
    // candidate is cached `crashed` (a driver crash or a failed validation);
    // a clean no-adapter machine must report `CPU (no GPU)` and leave the
    // Preferences toggle disabled.
    gSelectedBackend.clear();
    pictura::gpu_set_backend(2);
    gChainResetPath = false;
    for (const int code : pictura::gpu_candidates()) {
        const QString name = backendName(code);
        if (!name.isEmpty() && gpuBackendState(cache, name) == GpuBackendState::Crashed) {
            gChainResetPath = true;
            break;
        }
    }
    pictura::gpu_set_safe_mode(gChainResetPath);
#endif
}

QString selectedGpuBackend()
{
    return gSelectedBackend;
}

bool gpuChainHasResetPath()
{
    return gChainResetPath;
}

QStringList gpuCandidateNames()
{
    QStringList names;
    for (const int code : pictura::gpu_candidates()) {
        const QString name = backendName(code);
        if (!name.isEmpty()) {
            names.append(name);
        }
    }
    return names;
}

QString activeBackendLabel(bool gpuCompute)
{
    const rust::String label = pictura::gpu_active_backend_label(gpuCompute);
    return QString::fromUtf8(label.data(), int(label.size()));
}

QString aboutText(const QString& backendLabel)
{
    return QStringLiteral("Renderer: %1").arg(backendLabel);
}

QString takeGpuFailureNotice()
{
    const rust::String notice = pictura::gpu_take_failure_notice();
    return QString::fromUtf8(notice.data(), int(notice.size()));
}

int runGpuProbeChild(const char* backend)
{
    const QString name = QString::fromLatin1(backend ? backend : "");
    const int code = backendCode(name);
    if (code < 0) {
        return 1;
    }
#ifdef Q_OS_WIN
    const UINT mode = SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX;
    SetErrorMode(mode);
    DWORD previous = 0;
    SetThreadErrorMode(mode, &previous);
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS*) -> LONG {
        // Loader-lock safe: no CRT, allocation, or loader call. Exit 2 so the
        // parent classifies the crash as `crashed`, not a clean failure.
        TerminateProcess(GetCurrentProcess(), 2);
        return EXCEPTION_EXECUTE_HANDLER;
    });
#endif
    return pictura::gpu_probe_child(code);
}

} // namespace pictura
