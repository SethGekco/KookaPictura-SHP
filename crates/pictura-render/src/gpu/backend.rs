use std::borrow::Cow;
use std::sync::{Mutex, OnceLock};

use pictura_core::{BlendMode, Document, Layer, PixelBuffer};

use super::assemble::{mask_writer, padded, source_geom, source_writer, Region, RUN};
use super::resident::{mask_key, source_key, Resident, UploadKey};
use super::shader::{PLANAR_SHADER, SHADER, STROKE_SHADER};
use super::{mode_id, storage_entry, GpuError, NO_ADJ};

/// Bind-group layout and compute pipeline, both size-independent (buffer sizes
/// travel as bindings), so created once per shared device instead of rebuilt on
/// every composite. The params uniform lives on [`Gpu`] instead: it carries
/// per-dispatch state, so sharing one across concurrent composites raced.
struct ComputeResources {
    pipeline: wgpu::ComputePipeline,
    layout: wgpu::BindGroupLayout,
}

impl ComputeResources {
    fn new(device: &wgpu::Device) -> Self {
        let layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("pictura-blend-layout"),
            entries: &[
                storage_entry(0, false),
                storage_entry(1, true),
                storage_entry(2, true),
                wgpu::BindGroupLayoutEntry {
                    binding: 3,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    count: None,
                },
                storage_entry(4, true),
                storage_entry(5, true),
                storage_entry(6, true),
            ],
        });
        let module = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("pictura-blend"),
            source: wgpu::ShaderSource::Wgsl(Cow::Borrowed(SHADER)),
        });
        let pipeline_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("pictura-blend"),
            bind_group_layouts: &[Some(&layout)],
            immediate_size: 0,
        });
        let pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
            label: Some("pictura-blend"),
            layout: Some(&pipeline_layout),
            module: &module,
            entry_point: Some("cs_main"),
            compilation_options: Default::default(),
            cache: None,
        });
        Self { pipeline, layout }
    }
}

/// Bind-group layout and compute pipeline for the planar readback pass. Like
/// [`ComputeResources`] it is size-independent (the canvas and planar sizes and
/// the grid stride travel as bindings), so created once per device.
pub(super) struct PlanarResources {
    pub(super) pipeline: wgpu::ComputePipeline,
    pub(super) layout: wgpu::BindGroupLayout,
}

impl PlanarResources {
    fn new(device: &wgpu::Device) -> Self {
        let layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("pictura-planar-layout"),
            entries: &[
                storage_entry(0, true),
                storage_entry(1, false),
                wgpu::BindGroupLayoutEntry {
                    binding: 2,
                    visibility: wgpu::ShaderStages::COMPUTE,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    count: None,
                },
            ],
        });
        let module = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("pictura-planar"),
            source: wgpu::ShaderSource::Wgsl(Cow::Borrowed(PLANAR_SHADER)),
        });
        let pipeline_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("pictura-planar"),
            bind_group_layouts: &[Some(&layout)],
            immediate_size: 0,
        });
        let pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
            label: Some("pictura-planar"),
            layout: Some(&pipeline_layout),
            module: &module,
            entry_point: Some("cs_planar"),
            compilation_options: Default::default(),
            cache: None,
        });
        Self { pipeline, layout }
    }
}

/// Bind-group layout and compute pipeline for one paint dab. Like the others,
/// size-independent (sizes travel as bindings), so created once per device.
pub(super) struct StrokeResources {
    pub(super) pipeline: wgpu::ComputePipeline,
    pub(super) layout: wgpu::BindGroupLayout,
}

impl StrokeResources {
    fn new(device: &wgpu::Device) -> Self {
        // 0 layer (read-write), 1 coverage (read-write), 2 base (read-only),
        // 3 params. The dab recomposites each changed pixel from the immutable
        // base, exactly as the CPU stroke does, so overlapping dabs cannot
        // compound.
        let layout = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("pictura-stroke-layout"),
            entries: &[
                storage_entry(0, false),
                storage_entry(1, false),
                storage_entry(2, true),
                storage_entry(3, true),
            ],
        });
        let module = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("pictura-stroke"),
            source: wgpu::ShaderSource::Wgsl(Cow::Borrowed(STROKE_SHADER)),
        });
        let pipeline_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("pictura-stroke"),
            bind_group_layouts: &[Some(&layout)],
            immediate_size: 0,
        });
        let pipeline = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
            label: Some("pictura-stroke"),
            layout: Some(&pipeline_layout),
            module: &module,
            entry_point: Some("cs_dab"),
            compilation_options: Default::default(),
            cache: None,
        });
        Self { pipeline, layout }
    }
}

