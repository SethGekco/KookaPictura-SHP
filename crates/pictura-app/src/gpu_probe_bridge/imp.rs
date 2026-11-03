use std::io::Read;
use std::process::{Child, Command, Stdio};
use std::time::{Duration, Instant};

use pictura_render::GpuBackend;

use super::ffi::GpuProbeReply;

const TIMEOUT: Duration = Duration::from_secs(10);

pub(super) fn gpu_probe(kind: i32, child_command: &str) -> GpuProbeReply {
    let Some(backend) = GpuBackend::from_code(kind) else {
        return GpuProbeReply {
            code: 1,
            detail: "unknown backend".to_string(),
        };
    };
    if backend == GpuBackend::Cpu || !backend.is_compiled() {
        return GpuProbeReply {
            code: 1,
            detail: "backend not compiled".to_string(),
        };
    }
    let mut command = Command::new(child_command);
    command.arg("--gpu-probe").arg(backend.name());
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        // CREATE_NO_WINDOW: the probe is a GUI-subsystem binary, but a console
        // build must not flash a window for the child.
        command.creation_flags(0x0800_0000);
    }
    run_probe(command, TIMEOUT)
}

/// Spawn a prepared command with piped stdout, wait, and read the detail line.
///
/// The pipe is read only when the child exited on its own. After a timeout the
/// child is killed, but a grandchild it spawned can inherit the pipe write end
/// and keep it open, so a read would block forever. The child caps its line at
/// 512 characters, so a working probe cannot fill the pipe buffer while this
/// function is polling.
fn run_probe(mut command: Command, timeout: Duration) -> GpuProbeReply {
    command.stdout(Stdio::piped());
    let mut child = match command.spawn() {
        Ok(child) => child,
        Err(error) => {
            return GpuProbeReply {
                code: 1,
                detail: format!("probe could not start: {error}"),
            };
        }
    };
    let mut stdout = child.stdout.take();
    let (code, detail) = match wait_with_timeout(&mut child, timeout) {
        WaitOutcome::TimedOut => (2, "probe timed out or crashed".to_string()),
        WaitOutcome::Exited(code) => {
            let detail = stdout
                .take()
                .map(|mut pipe| {
                    let mut text = String::new();
                    let _ = pipe.read_to_string(&mut text);
                    text.trim().to_string()
                })
                .unwrap_or_default();
            let detail = if detail.is_empty() {
                match code {
                    0 => "probe reported success without a detail".to_string(),
                    1 => "probe failed without a detail".to_string(),
                    _ => "probe timed out or crashed".to_string(),
                }
            } else {
                detail
            };
            (code, detail)
        }
    };
    GpuProbeReply { code, detail }
}

pub(super) fn gpu_probe_child(kind: i32) -> i32 {
    let Some(backend) = GpuBackend::from_code(kind) else {
        return 1;
    };
    let report = pictura_render::probe_backend_outcome(backend);
    let detail: String = report.detail.chars().take(512).collect();
    println!("{detail}");
    match report.outcome {
        pictura_render::ProbeOutcome::Ok => 0,
        pictura_render::ProbeOutcome::NoAdapter => 1,
        pictura_render::ProbeOutcome::Invalid => 2,
    }
}

pub(super) fn gpu_set_backend(kind: i32) -> bool {
    let Some(backend) = GpuBackend::from_code(kind) else {
        return false;
    };
    match pictura_render::set_backend(backend) {
        Ok(()) => true,
        Err(current) => current == backend,
    }
}

pub(super) fn gpu_set_safe_mode(active: bool) {
    pictura_render::set_safe_mode(active);
}

pub(super) fn gpu_candidates() -> Vec<i32> {
    pictura_render::env_candidates()
        .0
        .into_iter()
        .map(GpuBackend::code)
        .collect()
}

pub(super) fn gpu_active_backend_label(gpu_compute: bool) -> String {
    pictura_render::backend_label(gpu_compute, pictura_render::gpu_backend_status())
}

