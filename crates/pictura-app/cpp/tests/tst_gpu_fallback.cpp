// A GPU failure at run time (out of video memory, a lost device) falls back to
// the CPU for the session and says so, instead of killing the open worker and
// leaving the document loading forever. Its own suite: the failure turns the
// GPU off for the whole process.

#include <QtTest/QtTest>

#include "frame.h"
#include "gpu_probe.h"

#include "pictura_app/src/cxxqt_object.cxxqt.h"
#include "pictura_app/src/gpu_probe_bridge.cxxqt.h"

#include <QtCore/QTemporaryDir>
#include <QtGui/QImage>
#include <QtWidgets/QStatusBar>

#include "qt_test_support.h"

class GpuFallbackTest : public QObject {
    Q_OBJECT

private slots:
    void injectedFailureOpensOnTheCpu();

private:
    pictura::test::ScopedStateHome stateHome_;
};

void GpuFallbackTest::injectedFailureOpensOnTheCpu()
{
    auto window = pictura::test::makeMainWindow();
    {
        pictura::PictureView probe;
        if (!probe.gpu_available()) {
            QSKIP("no usable GPU device: the fallback has nothing to fall back from");
        }
    }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("source.png"));
    QImage image(16, 16, QImage::Format_RGBA8888);
    image.fill(QColor(10, 20, 30));
    QVERIFY(image.save(path));

    pictura::gpu_inject_failure();
    QVERIFY2(window->openDocumentAtPath(path), "the open finishes instead of hanging");
    QCOMPARE(window->documentCount(), 1);
    pictura::PictureView* view = window->viewAt(0);
    QVERIFY(view);
    QVERIFY(view->gpu_compute());
    QCOMPARE(view->sample_argb(3, 3), 0xFF0A141Eu);
    const QString message = window->statusBar()->currentMessage();
    QVERIFY2(message.contains(QStringLiteral("The GPU failed")), qPrintable(message));
    QCOMPARE(pictura::activeBackendLabel(true), QStringLiteral("CPU (safe mode)"));
    QVERIFY(!view->gpu_available());
    QVERIFY2(pictura::takeGpuFailureNotice().isEmpty(), "the notice is shown once");

    const QString second = dir.filePath(QStringLiteral("second.png"));
    QVERIFY(image.save(second));
    QVERIFY2(window->openDocumentAtPath(second), "later opens stay on the CPU");
    QCOMPARE(window->documentCount(), 2);
    QCOMPARE(window->viewAt(1)->sample_argb(3, 3), 0xFF0A141Eu);
}

QTEST_MAIN(GpuFallbackTest)
#include "tst_gpu_fallback.moc"
