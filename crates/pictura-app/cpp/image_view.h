#pragma once

#include <QtCore/QElapsedTimer>
#include <QtCore/QLineF>
#include <QtCore/QList>
#include <QtCore/QPointF>
#include <QtCore/QRect>
#include <QtCore/QRectF>
#include <QtCore/QSize>
#include <QtCore/QSizeF>
#include <QtCore/QString>
#include <QtGui/QColor>
#include <QtGui/QImage>
#include <QtGui/QPainterPath>
#include <QtGui/QPolygonF>
#include <QtGui/QTransform>
#include <QtWidgets/QWidget>

#include "api/transform_api.h"
#include "smart_guides.h"
#include "theme.h"

#include <algorithm>
#include <array>
#include <functional>

class QPainter;

class QMouseEvent;
class QKeyEvent;
class QContextMenuEvent;
class QPaintEvent;
class QResizeEvent;
class QWheelEvent;
class QShowEvent;
class QHideEvent;
class QTimer;

namespace pictura {
enum class RulerUnit;
}

namespace pictura::selection_api {
struct SelectionContour;
}

namespace pictura {

/// A CPU-warped live projective preview: the warped image, its local
/// destination origin (relative to the moving layer's document origin), and
/// its pixels per document pixel; it covers `image.size() / scale`.
struct WarpedTransformPreview {
    QImage image;
    QPointF origin;
    double scale = 1.0;
    // The target quad crosses itself: an empty image means nothing is visible,
    // not a reason to fall back to Qt's projective draw (which fills outside).
    bool crossed = false;
};

// The central document canvas: paints a QImage under a pan/zoom transform over
// a plain canvas colour. GPU compositing is out of scope (CPU image for now).
class ImageView : public QWidget {
    Q_OBJECT

public:
    explicit ImageView(QWidget* parent = nullptr);

    // Replace the image and reset the view: fit-and-centre when the image is
    // larger than the viewport, otherwise 100% centred.
    void setImage(const QImage& image);

    // Replace the image but keep the current zoom and pan.
    void replaceImage(const QImage& image);

    // Present a document of `size` from the level provider alone, holding no
    // full-resolution image: as setImage (fit-and-centre) and replaceImage
    // (keep the view) respectively.
    void setDocument(const QSize& size);
    void replaceDocument(const QSize& size);

    // Overwrite the canvas at document-space (x, y) with a region-sized image
    // under CompositionMode_Source (no blending), then invalidate the zoom
    // present cache so the next paint rebuilds it. A null/empty region and a
    // null canvas are no-ops.
    void blitRegion(const QImage& region, int x, int y);

    // A document region refresh: patch the present crop over `docRect` where
    // possible and repaint. No pixels are passed; the level provider already
    // holds them.
    void damageRegion(const QRect& docRect);

    // A zoomed-out filter dialog's preview: `picture`, a reduced render of the
    // document rect `area`, drawn over the document there until cleared. The
    // document itself is untouched.
    void setFilterPreview(const QImage& picture, const QRect& area);
    void clearFilterPreview();
    bool filterPreviewShownForTest() const { return !filterPreview_.isNull(); }

    // The rubylith overlay of a layer mask: `overlay`, a reduced image of the
    // document rect `area`, drawn over the document until cleared. Display
    // only; the document is untouched.
    void setMaskOverlay(const QImage& overlay, const QRect& area);
    void clearMaskOverlay();
    QImage maskOverlayForTest() const { return maskOverlay_; }

    // Supplies document crops from the view pyramid so the present path never
    // scales the full-resolution `image()`. `crop` returns a premultiplied crop
    // of `level` at the level-space rect `(x, y, w, h)`; `levelCount`/`levelSize`
    // describe the pyramid; `canvasRevision` is a non-consuming revision of the
    // displayed pixels, so the present cache keys on it without draining the
    // damage account `image()` owns. Any member may be empty.
    struct LevelProvider {
        std::function<QImage(int level, int x, int y, int w, int h)> crop;
        std::function<int()> levelCount;
        std::function<QSize(int level)> levelSize;
        std::function<quint64()> canvasRevision;
        std::function<bool()> isPainting;
        // The view-pyramid level an in-progress large-brush preview patched;
        // 0 when no preview is running (the zoom picks the level as usual).
        std::function<int()> previewLevel;
        // A zero-copy view of a stored level (premultiplied RGBA8888), or an
        // empty image for level 0 / past the end. The image keeps the level
        // storage alive; do not retain it past the current paint.
        std::function<QImage(int level)> levelImage;
        // Report the level and document rect about to be presented, so a Rust
        // render session can composite a preview for exactly that frame.
        std::function<void(int level, const QRect& docRect)> reportView;
    };
    void setLevelProvider(LevelProvider provider);

    // Smooth sampling below 200 % zoom, nearest at and above it.
    static bool smoothSamplingForZoom(double zoom);
    // The coarsest level whose scale is still at least as fine as the screen.
    static int presentLevelForZoom(double zoom, int levelCount);

    // The document as one image: the one set by setImage/replaceImage, or, for
    // a document presented from the level provider, level 0 cropped whole. That
    // crop is the full resolution, so size-only callers use documentSize().
    QImage image() const;
    QSize documentSize() const { return docSize_; }
    bool hasDocument() const { return !docSize_.isEmpty(); }

    // Zoom about a cursor position so the point under the cursor stays put.
    void zoomAt(const QPointF& cursor, int angleDelta);
    void panBy(const QPointF& delta);

