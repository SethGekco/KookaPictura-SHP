use super::*;

use super::super::super::color_management::save_embed_default;
use super::super::super::filter_tools::store_planes;
use super::super::super::state::PictureViewRust;
use super::super::format_for_path;
use core::pin::Pin;
use cxx_qt::CxxQtType;
use cxx_qt_lib::QString;
use std::sync::Arc;
use std::time::Duration;

/// The live native-save worker and the document identity it snapshotted.
pub(in crate::cxxqt_object) struct SaveJob {
    handle: std::thread::JoinHandle<bool>,
    path: String,
    start_history: Option<u64>,
    start_revision: u64,
    generation: u64,
}

pub(in crate::cxxqt_object) fn save_start_rust(rust: &mut PictureViewRust, path: &str) -> bool {
    if rust
        .save_job
        .as_ref()
        .is_some_and(|job| job.handle.is_finished())
    {
        // An abandoned job must not wedge later saves; its write already landed
        // (or failed) and no caller is left to apply the result.
        rust.save_job = None;
    }
    if rust.save_job.is_some() {
        return false;
    }
    // Only a native path gets PSD bytes. Any other suffix (raster, PDF, or
    // one nothing here writes, such as `.shp`) is refused rather than given
    // a PSD under a foreign name; a path with no suffix is a PSD.
    let suffix = format_for_path(path);
    if !matches!(suffix.as_str(), "psd" | "psb") {
        return false;
    }
    let session = &rust.session;
    let Some(doc_ref) = session.doc.as_ref() else {
        return false;
    };
    let embed = rust
        .save_embed_profile
        .unwrap_or_else(|| save_embed_default(rust, true));
    let dirty = session.dirty;
    let planes = store_planes(session.vanishing_planes.as_ref(), doc_ref);
    let start_history = session.history.current_id();
    let start_revision = session.content_revision;
    let generation = session.document_generation;
    let cache = Arc::clone(&session.encode_cache);
    pictura_codec::warm_encode_idents(doc_ref);
    let doc = doc_ref.clone();
    let delay = rust.save_delay_for_test;
    let psb = suffix == "psb";
    let worker_path = path.to_string();
    let job_path = path.to_string();
    let handle = std::thread::spawn(move || {
        let mut doc = doc;
        if dirty {
            pictura_render::refresh_native_composite(&mut doc);
        }
        // A `.psb` path forces a version-2 PSB container; every other native
        // suffix writes a PSD.
        let mut doc = pictura_render::save_view(&doc);
        pictura_codec::set_vanishing_point_data(doc.to_mut(), planes.as_deref());
        if !embed {
            pictura_codec::remove_document_profile(doc.to_mut());
        }
        let written = if psb {
            pictura_codec::write_psb_cached(&doc, &cache)
        } else {
            pictura_codec::write_psd_cached(&doc, &cache)
        };
        let Ok(bytes) = written else {
            return false;
        };
        if delay > 0 {
            std::thread::sleep(Duration::from_millis(delay));
        }
        super::super::super::helpers::safe_io::write_atomically(&worker_path, &bytes, false).is_ok()
    });
    rust.save_job = Some(SaveJob {
        handle,
        path: job_path,
        start_history,
        start_revision,
        generation,
    });
    true
}

pub(in crate::cxxqt_object) fn save_running_rust(rust: &PictureViewRust) -> bool {
    rust.save_job
        .as_ref()
        .is_some_and(|job| !job.handle.is_finished())
}

pub(in crate::cxxqt_object) fn save_finish_rust(rust: &mut PictureViewRust) -> bool {
    let Some(job) = rust.save_job.take() else {
        return true;
    };
    let Ok(written) = job.handle.join() else {
        return false;
    };
    if !written {
        return false;
    }
    if rust.session.document_generation != job.generation {
        // The snapshot was written, but this is a different document: report
        // "not applied" so the frame keeps the replacement's path and title.
        return false;
    }
    rust.path = Some(job.path);
    let current = (
        rust.session.history.current_id(),
        rust.session.content_revision,
    );
    if current == (job.start_history, job.start_revision) {
        rust.session.mark_saved();
    }
    true
}

pub(super) fn save_start(mut view: Pin<&mut PictureView>, path: &QString) -> bool {
    save_start_rust(&mut view.as_mut().rust_mut(), &path.to_string())
}

pub(super) fn save_running(view: &PictureView) -> bool {
    save_running_rust(view.rust())
}

pub(super) fn save_finish(mut view: Pin<&mut PictureView>) -> bool {
    save_finish_rust(&mut view.as_mut().rust_mut())
}

pub(super) fn save_set_delay(mut view: Pin<&mut PictureView>, ms: u64) {
    view.as_mut().rust_mut().save_delay_for_test = ms;
}

#[cfg(test)]
mod tests {
    use super::*;
    use pictura_core::{BitDepth, ColorMode, Document, LayerStack};
    use std::sync::atomic::{AtomicU64, Ordering};

    static NEXT: AtomicU64 = AtomicU64::new(1);

    fn view() -> PictureViewRust {
        let rgba = vec![200u8; 8 * 8 * 4];
        let doc = Document::from_rgba("Layer 0", 8, 8, &rgba);
        let mut rust = PictureViewRust::default();
        rust.session.doc = Some(doc);
        rust.session.dirty = true;
        rust
    }