/// Keeps the wgpu objects that own the raw device alive for the process.
pub(super) struct Devices {
    _instance: wgpu::Instance,
    adapter: wgpu::Adapter,
    pub(super) device: wgpu::Device,
    pub(super) queue: wgpu::Queue,
    resources: OnceLock<ComputeResources>,
    planar: OnceLock<PlanarResources>,
    stroke: OnceLock<StrokeResources>,
    /// The CPU's exact Gradient Map luma-index correction, one bit per RGB
    /// triple. Built lazily: only a Gradient Map layer needs it.
    gradient_luma: OnceLock<wgpu::Buffer>,
    /// The last composite's readback buffer, kept for the next one.
    ///
    /// ponytail: a fresh gigabyte mapping costs ~0.8 s of page faults on the
    /// map before the first byte is read (an integrated GPU's "device" memory
    /// is system RAM), so the largest recent readback stays resident; release
    /// it under memory pressure if that ever matters.
    readback: Mutex<Option<wgpu::Buffer>>,
    /// Layer sources and coverages kept between composites (`resident.rs`).
    resident: Mutex<Resident>,
}

impl Devices {
    pub(super) fn adapter_info(&self) -> wgpu::AdapterInfo {
        self.adapter.get_info()
    }

    fn resources(&'static self) -> &'static ComputeResources {
        self.resources
            .get_or_init(|| ComputeResources::new(&self.device))
    }

    pub(super) fn planar_resources(&'static self) -> &'static PlanarResources {
        self.planar
            .get_or_init(|| PlanarResources::new(&self.device))
    }

    pub(super) fn stroke_resources(&'static self) -> &'static StrokeResources {
        self.stroke
            .get_or_init(|| StrokeResources::new(&self.device))
    }

    pub(super) fn gradient_luma(&'static self) -> &'static wgpu::Buffer {
        self.gradient_luma.get_or_init(|| {
            let words = gradient_luma_half_up_bits();
            let bytes: Vec<u8> = words.iter().flat_map(|w| w.to_le_bytes()).collect();
            let buffer = self.device.create_buffer(&wgpu::BufferDescriptor {
                label: Some("pictura-gradient-luma"),
                size: bytes.len() as u64,
                usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_DST,
                mapped_at_creation: false,
            });
            self.queue.write_buffer(&buffer, 0, &bytes);
            buffer
        })
    }
}

/// One bit per RGB triple: set when the CPU's f64 luma lands exactly on a half
/// and rounds up. For every other triple the exact integer `(n + 500) / 1000`
/// rounding matches the CPU; this table only corrects the ties.
fn gradient_luma_half_up_bits() -> Vec<u32> {
    let mut words = vec![0u32; (1 << 24) / 32];
    for r in 0..256u32 {
        for g in 0..256u32 {
            for b in 0..256u32 {
                let n = 299 * r + 587 * g + 114 * b;
                if n % 1000 != 500 {
                    continue;
                }
                let idx = (r << 16) | (g << 8) | b;
                let y = pictura_adjust::gradient_luma_index(r as u8, g as u8, b as u8) as u32;
                if y == n / 1000 + 1 {
                    words[(idx >> 5) as usize] |= 1 << (idx & 31);
                }
            }
        }
    }
    words
}

/// The most recent device-creation failure, read by the probe child to explain
/// a failed outcome. Process-local; the parent's own slot is separate.
static LAST_ERROR: Mutex<Option<String>> = Mutex::new(None);

pub(super) fn record_error(message: impl Into<String>) {
    *LAST_ERROR
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner()) = Some(message.into());
}

pub(super) fn take_last_error() -> Option<String> {
    LAST_ERROR
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
        .take()
}

async fn request_device() -> Option<Devices> {
    // Pin exactly one backend: `Instance::new` initializes every backend named
    // in the bitmask, and on Windows a multi-backend instance loads the
    // crashing Vulkan ICD. An uncompiled backend (including `Cpu`) reports
    // "no GPU" instead of aborting the process.
    let backend = super::selected_backend();
    if !backend.is_compiled() {
        return None;
    }
    let mut descriptor = wgpu::InstanceDescriptor::new_without_display_handle();
    descriptor.backends = backend.to_wgpu();
    descriptor.backend_options.dx12.shader_compiler = super::dx12_compiler();
    let instance = wgpu::Instance::new(descriptor);
    let adapter = match instance
        .request_adapter(&wgpu::RequestAdapterOptions {
            power_preference: wgpu::PowerPreference::HighPerformance,
            compatible_surface: None,
            force_fallback_adapter: false,
            apply_limit_buckets: false,
        })
        .await
    {
        Ok(adapter) => adapter,
        Err(error) => {
            record_error(format!("request_adapter: {error}"));
            return None;
        }
    };
    if !accelerates(adapter.get_info().device_type) {
        record_error("adapter is a software (CPU-type) device");
        return None;
    }
    if adapter.limits().max_storage_buffers_per_shader_stage < 4 {
        record_error("adapter has fewer than 4 storage buffers per shader stage");
        return None;
    }
    // Device timestamps are requested only for the opt-in metrics report, so
    // ordinary runs create the device exactly as before.
    let timestamps =
        crate::metrics::enabled() && adapter.features().contains(wgpu::Features::TIMESTAMP_QUERY);
    let required_features = if timestamps {
        wgpu::Features::TIMESTAMP_QUERY
    } else {
        wgpu::Features::empty()
    };
    let (device, queue) = match adapter
        .request_device(&wgpu::DeviceDescriptor {
            label: Some("pictura-render-gpu"),
            required_features,
            required_limits: adapter.limits(),
            experimental_features: wgpu::ExperimentalFeatures::disabled(),
            memory_hints: wgpu::MemoryHints::default(),
            trace: wgpu::Trace::Off,
        })
        .await
    {
        Ok(pair) => pair,
        Err(error) => {
            record_error(format!("request_device: {error}"));
            return None;
        }
    };
    // wgpu's default handler panics in the calling thread (`failure.rs`).
    device.on_uncaptured_error(std::sync::Arc::new(super::failure::uncaptured));
    device.set_device_lost_callback(|reason, message| {
        super::failure::disable(&format!("device lost, {reason:?}: {message}"));
    });
    Some(Devices {
        _instance: instance,
        adapter,
        device,
        queue,
        resources: OnceLock::new(),
        planar: OnceLock::new(),
        stroke: OnceLock::new(),
        gradient_luma: OnceLock::new(),
        readback: Mutex::new(None),
        resident: Mutex::new(Resident::default()),
    })
}

/// Whether an adapter of this type can speed compositing up. A software
/// device (Mesa's lavapipe/llvmpipe on Linux, WARP on Windows) emulates the GPU
/// on the CPU and measured slower than the CPU compositor itself, so it is
/// treated as no GPU at all, except in a build with the test-only
/// `software-gpu` feature, which runs the GPU parity tests on it.
pub(super) fn accelerates(device_type: wgpu::DeviceType) -> bool {
    crate::SOFTWARE_GPU || device_type != wgpu::DeviceType::Cpu
}

/// Set once the driver reports the shared device lost (a reset after a hung
/// job, for one) or the device reports an error (out of memory, for one). A
/// failed device never comes back, so every later caller sees
/// [`GpuError::Unavailable`] and takes the CPU path.
pub(crate) fn device_lost() -> bool {
    super::failure::failed()
}

pub(super) fn devices() -> Result<&'static Devices, GpuError> {
    static DEVICES: OnceLock<Option<Devices>> = OnceLock::new();
    if device_lost() {
        return Err(GpuError::Unavailable);
    }
    DEVICES
        .get_or_init(|| pollster::block_on(request_device()))
        .as_ref()
        .ok_or(GpuError::Unavailable)
}

/// The process-wide cached device and queue, shared by the compositor and the
/// M27 filter kernels. `None` when no Vulkan adapter is usable, or when the
/// device failed the compositor's one-time CPU-oracle check (a device that
/// miscompiles the compositor miscompiles the filter kernels too). Cheap to
/// call repeatedly and never panics.
pub(crate) fn shared_device() -> Option<(&'static wgpu::Device, &'static wgpu::Queue)> {
    if !super::gpu_available() {
        return None;
    }
    devices().ok().map(|d| (&d.device, &d.queue))
}

/// 2-D compute grid for `n` single-invocation items at 64 per workgroup.
///
/// `gx` covers one row (capped at the device's per-dimension limit) and `gy`
/// stacks the remaining rows, so `gx * 64 * gy >= n`. Returns `None` when the
/// `gx * gy` workgroup product itself exceeds the limit — the device cannot
/// address that many workgroups (65535² × 64 ≈ 2.8×10¹⁴ items). `gx` is always
/// ≥ 1; a zero-item dispatch yields `gy == 0`.
///
/// ponytail: with `n: u32` and the standard 65535 limit, `limit² × 64` exceeds
/// `u32::MAX`, so the product guard can never fire on a compliant device; it
/// exists for adapters that report a smaller `max_compute_workgroups_per_dimension`.
pub(crate) fn grid_2d(n: u32, max_per_dim: u32) -> Option<(u32, u32)> {
    let gx = n.div_ceil(64).min(max_per_dim).max(1);
    let gy = u64::from(n).div_ceil(u64::from(gx) * 64);
    (gy <= u64::from(max_per_dim)).then_some((gx, gy as u32))
}

/// Device timestamps around one composite's dispatches: slot 0 is the first
/// pass's start, slot 1 the end of every pass, so the last one stays.
struct Timing {
    set: wgpu::QuerySet,
    resolve: wgpu::Buffer,
    readback: wgpu::Buffer,
    /// Whether a pass has written slot 0 yet.
    started: std::sync::atomic::AtomicBool,
}

impl Timing {
    fn new(device: &wgpu::Device) -> Self {
        let size = 2 * u64::from(wgpu::QUERY_SIZE);
        Self {
            set: device.create_query_set(&wgpu::QuerySetDescriptor {
                label: Some("pictura-composite-timestamps"),
                ty: wgpu::QueryType::Timestamp,
                count: 2,
            }),
            resolve: device.create_buffer(&wgpu::BufferDescriptor {
                label: Some("pictura-timestamp-resolve"),
                size,
                usage: wgpu::BufferUsages::QUERY_RESOLVE | wgpu::BufferUsages::COPY_SRC,
                mapped_at_creation: false,
            }),
            readback: device.create_buffer(&wgpu::BufferDescriptor {
                label: Some("pictura-timestamp-readback"),
                size,
                usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST,
                mapped_at_creation: false,
            }),
            started: std::sync::atomic::AtomicBool::new(false),
        }
    }

    fn writes(&self) -> wgpu::ComputePassTimestampWrites<'_> {
        let first = !self
            .started
            .swap(true, std::sync::atomic::Ordering::Relaxed);
        wgpu::ComputePassTimestampWrites {
            query_set: &self.set,
            beginning_of_pass_write_index: first.then_some(0),
            end_of_pass_write_index: Some(1),
        }
    }

    fn ran(&self) -> bool {
        self.started.load(std::sync::atomic::Ordering::Relaxed)
    }
}

pub(super) struct Gpu {
    pub(super) device: wgpu::Device,
    pub(super) queue: wgpu::Queue,
    res: &'static ComputeResources,
    planar: &'static PlanarResources,
    params: wgpu::Buffer,
    /// The current text-gamma table; one zero entry while the setting is off.
    text_lut: wgpu::Buffer,
    /// The current adjustment lookup table (256 `vec4<f32>` entries).
    lut: wgpu::Buffer,
    /// The shared device, for the lazily built Gradient Map tie table.
    shared: &'static Devices,
    text_on: bool,
    /// Present only when the device was created with timestamp queries.
    timing: Option<Timing>,
    /// Region origin in document pixels.
    x0: u32,
    y0: u32,
    /// Region dimensions; the canvas is `w * h` packed words.
    w: u32,
    h: u32,
    pub(super) n: u32,
}

impl Gpu {
    pub(super) fn new(x0: u32, y0: u32, w: u32, h: u32) -> Result<Self, GpuError> {
        let shared = devices()?;
        let device = shared.device.clone();
        let queue = shared.queue.clone();
        let n = u64::from(w) * u64::from(h);
        let limits = device.limits();
        let bytes = n.saturating_mul(4);
        let limit = u64::from(limits.max_compute_workgroups_per_dimension);
        if bytes > limits.max_storage_buffer_binding_size
            || bytes > limits.max_buffer_size
            || n > limit.saturating_mul(limit).saturating_mul(64)
            || n > u64::from(u32::MAX)
        {
            return Err(GpuError::TooLarge);
        }
        let n = n as u32;
        let params = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("pictura-blend-params"),
            size: 80,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        static OFF_TABLE: [f32; 1] = [0.0];
        let curve = crate::text_gamma::current_curve();
        let text_on = curve.is_some();
        let table = curve.map_or(&OFF_TABLE[..], |curve| curve.samples());
        if text_on {
            debug_assert_eq!(
                table.len(),
                crate::text_gamma::STEPS + 1,
                "the uploaded text-gamma table must match the shader's 4096 steps"
            );
        }
        let bytes: Vec<u8> = table.iter().flat_map(|v| v.to_le_bytes()).collect();
        let text_lut = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("pictura-text-lut"),
            size: bytes.len() as u64,
            usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        queue.write_buffer(&text_lut, 0, &bytes);
        let lut = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("pictura-adj-lut"),
            size: 256 * 16,
            usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let timing = device
            .features()
            .contains(wgpu::Features::TIMESTAMP_QUERY)
            .then(|| Timing::new(&device));
        Ok(Self {
            device,
            queue,
            res: shared.resources(),
            planar: shared.planar_resources(),
            params,
            text_lut,
            lut,
            shared,
            text_on,
            timing,
            x0,
            y0,
            w,
            h,
            n,
        })
    }