    void zoomIn();
    void zoomOut();
    void fitOnScreen();
    void actualPixels();

    // Set an absolute zoom about a widget-space anchor (clamped to 0.01x..32x).
    void setZoom(double zoom, const QPointF& anchor);

    // Set the widget-space offset directly, clamped by the reveal margin. The
    // workspace scrollbars use this; every other path goes through clampOffset.
    void setOffset(const QPointF& offset);

    double zoom() const { return zoom_; }
    QPointF offset() const { return offset_; }

    // The document-space rectangle currently visible in the viewport (empty
    // when no image). Used by dialogs that preview the current canvas section.
    QRectF visibleDocumentRect() const;

    // Brush-size ring drawn in image space under the pan/zoom transform. The
    // diameter is in image pixels, so a zoomed canvas scales the ring for free.
    // The Eraser's Block mode draws a square of the same size instead, so the
    // two share one outline.
    void setBrushOutline(double diameter, const QPointF& imagePos);
    void setBlockOutline(double side, const QPointF& imagePos);
    void clearBrushOutline();
    bool hasBrushOutline() const { return brushOutlineActive_; }
    // Where the outline is drawn, in image space.
    QPointF brushOutlineImagePos() const { return brushOutlineImagePos_; }
    double brushOutlineDiameterForTest() const { return brushOutlineDiameter_; }
    bool brushOutlineIsSquareForTest() const { return brushOutlineSquare_; }
    // Device-pixel ring diameter for the current zoom (what a render shows).
    double brushOutlineScreenDiameterForTest() const
    {
        return brushOutlineDiameter_ * zoom_;
    }

    // Eyedropper sampling ring drawn in image space like the brush outline; the
    // diameter is the Sample Size in image pixels.
    void setSamplingRing(double diameter, const QPointF& imagePos);
    void clearSamplingRing();
    bool hasSamplingRingForTest() const { return samplingRingActive_; }
    double samplingRingDiameterForTest() const { return samplingRingDiameter_; }

    void setCanvasColor(const QColor& color);
    QColor canvasColor() const { return canvasColor_; }

    // Transparency checkerboard behind the document (the "Light" grid):
    // two tones in 8-screen-pixel cells, anchored to the document origin.
    static int transparencyCellSize();
    static QColor transparencyColorA();
    static QColor transparencyColorB();

    // When disabled, mouse presses are forwarded as tool events instead of
    // starting a pan. Default true.
    void setPanEnabled(bool enabled);
    // Start a left-button pan from `widgetPos`, as a pan-enabled press does.
    void beginPan(const QPointF& widgetPos)
    {
        panning_ = true;
        userAdjusted_ = true;
        pendingSettle_ = false;
        last_ = widgetPos;
    }
    bool panEnabled() const { return panEnabled_; }

    // View > Free Pan: when true (default) the canvas may pan off-centre even
    // when the document fits the viewport; when false an axis the document fits
    // is pinned to centre. Changing the value re-clamps the current offset.
    void setFreePan(bool enabled);
    bool freePan() const { return freePan_; }

    // View > Snap Canvas To Workspace: when true and the whole scaled document
    // fits the viewport, the canvas is pinned to the workspace centre, panning
    // is refused, and the workspace scrollbars hide. Changing the value
    // re-clamps the current offset. Default false.
    void setSnapToWorkspace(bool enabled);
    bool snapToWorkspace() const { return snapToWorkspace_; }
    // True while Snap is on and the whole scaled document fits the viewport, so
    // the canvas is locked to the centre and offers no pan affordance.
    bool workspaceSnapLocked() const;

    // Transient Space-key pan: while on, a left drag pans and the hand cursors
    // are used regardless of the active tool, and the tool's mouse-press is not
    // emitted. The owner restores the pan/cursor state on release.
    void setSpacePan(bool on);
    bool spacePanForTest() const { return spacePan_; }
    // Whether a pan drag is in progress (middle-button, Hand, or Space).
    bool isPanning() const { return panning_; }

    void setOverlayPolygon(const QPolygonF& polygon);
    void clearOverlay();

    // Committed-selection marching ants from the typed adapter contour (see
    // selection_api::contour); an empty contour clears.
    void setSelectionContour(const selection_api::SelectionContour& contour);
    // The Channels panel's visible colour channels (image_view_channels.cpp):
    // bit 0 red, 1 green, 2 blue; 0x7 shows all. One visible channel draws as
    // greyscale, as CS6 does by default; two draw in their own colours.
    void setChannelMask(int mask);
    int channelMask() const { return channelMask_; }
    void clearSelectionContour();
    // A move-selection drag shifts the committed ants by the drag delta rather
    // than retracing the translated mask; setting or clearing the contour
    // resets it.
    void setSelectionContourOffset(const QPointF& offset);
    void setSelectionEdgesVisible(bool on);
    bool selectionEdgesVisible() const { return selectionEdgesVisible_; }
    bool hasSelectionContourForTest() const { return !selectionContours_.isEmpty(); }
    int selectionContourLoopCountForTest() const { return selectionContours_.size(); }
    const QList<QPolygonF>& selectionContours() const { return selectionContours_; }
    QPointF selectionContourOffsetForTest() const { return selectionContourOffset_; }
    quint64 selectionContourRevisionForTest() const { return selectionContourRevision_; }
    int selectionContourSetCountForTest() const { return selectionContourSetCount_; }
    int antsTicksForTest() const { return antsTicks_; }
    QRect lastPaintRectForTest() const { return lastPaintRect_; }

