//! A GPU failure at run time falls back to the CPU instead of panicking.
//!
//! Its own test binary: the failure turns the GPU off for the whole process.

use pictura_core::{BitDepth, BlendMode, Channel, ColorMode, Document, Layer, PsdRect};
use pictura_render::{
    composite_active, composite_gpu, composite_region_active, composite_rgba, gpu_available,
    inject_gpu_failure, take_gpu_failure_notice, Backend, GpuError,
};

fn document() -> Document {
    const N: u32 = 8;
    let mut channels: Vec<Channel> = (0..4)
        .map(|id| Channel {
            id: if id == 3 { -1 } else { id as i16 },
            data: vec![0u8; (N * N) as usize].into(),
        })
        .collect();
    for i in 0..(N * N) as usize {
        channels[0].data.as_mut_slice()[i] = (i * 4) as u8;
        channels[1].data.as_mut_slice()[i] = 255 - (i * 3) as u8;
        channels[2].data.as_mut_slice()[i] = 90;
        channels[3].data.as_mut_slice()[i] = 128 + (i as u8 % 2) * 127;
    }
    let layer = Layer {
        name: "base".into(),
        rect: PsdRect {
            top: 0,
            left: 0,
            bottom: N as i32,
            right: N as i32,
        },
        blend: BlendMode::Normal,
        opacity: 255,
        fill: 255,
        visible: true,
        channels,
        ..Default::default()
    };
    let mut doc = Document::new(N, N, ColorMode::Rgb, BitDepth::Eight);
    doc.layers = pictura_core::LayerStack::from_layers(vec![layer]);
    doc
}

#[test]
fn an_injected_device_error_falls_back_to_the_cpu_for_the_session() {
    let doc = document();
    let cpu = composite_rgba(&doc);
    let had_gpu = gpu_available();

    inject_gpu_failure();
    let (buffer, backend) = composite_active(&doc, true);
    assert_eq!(backend, Backend::Cpu, "the failed composite falls back");
    assert_eq!(buffer.data, cpu.data, "the fallback is the CPU composite");
    assert!(!gpu_available(), "the GPU stays off");

    if had_gpu {
        let notice = take_gpu_failure_notice().expect("the failure leaves one notice");
        assert!(notice.contains("The GPU failed"), "{notice}");
        assert!(notice.contains("pictura-injected-failure"), "{notice}");
        assert!(
            take_gpu_failure_notice().is_none(),
            "the notice is taken once"
        );
    } else {
        eprintln!("no GPU device: only the CPU path was checked");
    }

    assert!(matches!(composite_gpu(&doc), Err(GpuError::Unavailable)));
    let (buffer, backend) = composite_active(&doc, true);
    assert_eq!(backend, Backend::Cpu);
    assert_eq!(buffer.data, cpu.data);
    let rect = PsdRect {
        top: 1,
        left: 2,
        bottom: 5,
        right: 7,
    };
    let (region, backend) = composite_region_active(&doc, rect, true);
    assert_eq!(backend, Backend::Cpu);
    assert_eq!((region.width, region.height), (5, 4));
}
