//! A GPU failure at run time turns the GPU off for the rest of the process
//! instead of panicking.
//!
//! wgpu's default handler for a device error raised outside an error scope
//! panics in the calling thread. An out-of-memory `create_buffer`, while
//! another program held most of the video memory, killed a document-open
//! worker that way, and the shell waited for its result forever. Every device
//! error (out of memory, validation, internal) and a lost device now land in
//! [`disable`]; every GPU composite runs inside [`guard`], which also catches a
//! panic. Callers then see [`GpuError::Unavailable`] and take the CPU path, and
//! the shell shows [`take_gpu_failure_notice`] once.
//!
//! The GPU stays off for the session: the memory pressure that caused the
//! failure usually belongs to another program and outlasts one document, and a
//! device that failed once may hold invalid objects.

use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Mutex, PoisonError};

use super::GpuError;

static FAILED: AtomicBool = AtomicBool::new(false);
static NOTICE: Mutex<Option<String>> = Mutex::new(None);
static INJECT: AtomicBool = AtomicBool::new(false);

/// Whether a run-time failure turned the GPU off for this process.
pub(crate) fn failed() -> bool {
    FAILED.load(Ordering::Relaxed)
}

/// Turn the GPU off for this process. The first failure leaves the notice.
pub(super) fn disable(reason: &str) {
    if FAILED.swap(true, Ordering::Relaxed) {
        return;
    }
    eprintln!("pictura: GPU failed ({reason}); using the CPU from now on");
    *NOTICE.lock().unwrap_or_else(PoisonError::into_inner) = Some(format!(
        "The GPU failed ({reason}); rendering on the CPU for the rest of this session."
    ));
}

/// The device's uncaptured-error handler (installed in `request_device`).
pub(super) fn uncaptured(error: wgpu::Error) {
    // wgpu's description is "<kind> Error", "Caused by:", then the cause
    // chain one line each; keep the chain on one line.
    let causes = |text: &str| {
        let chain: Vec<&str> = text
            .lines()
            .skip(1)
            .map(str::trim)
            .filter(|line| !line.is_empty() && *line != "Caused by:")
            .collect();
        chain.join(": ").chars().take(200).collect::<String>()
    };
    let reason = match &error {
        wgpu::Error::OutOfMemory { .. } => "out of video memory".to_string(),
        wgpu::Error::Validation { description, .. } => {
            format!("validation error: {}", causes(description))
        }
        wgpu::Error::Internal { description, .. } => {
            format!("internal error: {}", causes(description))
        }
    };
    disable(&reason);
}

/// The message for the user about the failure that turned the GPU off, once.
/// `None` when the GPU has not failed or the message was already taken.
pub fn take_gpu_failure_notice() -> Option<String> {
    NOTICE.lock().unwrap_or_else(PoisonError::into_inner).take()
}

/// Test hook: the next guarded GPU job raises a real device error (a buffer
/// with no usage, which wgpu rejects before allocating anything) through the
/// same handler an out-of-memory error reaches. Nothing happens without a
/// device.
#[doc(hidden)]
pub fn inject_gpu_failure() {
    INJECT.store(true, Ordering::Relaxed);
}

/// Run one GPU job. A device error reported while it ran, or a panic inside
/// it, turns the GPU off and answers [`GpuError::Unavailable`], so the caller
/// takes the CPU path instead of using a partial result or unwinding.
pub(super) fn guard<T>(job: impl FnOnce() -> Result<T, GpuError>) -> Result<T, GpuError> {
    if failed() {
        return Err(GpuError::Unavailable);
    }
    let outcome = catch_unwind(AssertUnwindSafe(|| {
        if INJECT.swap(false, Ordering::Relaxed) {
            if let Ok(shared) = super::devices() {
                drop(shared.device.create_buffer(&wgpu::BufferDescriptor {
                    label: Some("pictura-injected-failure"),
                    size: 4,
                    usage: wgpu::BufferUsages::empty(),
                    mapped_at_creation: false,
                }));
            }
        }
        job()
    }));
    match outcome {
        Ok(_) if failed() => Err(GpuError::Unavailable),
        Ok(result) => result,
        Err(payload) => {
            let message = payload
                .downcast_ref::<&str>()
                .map(|s| s.to_string())
                .or_else(|| payload.downcast_ref::<String>().cloned())
                .unwrap_or_default();
            disable(&format!(
                "panic: {}",
                message.lines().next().unwrap_or_default()
            ));
            Err(GpuError::Unavailable)
        }
    }
}