    // Live selection preview while a tool drags (the rubber band). Drawn with the
    // same animated marching-ants pen as a committed selection, and replaced by
    // the committed contour on release. Not affected by the Selection Edges
    // toggle: the user is actively defining the selection. Empty clears. When
    // `closed` is false the loops are drawn as open polylines (the Polygonal
    // Lasso rubber band must not show a phantom closing edge).
    void setSelectionPreview(const QList<QPolygonF>& loops, bool closed = true, bool solid = false);
    // Live selection shape: the rubber band, closed and clipped to the canvas.
    void setSelectionShapePreview(const QList<QPolygonF>& loops);
    void clearSelectionPreview();
    // A click-driven lasso's origin: a small hollow square, fixed on screen,
    // marking the point the outline closes to. Cleared with the preview.
    void setSelectionPreviewOrigin(const QPointF& imagePos);
    bool hasSelectionPreviewOriginForTest() const { return previewOriginActive_; }
    bool hasSelectionPreviewForTest() const { return !previewContours_.isEmpty(); }
    bool selectionPreviewOpenForTest() const
    {
        return !previewContours_.isEmpty() && !selectionPreviewClosed_;
    }
    bool selectionPreviewSolidForTest() const { return selectionPreviewSolid_; }
    int selectionPreviewLoopCountForTest() const { return previewContours_.size(); }
    int selectionPreviewPointCountForTest() const
    {
        return previewContours_.isEmpty() ? 0 : previewContours_.first().size();
    }
    const QList<QPolygonF>& selectionPreviewContours() const { return previewContours_; }

    // Move-tool live preview: draw a cached base plus the moved layer at a live
    // image-space offset, so the drag never composites the document.
    void beginMovePreview(const QImage& base, const QImage& layer, const QPointF& layerPos,
                          double opacity);
    void setMovePreviewDelta(const QPointF& delta);
    void endMovePreview();
    bool movePreviewActive() const { return movePreviewActive_; }

    // Free Transform preview: same cached base/layer as the Move preview, but the
    // layer is drawn under a live similarity transform (scale/rotate/translate
    // about the layer centre) and the quad with its eight handles is overlaid.
    void beginTransformPreview(const QImage& base, const QImage& layer, const QPointF& layerPos,
                               double opacity);
    void setTransformPreview(double scaleX, double scaleY, double angleRadians, double dx,
                             double dy);
    // Projective layer preview: nine space-separated coefficients in
    // `QTransform(m11,m12,m13,m21,m22,m23,m31,m32,m33)` order, mapping source
    // document space onto the target quad. Overrides the similarity preview
    // until the next `beginTransformPreview`. An empty `sourceRect` samples the
    // whole preview layer.
    void setTransformPreviewProjective(const std::array<double, 9>& coefficients,
                                       const QRectF& sourceRect);
    void setTransformQuad(const QPolygonF& corners);
    // The Free Transform reference point (image space) and its marker
    // visibility; drawn as a target glyph with the transform overlay.
    void setTransformPivot(const QPointF& pivot, bool visible);
    // Replace the previewed layer image and its document-space origin (a warped
    // preview recomputes the whole layer, so its rect may move).
    void updateTransformPreviewLayer(const QImage& layer, const QPointF& layerPos);
    // Warp mode's grid and control points (document space), drawn instead of
    // the box; an empty overlay brings the box back.
    void setTransformWarp(const transform_api::WarpOverlay& overlay);
    const transform_api::WarpOverlay& transformWarpForTest() const { return transformWarp_; }
    QImage transformPreviewLayerForTest() const { return moveLayer_; }
    QPointF transformPivotForTest() const { return transformPivot_; }
    bool transformPivotVisibleForTest() const { return transformPivotVisible_; }
    // The document-space map the moving layer is drawn under.
    QTransform transformLayerMapForTest() const
    {
        if (!transformProjectiveActive_) {
            return transformPreviewMatrix();
        }
        return QTransform::fromTranslate(-moveLayerPos_.x(), -moveLayerPos_.y())
            * transformProjective_
            * QTransform::fromTranslate(moveLayerPos_.x(), moveLayerPos_.y());
    }
    void clearTransformPreview();
    bool transformPreviewActive() const { return transformActive_; }

    // The document-space matrix the live preview applies to the cached layer
    // (`c + R·S·(u − c) + d`). paintEvent draws through the same matrix, so a
    // gesture's preview mapping can be checked against the committed mapping
    // without rendering.
    QTransform transformPreviewMatrix() const;

