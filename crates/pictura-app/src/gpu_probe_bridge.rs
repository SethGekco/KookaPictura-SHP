//! `pictura --gpu-probe` and the app's backend-selection edge.
//!
//! A free-function bridge, in its own file, because the probe runs before
//! `QApplication` and because `cxxqt_object.rs` is at its size ceiling.

mod imp;

use imp::*;

#[cxx_qt::bridge]
pub mod ffi {
    #[namespace = "pictura"]
    struct GpuProbeReply {
        code: i32,
        detail: String,
    }

    #[namespace = "pictura"]
    extern "Rust" {
        /// Parent side: spawn `child_command --gpu-probe <backend>`, wait up
        /// to 10 s, and return the outcome code (0 ok, 1 no adapter/clean
        /// failure, 2 validation failure, crash, or timeout) with its
        /// diagnostic detail.
        fn gpu_probe(kind: i32, child_command: &str) -> GpuProbeReply;
        /// Child side: pin the backend, create the device, and run the CPU
        /// oracle validation. 0 on success, 1 when no adapter exists, 2 when a
        /// device exists but fails validation. Prints the detail line.
        fn gpu_probe_child(kind: i32) -> i32;
        /// Pin the process to one backend. False when a different pin already
        /// resolved or the code is unknown.
        fn gpu_set_backend(kind: i32) -> bool;
        /// Mark the GPU path blocked (chain exhausted or validation failed).
        fn gpu_set_safe_mode(active: bool);
        /// The ordered candidate list for this launch, after the environment
        /// override and the compiled-backend intersection.
        fn gpu_candidates() -> Vec<i32>;
        /// The five-state status label for the status bar and About.
        fn gpu_active_backend_label(gpu_compute: bool) -> String;
        /// The message about a GPU failure at run time (out of video memory,
        /// a lost device) that turned the GPU off for this session; empty
        /// when none is pending. Taken once.
        fn gpu_take_failure_notice() -> String;
        /// Test hook: the next GPU composite raises a device error.
        fn gpu_inject_failure();
    }
}
