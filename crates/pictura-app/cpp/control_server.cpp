#include "control_server.h"
#include "control_scope.h"
#include "control_server_support.h"
#include "shp_support.h"

#include "api/document_api.h"
#include "api/history_api.h"

#include <QtCore/QBuffer>
#include <QtCore/QDir>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonParseError>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QRandomGenerator>
#include <QtCore/QSet>
#include <QtCore/QStandardPaths>
#include <QtGui/QAction>
#include <QtGui/QImage>
#include <QtNetwork/QLocalServer>
#include <QtNetwork/QLocalSocket>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMenu>

#include <cmath>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "commands.h"
#include "dialogs.h"
#include "filter_commands.h"
#include "frame.h"
#include "image_view.h"
#include "panels/panel_column.h"
#include "tools.h"

#include "pictura_app/src/cxxqt_object.cxxqt.h"

namespace pictura {

namespace {

// Commands whose real menu handler can open a modal dialog (`QFileDialog`,
// `QInputDialog`, `QMessageBox`, the New Document / Preferences / File Info
// dialogs) or reach the unsaved prompt (`close`/`revert`/`exit` call
// `askUnsaved`). `dispatch_command` refuses these because dispatch is
// synchronous on the GUI thread, so a modal would hang the socket forever.
//
// ponytail: hand-maintained denylist — a new modal-opening command must be
// added here or `dispatch_command` will hang the GUI thread on it. When
// uncertain, add it.
bool isModalCommand(const QString& id)
{
    static const QSet<QString> kModal = {
        QString::fromLatin1(command_ids::FileNew),
        QString::fromLatin1(command_ids::FileOpen),
        QString::fromLatin1(command_ids::FileOpenAsSmartObject),
        QString::fromLatin1(command_ids::FileOpenAs),
        QString::fromLatin1(command_ids::FilePlace),
        QString::fromLatin1(command_ids::FileInfo),
        QString::fromLatin1(command_ids::FileSave),
        QString::fromLatin1(command_ids::FileSaveAs),
        QString::fromLatin1(command_ids::FileExportAs),
        QString::fromLatin1(command_ids::FileQuickExportPng),
        QString::fromLatin1(command_ids::FileRevert),
        QString::fromLatin1(command_ids::FileClose),
        QString::fromLatin1(command_ids::FileCloseAll),
        QString::fromLatin1(command_ids::FileExit),
        QString::fromLatin1(command_ids::EditAssignProfile),
        QString::fromLatin1(command_ids::EditConvertProfile),
        QString::fromLatin1(command_ids::EditColorSettings),
        QString::fromLatin1(command_ids::EditPreferencesGeneral),
        QString::fromLatin1(command_ids::EditPreferencesInterface),
        QString::fromLatin1(command_ids::EditPreferencesPerformance),
        QString::fromLatin1(command_ids::EditPreferencesUnits),
        QString::fromLatin1(command_ids::EditPreferencesGuides),
        QString::fromLatin1(command_ids::EditPreferencesType),
        QString::fromLatin1(command_ids::EditPreferencesAgentControl),
        QString::fromLatin1(command_ids::ViewNewGuide),
        // Entering full screen can show the CS6 advisory (a modal message box).
        QString::fromLatin1(command_ids::ViewScreenModeFullWithMenuBar),
        QString::fromLatin1(command_ids::ViewScreenModeFull),
        QString::fromLatin1(command_ids::LayerNewLayer),
        QString::fromLatin1(command_ids::LayerNewGroup),
        QString::fromLatin1(command_ids::LayerNewGroupFromLayers),
        QString::fromLatin1(command_ids::LayerSmartObjectReplaceContents),
        QString::fromLatin1(command_ids::LayerSmartObjectExportContents),
        QString::fromLatin1(command_ids::SelectModifyBorder),
        QString::fromLatin1(command_ids::SelectModifySmooth),
        QString::fromLatin1(command_ids::SelectModifyExpand),
        QString::fromLatin1(command_ids::SelectModifyContract),
        QString::fromLatin1(command_ids::SelectModifyFeather),
        QString::fromLatin1(command_ids::SelectSave),
        QString::fromLatin1(command_ids::SelectLoad),
        QString::fromLatin1(command_ids::HelpAbout),
    };
    if (kModal.contains(id)) {
        return true;
    }
    // Every parameterised Filter entry opens its dialog and blocks the GUI
    // thread the same way; derive them from the command table so a new filter
    // cannot be forgotten here.
    static const QSet<QString> kFilterDialogs = [] {
        QSet<QString> ids;
        for (const FilterCommandSpec& spec : filterCommands()) {
            if (!spec.params.isEmpty()) {
                ids.insert(commandIdForPath(spec.path));
            }
        }
        return ids;
    }();
    return kFilterDialogs.contains(id);
}

QString screenModeName(PicturaMainWindow::ScreenMode mode)
{
    switch (mode) {
    case PicturaMainWindow::ScreenMode::FullWithMenuBar:
        return QStringLiteral("fullWithMenuBar");
    case PicturaMainWindow::ScreenMode::Full:
        return QStringLiteral("full");
    case PicturaMainWindow::ScreenMode::Standard:
    default:
        return QStringLiteral("standard");
    }
}

} // namespace

namespace control_detail {

QString documentId(const PictureView* view)
{
    return QString::number(quintptr(view), 16);
}

QJsonObject documentInfo(PicturaMainWindow* frame, int index)
{
    PictureView* view = frame->viewAt(index);
    ImageView* canvas = frame->canvasAt(index);
    QJsonObject info;
    info.insert(QStringLiteral("index"), index);
    info.insert(QStringLiteral("id"), documentId(view));
    info.insert(QStringLiteral("name"), frame->documentName(index));
    info.insert(QStringLiteral("path"), frame->documentPath(index));
    info.insert(QStringLiteral("width"), view ? view->document_width() : 0);
    info.insert(QStringLiteral("height"), view ? view->document_height() : 0);
    info.insert(QStringLiteral("mode"), view ? document_api::documentMode(*view) : QString());
    info.insert(QStringLiteral("depth"), view ? document_api::documentDepthBits(*view) : 0);
    info.insert(QStringLiteral("dirty"), frame->isDocumentDirty(index));
    info.insert(QStringLiteral("layers"), view ? view->layer_count() : 0);
    info.insert(QStringLiteral("selection_px"), view ? view->selection_count() : 0);
    info.insert(QStringLiteral("zoom"), canvas ? canvas->zoom() : 0.0);
    return info;
}

} // namespace control_detail

using control_detail::documentInfo;

ControlServer::ControlServer(PicturaMainWindow* frame, const QString& socketPath,
                             QObject* parent)
    : QObject(parent)
    , frame_(frame)
    , socketPath_(socketPath.isEmpty() ? defaultSocketPath() : socketPath)
{
    connect(frame_, &PicturaMainWindow::documentOpenFinished, this,
            [this](quint64 token, int outcome, const QString&) {
                onOpenFinished(token, outcome);
            });
}

ControlServer::~ControlServer()
{
    shutdown();
}

QString ControlServer::defaultSocketPath()
{
#ifdef _WIN32
    // A Windows QLocalServer is a named pipe, not a filesystem path, and there
    // is no getuid(). XDG_RUNTIME_DIR is a POSIX convention and must not shape
    // the name. Scope the pipe name to the user's temp directory so two users
    // never collide on one endpoint.
    QString scope = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    if (scope.isEmpty()) {
        scope = QDir::tempPath();
    }
    return QStringLiteral("pictura-control-%1")
        .arg(QString::number(static_cast<qulonglong>(qHash(scope)), 16));
#else
    const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (!runtime.isEmpty()) {
        return runtime + QStringLiteral("/pictura-control.sock");
    }
    return QDir::tempPath() + QStringLiteral("/pictura-control-%1.sock").arg(::getuid());
#endif
}

bool ControlServer::listen()
{
#ifndef _WIN32
    // Never clobber a non-socket path: only remove a stale file that stat
    // confirms is a Unix-domain socket. A Windows named pipe has no filesystem
    // entry to protect.
    struct stat existing{};
    if (::stat(socketPath_.toLocal8Bit().constData(), &existing) == 0
        && !S_ISSOCK(existing.st_mode)) {
        return false;
    }
#endif
    // Before removeServer: a live holder's socket must survive this attempt.
    if (!acquireLock()) {
        return false;
    }
    QLocalServer::removeServer(socketPath_);
    server_ = new QLocalServer(this);
    server_->setSocketOptions(QLocalServer::UserAccessOption);
    if (!server_->listen(socketPath_)) {
        delete server_;
        server_ = nullptr;
        releaseLock();
        return false;
    }
    if (!writeTokenFile()) {
        shutdown();
        return false;
    }
    connect(server_, &QLocalServer::newConnection, this, &ControlServer::onNewConnection);
    return true;
}

bool ControlServer::acquireLock()
{
#ifdef _WIN32
    // A stale time of 0 leaves a dead holder to the PID check alone.
    lock_ = std::make_unique<QLockFile>(lockPath());
    lock_->setStaleLockTime(0);
    if (!lock_->tryLock(0)) {
        lock_.reset();
        return false;
    }
    return true;
#else
    // Not QLockFile: Flatpak instances have separate PID namespaces, so a recorded
    // PID says nothing about the holder. The file stays, because unlinking a
    // locked file would let a second holder lock a new one.
    const int fd = ::open(lockPath().toLocal8Bit().constData(),
                          O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        return false;
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        return false;
    }
    lockFd_ = fd;
    return true;
#endif
}

void ControlServer::releaseLock()
{
#ifdef _WIN32
    lock_.reset();
#else
    if (lockFd_ >= 0) {
        ::close(lockFd_);
        lockFd_ = -1;
    }
#endif
}

QString ControlServer::tokenPath() const
{
    return tokenPathFor(socketPath_);
}

QString ControlServer::tokenPathFor(const QString& endpoint)
{
#ifdef _WIN32
    // A named pipe has no directory; the token lives in the user's temp dir.
    return QDir::tempPath() + QStringLiteral("/") + endpoint + QStringLiteral(".token");
#else
    return endpoint + QStringLiteral(".token");
#endif
}

QString ControlServer::lockPath() const
{
#ifdef _WIN32
    return QDir::tempPath() + QStringLiteral("/") + socketPath_ + QStringLiteral(".lock");
#else
    return socketPath_ + QStringLiteral(".lock");
#endif
}

bool ControlServer::writeTokenFile()
{
    quint32 words[8];
    QRandomGenerator::system()->fillRange(words);
    token_ = QByteArray(reinterpret_cast<const char*>(words), sizeof(words)).toHex();
    const QString path = tokenPath();
#ifndef _WIN32
    const QByteArray native = path.toLocal8Bit();
    // Replace a stale token (or anything else planted at the name) by removing
    // the entry itself, then create the file fresh and owner-only; O_EXCL and
    // O_NOFOLLOW refuse a symlink raced into place.
    struct stat existing{};
    if (::lstat(native.constData(), &existing) == 0) {
        if (S_ISDIR(existing.st_mode) || ::unlink(native.constData()) != 0) {
            return false;
        }
    }
    const int fd =
        ::open(native.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        return false;
    }
    const bool written = ::write(fd, token_.constData(), size_t(token_.size())) == token_.size();
    ::close(fd);
#else
    QFile file(path);
    const bool written = file.open(QIODevice::WriteOnly | QIODevice::Truncate)
                         && file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
                         && file.write(token_) == token_.size();
#endif
    tokenWritten_ = true;
    return written;
}

bool ControlServer::authorized(const QJsonObject& request) const
{
    const QByteArray offered = request.value(QStringLiteral("token")).toString().toLatin1();
    if (token_.isEmpty() || offered.size() != token_.size()) {
        return false;
    }
    // Constant-time: the comparison does not stop at the first wrong byte.
    unsigned char difference = 0;
    for (qsizetype i = 0; i < token_.size(); ++i) {
        difference |= static_cast<unsigned char>(offered[i] ^ token_[i]);
    }
    return difference == 0;
}

void ControlServer::shutdown()
{
    queue_.clear();
    queuedCounts_.clear();
    buffers_.clear();
    takePending();
    opening_ = {};
    parked_ = false;
    if (tokenWritten_) {
        QFile::remove(tokenPath());
        tokenWritten_ = false;
    }
    if (server_) {
        server_->close();
        delete server_;
        server_ = nullptr;
        // Only unlink the socket this server created. `QLocalServer::removeServer`
        // blindly QFile::removes the path, so calling it after a failed listen()
        // (e.g. `--control-socket` pointed at a regular file) would delete the
        // user's file. Re-stat and require a socket; a repeated shutdown is a
        // no-op because `server_` is already null.
#ifndef _WIN32
        struct stat current{};
        if (::stat(socketPath_.toLocal8Bit().constData(), &current) == 0
            && S_ISSOCK(current.st_mode)) {
            QLocalServer::removeServer(socketPath_);
        }
#else
        QLocalServer::removeServer(socketPath_);
#endif
    }
    releaseLock();
}

void ControlServer::onNewConnection()
{
    if (!server_) {
        return;
    }
    while (QLocalSocket* socket = server_->nextPendingConnection()) {
        if (buffers_.size() >= kMaxConnections) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        buffers_.insert(socket, QByteArray());
        connect(socket, &QLocalSocket::readyRead, this, &ControlServer::onReadyRead);
        connect(socket, &QLocalSocket::disconnected, this, &ControlServer::onDisconnected);
        emit clientsChanged();
    }
}

void ControlServer::onReadyRead()
{
    QLocalSocket* socket = qobject_cast<QLocalSocket*>(sender());
    if (!socket || !buffers_.contains(socket)) {
        return;
    }
    buffers_[socket].append(socket->readAll());
    // ponytail: 1 MiB per-socket ceiling; reject an unterminated request before
    // it can grow without bound rather than buffering forever.
    {
        const auto it = buffers_.find(socket);
        if (it != buffers_.end() && it->size() > kMaxRequestBytes && it->indexOf('\n') < 0) {
            writeLine(socket, error(QStringLiteral("bad_request"),
                                    QStringLiteral("request exceeds maximum size")));
            buffers_.remove(socket);
            socket->disconnectFromServer();
            return;
        }
    }
    for (;;) {
        const auto it = buffers_.find(socket);
        if (it == buffers_.end()) {
            break;
        }
        const int newline = it->indexOf('\n');
        if (newline < 0) {
            break;
        }
        const QByteArray line = it->left(newline);
        it->remove(0, newline + 1);
        int& queued = queuedCounts_[socket];
        if (queued >= kMaxQueuedPerConnection) {
            refuseLine(socket, line);
            continue;
        }
        ++queued;
        queue_.append({socket, line});
    }
    // Queued, not called: a request can spin a nested event loop, and a
    // disconnect in there frees the socket while this readyRead is on the stack.
    QMetaObject::invokeMethod(this, &ControlServer::pump, Qt::QueuedConnection);
}

void ControlServer::pump()
{
    if (running_ || parked_) {
        return;
    }
    running_ = true;
    while (!queue_.isEmpty() && !parked_) {
        const QueuedLine next = queue_.takeFirst();
        // A client that left needs no answer, and its queued work is unwanted.
        if (!next.socket || !buffers_.contains(next.socket.data())) {
            continue;
        }
        --queuedCounts_[next.socket.data()];
        handleLine(next.socket, next.line);
    }
    running_ = false;
}

void ControlServer::refuseLine(QLocalSocket* socket, const QByteArray& line)
{
    QJsonObject response =
        error(QStringLiteral("refused"), QStringLiteral("too many requests are waiting"));
    const QJsonValue id = QJsonDocument::fromJson(line).object().value(QStringLiteral("id"));
    if (id.isDouble()) {
        response.insert(QStringLiteral("id"), id);
    }
    writeLine(socket, response);
}

void ControlServer::onDisconnected()
{
    QLocalSocket* socket = qobject_cast<QLocalSocket*>(sender());
    if (!socket) {
        return;
    }
    buffers_.remove(socket);
    queuedCounts_.remove(socket);
    if (pending_.socket == socket && !pending_.target.isEmpty()) {
        takePending();
        QMetaObject::invokeMethod(this, &ControlServer::pump, Qt::QueuedConnection);
    }
    socket->deleteLater();
    emit clientsChanged();
    if (buffers_.isEmpty()) {
        emit lastClientLeft();
    }
}

void ControlServer::handleLine(const QPointer<QLocalSocket>& socket, const QByteArray& line)
{
    if (line.size() > kMaxRequestBytes) {
        writeLine(socket, error(QStringLiteral("bad_request"),
                                QStringLiteral("request exceeds maximum size")));
        return;
    }
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        writeLine(socket, error(QStringLiteral("bad_request"),
                                QStringLiteral("request is not a JSON object")));
        return;
    }
    const QJsonObject request = document.object();