    // Crop box (image space): shades outside it and draws rule-of-thirds
    // guides, its frame, and eight handles.
    void setCropBox(const QRectF& box);
    void clearCropBox();
    // A preview crop box (the box the tool starts with): its frame and handles
    // but no guides. Stays set until the next setCropPreview.
    void setCropPreview(bool preview);
    bool cropPreviewForTest() const { return cropPreview_; }
    // While the box is being rotated a dense grid replaces the guide overlay.
    void setCropRotating(bool rotating);
    bool cropRotatingForTest() const { return cropRotating_; }
    double cropStraightenForTest() const { return cropStraighten_; }
    // The active crop guide overlay: 0 Rule of Thirds, 1 Grid, 2 Diagonal,
    // 3 Triangle, 4 Golden Ratio, 5 Golden Spiral; -1 hides it (the show
    // mode). `orientation` mirrors it: bit 0 left-right, bit 1 top-bottom.
    // Drawn only on an active box.
    void setCropOverlay(int overlay, int orientation = 0);
    int cropOverlayOrientationForTest() const { return cropOverlayOrientation_; }
    // How the area outside the box is drawn. `color` is the shield tint (an
    // invalid colour matches the canvas); `showOutside` off hides the outside
    // area entirely; `dragging` thins the shield under Auto Adjust Opacity.
    struct CropShield {
        bool enabled = true;
        bool showOutside = true;
        QColor color;
        int opacity = 75;
        bool autoAdjust = true;
        bool dragging = false;
    };
    void setCropShield(const CropShield& shield);
    // Classic mode: the straighten angle turns the box over the fixed image
    // instead of turning the content behind a fixed box.
    void setCropBoxRotates(bool rotates);
    // The alpha (0-255) the shield paints with; test hook for the dim level.
    int cropShieldAlpha() const;
    // Pan so the image-space `area` is centred in the viewport.
    void centerOnImageRect(const QRectF& area);
    int cropOverlayForTest() const { return cropOverlay_; }
    // The Modern crop content offset (image space): the composite is drawn
    // shifted by this while the crop box overlay stays fixed, so an inside drag
    // pans the content under the box. Commit maps the box by `box - offset`.
    void setCropContentOffset(const QPointF& offset);
    QPointF cropContentOffset() const { return cropContentOffset_; }
    // The crop straighten angle: the content is previewed rotated about `pivot`
    // (image space, fixed for the session so moving the box does not move the
    // content) while the box stays axis-aligned. The canvas grows to the rotated
    // content's bounding box.
    void setCropStraighten(double degrees, const QPointF& pivot);
    // The straighten line tool's live horizon line (image space); drawn until
    // the gesture ends.
    void setCropStraightenLine(const QLineF& line);
    void clearCropStraightenLine();
    // The axis-aligned canvas rect (image space) the preview shows: the image
    // rect when straight, else the bounding box of the rotated canvas.
    QRectF cropCanvasImageRect() const;
    bool hasCropBoxForTest() const { return !cropBox_.isNull(); }
    // The crop box's four corners in widget space, as the overlay draws them
    // (independent of the content-rotation preview).
    QPolygonF cropBoxWidgetForTest() const;

    // Perspective Crop quad (TL, TR, BR, BL, image space): shades outside it and
    // draws its edges, a perspective-following 3x3 grid, and corner handles.
    void setPerspectiveCropQuad(const QPolygonF& quad, double gridSpacing);
    void clearPerspectiveCropQuad();
    bool hasPerspectiveCropQuadForTest() const { return perspectiveQuad_.size() == 4; }
    QPolygonF perspectiveCropQuadForTest() const { return perspectiveQuad_; }

    // Slice overlay (image space): user slices solid blue with a numbered
    // badge, auto slices dotted grey, the selected slice with orange handles,
    // plus the slice being dragged out.
    struct SliceOverlay {
        QRectF rect;
        int number = 0;
        bool user = false;
        bool selected = false;
    };
    void setSliceOverlay(const QList<SliceOverlay>& slices, const QRectF& dragging = QRectF());
    void clearSliceOverlay();
    int sliceOverlayCountForTest() const { return int(sliceOverlay_.size()); }
    bool sliceOverlayHasSelectionForTest() const
    {
        return std::any_of(sliceOverlay_.cbegin(), sliceOverlay_.cend(),
                           [](const SliceOverlay& s) { return s.selected; });
    }

    // Annotation overlay (image space): numbered color-sampler crosshairs,
    // note glyphs (the `currentNote` index outlined), and numbered Count marks,
    // shown with any tool, plus the Ruler's measuring line while that tool is
    // active.
    void setAnnotationOverlay(const QList<QPointF>& samplers, const QList<QPointF>& notes,
                              int currentNote);
    // One Count group's visible mark: its position, 1-based number, and the
    // group's colour/marker/label sizes.
    struct CountOverlayMark {
        QPointF pos;
        int number = 1;
        QColor color;
        int markerSize = 2;
        int labelSize = 12;
    };
    void setCountOverlay(const QList<CountOverlayMark>& marks);
    void setRulerLine(const QLineF& line);
    void clearRulerLine();
    int samplerOverlayCountForTest() const { return int(samplerOverlay_.size()); }
    int noteOverlayCountForTest() const { return int(noteOverlay_.size()); }
    int countOverlayCountForTest() const { return int(countOverlay_.size()); }
    bool hasRulerLineForTest() const { return rulerShown_; }