    /// A mappable readback buffer of at least `size` bytes: the kept one when
    /// it is large enough, else a fresh one.
    fn readback_buffer(&self, size: u64) -> wgpu::Buffer {
        let kept = devices()
            .ok()
            .and_then(|d| d.readback.lock().ok().and_then(|mut k| k.take()))
            .filter(|b| b.size() >= size);
        kept.unwrap_or_else(|| {
            self.device.create_buffer(&wgpu::BufferDescriptor {
                label: Some("pictura-readback"),
                size,
                usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST,
                mapped_at_creation: false,
            })
        })
    }

    pub(super) fn region(&self) -> Region {
        Region {
            x0: self.x0,
            y0: self.y0,
            w: self.w,
            h: self.h,
        }
    }

    /// The layout of a packed source that covers this whole region.
    pub(super) fn packed_layout(&self) -> SrcLayout {
        SrcLayout {
            x0: self.x0,
            y0: self.y0,
            w: self.w,
            h: self.h,
            gray: false,
            packed: true,
        }
    }

    /// Whether this composite covers the whole document, the only case whose
    /// uploads the resident store may retain.
    pub(super) fn is_document(&self, doc: &Document) -> bool {
        self.x0 == 0 && self.y0 == 0 && self.w == doc.width && self.h == doc.height
    }

