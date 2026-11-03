//! GPU (wgpu/Vulkan) compositor for the separable and non-separable blend
//! modes and the supported adjustment layers.
//!
//! `composite_gpu` mirrors the CPU oracle [`crate::composite_rgba`] pass for
//! pass: the same per-layer loop, mask/opacity handling, pass-through/isolated
//! group semantics, and the W3C + reference blend formulas, executed by a
//! wgpu compute shader. It is proven, not assumed — the parity test in
//! `tests/gpu_parity.rs` diffs it against the CPU compositor within ±1 LSB.
//!
//! # Region compositing
//!
//! [`composite_region_active`] runs the same kernel over a clamped document
//! rectangle: the canvas, the mask, and group inner canvases are region-sized,
//! and each invocation maps a region-local index to document coordinates for
//! layer-rect and mask sampling. A layer source is region-sized only when it is
//! built on a miss; a hit binds the whole-document upload kept by a previous
//! full composite and uses its document-space layout. Only the region is
//! dispatched and read back, so the output is byte-identical to the same slice
//! of the full composite. `composite_gpu` is just the region kernel over the
//! whole document.
//!
//! # CPU-only modes
//!
//! Dissolve is the only CPU-only blend mode. [`composite_gpu`] returns
//! [`GpuError::UnsupportedMode`] for a visible layer using it;
//! [`composite_gpu_or_cpu`] then falls back to the CPU compositor. DarkerColor
//! and LighterColor *are* handled (they are per-pixel whole-RGB comparisons, not
//! triplet math), and the four non-separable modes (Hue, Saturation, Color,
//! Luminosity) run the PDF/CSS triplet math on the GPU.
//!
//! # Adjustment layers
//!
//! Invert, Posterize, Threshold, Brightness/Contrast and Hue/Saturation
//! adjustment layers are applied on the GPU at the layer's stack position. Any
//! other adjustment (or an undecodable block) returns
//! [`GpuError::UnsupportedAdjustment`]; the caller falls back to CPU.
//!
//! # Never panics on a missing GPU
//!
//! Adapter/device creation, buffer sizing beyond the device limits, and the
//! readback all return [`GpuError`] instead of unwinding. The caller keeps the
//! CPU path.
//!
//! ponytail: source samples and mask coverage are assembled on the CPU per
//! layer and uploaded as planar 8-bit bytes; the shader does the unpack, blend
//! and source-over. Move channel sampling into WGSL if upload bandwidth ever
//! shows up in a profile.

use std::fmt;
use std::sync::OnceLock;

use pictura_adjust::Adjustment;
use pictura_core::{
    BitDepth, BlendMode, Channel, ColorLabel, ColorMode, Document, Layer, LockFlags, PixelBuffer,
    PsdRect,
};

mod assemble;
mod backend;
mod clip;
mod effect;
mod failure;
mod lut;
mod plan;
mod raster;
mod resident;
mod select;
mod shader;
mod stroke;

pub use failure::{inject_gpu_failure, take_gpu_failure_notice};
pub use select::{
    backend_label, compiled_backends, dx12_compiler, dx12_compiler_label, env_candidates,
    gpu_backend_status, platform, probe_backend, probe_backend_outcome, resolve_candidates,
    selected_backend, set_backend, set_safe_mode, GpuBackend, GpuBackendStatus, Platform,
    ProbeOutcome, ProbeReport,
};
pub use stroke::{GpuPaintMode, GpuStroke, GpuStrokeParams};

pub(crate) use backend::{device_lost, grid_2d, shared_device};
use backend::{devices, Gpu};

#[cfg(test)]
use assemble::{
    assemble_mask, assemble_mask_fill, assemble_mask_per_pixel, assemble_source,
    assemble_source_per_pixel, assemble_source_rowwise, mask_has_data, mask_influence_rect,
    source_geom, Region,
};

/// Why the GPU compositor could not run. The caller falls back to CPU.
#[derive(Debug, Clone)]
pub enum GpuError {
    /// No GPU adapter on the selected backend, or the device could not be
    /// created.
    Unavailable,
    /// The document needs buffers larger than the device limits.
    TooLarge,
    /// The GPU output could not be mapped back to the host.
    Readback,
    /// A mode with no GPU implementation is present in the stack.
    UnsupportedMode(BlendMode),
    /// An adjustment layer with no GPU implementation is present in the stack.
    UnsupportedAdjustment,
    /// A channel-less smart-object layer is present: it renders from its
    /// embedded source, which the GPU has no path for.
    UnsupportedSmartObject,
    /// A channel-less type layer is present (it renders from its text model):
    /// the GPU has no path for it.
    UnsupportedText,
    /// A visible layer carries an enabled object-based layer effect.
    UnsupportedLayerEffect,
    /// A layer carries a non-default Blend If range, which the GPU has no
    /// shader for.
    UnsupportedAdvancedBlending,
    /// The document records a source bit depth: the CPU composites it from
    /// native-precision samples, which the 8-bit shader cannot match.
    UnsupportedDepth,
    /// A smart object's enabled filters apply: the CPU re-renders its source
    /// through them, which the GPU has no path for.
    UnsupportedSmartFilter,
}