    // Guides (image_view_guides.cpp): full-length one-pixel lines across the
    // view at document columns (vertical) or rows (horizontal), drawn in the
    // guide colour, solid or dashed, plus the guide a ruler drag is placing.
    struct GuideLine {
        bool vertical = false;
        double position = 0.0;
        bool operator==(const GuideLine& other) const
        {
            return vertical == other.vertical && position == other.position;
        }
    };
    void setGuides(const QList<GuideLine>& guides);
    void setGuidesVisible(bool on);
    // SHP fork (image_view_shp.cpp): the frame grid of an SHP sheet. A null
    // cell size hides it; `active` outlines one frame's cell. `onion` ghosts
    // the cells before and after `active` into it at `onionOpacity` percent;
    // `offPalette` pixels are marked.
    struct ShpGrid {
        QSize cell;
        int cols = 0;
        int frames = 0;
        int gutter = 0;
        int active = -1;
        bool visible = true;
        bool onion = false;
        int onionOpacity = 35;
        // Off-palette pixels to mark, sheet coordinates (empty: none).
        QVector<QPoint> offPalette;
        // Shadow underlay: file frames from `shadowHalf` (0: none) are the
        // shadows of the frames before it, `first` is the file frame in
        // cell 0. Each art cell shows its shadow under its own pixels;
        // `activeShadow` is the active frame's shadow when that is off the
        // sheet (a paged window).
        int first = 0;
        int shadowHalf = 0;
        bool shadowUnderlay = false;
        QImage activeShadow;
        // House-color preview: opaque pixels of color remapFrom[i] show as
        // remapTo[i] (empty: off).
        QVector<QRgb> remapFrom;
        QVector<QRgb> remapTo;
        // TMP terrain (no cells): tile diamonds and extra blocks to outline,
        // image space; `activeTile` indexes tileOutlines.
        QVector<QPolygonF> tileOutlines;
        QVector<QRectF> extraOutlines;
        int activeTile = -1;
    };
    void setShpGrid(const ShpGrid& grid);
    const ShpGrid& shpGrid() const { return shpGrid_; }
    bool guidesVisible() const { return guidesVisible_; }
    void setGuideAppearance(const QColor& color, bool dashed);
    QColor guideColor() const { return guideColor_; }
    bool guidesDashed() const { return guidesDashed_; }
    void setGuidePreview(const GuideLine& guide);
    void clearGuidePreview();
    const QList<GuideLine>& guides() const { return guides_; }
    bool hasGuidePreviewForTest() const { return guidePreviewActive_; }
    // Smart alignment guides (image_view_smart_guides.cpp): magenta lines and
    // gap labels shown while a Move or Type drag aligns content; empty clears.
    void setSmartGuides(const QList<SmartGuideLine>& lines, const QList<SmartGuideGap>& gaps);
    void clearSmartGuides();
    // Whether a drag snaps to smart guides and draws them (default on).
    void setSmartGuidesEnabled(bool on);
    bool smartGuidesEnabled() const { return smartGuidesEnabled_; }
    bool hasSmartGuidesForTest() const { return !smartGuideLines_.isEmpty(); }
    QList<SmartGuideLine> smartGuidesForTest() const { return smartGuideLines_; }
    // The first gap label in the rulers' unit, or empty when there is none.
    QString smartGuideGapForTest() const;

    // The Work Path overlay (image space) while a Pen-group or path selection
    // tool is active: the curve, its anchors (`activeAnchor`, the one the Pen
    // just placed or Direct Selection picked, solid; every one solid with
    // `anchorsSolid`, Path Selection's selected component), handle lines, a
    // dashed preview (the Rubber Band or the Freeform trail), and the selected
    // component's bounding box when not null.
    struct PathOverlay {
        QPainterPath curve;
        QList<QPointF> anchors;
        int activeAnchor = -1;
        bool anchorsSolid = false;
        QList<QLineF> handles;
        QPainterPath preview;
        QRectF bounds;
    };
    void setPathOverlay(const PathOverlay& overlay);
    void clearPathOverlay();
    int pathOverlayAnchorCountForTest() const { return int(pathOverlay_.anchors.size()); }
    bool pathOverlayHasPreviewForTest() const { return !pathOverlay_.preview.isEmpty(); }
    bool pathOverlayAnchorsSolidForTest() const { return pathOverlay_.anchorsSolid; }
    int pathOverlayActiveAnchorForTest() const { return pathOverlay_.activeAnchor; }
    QRectF pathOverlayBoundsForTest() const { return pathOverlay_.bounds; }

    // The Type tools' live text (image space) while typing: `image` (the
    // engine's own render, so the preview is the commit) at `topLeft`, and the
    // caret. The Type Mask tools pass `mask`: the `canvas` is tinted red and
    // `image` is the tint over the type's rect, the letters cut out of it.
    // `selection` holds one quad per selected character.
    // A Type session's overlay. `placement` is point type's origin (the
    // square on the baseline), `baselines` the guide under each typed line, and
    // `box` paragraph type's box (empty for point type) with `overflow` the
    // `+` on its bottom-right handle. Every set shows the caret and restarts
    // its blink.
    struct TypeOverlay {
        bool active = false;
        bool mask = false;
        QSize canvas;
        QImage image;
        QPoint topLeft;
        QLineF caret;
        QList<QPolygonF> selection;
        bool hasPlacement = false;
        QPointF placement;
        QList<QLineF> baselines;
        QPolygonF box;
        bool overflow = false;
        // 1 on-path, 2 area: the box is the path outline, with no handles.
        int pathKind = 0;
        bool hasOverflowPoint = false;
        QPointF overflowPoint;
        // Over a dark image the caret, guides, and placement point draw white,
        // as CS6's do, else black.
        bool lightInk = false;
        // The box's reference point (rotation pivot), when a box is shown.
        bool hasPivot = false;
        QPointF pivot;
    };
    // Half of the caret's 1.5 s blink cycle.
    static constexpr int kTypeCaretBlinkMs = 750;
    void setTypeOverlay(const TypeOverlay& overlay);
    void clearTypeOverlay();
    bool typeOverlayActiveForTest() const { return typeOverlay_.active; }
    bool typeOverlayHasTextForTest() const { return !typeOverlay_.image.isNull(); }
    QImage typeOverlayImageForTest() const { return typeOverlay_.image; }
    int typeOverlaySelectionForTest() const { return int(typeOverlay_.selection.size()); }
    QLineF typeOverlayCaretForTest() const { return typeOverlay_.caret; }
    bool typeCaretShownForTest() const { return typeCaretShown_; }
    int typeCaretBlinkIntervalForTest() const;
    bool typeOverlayHasPlacementForTest() const { return typeOverlay_.hasPlacement; }
    QPointF typeOverlayPlacementForTest() const { return typeOverlay_.placement; }
    QList<QLineF> typeOverlayBaselinesForTest() const { return typeOverlay_.baselines; }
    QPolygonF typeOverlayBoxForTest() const { return typeOverlay_.box; }
    bool typeOverlayOverflowForTest() const { return typeOverlay_.overflow; }
    QPointF typeOverlayOverflowPointForTest() const { return typeOverlay_.overflowPoint; }
    bool typeOverlayHasOverflowPointForTest() const { return typeOverlay_.hasOverflowPoint; }
    int typeOverlayPathKindForTest() const { return typeOverlay_.pathKind; }
    bool typeOverlayLightInkForTest() const { return typeOverlay_.lightInk; }
    QPointF typeOverlayPivotForTest() const { return typeOverlay_.pivot; }