    pub(super) fn zero_canvas(&self) -> wgpu::Buffer {
        // ponytail: clear_buffer (GPU memset) over a host `vec![0; n*4]` upload;
        // COPY_DST is required by clear_buffer and the readback's COPY_SRC.
        let canvas = self.device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("pictura-canvas"),
            size: u64::from(self.n) * 4,
            usage: wgpu::BufferUsages::STORAGE
                | wgpu::BufferUsages::COPY_SRC
                | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let mut encoder = self
            .device
            .create_command_encoder(&wgpu::CommandEncoderDescriptor {
                label: Some("pictura-canvas-clear"),
            });
        encoder.clear_buffer(&canvas, 0, None);
        self.queue.submit(Some(encoder.finish()));
        canvas
    }

    /// Assemble the source for one pixel layer, matching `composite_pixels`:
    /// planar 8-bit channel planes, grayscale replicating channel 0 (one colour
    /// plane), and straight alpha defaulting to 255 when absent. A
    /// whole-document upload is looked up by the layer's content key; on a miss
    /// only the region intersection is assembled, and a region-sized upload is
    /// not retained. The shader samples by document pixel; outside the layer's
    /// clipped document rect the source alpha is 0.
    pub(super) fn build_source(
        &self,
        layer: &Layer,
        doc: &Document,
    ) -> Option<(wgpu::Buffer, SrcLayout)> {
        let g = source_geom(self.region(), layer, doc)?;
        let document = Region {
            x0: 0,
            y0: 0,
            w: doc.width,
            h: doc.height,
        };
        let full = source_geom(document, layer, doc)?;
        let keep = self.is_document(doc);
        let key = source_key(layer, (doc.width, doc.height), full.gray);
        let (buffer, hit) = self.kept_or_upload(key, keep, || {
            let g = if keep { &full } else { &g };
            self.upload_with("pictura-src", g.planes * g.n, source_writer(layer, g))
        });
        let layout = if hit || keep {
            full.layout()
        } else {
            g.layout()
        };
        Some((buffer, layout))
    }