impl fmt::Display for GpuError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            GpuError::Unavailable => {
                write!(f, "no usable GPU adapter/device on the selected backend")
            }
            GpuError::TooLarge => write!(f, "document exceeds GPU buffer limits"),
            GpuError::Readback => write!(f, "GPU readback failed"),
            GpuError::UnsupportedMode(m) => write!(f, "blend mode {m:?} is CPU-only"),
            GpuError::UnsupportedAdjustment => write!(f, "adjustment kind is CPU-only"),
            GpuError::UnsupportedSmartObject => {
                write!(f, "channel-less smart-object source is CPU-only")
            }
            GpuError::UnsupportedText => {
                write!(f, "channel-less type layer is CPU-only")
            }
            GpuError::UnsupportedLayerEffect => {
                write!(f, "layer effect is CPU-only")
            }
            GpuError::UnsupportedAdvancedBlending => {
                write!(f, "advanced blending (Blend If/knockout) is CPU-only")
            }
            GpuError::UnsupportedDepth => {
                write!(
                    f,
                    "a 16/32-bit source composites at native depth on the CPU"
                )
            }
            GpuError::UnsupportedSmartFilter => write!(f, "smart filters are CPU-only"),
        }
    }
}

impl std::error::Error for GpuError {}

/// The compositing backend that produced a buffer.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Backend {
    /// The wgpu compute compositor on the selected backend.
    Gpu,
    /// The CPU oracle [`crate::composite_rgba`].
    Cpu,
}

/// Composite `doc` on the GPU for Normal + every separable mode.
///
/// Returns the same 4-channel planar straight-alpha 8-bit buffer as
/// [`crate::composite_rgba`], within ±1 LSB for the supported modes. Returns
/// [`GpuError`] when no device on the selected backend is usable or the stack
/// contains a CPU-only mode; it never panics.
pub fn composite_gpu(doc: &Document) -> Result<PixelBuffer, GpuError> {
    composite_gpu_region(doc, 0, 0, doc.width, doc.height)
}

/// Composite a document-space region `[x0, x0+rw) × [y0, y0+rh)` on the GPU.
///
/// The same kernel as [`composite_gpu`], with every buffer (canvas, per-layer
/// source, mask, group inner canvases) sized to the region and each invocation
/// mapping a region-local index to document coordinates. `x0 + rw <= doc.width`
/// and `y0 + rh <= doc.height` are the caller's responsibility.
fn composite_gpu_region(
    doc: &Document,
    x0: u32,
    y0: u32,
    rw: u32,
    rh: u32,
) -> Result<PixelBuffer, GpuError> {
    // A stack the GPU cannot composite exactly must report why, even when the
    // device is unusable, so the eligibility check precedes the availability
    // gate.
    let region = PsdRect {
        left: x0 as i32,
        top: y0 as i32,
        right: (x0 + rw) as i32,
        bottom: (y0 + rh) as i32,
    };
    let full = x0 == 0 && y0 == 0 && rw == doc.width && rh == doc.height;
    let plans = plan::plan_stack(doc, (!full).then_some(region))
        .inspect_err(crate::metrics::count_decline)?;
    // A device that cannot reproduce the CPU oracle on the validation scene is
    // treated as no device: a wrong-but-plausible composite is worse than the
    // CPU path. See `gpu_composite_verified`.
    if !gpu_available() {
        crate::metrics::count_decline(&GpuError::Unavailable);
        return Err(GpuError::Unavailable);
    }
    composite_gpu_region_raw(doc, &plans, x0, y0, rw, rh).inspect_err(crate::metrics::count_decline)
}