    // `id`, when present, must be an integer; echo it on early errors.
    const bool hasId = request.contains(QStringLiteral("id"));
    const QJsonValue idValue = request.value(QStringLiteral("id"));
    bool idValid = true;
    int id = 0;
    if (hasId) {
        if (idValue.isDouble()) {
            const double asDouble = idValue.toDouble();
            if (asDouble != std::floor(asDouble) || asDouble < -2147483648.0
                || asDouble > 2147483647.0) {
                idValid = false;
            } else {
                id = static_cast<int>(asDouble);
            }
        } else {
            idValid = false;
        }
    }
    const bool methodValid = request.value(QStringLiteral("method")).isString();
    const bool paramsValid =
        !request.contains(QStringLiteral("params")) || request.value(QStringLiteral("params")).isObject();
    if (!idValid || !methodValid || !paramsValid) {
        QJsonObject response = error(QStringLiteral("bad_request"),
                                     QStringLiteral("id, method, and params have invalid types"));
        if (hasId && idValid) {
            response.insert(QStringLiteral("id"), id);
        }
        writeLine(socket, response);
        return;
    }
    if (!authorized(request)) {
        QJsonObject response = error(QStringLiteral("unauthorized"),
                                     QStringLiteral("missing or wrong session token"));
        if (hasId) {
            response.insert(QStringLiteral("id"), id);
        }
        writeLine(socket, response);
        return;
    }
    const QString method = request.value(QStringLiteral("method")).toString();
    const QJsonObject params = request.value(QStringLiteral("params")).toObject();
    serve(socket, id, method, params);
}