    /// The upload kept under `key`, or `upload()`'s and whether it came from
    /// the store. A `None` key (an unstamped plane) always uploads and keeps
    /// nothing. `keep` is false for a region-sized upload, whose bytes are
    /// addressable only by that region.
    pub(super) fn kept_or_upload(
        &self,
        key: Option<UploadKey>,
        keep: bool,
        upload: impl FnOnce() -> wgpu::Buffer,
    ) -> (wgpu::Buffer, bool) {
        let resident = || devices().ok().and_then(|d| d.resident.lock().ok());
        if let Some(buffer) = key.as_ref().and_then(|k| resident().and_then(|r| r.get(k))) {
            crate::metrics::count_resident(true);
            return (buffer, true);
        }
        if key.is_some() {
            crate::metrics::count_resident(false);
        }
        let buffer = upload();
        if let (Some(key), true, Some(mut r)) = (key, keep, resident()) {
            let (entries, bytes) = r.put(key, buffer.clone(), buffer.size());
            crate::metrics::set_resident(entries, bytes);
        }
        (buffer, false)
    }

    /// A storage buffer of `len` bytes (padded to a word) whose bytes `write`
    /// produces straight into the queue's staging memory, a [`RUN`] per staging
    /// write, the runs in parallel: a large layer is never first assembled
    /// into a host buffer and then copied again.
    pub(super) fn upload_with(
        &self,
        label: &str,
        len: usize,
        write: impl Fn(usize, wgpu::WriteOnly<'_, [u8]>) + Sync,
    ) -> wgpu::Buffer {
        use rayon::prelude::*;
        let size = padded(len).max(4);
        let buffer = self.device.create_buffer(&wgpu::BufferDescriptor {
            label: Some(label),
            size: size as u64,
            usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        // Every run lies inside the fresh buffer, so a refused write means the
        // device is lost: leave the buffer unwritten, and `read_canvas` fails.
        let views: Option<Vec<_>> = (0..size)
            .step_by(RUN)
            .map(|at| {
                let bytes =
                    wgpu::BufferSize::new(RUN.min(size - at) as u64).expect("a run is never empty");
                let view = self.queue.write_buffer_with(&buffer, at as u64, bytes)?;
                Some((at, view))
            })
            .collect();
        views
            .unwrap_or_default()
            .into_par_iter()
            .for_each(|(at, mut view)| write(at, view.slice(..)));
        buffer
    }

    /// Per-region-pixel mask coverage as an 8-bit plane, reusing the CPU
    /// `mask_alpha`. A group's inner buffer and an adjustment layer act across
    /// the whole region; a pixel layer only over its layer-rect intersection
    /// with the region (outside it the source alpha is 0, so the coverage is
    /// irrelevant).
    ///
    /// Coverage is retained only for a whole-document composite: the shader
    /// reads it by region-local index, so a sub-region upload cannot serve
    /// another region. Constant coverage (no enabled data-carrying raster mask)
    /// is rebuilt row-wise and never kept; a vector-mask fill carries no stamp
    /// and is rebuilt for each composite.
    ///
    /// ponytail: the plane is region-sized even for a rect-scoped layer; bound
    /// it by the rect if a many-small-layers mask profile ever shows up.
    pub(super) fn build_mask(&self, layer: &Layer, doc: &Document) -> wgpu::Buffer {
        let region = self.region();
        let len = region.w as usize * region.h as usize;
        self.kept_or_upload(mask_key(region, layer), self.is_document(doc), || {
            self.upload_with("pictura-mask", len, mask_writer(region, layer))
        })
        .0
    }

    pub(super) fn composite_plan(
        &self,
        canvas: &wgpu::Buffer,
        plan: &super::plan::LayerPlan,
        doc: &Document,
    ) {
        let layer = plan.layer;
        if let Some(passes) = &plan.effects {
            for pass in &passes.below {
                self.composite_effect_pass(canvas, pass, layer.opacity, doc);
            }
        }
        match &plan.body {
            super::plan::Body::Pixels => {
                if let Some((src, layout)) = self.build_source(layer, doc) {
                    let mask = self.build_mask(layer, doc);
                    self.dispatch(
                        canvas,
                        &src,
                        &mask,
                        layout,
                        layer.blend,
                        layer.opacity,
                        layer.fill,
                        layer.type_tool.is_some(),
                        NO_ADJ,
                        None,
                    );
                }
            }
            super::plan::Body::Raster(cached) | super::plan::Body::Fill(cached) => {
                let (src, layout) = self.build_cached_source(cached, doc);
                let mask = self.build_mask(layer, doc);
                self.dispatch(
                    canvas,
                    &src,
                    &mask,
                    layout,
                    layer.blend,
                    layer.opacity,
                    layer.fill,
                    layer.type_tool.is_some(),
                    NO_ADJ,
                    None,
                );
            }
            super::plan::Body::Adjustment(super::plan::AdjustmentPlan::Inline(adj)) => {
                let mask = self.build_mask(layer, doc);
                self.dispatch(
                    canvas,
                    &mask,
                    &mask,
                    PACKED_SRC,
                    layer.blend,
                    layer.opacity,
                    255,
                    layer.type_tool.is_some(),
                    *adj,
                    None,
                );
            }
            super::plan::Body::Adjustment(super::plan::AdjustmentPlan::Lut(lut)) => {
                let mask = self.build_mask(layer, doc);
                let kind = super::lut::kind(lut);
                self.dispatch(
                    canvas,
                    &mask,
                    &mask,
                    PACKED_SRC,
                    layer.blend,
                    layer.opacity,
                    255,
                    layer.type_tool.is_some(),
                    (kind, 0, 0, 0),
                    Some(lut),
                );
            }
            super::plan::Body::Group {
                pass_through: true,
                children,
            } => {
                for child in children {
                    self.composite_plan(canvas, child, doc);
                }
            }
            super::plan::Body::Group {
                pass_through: false,
                children,
            } => {
                let inner = self.zero_canvas();
                for child in children {
                    self.composite_plan(&inner, child, doc);
                }
                let mask = self.build_mask(layer, doc);
                self.dispatch(
                    canvas,
                    &inner,
                    &mask,
                    self.packed_layout(),
                    layer.blend,
                    layer.opacity,
                    255,
                    layer.type_tool.is_some(),
                    NO_ADJ,
                    None,
                );
            }
            super::plan::Body::ClipGroup { base, clipped } => {
                self.composite_clip_group(canvas, base, clipped, doc);
            }
            super::plan::Body::Decline(_) => {}
        }
        if let Some(passes) = &plan.effects {
            for pass in &passes.above {
                self.composite_effect_pass(canvas, pass, layer.opacity, doc);
            }
        }
    }

    #[allow(clippy::too_many_arguments)]
    pub(super) fn dispatch(
        &self,
        canvas: &wgpu::Buffer,
        src: &wgpu::Buffer,
        mask: &wgpu::Buffer,
        layout: SrcLayout,
        mode: BlendMode,
        opacity: u8,
        fill: u8,
        text: bool,
        adj: (u32, i32, i32, i32),
        lut_table: Option<&pictura_adjust::AdjustmentLut>,
    ) {
        if let Some(table) = lut_table {
            self.queue
                .write_buffer(&self.lut, 0, &super::lut::bytes(table));
        }
        let text_on = text && self.text_on;
        let flags = u32::from(layout.packed) | (u32::from(layout.gray) << 1);
        let (gx, gy) = grid_2d(
            self.n,
            self.device.limits().max_compute_workgroups_per_dimension,
        )
        .expect("Gpu::new rejected a canvas past the 2-D workgroup limit");
        let params = [
            mode_id(mode).to_le_bytes(),
            (opacity as f32 / 255.0).to_le_bytes(),
            (fill as f32 / 255.0).to_le_bytes(),
            self.n.to_le_bytes(),
            adj.0.to_le_bytes(),
            adj.1.to_le_bytes(),
            adj.2.to_le_bytes(),
            adj.3.to_le_bytes(),
            flags.to_le_bytes(),
            layout.x0.to_le_bytes(),
            layout.y0.to_le_bytes(),
            layout.w.to_le_bytes(),
            layout.h.to_le_bytes(),
            self.x0.to_le_bytes(),
            self.y0.to_le_bytes(),
            self.w.to_le_bytes(),
            (gx * 64).to_le_bytes(),
            u32::from(text_on).to_le_bytes(),
        ]
        .concat();
        self.queue.write_buffer(&self.params, 0, &params);

        let bind_group = self.device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("pictura-blend"),
            layout: &self.res.layout,
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: canvas.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 1,
                    resource: src.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 2,
                    resource: mask.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 3,
                    resource: self.params.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 4,
                    resource: self.text_lut.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 5,
                    resource: self.lut.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 6,
                    resource: if adj.0 == 7 {
                        self.shared.gradient_luma().as_entire_binding()
                    } else {
                        self.lut.as_entire_binding()
                    },
                },
            ],
        });