/// The ungated region kernel. Callers own the plan: [`composite_gpu_region`]
/// builds it before the availability gate, and [`gpu_composite_verified`]
/// checks its static validation scene. Splitting it out lets the validation
/// bypass [`gpu_available`] without recursing. A device error or a panic while
/// it runs turns the GPU off and answers [`GpuError::Unavailable`]
/// (`failure.rs`).
fn composite_gpu_region_raw(
    doc: &Document,
    plans: &[plan::LayerPlan<'_>],
    x0: u32,
    y0: u32,
    rw: u32,
    rh: u32,
) -> Result<PixelBuffer, GpuError> {
    if rw == 0 || rh == 0 {
        return Ok(PixelBuffer::new(rw, rh, 4));
    }
    failure::guard(|| {
        let gpu = Gpu::new(x0, y0, rw, rh)?;
        let canvas = gpu.zero_canvas();
        for layer_plan in plans {
            gpu.composite_plan(&canvas, layer_plan, doc);
        }
        gpu.read_canvas(&canvas)
    })
}

/// Composite only `rect` (clamped to the document) through the active backend.
///
/// Returns a `rect`-sized buffer, byte-identical to the corresponding
/// sub-rectangle of `composite_active(doc, gpu_enabled).0`. An empty clamped
/// intersection returns a zero-dimension buffer and never panics.
pub fn composite_region_active(
    doc: &Document,
    rect: PsdRect,
    gpu_enabled: bool,
) -> (PixelBuffer, Backend) {
    let x0 = rect.left.max(0);
    let y0 = rect.top.max(0);
    let x1 = rect.right.min(doc.width as i32);
    let y1 = rect.bottom.min(doc.height as i32);
    if x1 <= x0 || y1 <= y0 {
        // Nothing ran, so report the fallback backend rather than a device.
        return (PixelBuffer::new(0, 0, 4), Backend::Cpu);
    }
    let w = (x1 - x0) as u32;
    let h = (y1 - y0) as u32;
    let (x0, y0) = (x0 as u32, y0 as u32);
    if gpu_enabled && gpu_available() {
        if let Ok(buf) = composite_gpu_region(doc, x0, y0, w, h) {
            return (buf, Backend::Gpu);
        }
    }
    (composite_cpu_region(doc, x0, y0, w, h), Backend::Cpu)
}

/// The CPU region fallback.
///
/// Composites only the requested region. Cached sources are sampled and effect
/// passes are replayed into the region, so the result is byte-identical to the
/// same slice of the full CPU composite for every stack.
fn composite_cpu_region(doc: &Document, x0: u32, y0: u32, rw: u32, rh: u32) -> PixelBuffer {
    crate::composite::composite_rgba_region(doc, x0, y0, rw, rh)
}

/// Try [`composite_gpu`]; on any [`GpuError`] fall back to the CPU oracle.
pub fn composite_gpu_or_cpu(doc: &Document) -> PixelBuffer {
    composite_gpu(doc).unwrap_or_else(|_| crate::composite_rgba(doc))
}

/// Whether a usable adapter/device exists on the selected backend and its
/// compositor reproduces the CPU oracle.
///
/// Cached behind `devices()`'s and [`gpu_composite_verified`]'s `OnceLock`s;
/// cheap to call repeatedly and never panics.
pub fn gpu_available() -> bool {
    devices().is_ok() && gpu_composite_verified()
}

/// Whether a device exists on the selected backend, without running the CPU
/// oracle validation. Lets the probe distinguish "no adapter" from "adapter
/// present but validation failed". Never panics.
pub fn device_available() -> bool {
    devices().is_ok()
}

/// A read-only summary of the adapter the compositor would use.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GpuAdapterInfo {
    pub name: String,
    pub device_type: String,
    pub backend: String,
    pub driver: String,
    pub driver_info: String,
}

/// The cached compositor adapter's summary, or `None` when no usable adapter
/// exists. Never panics.
pub fn gpu_adapter_info() -> Option<GpuAdapterInfo> {
    let info = devices().ok()?.adapter_info();
    Some(GpuAdapterInfo {
        name: info.name,
        device_type: format!("{:?}", info.device_type),
        backend: format!("{:?}", info.backend),
        driver: info.driver,
        driver_info: info.driver_info,
    })
}

/// How many times the validation scene ran, for the at-most-once test.
#[cfg(test)]
static VALIDATION_RUNS: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);

/// Whether the device's compute compositor reproduces the CPU oracle.
///
/// Some drivers (observed on Mesa's Honeykrisp for the Apple M1) accept the
/// compositor's pipelines but return wrong pixels for anything but the
/// simplest stacks. A wrong composite is not a [`GpuError`], so it cannot be
/// caught by the per-call fallback; this one-time check composites a small
/// representative scene on the GPU and compares it to [`crate::composite_rgba`].
/// A mismatch disables the GPU for the process, so every compositing call keeps
/// the CPU path. Cached; the check runs at most once.
fn gpu_composite_verified() -> bool {
    static VERIFIED: OnceLock<bool> = OnceLock::new();
    *VERIFIED.get_or_init(|| {
        #[cfg(test)]
        VALIDATION_RUNS.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        let doc = validation_document();
        let Ok(plans) = plan::plan_stack(&doc, None) else {
            backend::record_error("validation plan failed");
            return false;
        };
        let cpu = crate::composite_rgba(&doc);
        let Ok(gpu) = composite_gpu_region_raw(&doc, &plans, 0, 0, doc.width, doc.height) else {
            backend::record_error("validation composite failed");
            return false;
        };
        let matches = gpu.width == cpu.width
            && gpu.height == cpu.height
            && gpu.data.len() == cpu.data.len()
            && gpu
                .data
                .iter()
                .zip(cpu.data.iter())
                .all(|(g, c)| g.abs_diff(*c) <= 1);
        if !matches {
            backend::record_error("validation mismatch");
        }
        matches
    })
}