    fn path(name: &str) -> String {
        let dir = std::env::temp_dir().join(format!(
            "pictura-save-job-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        std::fs::create_dir_all(&dir).unwrap();
        dir.join(name).to_string_lossy().into_owned()
    }

    fn wait(rust: &PictureViewRust) {
        while save_running_rust(rust) {
            std::thread::yield_now();
        }
    }

    #[test]
    fn a_started_save_finishes_writes_and_clears_modified() {
        let mut rust = view();
        let path = path("save.psd");
        assert!(save_start_rust(&mut rust, &path));
        wait(&rust);
        assert!(save_finish_rust(&mut rust));
        assert_eq!(rust.path.as_deref(), Some(path.as_str()));
        assert!(!rust.session.dirty);
        assert!(std::path::Path::new(&path).is_file());
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn a_foreign_suffix_never_gets_psd_bytes() {
        for name in [
            "sprite.shp",
            "sprite.SHP",
            "notes.foo",
            "image.png",
            "doc.pdf",
        ] {
            let mut rust = view();
            let path = path(name);
            assert!(!save_start_rust(&mut rust, &path), "{name} is refused");
            assert!(rust.save_job.is_none());
            assert!(
                !std::path::Path::new(&path).exists(),
                "{name}: nothing written"
            );
            assert!(rust.session.dirty);
            assert!(rust.path.is_none());
        }
        let mut rust = view();
        let bare = path("no-suffix");
        assert!(
            save_start_rust(&mut rust, &bare),
            "no suffix still means PSD"
        );
        wait(&rust);
        assert!(save_finish_rust(&mut rust));
        assert!(std::fs::read(&bare).unwrap().starts_with(b"8BPS"));
        let _ = std::fs::remove_file(&bare);
    }

    #[test]
    fn a_second_start_while_running_is_refused() {
        let mut rust = view();
        let path = path("twice.psd");
        assert!(save_start_rust(&mut rust, &path));
        assert!(!save_start_rust(&mut rust, &path));
        wait(&rust);
        assert!(save_finish_rust(&mut rust));
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn an_edit_during_the_save_keeps_the_document_modified() {
        let mut rust = view();
        let path = path("stale.psd");
        let snapshot = rust.session.doc.clone().expect("document");
        assert!(save_start_rust(&mut rust, &path));
        rust.session.content_revision = rust.session.content_revision.wrapping_add(1);
        wait(&rust);
        assert!(save_finish_rust(&mut rust));
        assert!(rust.session.dirty);
        assert_eq!(rust.path.as_deref(), Some(path.as_str()));
        let written = std::fs::read(&path).expect("saved file");
        let expected = pictura_codec::write_psd(&snapshot).expect("snapshot bytes");
        assert_eq!(written, expected, "the file holds the pre-edit snapshot");
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn a_replaced_document_keeps_its_path_and_dirty_state() {
        let mut rust = view();
        rust.path = Some("original.psd".to_string());
        let path = path("replaced.psd");
        assert!(save_start_rust(&mut rust, &path));
        rust.session.doc = Some(Document::from_rgba("Other", 4, 4, &[1u8; 4 * 4 * 4]));
        rust.session.document_generation = rust.session.document_generation.wrapping_add(1);
        wait(&rust);
        assert!(!save_finish_rust(&mut rust));
        assert_eq!(rust.path.as_deref(), Some("original.psd"));
        assert!(rust.session.dirty);
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn a_failed_write_leaves_the_document_untouched() {
        let mut rust = view();
        let path = path("save.psd");
        std::fs::create_dir_all(&path).unwrap();
        assert!(save_start_rust(&mut rust, &path));
        wait(&rust);
        assert!(!save_finish_rust(&mut rust));
        assert!(rust.path.is_none());
        assert!(rust.session.dirty);
        let dir = std::path::Path::new(&path).parent().expect("parent");
        let leftovers = std::fs::read_dir(dir)
            .unwrap()
            .filter_map(Result::ok)
            .filter(|entry| entry.file_name().to_string_lossy().ends_with(".tmp"))
            .count();
        assert_eq!(leftovers, 0, "a failed save leaves no temp file");
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn a_synchronous_save_writes_the_worker_bytes() {
        let mut rust = view();
        let path = path("sync.psd");
        assert!(save_start_rust(&mut rust, &path));
        wait(&rust);
        assert!(save_finish_rust(&mut rust));
        let written = std::fs::read(&path).expect("saved file");
        let doc = rust.session.doc.as_ref().expect("document");
        let expected = pictura_codec::write_psd(doc).expect("fresh encode");
        assert_eq!(written, expected);
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn a_layered_document_round_trips_through_the_job() {
        let rgba = vec![10u8; 4 * 4 * 4];
        let mut doc = Document::from_rgba("Layer 0", 4, 4, &rgba);
        let mut layer = doc.layers.get(0).expect("layer").clone();
        layer.name = "Layer 1".to_string();
        layer.channels[1].data.as_mut_slice().fill(9);
        let original = doc.layers.get(0).expect("layer").clone();
        doc.layers = LayerStack::from_layers(vec![layer, original]);
        doc.composite = pictura_core::PixelBuffer {
            width: 4,
            height: 4,
            channels: 4,
            data: vec![10u8; 4 * 4 * 4].into(),
        };
        doc.mode = ColorMode::Rgb;
        doc.depth = BitDepth::Eight;
        let mut rust = PictureViewRust::default();
        rust.session.doc = Some(doc);
        rust.session.dirty = true;
        let path = path("layers.psd");
        assert!(save_start_rust(&mut rust, &path));
        wait(&rust);
        assert!(save_finish_rust(&mut rust));
        let back = pictura_codec::read_psd(&std::fs::read(&path).unwrap()).expect("reads");
        assert_eq!(back.layers.len(), 2);
        let _ = std::fs::remove_file(&path);
    }
}