        let mut encoder = self
            .device
            .create_command_encoder(&wgpu::CommandEncoderDescriptor {
                label: Some("pictura-blend"),
            });
        {
            let mut pass = encoder.begin_compute_pass(&wgpu::ComputePassDescriptor {
                label: Some("pictura-blend"),
                timestamp_writes: self.timing.as_ref().map(Timing::writes),
            });
            pass.set_pipeline(&self.res.pipeline);
            pass.set_bind_group(0, &bind_group, &[]);
            pass.dispatch_workgroups(gx, gy, 1);
        }
        self.queue.submit(Some(encoder.finish()));
    }

    pub(super) fn read_canvas(&self, canvas: &wgpu::Buffer) -> Result<PixelBuffer, GpuError> {
        // De-interleave on the GPU: the planar pass writes four byte planes into
        // one storage buffer, so the host copies plane slices instead of
        // gathering a channel per pixel. `pw` words per plane (word-aligned by
        // construction); bytes past `n` in the last word of a plane are padding.
        let n = self.n;
        let pw = n.div_ceil(4);
        let size = u64::from(pw) * 16;
        let planar = self.device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("pictura-planar"),
            size,
            usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
            mapped_at_creation: false,
        });
        let params = self.device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("pictura-planar-params"),
            size: 32,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let (gx, gy) = grid_2d(
            pw,
            self.device.limits().max_compute_workgroups_per_dimension,
        )
        .expect("Gpu::new rejected a canvas past the 2-D workgroup limit");
        // Whole-buffer addresses: bx = by = 0, bw = lw = canvas width.
        let params_data = [
            n.to_le_bytes(),
            pw.to_le_bytes(),
            (gx * 64).to_le_bytes(),
            0u32.to_le_bytes(),
            0u32.to_le_bytes(),
            self.w.to_le_bytes(),
            self.w.to_le_bytes(),
            0u32.to_le_bytes(),
        ]
        .concat();
        self.queue.write_buffer(&params, 0, &params_data);

        let bind_group = self.device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("pictura-planar"),
            layout: &self.planar.layout,
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: canvas.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 1,
                    resource: planar.as_entire_binding(),
                },
                wgpu::BindGroupEntry {
                    binding: 2,
                    resource: params.as_entire_binding(),
                },
            ],
        });
        let mut encoder = self
            .device
            .create_command_encoder(&wgpu::CommandEncoderDescriptor {
                label: Some("pictura-planar"),
            });
        {
            let mut pass = encoder.begin_compute_pass(&wgpu::ComputePassDescriptor {
                label: Some("pictura-planar"),
                timestamp_writes: None,
            });
            pass.set_pipeline(&self.planar.pipeline);
            pass.set_bind_group(0, &bind_group, &[]);
            pass.dispatch_workgroups(gx, gy, 1);
        }
        self.queue.submit(Some(encoder.finish()));

        let staging = self.readback_buffer(size);
        let mut encoder = self
            .device
            .create_command_encoder(&wgpu::CommandEncoderDescriptor {
                label: Some("pictura-readback"),
            });
        encoder.copy_buffer_to_buffer(&planar, 0, &staging, 0, size);
        let timing = self.timing.as_ref().filter(|t| t.ran());
        if let Some(t) = timing {
            encoder.resolve_query_set(&t.set, 0..2, &t.resolve, 0);
            encoder.copy_buffer_to_buffer(&t.resolve, 0, &t.readback, 0, t.readback.size());
        }
        self.queue.submit(Some(encoder.finish()));

        let slice = staging.slice(..size);
        let (tx, rx) = std::sync::mpsc::channel();
        slice.map_async(wgpu::MapMode::Read, move |result| {
            let _ = tx.send(result);
        });
        let timing_rx = timing.map(|t| {
            let (tx, rx) = std::sync::mpsc::channel();
            t.readback
                .slice(..)
                .map_async(wgpu::MapMode::Read, move |result| {
                    let _ = tx.send(result);
                });
            (t, rx)
        });
        let _ = self.device.poll(wgpu::PollType::wait_indefinitely());
        rx.recv()
            .map_err(|_| GpuError::Readback)?
            .map_err(|_| GpuError::Readback)?;
        if device_lost() {
            return Err(GpuError::Readback);
        }
        if let Some((t, rx)) = timing_rx {
            if matches!(rx.recv(), Ok(Ok(()))) {
                self.publish_device_time(t);
            }
        }

        // Each plane is `pw` words; copy exactly `n` bytes of it into the
        // planar `PixelBuffer`, ignoring the tail padding.
        let mapped = slice.get_mapped_range().map_err(|_| GpuError::Readback)?;
        let plane = pw as usize * 4;
        let n = n as usize;
        let src: &[u8] = &mapped;
        // One allocation straight from zeroed pages, each plane copied in one
        // pass: a 267-megapixel readback is a gigabyte. Its pages are faulted
        // in on every core first; the copy itself stays on one thread, since
        // parallel reads of the mapped range measured four times slower.
        let data = pictura_core::Plane::build(4 * n, |out| {
            use rayon::prelude::*;
            if n == 0 {
                return;
            }
            out.par_chunks_mut(RUN).for_each(|run| run.fill(0));
            for (c, dst) in out.chunks_mut(n).enumerate() {
                dst.copy_from_slice(&src[c * plane..c * plane + n]);
            }
        });
        let out = PixelBuffer {
            width: self.w,
            height: self.h,
            channels: 4,
            data,
        };
        drop(mapped);
        staging.unmap();
        if let Ok(mut kept) = devices()?.readback.lock() {
            *kept = Some(staging);
        }
        Ok(out)
    }

    /// Publish the mapped begin/end timestamps as the composite's device time.
    fn publish_device_time(&self, t: &Timing) {
        let ticks = {
            let Ok(mapped) = t.readback.slice(..).get_mapped_range() else {
                return;
            };
            let word = |i: usize| {
                let mut bytes = [0u8; 8];
                bytes.copy_from_slice(&mapped[i * 8..i * 8 + 8]);
                u64::from_le_bytes(bytes)
            };
            word(1).saturating_sub(word(0))
        };
        t.readback.unmap();
        let ns = ticks as f64 * f64::from(self.queue.get_timestamp_period());
        crate::metrics::publish_device_ms(ns / 1.0e6);
    }

    /// De-interleave packed RGBA8 readback into the planar straight-alpha
    /// `PixelBuffer`, identical in shape to `composite_rgba`. Each LE `u32`
    /// unpacked and masked into four channels is a pure byte permutation, so
    /// walking 4-byte chunks writes the same bytes. The live readback now
    /// planarizes on the GPU; this host gather stays for the profile micro-
    /// benchmark and as the retained reference.
    #[cfg(test)]
    pub(super) fn to_pixel_buffer(&self, packed: &[u8]) -> PixelBuffer {
        let plane = self.n as usize;
        let mut out = PixelBuffer::new(self.w, self.h, 4);
        for (i, px) in packed.as_chunks::<4>().0.iter().take(plane).enumerate() {
            out.data[i] = px[0];
            out.data[plane + i] = px[1];
            out.data[2 * plane + i] = px[2];
            out.data[3 * plane + i] = px[3];
        }
        out
    }
}