void ControlServer::writeLine(QLocalSocket* socket, const QJsonObject& response)
{
    // A nested event loop in the request can have freed the socket.
    if (!socket) {
        return;
    }
    socket->write(QJsonDocument(response).toJson(QJsonDocument::Compact));
    socket->write("\n");
    socket->flush();
}

QJsonObject ControlServer::ok(const QJsonObject& result) const
{
    return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("result"), result}};
}

QJsonObject ControlServer::error(const QString& code, const QString& message) const
{
    return QJsonObject{
        {QStringLiteral("ok"), false},
        {QStringLiteral("error"),
         QJsonObject{{QStringLiteral("code"), code}, {QStringLiteral("message"), message}}}};
}

QJsonObject ControlServer::dispatch(const QString& method, const QJsonObject& params)
{
    const UnsavedChoice cancel = UnsavedChoice::Cancel;
    const ControlScope scope(policy_.live ? &cancel : nullptr);
    const ShpSupport::Quiet shpQuiet; // KookaPictura-SHP fork: no size-limit dialogs
    const QJsonObject response = route(method, params);
    if (scope.cancelledAny()) {
        return error(QStringLiteral("refused"),
                     QStringLiteral("request reached a dialog: %1").arg(scope.cancelled()));
    }
    if (!shpQuiet.refusal().isEmpty()) {
        return error(QStringLiteral("refused"), shpQuiet.refusal());
    }
    return response;
}

