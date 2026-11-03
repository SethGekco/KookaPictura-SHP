#include "selftest_paint_perf.h"
#include "selftest_report.h"

#include "commands.h"
#include "frame.h"
#include "image_view.h"

#include "pictura_app/src/cxxqt_object.cxxqt.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtCore/QEventLoop>
#include <QtCore/QObject>
#include <QtGui/QImage>
#include <QtWidgets/QApplication>

#include <algorithm>

namespace {

// Byte-for-byte widget-image equality (both grabs share the same format).
bool sameImage(const QImage& a, const QImage& b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            if (a.pixel(x, y) != b.pixel(x, y)) {
                return false;
            }
        }
    }
    return true;
}

// The largest per-channel difference between two images, or -1 on a size
// mismatch.
int maxChannelDiff(const QImage& a, const QImage& b)
{
    if (a.size() != b.size()) {
        return -1;
    }
    int worst = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            const QRgb pa = a.pixel(x, y);
            const QRgb pb = b.pixel(x, y);
            worst = std::max(worst, qAbs(qRed(pa) - qRed(pb)));
            worst = std::max(worst, qAbs(qGreen(pa) - qGreen(pb)));
            worst = std::max(worst, qAbs(qBlue(pa) - qBlue(pb)));
            worst = std::max(worst, qAbs(qAlpha(pa) - qAlpha(pb)));
        }
    }
    return worst;
}

// Per-channel equality within `tol` LSB. The GPU is an accelerator, not an
// oracle: the compute dab matches the CPU stroke within ±1 LSB, so a
// GPU-authored commit is compared with that documented tolerance.
bool imageWithinTolerance(const QImage& a, const QImage& b, int tol)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            const QRgb pa = a.pixel(x, y);
            const QRgb pb = b.pixel(x, y);
            if (qAbs(qRed(pa) - qRed(pb)) > tol || qAbs(qGreen(pa) - qGreen(pb)) > tol
                || qAbs(qBlue(pa) - qBlue(pb)) > tol || qAbs(qAlpha(pa) - qAlpha(pb)) > tol) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