/// A tiny scene that exercises the paths a broken driver gets wrong: an opaque
/// gradient backdrop plus a half-transparent gradient source in a separable
/// mode. Kept small so the check is cheap; a uniform-white stack would pass on a
/// device that only composites the first few pixels correctly.
fn validation_document() -> Document {
    const N: u32 = 8;
    let layer = |name: &str, blend: BlendMode, sample: fn(u32, u32) -> (u8, u8, u8, u8)| {
        let mut channels: Vec<Channel> = (0..4)
            .map(|id| Channel {
                id: if id == 3 { -1 } else { id as i16 },
                data: vec![0u8; (N * N) as usize].into(),
            })
            .collect();
        for y in 0..N {
            for x in 0..N {
                let (r, g, b, a) = sample(x, y);
                let i = (y * N + x) as usize;
                channels[0].data.as_mut_slice()[i] = r;
                channels[1].data.as_mut_slice()[i] = g;
                channels[2].data.as_mut_slice()[i] = b;
                channels[3].data.as_mut_slice()[i] = a;
            }
        }
        Layer {
            name: name.into(),
            rect: PsdRect {
                top: 0,
                left: 0,
                bottom: N as i32,
                right: N as i32,
            },
            blend,
            opacity: 255,
            fill: 255,
            lock: LockFlags::default(),
            color: ColorLabel::None,
            clipping: false,
            visible: true,
            mask: None,
            adjustment: None,
            channels,
            children: pictura_core::LayerStack::default(),
            is_group: false,
            background: false,
            ..Default::default()
        }
    };
    let mut doc = Document::new(N, N, ColorMode::Rgb, BitDepth::Eight);
    doc.layers = pictura_core::LayerStack::from_layers(vec![
        layer("base", BlendMode::Normal, |x, y| {
            (x as u8 * 32, y as u8 * 32, (x + y) as u8 * 16, 255)
        }),
        layer("top", BlendMode::Multiply, |x, y| {
            (
                x as u8 * 16 + 8,
                y as u8 * 16 + 8,
                255 - x as u8 * 16,
                64 + x as u8 * 16,
            )
        }),
    ]);
    doc
}

/// Composite `doc` on the GPU when `gpu_enabled && gpu_available()`, otherwise
/// on the CPU, reporting which backend ran.
///
/// Never panics: any [`GpuError`] (unavailable device, oversize document,
/// unsupported mode/adjustment, readback failure) falls back to the CPU
/// composite for that call.
pub fn composite_active(doc: &Document, gpu_enabled: bool) -> (PixelBuffer, Backend) {
    if gpu_enabled && gpu_available() {
        if let Ok(buf) = composite_gpu(doc) {
            return (buf, Backend::Gpu);
        }
    }
    (crate::composite_rgba(doc), Backend::Cpu)
}

/// No adjustment: the ordinary blend/source-over dispatch.
const NO_ADJ: (u32, i32, i32, i32) = (0, 0, 0, 0);

/// A COMPUTE-visible storage-buffer bind-group layout entry, shared by the
/// compositor and filter pipelines (both alias their storage as `array<u32>`).
pub(crate) fn storage_entry(binding: u32, read_only: bool) -> wgpu::BindGroupLayoutEntry {
    wgpu::BindGroupLayoutEntry {
        binding,
        visibility: wgpu::ShaderStages::COMPUTE,
        ty: wgpu::BindingType::Buffer {
            ty: wgpu::BufferBindingType::Storage { read_only },
            has_dynamic_offset: false,
            min_binding_size: None,
        },
        count: None,
    }
}

/// Map a decoded adjustment to the shader's kind id plus up to three integer
/// params, or `None` when the GPU has no implementation for it.
fn adjustment_params(adjustment: &Adjustment) -> Option<(u32, i32, i32, i32)> {
    match adjustment {
        Adjustment::Invert => Some((1, 0, 0, 0)),
        Adjustment::Posterize(levels) => Some((2, i32::from(*levels), 0, 0)),
        Adjustment::Threshold(level) => Some((3, i32::from(*level), 0, 0)),
        Adjustment::BrightnessContrast(p) => {
            Some((4, i32::from(p.brightness), i32::from(p.contrast), 0))
        }
        // The shader carries Master only; colour ranges run on the CPU.
        Adjustment::HueSaturation(p) if p.ranges.is_empty() => Some((
            5,
            i32::from(p.hue),
            i32::from(p.saturation),
            i32::from(p.lightness),
        )),
        _ => None,
    }
}

/// Whether the GPU can composite `doc` exactly as the CPU oracle would.
#[cfg(test)]
fn check_supported(doc: &Document) -> Result<(), GpuError> {
    plan::plan_stack(doc, None).map(|_| ())
}

/// Fail a device-dependent test that has no usable GPU. Such tests are
/// `#[ignore]`d, so reaching this without a device is a misconfigured run, not
/// a pass.
#[cfg(test)]
pub(crate) fn require_gpu() {
    require_device(gpu_available());
}

#[cfg(test)]
pub(crate) fn require_device(available: bool) {
    assert!(
        available,
        "no usable GPU device on the selected backend; this test is #[ignore]d and needs one \
         (software-gpu in CI)"
    );
}

#[cfg(test)]
mod eligibility_tests;