const QList<ControlServer::MethodRow>& ControlServer::methodTable()
{
    // `document` and `pointer` refine their tier by op in tierOf().
    static const QList<MethodRow> table = {
        {"status", &ControlServer::methodStatus, ControlTier::Read},
        {"get_pixel", &ControlServer::methodGetPixel, ControlTier::Read},
        {"list_layers", &ControlServer::methodListLayers, ControlTier::Read},
        {"list_commands", &ControlServer::methodListCommands, ControlTier::Read},
        {"dispatch_command", &ControlServer::methodDispatchCommand, ControlTier::Edit},
        {"document", &ControlServer::methodDocument, ControlTier::Read},
        {"edit", &ControlServer::methodEdit, ControlTier::Edit},
        {"set_unsaved_policy", &ControlServer::methodSetUnsavedPolicy, ControlTier::Admin},
        {"screenshot", &ControlServer::methodScreenshot, ControlTier::Read},
        {"ui_tree", &ControlServer::methodUiTree, ControlTier::Read},
        {"layer_thumbnail", &ControlServer::methodLayerThumbnail, ControlTier::Read},
        {"set_tool", &ControlServer::methodSetTool, ControlTier::Edit},
        {"tool_options", &ControlServer::methodToolOptions, ControlTier::Edit},
        {"selection", &ControlServer::methodSelection, ControlTier::Edit},
        {"filter", &ControlServer::methodFilter, ControlTier::Edit},
        {"adjustment", &ControlServer::methodAdjustment, ControlTier::Edit},
        {"layer_op", &ControlServer::methodLayerOp, ControlTier::Edit},
        {"set_gpu_compute", &ControlServer::methodSetGpuCompute, ControlTier::Admin},
        {"metrics", &ControlServer::methodMetrics, ControlTier::Read},
        {"pointer", &ControlServer::methodPointer, ControlTier::Admin},
        {"key", &ControlServer::methodKey, ControlTier::Admin},
        {"list_filters", &ControlServer::methodListFilters, ControlTier::Read},
        {"list_adjustments", &ControlServer::methodListAdjustments, ControlTier::Read},
        {"layer_tree", &ControlServer::methodLayerTree, ControlTier::Read},
        {"adjust_apply", &ControlServer::methodAdjustApply, ControlTier::Edit},
        {"run_command", &ControlServer::methodRunCommand, ControlTier::Edit},
        {"set_active_layer", &ControlServer::methodSetActiveLayer, ControlTier::Edit},
        {"history", &ControlServer::methodHistory, ControlTier::Edit},
        {"commit_crop", &ControlServer::methodCommitCrop, ControlTier::Edit},
        {"export_png", &ControlServer::methodExportPng, ControlTier::Save},
    };
    return table;
}

