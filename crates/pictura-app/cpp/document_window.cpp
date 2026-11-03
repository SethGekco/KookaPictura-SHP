#include "document_window.h"

#include "image_view.h"
#include "mix_support.h"

#include "pictura_app/src/cxxqt_object.cxxqt.h"
#include "pictura_app/src/cxxqt_object/image_adjust/image_mode.cxxqt.h"

#include <QtCore/QFileInfo>
#include <QtCore/QHash>
#include <QtGui/QFontMetrics>

namespace pictura {

namespace {

// A document tab's title is elided to this text width so the close button (or
// the loading spinner) always has room inside the tab. The full title stays in
// the tab tooltip.
constexpr int kDocumentTabMaxTextWidth = 200;

// The label's mode name for an `image_mode()` key.
QString modeLabel(const QString& mode)
{
    static const QHash<QString, QString> labels = {
        {QStringLiteral("bitmap"), QStringLiteral("Bitmap")},
        {QStringLiteral("grayscale"), QStringLiteral("Grayscale")},
        {QStringLiteral("duotone"), QStringLiteral("Duotone")},
        {QStringLiteral("indexed"), QStringLiteral("Index")},
        {QStringLiteral("rgb"), QStringLiteral("RGB")},
        {QStringLiteral("cmyk"), QStringLiteral("CMYK")},
        {QStringLiteral("lab"), QStringLiteral("Lab")},
        {QStringLiteral("multichannel"), QStringLiteral("Multichannel")},
    };
    return labels.value(mode, mode.toUpper());
}

} // namespace

QString documentDisplayName(const DocEntry& doc)
{
    // KookaPictura-SHP fork: an entry opened from a MIX reads "gi.shp in ra2.mix".
    if (isMixMember(doc.path)) {
        return mixMemberTitle(doc.path);
    }
    if (!doc.displayName.isEmpty()) {
        return doc.displayName;
    }
    if (!doc.path.isEmpty()) {
        return QFileInfo(doc.path).fileName();
    }
    return QStringLiteral("Untitled-%1").arg(doc.untitledNumber);
}

QString fullLabel(const DocWindow* window)
{
    if (!window || !window->doc) {
        return QString();
    }
    const DocEntry& doc = *window->doc;
    QString label = documentDisplayName(doc);
    if (doc.loading) {
        return label;
    }
    const int ordinal = int(doc.windows.indexOf(const_cast<DocWindow*>(window))) + 1;
    if (ordinal >= 2) {
        label += QStringLiteral(":%1").arg(ordinal);
    }
    if (window->canvas) {
        label += QStringLiteral(" @ %1%").arg(qRound(window->canvas->zoom() * 100.0));
    }
    if (doc.view && doc.view->has_document()) {
        const QString mode = modeLabel(image_mode(*doc.view));
        const int bits = image_depth_bits(*doc.view);
        if (!mode.isEmpty() && bits > 0) {
            label += QStringLiteral(" (%1/%2)").arg(mode).arg(bits);
        }
    }
    if (doc.watermarkDetected) {
        label += QStringLiteral(" \u00A9");
    }
    return label;
}

QString documentTabText(const QString& label, bool dirty, const QFontMetrics& metrics)
{
    const int starWidth = dirty ? metrics.horizontalAdvance(QStringLiteral(" *")) : 0;
    const int textWidth = qMax(0, kDocumentTabMaxTextWidth - starWidth);
    QString text = metrics.elidedText(label, Qt::ElideRight, textWidth);
    if (dirty) {
        text += QStringLiteral(" *");
    }
    return text;
}

} // namespace pictura