    // Live marquee size readout ("W x H"), painted as a tooltip offset from the
    // mapped cursor. Empty text or clearDragSizeHint() hides it.
    void setDragSizeHint(const QString& text, const QPointF& imagePos);
    void clearDragSizeHint();

    // The unit and resolution drag readouts measure in (the rulers').
    void setMeasureUnit(RulerUnit unit, bool traditionalPoints);
    void setPpiProvider(std::function<double()> ppi);
    // The Move tool's readout for a move of `delta` document pixels:
    // "ΔX: 0.500 in" over "ΔY: 0.250 in".
    QString offsetReadout(const QPointF& delta) const;
    // Two labelled lengths in document pixels, one per line, in the rulers'
    // unit ("W: 1.091 in" over "H: 0.988 in"); Percent measures the first
    // against the document width, the second against its height.
    QString measureReadout(const QString& first, double firstPx, const QString& second,
                           double secondPx) const;
    // The moving layer's content box (document pixels, at drag start), drawn
    // as marching ants that follow the move preview; empty clears it.
    void setMoveOutline(const QRectF& bounds);
    QRectF moveOutlineForTest() const { return moveOutline_.translated(moveDelta_); }
    // The Move tool's idle `Show Transform Controls` box (no session): drawn
    // like Free Transform's box; empty hides it.
    void setControlsBox(const QPolygonF& box);
    QPolygonF controlsBoxForTest() const { return controlsBox_; }
    bool hasDragSizeHintForTest() const { return dragSizeActive_ && !dragSizeText_.isEmpty(); }
    QString dragSizeHintForTest() const { return dragSizeText_; }

    // Test hooks for the internal level-crop present cache.
    bool presentCacheRebuiltOnLastPaint() const { return presentCacheRebuiltLastPaint_; }
    int presentCacheRebuildCount() const { return presentCache_.rebuilds; }
    bool holdsImageForTest() const { return !image_.isNull(); }
    QSize presentCacheImageSize() const { return presentedImageSize_; }
    qint64 presentCacheImageKey() const { return presentedImageKey_; }
    int presentLevelForTest() const { return presentLevel(); }
    bool presentCacheValidForTest() const { return presentCache_.valid; }
    QImage presentCacheCropForTest() const { return presentCache_.crop.copy(); }
    void setPresentCacheEnabledForTest(bool enabled);
    void setPresentLevelCropForTest(bool enabled);

    // Map a widget-space point to document/image coordinates.
    QPointF widgetToImage(const QPointF& widgetPos) const;
    QPointF imageToWidget(const QPointF& imagePos) const;