QStringList ControlServer::methodNames()
{
    QStringList names;
    for (const MethodRow& row : methodTable()) {
        names.append(QString::fromLatin1(row.name));
    }
    return names;
}

bool ControlServer::isMethod(const QString& method)
{
    return methodNames().contains(method);
}

QJsonObject ControlServer::route(const QString& method, const QJsonObject& params)
{
    for (const MethodRow& row : methodTable()) {
        if (method == QLatin1String(row.name)) {
            return (this->*row.handler)(params);
        }
    }
    return error(QStringLiteral("unknown_method"),
                 QStringLiteral("unknown method: %1").arg(method));
}

QJsonObject ControlServer::methodStatus(const QJsonObject&)
{
    QJsonObject result;
    const int count = frame_->documentCount();
    result.insert(QStringLiteral("documents"), count);
    result.insert(QStringLiteral("active"), frame_->activeDocumentIndex());
    QJsonArray documents;
    for (int i = 0; i < count; ++i) {
        documents.append(documentInfo(frame_, i));
    }
    result.insert(QStringLiteral("documents_info"), documents);
    result.insert(QStringLiteral("active_tool"), toolIdName(frame_->activeTool()));
    PictureView* active = frame_->activeView();
    result.insert(QStringLiteral("backend"),
                  active ? active->active_backend() : QString());
    result.insert(QStringLiteral("gpu_available"), active ? active->gpu_available() : false);
    result.insert(QStringLiteral("brightness"), frame_->brightnessLevel());
    result.insert(QStringLiteral("screen_mode"), screenModeName(frame_->screenMode()));
    QJsonArray panels;
    for (const QString& name : frame_->panelObjectNames()) {
        PanelColumn* column = frame_->columnForPanel(name);
        panels.append(QJsonObject{
            {QStringLiteral("name"), name},
            {QStringLiteral("visible"), column && column->isPanelVisible(name)}});
    }
    result.insert(QStringLiteral("panels"), panels);
    return ok(result);
}