/// How the shader should read the source binding for one dispatch.
#[derive(Clone, Copy)]
pub(super) struct SrcLayout {
    pub(super) x0: u32,
    pub(super) y0: u32,
    pub(super) w: u32,
    pub(super) h: u32,
    pub(super) gray: bool,
    pub(super) packed: bool,
}

/// A group's inner canvas: one packed RGBA word per region pixel.
pub(super) const PACKED_SRC: SrcLayout = SrcLayout {
    x0: 0,
    y0: 0,
    w: 0,
    h: 0,
    gray: false,
    packed: true,
};

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn gradient_luma_bitset_matches_cpu_for_every_triple() {
        let bits = gradient_luma_half_up_bits();
        for r in 0..256u32 {
            for g in 0..256u32 {
                for b in 0..256u32 {
                    let n = 299 * r + 587 * g + 114 * b;
                    let mut y = n / 1000;
                    let rem = n % 1000;
                    if rem > 500 {
                        y += 1;
                    } else if rem == 500 {
                        let idx = (r << 16) | (g << 8) | b;
                        let bit = (bits[(idx >> 5) as usize] >> (idx & 31)) & 1;
                        y += bit;
                    }
                    assert_eq!(
                        y,
                        u32::from(pictura_adjust::gradient_luma_index(
                            r as u8, g as u8, b as u8
                        )),
                        "RGB {r},{g},{b}"
                    );
                }
            }
        }
    }
}