    // Rotate View: the canvas turns about the widget centre, the document
    // untouched. Zoom and offset are kept in the unrotated "view" frame, so
    // the whole paint is rotated once and only input mapping needs undoing.
    double rotation() const { return rotation_; }
    // Degrees clockwise, normalised to (-180, 180].
    void setRotation(double degrees);
    // The widget-from-view transform (identity at 0°).
    QTransform viewRotation() const;
    // The widget rect in the view frame (its bounding box when rotated).
    QRectF viewRect() const;
    // The compass the Rotate View tool shows while dragging.
    void setCompassVisible(bool visible);
    bool compassVisibleForTest() const { return compassVisible_; }

signals:
    void zoomChanged(double zoom);
    // The pan or zoom changed by any path (pan, zoom, fit, 100%, resize, the
    // Navigator proxy). The workspace scrollbars project this into their ranges.
    void viewChanged();
    // A Hand drag (the Hand tool or the Space form) panned the canvas by
    // `delta`, with `mods` held; the frame fans it out for Scroll All Windows.
    void handPanned(const QPointF& delta, Qt::KeyboardModifiers mods);
    void mousePressed(const QPointF& imagePos, int button, int modifiers);
    void mouseMoved(const QPointF& imagePos);
    void mouseReleased(const QPointF& imagePos);
    // A modifier key (Alt/Shift/Ctrl/Meta) was pressed or released, so a tool
    // observes the edge even without pointer movement.
    void modifierKeyChanged(int modifiers);
    // A right-click on the canvas; the frame shows the active tool's context
    // menu at the global position (the Zoom tool's preset menu).
    void contextMenuRequested(const QPoint& globalPos);
    // Free Transform Enter/Return (commit) and Escape (cancel).
    void transformCommitRequested();
    void transformCancelRequested();

protected:
    void paintEvent(QPaintEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    // The one brush outline: a circle for the brush tools, a square for the
    // Eraser's Block mode.
    void setBrushOutlineShape(double diameter, const QPointF& imagePos, bool square);
    void paintCropGroupOverlays(QPainter& painter);
    // The Free Transform reference point marker (image_view_overlays.cpp).
    void paintTransformPivot(QPainter& painter);
    void paintWarpOverlay(QPainter& painter);
    // Rewarp the projective preview when its inputs, the visible region, or the
    // view scale changed since the last warp.
    void refreshTransformWarp();
    // The Free Transform box, handles, rotate knob, and reference point,
    // unclipped by the document rect (image_view_overlays.cpp).
    void paintTransformOverlay(QPainter& painter);
    void paintAnnotations(QPainter& painter);
    void paintGuides(QPainter& painter);
    void paintShpGrid(QPainter& painter);
    void paintTmpOutlines(QPainter& painter);
    void paintShpOnion(QPainter& painter);
    void paintShpOffPalette(QPainter& painter);
    void paintShpShadow(QPainter& painter);
    void paintShpHouse(QPainter& painter);
    QImage shpCellImage(int cell) const;
    // The house-color preview of the last paint, kept while the pixels, the
    // region and the colors stay.
    struct ShpHouseCache {
        quint64 revision = 0;
        QRect region;
        QVector<QRgb> from;
        QVector<QRgb> to;
        QImage image;
    };
    ShpHouseCache shpHouseCache_;
    void paintSmartGuides(QPainter& painter);
    QString formatGuideDistance(double documentPixels) const;
    QList<SmartGuideLine> smartGuideLines_;
    QList<SmartGuideGap> smartGuideGaps_;
    bool smartGuidesEnabled_ = true;
    QList<GuideLine> guides_;
    ShpGrid shpGrid_;
    bool guidesVisible_ = true;
    QColor guideColor_{0x4a, 0xff, 0xff};
    bool guidesDashed_ = false;
    GuideLine guidePreview_;
    bool guidePreviewActive_ = false;
    void paintPathOverlay(QPainter& painter);
    void paintTypeOverlay(QPainter& painter);
    void paintMoveOutline(QPainter& painter);
    QRectF moveOutline_;
    QPolygonF controlsBox_;
    int measureUnit_ = 1;
    bool measureTraditional_ = false;
    std::function<double()> measurePpi_;
    void paintFilterPreview(QPainter& painter);
    QImage filterPreview_;
    QRect filterPreviewArea_;
    void paintMaskOverlay(QPainter& painter);
    QImage maskOverlay_;
    QRect maskOverlayArea_;
    PathOverlay pathOverlay_;
    TypeOverlay typeOverlay_;
    QTimer* typeCaretBlink_ = nullptr;
    bool typeCaretShown_ = true;
    QList<QPointF> samplerOverlay_;
    QList<QPointF> noteOverlay_;
    QList<CountOverlayMark> countOverlay_;
    int currentNote_ = -1;
    QLineF rulerLine_;
    bool rulerShown_ = false;
    QPolygonF perspectiveQuad_;
    // The perspective grid's spacing in document pixels (one inch); 0 hides it.
    double perspectiveGridSpacing_ = 0.0;
    QRectF cropBox_;
    bool cropPreview_ = false;
    bool cropRotating_ = false;
    int cropOverlay_ = 0;
    int cropOverlayOrientation_ = 0;
    CropShield cropShield_;
    bool cropBoxRotates_ = false;
    QPointF cropContentOffset_;
    double cropStraighten_ = 0.0;
    QPointF cropStraightenPivot_;
    QLineF cropStraightenLine_;
    QList<SliceOverlay> sliceOverlay_;
    QRectF sliceDrag_;
    void centreImage();
    void applyInitialView();
    void clampOffset();
    void updateAntsTimer();
    // The widget region covered by the animated outline(s), for a partial
    // repaint on each ants tick.
    QRect antsUpdateRect() const;

    struct PresentCache {
        qint64 key = -1;
        quint64 revision = 0;
        int level = -1;
        QRect docRect;
        // The level-space rectangle `crop` covers (docRect / 2^level).
        QRect levelRect;
        bool valid = false;
        int rebuilds = 0;
        QImage crop;
    };
    // The level the zoom (and a live preview) selects, or -1 when there is no
    // provider or document to present.
    int presentLevel() const;
    // The level-space source rectangle and its document-space target.
    bool presentLevelRect(int level, QRect& docRect, QRect& sourceRect) const;
    // The premultiplied crop of `level` over `sourceRect`, cached by frame,
    // revision, level and document rect. Level 0 is repaired in place from a
    // damage rect; a level with no zero-copy view uses the same cache. When only
    // the visible rectangle changed at the cached level, the overlap is reused
    // and only the newly exposed band is cropped.
    const QImage* presentCrop(int level, const QRect& docRect, const QRect& sourceRect);
    // Rebuild the cached crop from its own overlap plus the newly exposed band
    // for a changed visible rectangle at the same level. False (the caller
    // re-crops whole) when the rectangles do not overlap or a band comes back
    // short.
    bool patchPresentCache(int level, const QRect& docRect, const QRect& sourceRect);
    void dropPresentCache();

    // Held only for a canvas without a level provider (tests, dialogs); a
    // document canvas presents from pyramid crops and keeps none.
    QImage image_;
    QSize docSize_;
    // Bumped whenever the document is replaced; keys the present caches.
    qint64 frameKey_ = 0;
    int channelMask_ = 0x7;
    // The single visible channel as greyscale, rebuilt when the image or the
    // mask changes.
    QImage channelImage_;
    qint64 channelImageKey_ = -1;
    int channelImageMask_ = -1;
    // The visible channel when exactly one is, else -1.
    int singleChannel() const;
    // `source` with only the visible channel, as greyscale, cached by `key`.
    const QImage& channelImage(const QImage& source, qint64 key);
    QColor canvasColor_{Theme::workspaceColor()};
    double zoom_ = 1.0;
    QPointF offset_;
    double rotation_ = 0.0;
    bool compassVisible_ = false;
    void paintCompass(QPainter& painter);
    QPointF last_;
    bool panEnabled_ = true;
    bool freePan_ = true;
    bool snapToWorkspace_ = false;
    bool panning_ = false;
    bool spacePan_ = false;
    // False after a load/fit so the first resize settles the canvas, then any
    // pan/zoom (and the initial fit itself) sets it true so a later tool-switch
    // resize keeps the view instead of snapping the zoom.
    bool userAdjusted_ = false;
    // The viewport size the offset was last laid out against. An adjusted
    // view's resize shifts the offset by half the change so the document point
    // at the centre stays put (CS6 on maximize/restore). Not oldSize(): a
    // hidden view's deferred first resize carries none.
    QSize laidOutSize_;
    // A document may be set before the canvas has its laid-out viewport, so
    // the automatic fit runs against a stale size (tiny zoom, top-left clamp).
    // While the settle is pending every resize re-fits; the first visible
    // paint ends it, and any explicit user view change ends it early.
    bool pendingSettle_ = false;
    QPolygonF overlayPolygon_;

    QList<QPolygonF> selectionContours_;
    QPointF selectionContourOffset_;
    // The selection revision the stored contour was built for, and how many
    // contours have been applied (the cache test's no-repaint signal).
    quint64 selectionContourRevision_ = ~quint64(0);
    int selectionContourSetCount_ = 0;
    // The dirty rect the last paint was asked to draw, for the ants test.
    QRect lastPaintRect_;
    QList<QPolygonF> previewContours_;
    bool selectionPreviewClosed_ = true;
    bool selectionPreviewSolid_ = false;
    bool previewOriginActive_ = false;
    QPointF previewOrigin_;
    bool selectionEdgesVisible_ = true;
    int antsPhase_ = 0;
    int antsTicks_ = 0;
    QTimer* antsTimer_ = nullptr;

    PresentCache presentCache_;
    // What the last paint presented, for the present test hooks.
    int presentedLevel_ = -1;
    QRect presentedDocRect_;
    QSize presentedImageSize_;
    qint64 presentedImageKey_ = -1;
    LevelProvider levelProvider_;
    bool presentCacheRebuiltLastPaint_ = false;
    bool presentCacheEnabledForTest_ = true;
    bool presentLevelCropForTest_ = true;

    bool movePreviewActive_ = false;
    QImage moveBase_;
    QImage moveLayer_;
    QPointF moveLayerPos_;
    QPointF moveDelta_;
    double moveOpacity_ = 1.0;

    bool transformActive_ = false;
    double transformScaleX_ = 1.0;
    double transformScaleY_ = 1.0;
    double transformAngle_ = 0.0;
    double transformDx_ = 0.0;
    double transformDy_ = 0.0;
    QTransform transformProjective_;
    std::array<double, 9> transformProjectiveCoefficients_{};
    QRectF transformProjectiveSource_;
    // The warp covers only the visible region at the view scale, so paint
    // rebuilds it when either changes.
    WarpedTransformPreview transformWarped_;
    QRectF transformWarpedVisible_;
    double transformWarpedScale_ = 0.0;
    bool transformWarpedStale_ = true;
    bool transformProjectiveActive_ = false;
    QPolygonF transformQuad_;
    QPointF transformPivot_;
    bool transformPivotVisible_ = false;
    transform_api::WarpOverlay transformWarp_;
    // The pointer in image space, for revealing the warp handles near it.
    QPointF warpHover_;

    QString dragSizeText_;
    QPointF dragSizeImagePos_;
    bool dragSizeActive_ = false;

    bool brushOutlineActive_ = false;
    bool brushOutlineSquare_ = false;
    double brushOutlineDiameter_ = 0.0;
    QPointF brushOutlineImagePos_;

    bool samplingRingActive_ = false;
    double samplingRingDiameter_ = 0.0;
    QPointF samplingRingImagePos_;

    // Drag-start latency probe: reports the press -> handler -> first move ->
    // paint gaps to stderr when a drag start exceeds one frame, or always when
    // PICTURA_PRESS_TRACE is set. Left in deliberately; it is a few loads.
    struct PressTrace {
        QElapsedTimer clock;
        qint64 pressHwMs = -1;
        qint64 handledNs = -1;
        qint64 firstMoveNs = -1;
        qint64 firstMoveHwMs = -1;
        bool armed = false;
    };
    PressTrace pressTrace_;
};

/// Warp `source` (whose document origin is `sourceOrigin`) by `coefficients`
/// (source-local to destination-local, `QTransform` order), keeping only the
/// part inside `visibleDocument`, sampled at `scale` output pixels per document
/// pixel. Empty when nothing is visible.
WarpedTransformPreview warpTransformPreview(const QImage& source, const QPointF& sourceOrigin,
                                            const std::array<double, 9>& coefficients,
                                            const QRectF& sourceRect,
                                            const QRectF& visibleDocument, double scale);

} // namespace pictura