QJsonObject ControlServer::methodGetPixel(const QJsonObject& params)
{
    PictureView* view = frame_->activeView();
    if (!view || !view->has_document()) {
        return error(QStringLiteral("no_document"), QStringLiteral("no active document"));
    }
    if (!params.contains(QStringLiteral("x")) || !params.contains(QStringLiteral("y"))) {
        return error(QStringLiteral("invalid_param"),
                     QStringLiteral("x and y are required"));
    }
    const int x = params.value(QStringLiteral("x")).toInt(-1);
    const int y = params.value(QStringLiteral("y")).toInt(-1);
    if (x < 0 || y < 0 || x >= view->document_width() || y >= view->document_height()) {
        return error(QStringLiteral("invalid_param"),
                     QStringLiteral("coordinate out of range"));
    }
    const quint32 argb = view->sample_argb(x, y);
    QJsonObject result;
    result.insert(QStringLiteral("x"), x);
    result.insert(QStringLiteral("y"), y);
    result.insert(QStringLiteral("argb"),
                  QStringLiteral("0x%1").arg(argb, 8, 16, QLatin1Char('0')));
    result.insert(QStringLiteral("a"), static_cast<int>((argb >> 24) & 0xff));
    result.insert(QStringLiteral("r"), static_cast<int>((argb >> 16) & 0xff));
    result.insert(QStringLiteral("g"), static_cast<int>((argb >> 8) & 0xff));
    result.insert(QStringLiteral("b"), static_cast<int>(argb & 0xff));
    return ok(result);
}