int pictura::runPaintPerfChecks(pictura::PicturaMainWindow& frame)
{
    // pp_dab_region (344): each paint_dab reports a dab-sized dirty rect, not
    // the accumulated stroke union. With a 12 px step over a long stroke the
    // cumulative bug would report a rect that grows with the whole stroke;
    // here every rect stays near the brush diameter. No wall-clock is measured
    // (flaky); the region/behavior is the assertion.
    {
        const bool created = frame.newDocument(QStringLiteral("PaintRegion"), 256, 256,
                                               QStringLiteral("rgb"), 8,
                                               QStringLiteral("white"));
        PictureView* view = frame.activeView();
        if (!created || !view) {
            return pictura::selfTest().fail(344, "paint region fixture");
        }
        const int doc = frame.activeDocumentIndex();
        view->set_active_layer(QStringLiteral("0"));

        int blits = 0;
        int maxW = 0;
        int firstX = -1;
        int lastX = -1;
        auto conn = QObject::connect(
            view, &PictureView::regionDamaged,
            [&blits, &maxW, &firstX, &lastX](int x, int, int w, int) {
                ++blits;
                maxW = std::max(maxW, w);
                if (firstX < 0) {
                    firstX = x;
                }
                lastX = x;
            });

        const bool begun = view->begin_paint(0xFFFF0000u, 0xFFFFFFFFu, 8, 100, 100, 0, 100, 100,
                                             150, QStringLiteral("normal"), false, false);
        int dabs = 0;
        for (int i = 0; i < 16; ++i) {
            if (view->paint_dab(40.0 + i * 12.0, 128.0, 1.0)) {
                ++dabs;
            }
            // Presents are frame-bounded: without an explicit flush only the
            // frame-opening dab would blit, so ask for each one by hand.
            view->flush_present();
        }
        // Only the per-dab blits are asserted to be dab-sized; the commit
        // refreshes the whole stroke extent, so disconnect before releasing.
        QObject::disconnect(conn);
        const bool ended = view->end_paint();

        const int span = lastX - firstX;
        const bool regionPerDab = blits >= dabs && dabs >= 16;
        const bool dabSized = maxW > 0 && maxW <= 16;
        const bool notStrokeSized = span >= 150 && maxW * 4 < span;
        ST_BEGIN("pp_dab_region");
        ST_PASS("pp_dab_region dabs=%d blits=%d maxW=%d span=%d", dabs, blits, maxW, span);
        if (!begun || !ended || !regionPerDab || !dabSized || !notStrokeSized) {
            frame.closeDocument(doc, false);
            return pictura::selfTest().fail(344, "dab region not incremental");
        }
        frame.closeDocument(doc, false);
    }

    // pp_present_cache (345): after a region blit the level crop is refreshed
    // once, reused on the next paint, and byte-identical to a fresh re-render
    // (cache disabled), so the optimization cannot change the rendered result.
    {
        const bool created = frame.newDocument(QStringLiteral("PaintCache"), 96, 96,
                                               QStringLiteral("rgb"), 8,
                                               QStringLiteral("white"));
        PictureView* view = frame.activeView();
        ImageView* canvas = frame.imageView();
        if (!created || !view || !canvas) {
            return pictura::selfTest().fail(345, "present cache fixture");
        }
        const int doc = frame.activeDocumentIndex();
        view->set_active_layer(QStringLiteral("0"));
        frame.resize(420, 360);
        QApplication::processEvents();
        canvas->actualPixels();
        QApplication::processEvents();
        // Warm the crop (two paints at the same zoom reuse it).
        QImage warm(canvas->size(), QImage::Format_ARGB32);
        canvas->render(&warm);
        canvas->render(&warm);

        const bool begun = view->begin_paint(0xFFFF0000u, 0xFFFFFFFFu, 8, 100, 100, 0, 100, 100,
                                             25, QStringLiteral("normal"), false, false);
        const bool dabbed = view->paint_dab(48.0, 48.0, 1.0);
        QImage patched(canvas->size(), QImage::Format_ARGB32);
        canvas->render(&patched);
        canvas->setPresentCacheEnabledForTest(false);
        QImage direct(canvas->size(), QImage::Format_ARGB32);
        canvas->render(&direct);
        canvas->setPresentCacheEnabledForTest(true);
        view->end_paint();
        const bool same = sameImage(patched, direct);
        // After the commit the level-crop path resumes: the crop is reused across
        // paints and equals a full-resolution transform draw at level 0.
        QImage cropA(canvas->size(), QImage::Format_ARGB32);
        canvas->render(&cropA);
        const int rebuilds = canvas->presentCacheRebuildCount();
        QImage cropB(canvas->size(), QImage::Format_ARGB32);
        canvas->render(&cropB);
        const bool cropReused = !canvas->presentCacheRebuiltOnLastPaint()
                                && canvas->presentCacheRebuildCount() == rebuilds
                                && sameImage(cropA, cropB);
        canvas->setPresentLevelCropForTest(false);
        QImage full(canvas->size(), QImage::Format_ARGB32);
        canvas->render(&full);
        canvas->setPresentLevelCropForTest(true);
        const bool cropIdentical = sameImage(cropA, full);
        ST_BEGIN("pp_present_cache");
        ST_PASS("pp_present_cache begun=%d dab=%d same=%d reused=%d identical=%d", begun ? 1 : 0,
                dabbed ? 1 : 0, same ? 1 : 0, cropReused ? 1 : 0, cropIdentical ? 1 : 0);
        if (!begun || !dabbed || !same || !cropReused || !cropIdentical) {
            frame.closeDocument(doc, false);
            return pictura::selfTest().fail(345, "present crop changed the render");
        }
        frame.closeDocument(doc, false);
    }

    // pp_no_registry_refresh (394): while a stroke is active the paint region
    // handler must not re-run the command registry per dab. The commit's region
    // requests one refresh through the debounced pass; pump the event loop past
    // the 120 ms interval before asserting it. No wall-clock budget.
    {
        const bool created = frame.newDocument(QStringLiteral("PaintRefresh"), 128, 128,
                                               QStringLiteral("rgb"), 8,
                                               QStringLiteral("white"));
        PictureView* view = frame.activeView();
        CommandRegistry* registry = frame.registry();
        if (!created || !view || !registry) {
            return pictura::selfTest().fail(394, "registry refresh fixture");
        }
        const int doc = frame.activeDocumentIndex();
        view->set_active_layer(QStringLiteral("0"));
        const bool begun = view->begin_paint(0xFFFF0000u, 0xFFFFFFFFu, 8, 100, 100, 0, 100, 100,
                                             12, QStringLiteral("normal"), false, false);
        const int before = registry->refreshCount();
        int dabs = 0;
        for (int i = 0; i < 16; ++i) {
            if (view->paint_dab(32.0 + i * 4.0, 64.0, 1.0)) {
                ++dabs;
            }
        }
        const int mid = registry->refreshCount();
        const bool ended = view->end_paint();
        const int afterSync = registry->refreshCount();
        QElapsedTimer wait;
        wait.start();
        while (wait.elapsed() < 200) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
        const int after = registry->refreshCount();
        const bool noPerDab = mid == before;
        const bool noSyncOnCommit = afterSync == mid;
        const bool onceOnCommit = after == mid + 1;
        ST_BEGIN("pp_no_registry_refresh");
        ST_PASS("pp_no_registry_refresh dabs=%d before=%d mid=%d afterSync=%d after=%d", dabs,
                before, mid, afterSync, after);
        if (!begun || !ended || dabs < 16 || !noPerDab || !noSyncOnCommit || !onceOnCommit) {
            frame.closeDocument(doc, false);
            return pictura::selfTest().fail(394, "registry refresh timing");
        }
        frame.closeDocument(doc, false);
    }

    // pp_present_flush (544): only the dab that opens a present frame blits;
    // the frame's later dabs accumulate and `flush_present` shows them as one
    // region, after which the displayed image matches the same stroke once it
    // commits. Frame-bounded present, so a fast stroke composites at the frame
    // rate rather than the input rate.
    {
        const bool created = frame.newDocument(QStringLiteral("PaintFlush"), 256, 256,
                                               QStringLiteral("rgb"), 8,
                                               QStringLiteral("white"));
        PictureView* view = frame.activeView();
        if (!created || !view) {
            return pictura::selfTest().fail(544, "present flush fixture");
        }
        const int doc = frame.activeDocumentIndex();
        view->set_active_layer(QStringLiteral("0"));

        int blits = 0;
        int flushX = -1;
        int flushW = 0;        auto conn = QObject::connect(
            view, &PictureView::regionDamaged,
            [&blits, &flushX, &flushW](int x, int, int w, int) {
                ++blits;
                if (blits > 1) {
                    flushX = x;
                    flushW = w;
                }
            });

        const bool begun = view->begin_paint(0xFFFF0000u, 0xFFFFFFFFu, 8, 100, 100, 0, 100, 100,
                                             150, QStringLiteral("normal"), false, false);
        int dabs = 0;
        for (int i = 0; i < 4; ++i) {
            if (view->paint_dab(40.0 + i * 12.0, 128.0, 1.0)) {
                ++dabs;
            }
        }
        const int beforeFlush = blits;
        const bool painting = view->is_painting();
        const bool flushed = view->flush_present();
        const int afterFlush = blits;
        const QImage midStroke = view->image();

        QObject::disconnect(conn);
        const bool ended = view->end_paint();
        const QImage afterCommit = view->image();

        // Dabs 2..4 sit at x = 52, 64, 76 under an 8 px brush, so the flushed
        // region starts at 48 and spans all three; dab 1 at x = 40 is not in it.
        const bool oneOnOpen = beforeFlush == 1;
        const bool coalesced = flushed && afterFlush == 2 && flushX >= 48 && flushW >= 24;
        const bool identical = sameImage(midStroke, afterCommit);
        ST_BEGIN("pp_present_flush");
        ST_PASS("pp_present_flush dabs=%d before=%d after=%d flushed=%d x=%d w=%d identical=%d",
                dabs, beforeFlush, afterFlush, flushed ? 1 : 0, flushX, flushW,
                identical ? 1 : 0);
        if (!begun || !ended || dabs < 4 || !painting || !oneOnOpen || !coalesced
            || !identical) {
            frame.closeDocument(doc, false);
            return pictura::selfTest().fail(544, "in-stroke present was not frame-bounded");
        }
        frame.closeDocument(doc, false);
    }

    // pp_large_preview (545): a brush above the raster budget presents the
    // in-progress stroke from a reduced view-pyramid level while the button is
    // down, emits regionDamaged so the canvas repaints, and the commit restores
    // the zoom-selected level with the canvas byte-identical to a full
    // recomposite. The exact full-resolution stroke still lands at release.
    {
        const bool created = frame.newDocument(QStringLiteral("PaintPreview"), 2048, 2048,
                                               QStringLiteral("rgb"), 8,
                                               QStringLiteral("white"));
        PictureView* view = frame.activeView();
        ImageView* canvas = frame.imageView();
        if (!created || !view || !canvas) {
            return pictura::selfTest().fail(545, "large preview fixture");
        }
        const int doc = frame.activeDocumentIndex();
        view->set_active_layer(QStringLiteral("0"));
        // Pin this check to the reduced-level preview: with a GPU adapter the
        // stroke would otherwise present on the GPU path instead.
        view->set_gpu_compute(false);
        frame.resize(420, 360);
        QApplication::processEvents();
        // 100% so the post-commit crop is level 0: the level crop and the
        // full-resolution draw are then the same pixels (as 345 relies on).
        canvas->setZoom(1.0, canvas->rect().center());
        QApplication::processEvents();

        int blits = 0;
        auto conn = QObject::connect(view, &PictureView::regionDamaged,
                                     [&blits](int, int, int, int) { ++blits; });

        // 512 px on 2048² exceeds the 262 144 px budget (514² = 264 196), so the
        // level-3 preview engages; level 3 exists (2048 → 1024 → 512 → 256).
        const bool begun = view->begin_paint(0xFFFF0000u, 0xFFFFFFFFu, 512, 100, 100, 0, 100, 100,
                                             25, QStringLiteral("normal"), false, false);
        int dabs = 0;
        for (int i = 0; i < 4; ++i) {
            if (view->paint_dab(1024.0, 900.0 + i * 80.0, 1.0)) {
                ++dabs;
            }
            view->flush_present();
        }
        const int previewLevel = view->preview_present_level();
        const int previewBlits = blits;
        const bool painting = view->is_painting();

        QObject::disconnect(conn);
        const bool ended = view->end_paint();
        view->set_gpu_compute(true);
        const int levelAfter = view->preview_present_level();
        QApplication::processEvents();

        QImage cropA(canvas->size(), QImage::Format_ARGB32);
        canvas->render(&cropA);
        canvas->setPresentLevelCropForTest(false);
        QImage full(canvas->size(), QImage::Format_ARGB32);
        canvas->render(&full);
        canvas->setPresentLevelCropForTest(true);
        const bool identical = sameImage(cropA, full);

        const bool previewed = previewLevel == 3 && previewBlits > 0;
        const bool restored = levelAfter == 0;
        ST_BEGIN("pp_large_preview");
        ST_PASS("pp_large_preview dabs=%d level=%d blits=%d after=%d identical=%d", dabs,
                previewLevel, previewBlits, levelAfter, identical ? 1 : 0);
        if (!begun || !ended || dabs < 4 || !painting || !previewed || !restored || !identical) {
            frame.closeDocument(doc, false);
            return pictura::selfTest().fail(545, "large-brush preview did not present");
        }
        frame.closeDocument(doc, false);
    }

    // pp_gpu_commit (546): a stroke committed on the GPU matches the same
    // stroke with the GPU disabled within ±1 LSB per channel. The GPU stroke is
    // now authoritative — it patches the working layer per dab, so `end_paint`
    // commits the GPU-authored document with no CPU replay. Skips without an
    // adapter; the skip ends only this check, so pp_pyramid_per_frame still
    // runs on the CPU path.
    do {
        ST_BEGIN("pp_gpu_commit");
        const bool created = frame.newDocument(QStringLiteral("PaintGpuCommit"), 1024, 1024,
                                               QStringLiteral("rgb"), 8,
                                               QStringLiteral("white"));
        PictureView* view = frame.activeView();
        if (!created || !view) {
            return pictura::selfTest().fail(546, "gpu commit fixture");
        }
        const int gpuDoc = frame.activeDocumentIndex();
        if (!view->gpu_available()) {
            frame.closeDocument(gpuDoc, false);
            ST_SKIP("no GPU adapter");
            break;
        }
        view->set_active_layer(QStringLiteral("0"));
        view->set_gpu_compute(true);
        // 600 px exceeds the 262 144 px raster budget (602² = 362 404), so the
        // GPU path engages.
        const bool begun = view->begin_paint(0xFFFF0000u, 0xFFFFFFFFu, 600, 100, 100, 0, 100, 100,
                                             25, QStringLiteral("normal"), false, false);
        int dabs = 0;
        for (int i = 0; i < 6; ++i) {
            if (view->paint_dab(150.0 + i * 150.0, 512.0, 1.0)) {
                ++dabs;
            }
            view->flush_present();
        }
        const bool ended = view->end_paint();
        const QImage gpuImage = view->image();
        frame.closeDocument(gpuDoc, false);

        const bool created2 = frame.newDocument(QStringLiteral("PaintCpuCommit"), 1024, 1024,
                                                QStringLiteral("rgb"), 8,
                                                QStringLiteral("white"));
        PictureView* cpu = frame.activeView();
        if (!created2 || !cpu) {
            return pictura::selfTest().fail(546, "gpu commit cpu fixture");
        }
        const int cpuDoc = frame.activeDocumentIndex();
        cpu->set_active_layer(QStringLiteral("0"));
        cpu->set_gpu_compute(false);
        const bool begun2 = cpu->begin_paint(0xFFFF0000u, 0xFFFFFFFFu, 600, 100, 100, 0, 100, 100,
                                             25, QStringLiteral("normal"), false, false);
        int dabs2 = 0;
        for (int i = 0; i < 6; ++i) {
            if (cpu->paint_dab(150.0 + i * 150.0, 512.0, 1.0)) {
                ++dabs2;
            }
            cpu->flush_present();
        }
        const bool ended2 = cpu->end_paint();
        const QImage cpuImage = cpu->image();
        cpu->set_gpu_compute(true);
        frame.closeDocument(cpuDoc, false);

        const bool withinOneLsb = imageWithinTolerance(gpuImage, cpuImage, 1);
        ST_PASS("pp_gpu_commit gpu=%d cpu=%d same_lsb1=%d worst=%d", dabs, dabs2,
                withinOneLsb ? 1 : 0, maxChannelDiff(gpuImage, cpuImage));
        if (!begun || !ended || !begun2 || !ended2 || dabs < 6 || dabs2 < 6 || !withinOneLsb) {
            return pictura::selfTest().fail(546, "gpu commit differs from the CPU by >1 LSB");
        }
    } while (false);

    // pp_pyramid_per_frame (547): several dabs arriving between two presents
    // rebuild the view pyramid once, at the frame flush, not once per dab, and
    // the committed pixels are unchanged. `canvas_revision` is bumped once per
    // pyramid rebuild, so its delta across the frame is the rebuild count.
    {
        ST_BEGIN("pp_pyramid_per_frame");
        const bool created = frame.newDocument(QStringLiteral("PaintPyramidFrame"), 1024, 1024,
                                               QStringLiteral("rgb"), 8,
                                               QStringLiteral("white"));
        PictureView* view = frame.activeView();
        if (!created || !view) {
            return pictura::selfTest().fail(547, "pyramid frame fixture");
        }
        const int doc = frame.activeDocumentIndex();
        view->set_active_layer(QStringLiteral("0"));
        view->set_gpu_compute(true);
        // Above the 262 144 px raster budget a GPU adapter presents on the
        // deferred-pyramid path; without one, a sub-budget brush exercises the
        // frame-bounded exact path.
        const bool gpu = view->gpu_available();
        const int diameter = gpu ? 600 : 8;
        const bool begun = view->begin_paint(0xFFFF0000u, 0xFFFFFFFFu, diameter, 100, 100, 0, 100,
                                             100, 25, QStringLiteral("normal"), false, false);
        if (!begun) {
            frame.closeDocument(doc, false);
            return pictura::selfTest().fail(547, "pyramid begin");
        }
        const quint64 before = view->canvas_revision();
        int dabs = 0;
        for (int i = 0; i < 12; ++i) {
            if (view->paint_dab(120.0 + i * 60.0, 120.0 + i * 60.0, 1.0)) {
                ++dabs;
            }
        }
        const bool flushed = view->flush_present();
        const quint64 updates = view->canvas_revision() - before;
        const bool ended = view->end_paint();
        const QImage deferredImage = view->image();
        frame.closeDocument(doc, false);

        const bool created2 = frame.newDocument(QStringLiteral("PaintPyramidCpu"), 1024, 1024,
                                                QStringLiteral("rgb"), 8,
                                                QStringLiteral("white"));
        PictureView* cpu = frame.activeView();
        if (!created2 || !cpu) {
            return pictura::selfTest().fail(547, "pyramid cpu fixture");
        }
        const int cpuDoc = frame.activeDocumentIndex();
        cpu->set_active_layer(QStringLiteral("0"));
        cpu->set_gpu_compute(false);
        const bool begun2 = cpu->begin_paint(0xFFFF0000u, 0xFFFFFFFFu, diameter, 100, 100, 0, 100,
                                             100, 25, QStringLiteral("normal"), false, false);
        int dabs2 = 0;
        for (int i = 0; i < 12; ++i) {
            if (cpu->paint_dab(120.0 + i * 60.0, 120.0 + i * 60.0, 1.0)) {
                ++dabs2;
            }
        }
        cpu->flush_present();
        const bool ended2 = cpu->end_paint();
        const QImage baselineImage = cpu->image();
        cpu->set_gpu_compute(true);
        frame.closeDocument(cpuDoc, false);

        const bool identical = imageWithinTolerance(deferredImage, baselineImage, 1);
        const bool frameBounded = updates <= 2 && updates < quint64(dabs);
        ST_PASS("pp_pyramid_per_frame gpu=%d dabs=%d updates=%llu flushed=%d same_lsb1=%d worst=%d",
                gpu ? 1 : 0, dabs, static_cast<unsigned long long>(updates), flushed ? 1 : 0,
                identical ? 1 : 0, maxChannelDiff(deferredImage, baselineImage));
        if (!begun || !begun2 || !ended || !ended2 || dabs < 4 || !flushed || !frameBounded
            || !identical) {
            return pictura::selfTest().fail(547, "pyramid rebuilt per dab");
        }
    }

    return 0;
}
