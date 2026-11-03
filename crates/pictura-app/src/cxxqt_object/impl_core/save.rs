//! Document save: native PSD/PSB through the worker job, raster with the
//! per-document profile choice, SHP and TMP through the fork's writers. Any
//! other suffix is refused, never written as a PSD under that name.

use super::*;

impl qobject::PictureView {
    pub fn save(mut self: Pin<&mut Self>, path: &QString) -> bool {
        let path_string = path.to_string();
        let suffix = format_for_path(&path_string);
        if matches!(suffix.as_str(), "pdf" | "pdp") {
            return super::super::pdf_export::save_with_last_options(
                self,
                &QString::from(path_string.as_str()),
            );
        }
        if suffix == "shp" || super::super::shp::tmp_suffix(&suffix) {
            return super::super::shp::save_westwood(self, &path_string, &suffix);
        }
        if raster_writer_for_suffix(&suffix).is_none() {
            if !super::save_jobs::save_start_rust(&mut self.as_mut().rust_mut(), &path_string) {
                return false;
            }
            return super::save_jobs::save_finish_rust(&mut self.as_mut().rust_mut());
        }
        let native = false;
        let embed = {
            let rust = self.rust();
            rust.save_embed_profile
                .unwrap_or_else(|| super::super::color_management::save_embed_default(rust, native))
        };
        let dirty = self.rust().session.dirty;
        let Some(format) = raster_writer_for_suffix(&suffix) else {
            return false;
        };
        let gpu_compute = self.rust().gpu_compute;
        let options = self.rust().save_raster_options.unwrap_or_default();
        let keeps_working = matches!(
            format.to_ascii_lowercase().as_str(),
            "jpg" | "jpeg" | "jpe" | "tif" | "tiff"
        );
        let (rgba, width, height, metadata) = {
            let mut rust = self.as_mut().rust_mut();
            let session = &mut rust.session;
            let Some(doc) = session.doc.as_mut() else {
                return false;
            };
            if dirty {
                pictura_render::refresh_native_composite(doc);
            }
            let buffer = current_buffer(doc, gpu_compute);
            let rgba = if keeps_working {
                buffer_to_rgba_bytes(&buffer)
            } else {
                let srgb = pictura_codec::buffer_to_srgb(doc, &buffer);
                buffer_to_rgba_bytes(&srgb)
            };
            let mut metadata = pictura_codec::raster_metadata(doc);
            metadata.vanishing_point = store_planes(session.vanishing_planes.as_ref(), doc);
            if keeps_working && embed {
                // The raster pixels are the working RGB values; a CMYK
                // source keeps its profile only in PSD/PSB.
                metadata.icc = Some(if doc.source_mode == Some(pictura_core::ColorMode::Cmyk) {
                    pictura_codec::Profile::srgb().to_icc()
                } else {
                    doc.document_icc
                        .clone()
                        .unwrap_or_else(|| pictura_codec::Profile::srgb().to_icc().into())
                        .to_vec()
                });
            }
            (rgba, buffer.width, buffer.height, metadata)
        };
        if !super::super::export::write_raster_with_metadata(
            &rgba,
            width,
            height,
            &path_string,
            format,
            &metadata,
            &options,
        ) {
            return false;
        }
        let mut view = self.as_mut().rust_mut();
        view.path = Some(path_string);
        view.session.mark_saved();
        true
    }
}