pub(super) fn gpu_take_failure_notice() -> String {
    pictura_render::take_gpu_failure_notice().unwrap_or_default()
}

pub(super) fn gpu_inject_failure() {
    pictura_render::inject_gpu_failure();
}

/// Whether the child exited on its own or had to be killed after the timeout.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum WaitOutcome {
    /// 0 = ok, 1 = no adapter/clean failure, 2 = validation failure or crash.
    Exited(i32),
    /// The deadline passed and the child was killed (or did not reap).
    TimedOut,
}

fn wait_with_timeout(child: &mut Child, timeout: Duration) -> WaitOutcome {
    let deadline = Instant::now() + timeout;
    loop {
        match child.try_wait() {
            Ok(Some(status)) => {
                return WaitOutcome::Exited(match status.code() {
                    Some(0) => 0,
                    Some(1) => 1,
                    _ => 2,
                });
            }
            Ok(None) if Instant::now() < deadline => {
                std::thread::sleep(Duration::from_millis(25));
            }
            _ => {
                // A driver stuck in kernel mode may not honor TerminateProcess
                // promptly; reap briefly, then return without an unbounded wait.
                let _ = child.kill();
                let reap_deadline = Instant::now() + Duration::from_millis(500);
                while Instant::now() < reap_deadline {
                    if matches!(child.try_wait(), Ok(Some(_))) {
                        break;
                    }
                    std::thread::sleep(Duration::from_millis(10));
                }
                return WaitOutcome::TimedOut;
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn shell(script: &str) -> Command {
        #[cfg(windows)]
        {
            let mut command = Command::new("cmd");
            command.args(["/C", script]);
            command
        }
        #[cfg(not(windows))]
        {
            let mut command = Command::new("sh");
            command.args(["-c", script]);
            command
        }
    }

    fn hung_script() -> &'static str {
        #[cfg(windows)]
        {
            // `sleep` does not exist in cmd; ping is the portable wait.
            "ping -n 60 127.0.0.1 >NUL"
        }
        #[cfg(not(windows))]
        {
            "sleep 60"
        }
    }

    #[test]
    fn wait_with_timeout_maps_success() {
        let mut child = shell("exit 0").spawn().unwrap();
        assert_eq!(
            wait_with_timeout(&mut child, TIMEOUT),
            WaitOutcome::Exited(0)
        );
    }

    #[test]
    fn wait_with_timeout_maps_failure() {
        let mut child = shell("exit 1").spawn().unwrap();
        assert_eq!(
            wait_with_timeout(&mut child, TIMEOUT),
            WaitOutcome::Exited(1)
        );
    }

    #[test]
    fn wait_with_timeout_kills_a_hung_child() {
        let mut child = shell(hung_script()).spawn().unwrap();
        let started = Instant::now();
        assert_eq!(
            wait_with_timeout(&mut child, Duration::from_millis(100)),
            WaitOutcome::TimedOut
        );
        assert!(started.elapsed() < Duration::from_secs(5));
        assert!(child.try_wait().unwrap().is_some());
    }

    #[test]
    fn gpu_probe_captures_the_child_detail() {
        let reply = run_probe(shell("echo compiler=FXC"), TIMEOUT);
        assert_eq!(reply.code, 0);
        assert_eq!(reply.detail, "compiler=FXC");
    }

    #[test]
    fn gpu_probe_timeout_does_not_block() {
        let started = Instant::now();
        let reply = run_probe(shell(hung_script()), Duration::from_millis(100));
        assert_eq!(reply.code, 2);
        assert!(reply.detail.contains("timed out"), "{}", reply.detail);
        assert!(started.elapsed() < Duration::from_secs(5));
    }

    #[test]
    fn unknown_backend_code_is_a_clean_failure() {
        assert_eq!(gpu_probe_child(99), 1);
        assert!(!gpu_set_backend(99));
    }
}