/// Blend mode ids shared with the WGSL `switch`. `PassThrough` is Normal here
/// because the CPU isolated-group path treats it as Normal, and true
/// pass-through groups never reach a dispatch.
fn mode_id(mode: BlendMode) -> u32 {
    match mode {
        BlendMode::Normal | BlendMode::PassThrough => 1,
        BlendMode::Darken => 2,
        BlendMode::Multiply => 3,
        BlendMode::ColorBurn => 4,
        BlendMode::LinearBurn => 5,
        BlendMode::DarkerColor => 6,
        BlendMode::Lighten => 7,
        BlendMode::Screen => 8,
        BlendMode::ColorDodge => 9,
        BlendMode::LinearDodge => 10,
        BlendMode::LighterColor => 11,
        BlendMode::Overlay => 12,
        BlendMode::SoftLight => 13,
        BlendMode::HardLight => 14,
        BlendMode::VividLight => 15,
        BlendMode::LinearLight => 16,
        BlendMode::PinLight => 17,
        BlendMode::HardMix => 18,
        BlendMode::Difference => 19,
        BlendMode::Exclusion => 20,
        BlendMode::Subtract => 21,
        BlendMode::Divide => 22,
        BlendMode::Hue => 23,
        BlendMode::Saturation => 24,
        BlendMode::Color => 25,
        BlendMode::Luminosity => 26,
        // Rejected by `check_supported` before any dispatch.
        BlendMode::Dissolve => 0,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use pictura_core::{
        BitDepth, Channel, ColorLabel, ColorMode, Document, Layer, LayerMask, LockFlags, PsdRect,
        SmartObject, SmartObjectKind,
    };
    use std::time::Instant;

    #[test]
    fn the_validation_scene_is_a_real_blend_of_alpha_pixels() {
        let doc = validation_document();
        assert_eq!((doc.width, doc.height), (8, 8));
        assert_eq!(doc.layers.len(), 2, "a backdrop plus a blended source");
        // The source is partially transparent and not a flat colour: a device
        // that drops alpha or composites only the first few pixels diverges.
        let alpha = doc.layers[1]
            .channels
            .iter()
            .find(|c| c.id == -1)
            .expect("the source carries alpha");
        assert!(alpha.data.iter().any(|&a| a != 0 && a != 255));
        let red = doc.layers[1]
            .channels
            .iter()
            .find(|c| c.id == 0)
            .expect("the source carries red");
        assert!(red.data.iter().any(|&r| r != red.data[0]));
    }

    /// The gate and the composite must agree: a device is available exactly when
    /// the validation scene composites on it, and a device that fails validation
    /// reports `Unavailable` so the caller keeps the CPU path.
    #[test]
    fn the_gpu_gate_is_consistent_with_the_validation_scene() {
        let doc = validation_document();
        if gpu_available() {
            assert!(
                composite_gpu(&doc).is_ok(),
                "an available device must composite the validation scene"
            );
        } else {
            assert!(matches!(composite_gpu(&doc), Err(GpuError::Unavailable)));
        }
    }

    /// The largest per-byte difference between two composites.
    fn max_delta(a: &PixelBuffer, b: &PixelBuffer) -> u8 {
        assert_eq!(a.data.len(), b.data.len());
        a.data
            .iter()
            .zip(b.data.iter())
            .map(|(x, y)| x.abs_diff(*y))
            .max()
            .unwrap_or(0)
    }

    #[test]
    #[ignore = "requires a Vulkan device; run with --profile gpu --run-ignored all"]
    fn resident_uploads_are_reused_and_never_stale() {
        require_gpu();
        let (w, h) = (64u32, 48u32);
        let mut doc = Document::new(w, h, ColorMode::Rgb, BitDepth::Eight);
        let rect = PsdRect {
            top: 0,
            left: 0,
            bottom: h as i32,
            right: w as i32,
        };
        doc.layers = pictura_core::LayerStack::from_layers(
            (0..2u8)
                .map(|k| Layer {
                    rect,
                    opacity: 200,
                    fill: 255,
                    visible: true,
                    channels: (-1..3)
                        .map(|id| Channel {
                            id,
                            data: (0..(w * h) as usize)
                                .map(|i| (i * (3 + k as usize) + (id + 1) as usize * 40) as u8)
                                .collect(),
                        })
                        .collect(),
                    ..Default::default()
                })
                .collect(),
        );
        // Stamp every plane, as the history does after each capture.
        let stamp_all = |doc: &mut Document| {
            for layer in &mut doc.layers {
                for c in &mut layer.channels {
                    c.data.set_stamp(pictura_core::fresh_stamp());
                }
            }
        };
        stamp_all(&mut doc);
        let check = |doc: &Document, what: &str| {
            let gpu = composite_gpu(doc).expect("gpu composite");
            let cpu = crate::composite_rgba(doc);
            assert!(
                max_delta(&gpu, &cpu) <= 1,
                "{what}: GPU differs from the CPU"
            );
        };

        check(&doc, "first");
        let hits = crate::metrics::counters().resident_hits;
        check(&doc, "again");
        assert!(
            crate::metrics::counters().resident_hits > hits,
            "an unchanged document re-uploaded its layers"
        );

        doc.layers[1].blend = BlendMode::Multiply;
        check(&doc, "blend change");

        // A write clears the plane's stamp: the cache must not serve it.
        doc.layers[1].channels[1].data.as_mut_slice()[100] ^= 0xFF;
        check(&doc, "unstamped edit");

        // The history then stamps the new bytes afresh.
        doc.layers[1].channels[1].data.as_mut_slice()[200] ^= 0xFF;
        stamp_all(&mut doc);
        check(&doc, "restamped edit");
    }

    #[test]
    fn gpu_adapter_info_never_panics() {
        if let Some(info) = super::gpu_adapter_info() {
            assert!(!info.name.is_empty(), "adapter name");
            assert!(!info.device_type.is_empty(), "device type");
            assert!(!info.backend.is_empty(), "backend");
        }
    }

    #[test]
    #[cfg(not(feature = "software-gpu"))]
    fn a_software_adapter_is_no_gpu() {
        assert!(!backend::accelerates(wgpu::DeviceType::Cpu));
        for real in [
            wgpu::DeviceType::DiscreteGpu,
            wgpu::DeviceType::IntegratedGpu,
            wgpu::DeviceType::VirtualGpu,
            wgpu::DeviceType::Other,
        ] {
            assert!(backend::accelerates(real), "{real:?}");
        }
    }

    #[test]
    fn grid_2d_tiles_without_gaps_or_overlap() {
        for n in [1u32, 63, 64, 65, 4096, 4_194_241, 8_388_608, 16_000_000] {
            let (gx, gy) = grid_2d(n, 65535).expect("65535 limit accepts every u32 count");
            assert!((1..=65535).contains(&gx), "gx {gx} out of range");
            assert!((1..=65535).contains(&gy), "gy {gy} out of range");
            // Invocation (x, y) maps to x + y * gx * 64, so the rows are
            // contiguous [y*gx*64, (y+1)*gx*64); covering n needs the product.
            assert!(gx as u64 * 64 * gy as u64 >= n as u64);
        }
    }

    #[test]
    fn grid_2d_rejects_product_past_limit_with_synthetic_small_limit() {
        // Synthetic 2-workgroup limit => 2*2*64 = 256 items.
        assert_eq!(grid_2d(256, 2), Some((2, 2)));
        assert_eq!(grid_2d(257, 2), None);
        // A limit of 1 rejects anything past a single 64-wide row.
        assert_eq!(grid_2d(64, 1), Some((1, 1)));
        assert_eq!(grid_2d(65, 1), None);
    }

    fn px_rect(w: u32, h: u32) -> PsdRect {
        PsdRect {
            top: 0,
            left: 0,
            bottom: h as i32,
            right: w as i32,
        }
    }

    fn pix_layer(w: u32, h: u32, channels: Vec<Channel>) -> Layer {
        Layer {
            name: "t".into(),
            rect: px_rect(w, h),
            blend: BlendMode::Normal,
            opacity: 255,
            fill: 255,
            lock: LockFlags::default(),
            color: ColorLabel::None,
            clipping: false,
            visible: true,
            mask: None,
            adjustment: None,
            channels,
            children: pictura_core::LayerStack::default(),
            is_group: false,
            background: false,
            ..Default::default()
        }
    }

    fn ramp(n: usize, seed: u8) -> Vec<u8> {
        (0..n)
            .map(|i| (i as u8).wrapping_mul(31).wrapping_add(seed))
            .collect()
    }

    #[test]
    fn rowwise_source_matches_per_pixel() {
        let (w, h) = (5u32, 3u32);
        let n = (w * h) as usize;
        let region = Region { x0: 0, y0: 0, w, h };
        let doc = Document::new(w, h, ColorMode::Rgb, BitDepth::Eight);
        let chans = |ids: &[i16]| -> Vec<Channel> {
            ids.iter()
                .map(|&id| Channel {
                    id,
                    data: ramp(n, id as u8).into(),
                })
                .collect()
        };

        // RGB with all four channels: row-wise and per-pixel agree.
        let all = pix_layer(w, h, chans(&[0, 1, 2, -1]));
        let g = source_geom(region, &all, &doc).unwrap();
        let fast = assemble_source_rowwise(&all, &g).expect("covering RGB layer is row-wise");
        assert_eq!(fast, assemble_source_per_pixel(&all, &g));
        assert_eq!(assemble_source(region, &all, &doc).unwrap().0, fast);

        // Missing alpha: the alpha plane fills 255.
        let no_alpha = pix_layer(w, h, chans(&[0, 1, 2]));
        let g = source_geom(region, &no_alpha, &doc).unwrap();
        let fast = assemble_source_rowwise(&no_alpha, &g).expect("missing alpha is row-wise");
        assert_eq!(fast, assemble_source_per_pixel(&no_alpha, &g));
        assert!(
            fast[3 * n..4 * n].iter().all(|&b| b == 255),
            "absent alpha must read 255"
        );

        // Missing green/blue alias channel 0.
        let mono = pix_layer(w, h, chans(&[0, -1]));
        let g = source_geom(region, &mono, &doc).unwrap();
        let fast = assemble_source_rowwise(&mono, &g).expect("absent G/B alias channel 0");
        assert_eq!(fast, assemble_source_per_pixel(&mono, &g));
        assert_eq!(&fast[n..2 * n], &fast[..n]);
        assert_eq!(&fast[2 * n..3 * n], &fast[..n]);

        // Grayscale document: exactly two planes, colour from channel 0.
        let gdoc = Document::new(w, h, ColorMode::Grayscale, BitDepth::Eight);
        let gray = pix_layer(w, h, chans(&[0, -1]));
        let g = source_geom(region, &gray, &gdoc).unwrap();
        assert_eq!(g.planes, 2, "grayscale is two planes");
        let fast = assemble_source_rowwise(&gray, &g).expect("gray layer is row-wise");
        assert_eq!(fast, assemble_source_per_pixel(&gray, &g));

        // A short colour plane cannot cover the intersection -> fallback.
        let mut short = pix_layer(w, h, chans(&[0, 1, 2, -1]));
        short.channels[0].data.truncate(n - 1);
        let g = source_geom(region, &short, &doc).unwrap();
        assert!(
            assemble_source_rowwise(&short, &g).is_none(),
            "a short plane must take the per-pixel fallback"
        );
        assert_eq!(
            assemble_source(region, &short, &doc).unwrap().0,
            assemble_source_per_pixel(&short, &g)
        );
    }

    #[test]
    fn rowwise_mask_matches_per_pixel() {
        let (w, h) = (6u32, 4u32);
        let n = (w * h) as usize;
        let region = Region { x0: 0, y0: 0, w, h };

        // Maskless pixel layer -> constant-255 row fill.
        let mut l = pix_layer(w, h, Vec::new());
        assert!(!mask_has_data(&l));
        let r = mask_influence_rect(region, &l);
        assert_eq!(
            assemble_mask(region, &l),
            assemble_mask_per_pixel(region, &l, r)
        );
        assert_eq!(assemble_mask(region, &l), assemble_mask_fill(region, r));

        // A disabled mask still reads 255 everywhere.
        l.mask = Some(LayerMask {
            rect: px_rect(w, h),
            default_color: 9,
            disabled: true,
            flags: 0,
            data: Some(ramp(n, 5).into()),
            ..Default::default()
        });
        assert!(!mask_has_data(&l));
        let r = mask_influence_rect(region, &l);
        assert_eq!(
            assemble_mask(region, &l),
            assemble_mask_per_pixel(region, &l, r)
        );
        assert_eq!(assemble_mask(region, &l), assemble_mask_fill(region, r));

        // An enabled data-carrying mask keeps the per-pixel path.
        l.mask = Some(LayerMask {
            rect: px_rect(w, h),
            default_color: 9,
            disabled: false,
            flags: 0,
            data: Some(ramp(n, 5).into()),
            ..Default::default()
        });
        assert!(mask_has_data(&l));
        let r = mask_influence_rect(region, &l);
        assert_eq!(
            assemble_mask(region, &l),
            assemble_mask_per_pixel(region, &l, r)
        );
    }

    #[test]
    fn channel_less_smart_object_is_accepted() {
        let (w, h) = (2u32, 2u32);
        let n = (w * h) as usize;
        let mut source = Document::new(w, h, ColorMode::Rgb, BitDepth::Eight);
        for i in 0..n {
            source.composite.data[i] = 10;
            source.composite.data[n + i] = 20;
            source.composite.data[2 * n + i] = 30;
        }
        let payload = pictura_codec::write_psd(&source).expect("payload writes");

        let mut doc = Document::new(w, h, ColorMode::Rgb, BitDepth::Eight);
        doc.layers = pictura_core::LayerStack::from_layers(vec![Layer {
            name: "placed".into(),
            rect: px_rect(w, h),
            channels: Vec::new(),
            smart_object: Some(
                {
                    let mut so = SmartObject::default();
                    so.filename = "source.psd".into();
                    so.kind = SmartObjectKind::Embedded;
                    so
                }
                .with_payload(payload),
            ),
            ..Default::default()
        }]);

        // The cached smart source is accepted; the CPU oracle renders the
        // embedded source rather than the channel-less layer's opaque black.
        assert!(check_supported(&doc).is_ok());
        let (out, backend) = composite_active(&doc, false);
        assert_eq!(backend, Backend::Cpu);
        for i in 0..n {
            assert_eq!(
                [
                    out.data[i],
                    out.data[n + i],
                    out.data[2 * n + i],
                    out.data[3 * n + i]
                ],
                [10, 20, 30, 255],
                "pixel {i}"
            );
        }
    }

    fn profile_layer(n: u32, seed: u32, blend: BlendMode) -> Layer {
        let px = (n as usize) * (n as usize);
        let (mut r, mut g, mut b, mut a) =
            (vec![0u8; px], vec![0u8; px], vec![0u8; px], vec![0u8; px]);
        for y in 0..n {
            for x in 0..n {
                let i = (y * n + x) as usize;
                r[i] = (x * 7 + seed * 31) as u8;
                g[i] = (y * 5 + seed * 17) as u8;
                b[i] = (x ^ y) as u8;
                a[i] = if seed == 0 { 255 } else { 160 };
            }
        }
        Layer {
            name: format!("layer{seed}"),
            rect: px_rect(n, n),
            blend,
            opacity: 255,
            fill: 255,
            lock: LockFlags::default(),
            color: ColorLabel::None,
            clipping: false,
            visible: true,
            mask: None,
            adjustment: None,
            channels: vec![
                Channel {
                    id: 0,
                    data: r.into(),
                },
                Channel {
                    id: 1,
                    data: g.into(),
                },
                Channel {
                    id: 2,
                    data: b.into(),
                },
                Channel {
                    id: -1,
                    data: a.into(),
                },
            ],
            children: pictura_core::LayerStack::default(),
            is_group: false,
            background: false,
            ..Default::default()
        }
    }

    /// The transfer half of the readback only (staging + copy + poll + map), so
    /// the profile can separate it from the de-interleave.
    fn read_transfer(gpu: &Gpu, canvas: &wgpu::Buffer) -> Result<(), GpuError> {
        let size = u64::from(gpu.n) * 4;
        let staging = gpu.device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("pictura-profile-transfer"),
            size,
            usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let mut encoder = gpu
            .device
            .create_command_encoder(&wgpu::CommandEncoderDescriptor {
                label: Some("pictura-profile-transfer"),
            });
        encoder.copy_buffer_to_buffer(canvas, 0, &staging, 0, size);
        gpu.queue.submit(Some(encoder.finish()));
        let slice = staging.slice(..);
        let (tx, rx) = std::sync::mpsc::channel();
        slice.map_async(wgpu::MapMode::Read, move |r| {
            let _ = tx.send(r);
        });
        let _ = gpu.device.poll(wgpu::PollType::wait_indefinitely());
        rx.recv()
            .map_err(|_| GpuError::Readback)?
            .map_err(|_| GpuError::Readback)?;
        let _mapped = slice.get_mapped_range().map_err(|_| GpuError::Readback)?;
        Ok(())
    }

    fn phase_ms(t: Instant) -> f64 {
        t.elapsed().as_secs_f64() * 1000.0
    }

    fn run_profile(n: u32) {
        if !gpu_available() {
            println!("gpu_phase {n}x{n} n/a: no usable Vulkan adapter");
            return;
        }
        let mut doc = Document::new(n, n, ColorMode::Rgb, BitDepth::Eight);
        doc.layers = pictura_core::LayerStack::from_layers(vec![
            profile_layer(n, 0, BlendMode::Normal),
            profile_layer(n, 1, BlendMode::Multiply),
        ]);
        // Warm-up keeps device/pipeline setup out of the phase numbers.
        let _ = composite_gpu_region(&doc, 0, 0, n, n);
        let gpu = match Gpu::new(0, 0, n, n) {
            Ok(g) => g,
            Err(e) => {
                println!("gpu_phase {n}x{n} n/a: {e}");
                return;
            }
        };

        let t = Instant::now();
        let canvas = gpu.zero_canvas();
        println!("gpu_phase zero_canvas {:.3} ms", phase_ms(t));

        let t = Instant::now();
        let srcs: Vec<_> = doc
            .layers
            .iter()
            .map(|l| gpu.build_source(l, &doc))
            .collect();
        println!("gpu_phase build_source {:.3} ms", phase_ms(t));

        let t = Instant::now();
        let masks: Vec<_> = doc.layers.iter().map(|l| gpu.build_mask(l, &doc)).collect();
        println!("gpu_phase build_mask {:.3} ms", phase_ms(t));

        let t = Instant::now();
        for (i, layer) in doc.layers.iter().enumerate() {
            if let Some((src, layout)) = &srcs[i] {
                gpu.dispatch(
                    &canvas,
                    src,
                    &masks[i],
                    *layout,
                    layer.blend,
                    layer.opacity,
                    layer.fill,
                    layer.type_tool.is_some(),
                    NO_ADJ,
                    None,
                );
            }
        }
        println!("gpu_phase dispatch {:.3} ms", phase_ms(t));

        let t = Instant::now();
        let _ = read_transfer(&gpu, &canvas);
        println!("gpu_phase read_submit_poll_map {:.3} ms", phase_ms(t));

        // Standalone de-interleave micro-benchmark on a synthetic packed buffer;
        // it is not part of the composite total (whose de-interleave is now the
        // GPU planar pass plus plane copies in `read_canvas`, printed as
        // `readback` below). Kept to measure the retained host gather.
        let packed = vec![0u8; gpu.n as usize * 4];
        let t = Instant::now();
        let _ = gpu.to_pixel_buffer(&packed);
        println!(
            "gpu_phase to_pixel_buffer_micro (not in total) {:.3} ms",
            phase_ms(t)
        );

        let t = Instant::now();
        let _ = gpu.read_canvas(&canvas);
        println!("gpu_phase readback {:.3} ms", phase_ms(t));

        let t = Instant::now();
        let _ = composite_gpu_region(&doc, 0, 0, n, n);
        println!("gpu_phase total {:.3} ms", phase_ms(t));
    }

    #[test]
    #[ignore = "requires a Vulkan GPU; run with --ignored --nocapture"]
    fn composite_profile_4000() {
        run_profile(4000);
    }

    #[test]
    #[ignore = "requires a Vulkan GPU; run with --ignored --nocapture"]
    fn composite_profile_1024() {
        run_profile(1024);
    }
}