QJsonObject ControlServer::methodListLayers(const QJsonObject&)
{
    PictureView* view = frame_->activeView();
    if (!view || !view->has_document()) {
        return error(QStringLiteral("no_document"), QStringLiteral("no active document"));
    }
    const int count = view->layer_count();
    QJsonArray layers;
    for (int i = 0; i < count; ++i) {
        layers.append(QJsonObject{
            {QStringLiteral("index"), i},
            {QStringLiteral("name"), view->layer_name(i)},
            {QStringLiteral("kind"), view->layer_kind(i)},
            {QStringLiteral("visible"), view->layer_visible(i)},
            {QStringLiteral("opacity"), view->layer_opacity(i)},
            {QStringLiteral("blend"), view->layer_blend(i)},
            {QStringLiteral("fill"), view->layer_fill(i)},
            {QStringLiteral("lock"), view->layer_lock(i)},
            {QStringLiteral("color"), view->layer_color(i)}});
    }
    QJsonObject result;
    result.insert(QStringLiteral("layers"), layers);
    result.insert(QStringLiteral("count"), count);
    result.insert(QStringLiteral("active"), view->active_layer_path());
    return ok(result);
}

QJsonObject ControlServer::methodListCommands(const QJsonObject& params)
{
    const bool implementedOnly = params.value(QStringLiteral("implemented_only")).toBool(false);
    QJsonArray commands;
    for (const CommandInfo& info : frame_->registry()->describe()) {
        if (info.id.isEmpty() || (implementedOnly && !info.implemented)) {
            continue;
        }
        commands.append(QJsonObject{
            {QStringLiteral("id"), info.id},
            {QStringLiteral("path"), QJsonArray::fromStringList(info.path)},
            {QStringLiteral("label"), info.label},
            {QStringLiteral("implemented"), info.implemented},
            {QStringLiteral("enabled"), info.enabled},
            {QStringLiteral("checked"), info.checked}});
    }
    QJsonObject result;
    result.insert(QStringLiteral("commands"), commands);
    result.insert(QStringLiteral("count"), commands.size());
    return ok(result);
}

QJsonObject ControlServer::methodDispatchCommand(const QJsonObject& params)
{
    if (!params.value(QStringLiteral("id")).isString()) {
        return error(QStringLiteral("invalid_param"), QStringLiteral("id must be a string"));
    }
    const QString id = params.value(QStringLiteral("id")).toString();
    CommandRegistry* registry = frame_->registry();

    const QList<CommandInfo> infos = registry->describe();
    const CommandInfo* info = nullptr;
    for (const CommandInfo& candidate : infos) {
        if (candidate.id == id) {
            info = &candidate;
            break;
        }
    }
    if (!info) {
        return error(QStringLiteral("invalid_param"),
                     QStringLiteral("unknown command id: %1").arg(id));
    }
    if (isModalCommand(id)) {
        return error(QStringLiteral("refused"),
                     QStringLiteral("command opens a modal dialog: %1").arg(id));
    }
    if (!info->implemented) {
        return error(QStringLiteral("refused"),
                     QStringLiteral("command is not implemented: %1").arg(id));
    }
    if (!info->enabled) {
        return error(QStringLiteral("refused"),
                     QStringLiteral("command is not enabled: %1").arg(id));
    }
    QAction* action = registry->action(id);
    const bool dispatched = registry->dispatch(id);
    registry->refresh();
    QJsonObject result;
    result.insert(QStringLiteral("dispatched"), dispatched);
    result.insert(QStringLiteral("enabled"), action ? action->isEnabled() : false);
    return ok(result);
}

} // namespace pictura

#include "moc_control_server.cpp"
