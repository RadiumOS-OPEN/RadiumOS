// Host-side test: draws the demo cube through the C API into an image and writes /tmp/vk/test.ppm.
// Run from the throwaway crate in /tmp/vkdev (`cargo test`), which includes this module on the host.

use super::*;
use std::fs;
use std::vec::Vec;

static mut LAST_FRAME: Vec<u32> = Vec::new();
static mut LAST_DIMS: (i32, i32) = (0, 0);

#[no_mangle] pub extern "C" fn fbcon_suspend() {}
#[no_mangle] pub extern "C" fn fbcon_resume() {}
#[no_mangle] pub unsafe extern "C" fn fbcon_blit(src: *const u32, stride: i32, _x: i32, _y: i32, w: i32, h: i32) {
    if w == 1280 { return; }
    let frame = &mut *core::ptr::addr_of_mut!(LAST_FRAME);
    frame.clear();
    for row in 0..h { for col in 0..w { frame.push(*src.add((row * stride + col) as usize)); } }
    *core::ptr::addr_of_mut!(LAST_DIMS) = (w, h);
}

fn words(bytes: &[u8]) -> Vec<u32> { bytes.chunks(4).map(|c| u32::from_le_bytes([c[0], c[1], c[2], c[3]])).collect() }

const FACES: [([[f32; 3]; 4], [f32; 3]); 6] = [
    ([[-1., -1., 1.], [1., -1., 1.], [1., 1., 1.], [-1., 1., 1.]], [1.0, 0.2, 0.2]),
    ([[1., -1., -1.], [-1., -1., -1.], [-1., 1., -1.], [1., 1., -1.]], [0.2, 1.0, 0.2]),
    ([[1., -1., 1.], [1., -1., -1.], [1., 1., -1.], [1., 1., 1.]], [0.3, 0.4, 1.0]),
    ([[-1., -1., -1.], [-1., -1., 1.], [-1., 1., 1.], [-1., 1., -1.]], [1.0, 1.0, 0.2]),
    ([[-1., 1., 1.], [1., 1., 1.], [1., 1., -1.], [-1., 1., -1.]], [0.2, 1.0, 1.0]),
    ([[-1., -1., -1.], [1., -1., -1.], [1., -1., 1.], [-1., -1., 1.]], [1.0, 0.3, 1.0]),
];

unsafe fn mem_for(size: u64, out: &mut H) -> *mut u8 {
    let ai = MemoryAllocateInfo { s_type: 5, p_next: core::ptr::null(), size, memory_type_index: 0 };
    assert_eq!(vkAllocateMemory(core::ptr::null_mut(), &ai, core::ptr::null(), out), 0);
    let mut p: *mut c_void = core::ptr::null_mut();
    assert_eq!(vkMapMemory(core::ptr::null_mut(), *out, 0, size, 0, &mut p), 0);
    p as *mut u8
}

unsafe fn make_buffer(data: &[u8], usage: u32) -> H {
    let d = core::ptr::null_mut();
    let bi = BufferCreateInfo { s_type: 12, p_next: core::ptr::null(), flags: 0, size: data.len() as u64, usage, sharing_mode: 0, qfi_count: 0, p_qfi: core::ptr::null() };
    let mut buf = 0;
    assert_eq!(vkCreateBuffer(d, &bi, core::ptr::null(), &mut buf), 0);
    let mut req = MemoryRequirements { size: 0, alignment: 0, memory_type_bits: 0 };
    vkGetBufferMemoryRequirements(d, buf, &mut req);
    let mut mem = 0;
    let p = mem_for(req.size, &mut mem);
    assert_eq!(vkBindBufferMemory(d, buf, mem, 0), 0);
    core::ptr::copy_nonoverlapping(data.as_ptr(), p, data.len());
    buf
}

unsafe fn make_image(w: u32, h: u32, format: u32, usage: u32) -> (H, H, H, *mut u8) {
    let d = core::ptr::null_mut();
    let ci = ImageCreateInfo { s_type: 14, p_next: core::ptr::null(), flags: 0, image_type: 1, format, width: w, height: h, depth: 1, mip_levels: 1, array_layers: 1, samples: 1, tiling: 0, usage, sharing_mode: 0, qfi_count: 0, p_qfi: core::ptr::null(), initial_layout: 0 };
    let mut img = 0;
    assert_eq!(vkCreateImage(d, &ci, core::ptr::null(), &mut img), 0);
    let mut req = MemoryRequirements { size: 0, alignment: 0, memory_type_bits: 0 };
    vkGetImageMemoryRequirements(d, img, &mut req);
    let mut mem = 0;
    let p = mem_for(req.size, &mut mem);
    assert_eq!(vkBindImageMemory(d, img, mem, 0), 0);
    let vi = ImageViewCreateInfo { s_type: 15, p_next: core::ptr::null(), flags: 0, image: img, view_type: 1, format, components: [0; 4], aspect: 1, base_mip: 0, level_count: 1, base_layer: 0, layer_count: 1 };
    let mut view = 0;
    assert_eq!(vkCreateImageView(d, &vi, core::ptr::null(), &mut view), 0);
    (img, mem, view, p)
}

#[test]
fn renders_cube() {
    unsafe {
        let d: *mut c_void = core::ptr::null_mut();
        let (w, h) = (320u32, 180u32);

        // geometry
        let mut vdata: Vec<f32> = Vec::new();
        let mut idata: Vec<u16> = Vec::new();
        for (f, (corners, color)) in FACES.iter().enumerate() {
            for c in corners { vdata.extend_from_slice(c); vdata.extend_from_slice(color); }
            let b = (f * 4) as u16;
            idata.extend_from_slice(&[b, b + 1, b + 2, b + 2, b + 3, b]);
        }
        let vbuf = make_buffer(core::slice::from_raw_parts(vdata.as_ptr() as *const u8, vdata.len() * 4), 0x80);
        let ibuf = make_buffer(core::slice::from_raw_parts(idata.as_ptr() as *const u8, idata.len() * 2), 0x40);
        let mut mvp = [0.0f32; 16];
        vkRadiumDemoMatrix(mvp.as_mut_ptr(), 0.7, w as f32 / h as f32);
        let ubuf = make_buffer(core::slice::from_raw_parts(mvp.as_ptr() as *const u8, 64), 0x10);

        // targets
        let (cimg, _cm, cview, cptr) = make_image(w, h, 44, 0x10);
        let (_dimg, _dm, dview, _dptr) = make_image(w, h, 126, 0x20);

        // render pass and framebuffer
        let atts = [
            AttachmentDescription { flags: 0, format: 44, samples: 1, load_op: 1, store_op: 0, stencil_load_op: 2, stencil_store_op: 1, initial_layout: 0, final_layout: 1000001002 },
            AttachmentDescription { flags: 0, format: 126, samples: 1, load_op: 1, store_op: 1, stencil_load_op: 2, stencil_store_op: 1, initial_layout: 0, final_layout: 3 },
        ];
        let cref = AttachmentReference { attachment: 0, layout: 2 };
        let dref = AttachmentReference { attachment: 1, layout: 3 };
        let sub = SubpassDescription { flags: 0, bind_point: 0, input_count: 0, p_input: core::ptr::null(), color_count: 1, p_color: &cref, p_resolve: core::ptr::null(), p_depth: &dref, preserve_count: 0, p_preserve: core::ptr::null() };
        let rpi = RenderPassCreateInfo { s_type: 38, p_next: core::ptr::null(), flags: 0, attachment_count: 2, p_attachments: atts.as_ptr(), subpass_count: 1, p_subpasses: &sub, dependency_count: 0, p_dependencies: core::ptr::null() };
        let mut rp = 0;
        assert_eq!(vkCreateRenderPass(d, &rpi, core::ptr::null(), &mut rp), 0);
        let views = [cview, dview];
        let fbi = FramebufferCreateInfo { s_type: 37, p_next: core::ptr::null(), flags: 0, render_pass: rp, attachment_count: 2, p_attachments: views.as_ptr(), width: w, height: h, layers: 1 };
        let mut fb = 0;
        assert_eq!(vkCreateFramebuffer(d, &fbi, core::ptr::null(), &mut fb), 0);

        // shaders
        let vs_words = words(include_bytes!("../../../tools/shaders/cube.vert.spv"));
        let fs_words = words(include_bytes!("../../../tools/shaders/cube.frag.spv"));
        let (mut vsm, mut fsm) = (0, 0);
        let smi = |w: &Vec<u32>| ShaderModuleCreateInfo { s_type: 16, p_next: core::ptr::null(), flags: 0, code_size: w.len() * 4, p_code: w.as_ptr() };
        assert_eq!(vkCreateShaderModule(d, &smi(&vs_words), core::ptr::null(), &mut vsm), 0);
        assert_eq!(vkCreateShaderModule(d, &smi(&fs_words), core::ptr::null(), &mut fsm), 0);

        // layouts and descriptors
        let dsb = DescriptorSetLayoutBinding { binding: 0, descriptor_type: 6, count: 1, stage_flags: 1, p_immutable: core::ptr::null() };
        let dslci = DescriptorSetLayoutCreateInfo { s_type: 32, p_next: core::ptr::null(), flags: 0, binding_count: 1, p_bindings: &dsb };
        let mut dsl = 0;
        assert_eq!(vkCreateDescriptorSetLayout(d, &dslci, core::ptr::null(), &mut dsl), 0);
        let pcr = PushConstantRange { stage_flags: 0x10, offset: 0, size: 4 };
        let plci = PipelineLayoutCreateInfo { s_type: 30, p_next: core::ptr::null(), flags: 0, set_layout_count: 1, p_set_layouts: &dsl, push_range_count: 1, p_push_ranges: &pcr };
        let mut layout = 0;
        assert_eq!(vkCreatePipelineLayout(d, &plci, core::ptr::null(), &mut layout), 0);
        let dpci = DescriptorPoolCreateInfo { s_type: 33, p_next: core::ptr::null(), flags: 0, max_sets: 1, pool_size_count: 0, p_pool_sizes: core::ptr::null() };
        let mut pool = 0;
        assert_eq!(vkCreateDescriptorPool(d, &dpci, core::ptr::null(), &mut pool), 0);
        let dsai = DescriptorSetAllocateInfo { s_type: 34, p_next: core::ptr::null(), pool, count: 1, p_layouts: &dsl };
        let mut set = 0;
        assert_eq!(vkAllocateDescriptorSets(d, &dsai, &mut set), 0);
        let dbi = DescriptorBufferInfo { buffer: ubuf, offset: 0, range: 64 };
        let wds = WriteDescriptorSet { s_type: 35, p_next: core::ptr::null(), dst_set: set, dst_binding: 0, dst_array: 0, count: 1, descriptor_type: 6, p_image: core::ptr::null(), p_buffer: &dbi, p_texel: core::ptr::null() };
        vkUpdateDescriptorSets(d, 1, &wds, 0, core::ptr::null());

        // pipeline
        let name = b"main\0";
        let stages = [
            PipelineShaderStageCreateInfo { s_type: 18, p_next: core::ptr::null(), flags: 0, stage: 1, module: vsm, p_name: name.as_ptr(), p_spec: core::ptr::null() },
            PipelineShaderStageCreateInfo { s_type: 18, p_next: core::ptr::null(), flags: 0, stage: 0x10, module: fsm, p_name: name.as_ptr(), p_spec: core::ptr::null() },
        ];
        let vb = VertexInputBindingDescription { binding: 0, stride: 24, input_rate: 0 };
        let va = [VertexInputAttributeDescription { location: 0, binding: 0, format: 106, offset: 0 }, VertexInputAttributeDescription { location: 1, binding: 0, format: 106, offset: 12 }];
        let vi = PipelineVertexInputStateCreateInfo { s_type: 19, p_next: core::ptr::null(), flags: 0, binding_count: 1, p_bindings: &vb, attribute_count: 2, p_attributes: va.as_ptr() };
        let ia = PipelineInputAssemblyStateCreateInfo { s_type: 20, p_next: core::ptr::null(), flags: 0, topology: 3, primitive_restart: 0 };
        let vp = Viewport { x: 0.0, y: 0.0, width: w as f32, height: h as f32, min_depth: 0.0, max_depth: 1.0 };
        let sc = Rect2D { offset: Offset2D { x: 0, y: 0 }, extent: Extent2D { width: w, height: h } };
        let vps = PipelineViewportStateCreateInfo { s_type: 22, p_next: core::ptr::null(), flags: 0, viewport_count: 1, p_viewports: &vp, scissor_count: 1, p_scissors: &sc };
        let rs = PipelineRasterizationStateCreateInfo { s_type: 23, p_next: core::ptr::null(), flags: 0, depth_clamp: 0, discard: 0, polygon_mode: 0, cull_mode: 2, front_face: 1, depth_bias: 0, bias_constant: 0.0, bias_clamp: 0.0, bias_slope: 0.0, line_width: 1.0 };
        let so = StencilOpState { a: [0; 7] };
        let so2 = StencilOpState { a: [0; 7] };
        let ds = PipelineDepthStencilStateCreateInfo { s_type: 25, p_next: core::ptr::null(), flags: 0, depth_test: 1, depth_write: 1, depth_op: 1, bounds_test: 0, stencil_test: 0, front: so, back: so2, min_bounds: 0.0, max_bounds: 1.0 };
        let cba = PipelineColorBlendAttachmentState { enable: 0, src_color: 1, dst_color: 0, color_op: 0, src_alpha: 1, dst_alpha: 0, alpha_op: 0, write_mask: 0xF };
        let cb = PipelineColorBlendStateCreateInfo { s_type: 26, p_next: core::ptr::null(), flags: 0, logic_op_enable: 0, logic_op: 0, attachment_count: 1, p_attachments: &cba, constants: [0.0; 4] };
        let gpi = GraphicsPipelineCreateInfo {
            s_type: 28, p_next: core::ptr::null(), flags: 0, stage_count: 2, p_stages: stages.as_ptr(), p_vertex_input: &vi, p_input_assembly: &ia,
            p_tessellation: core::ptr::null(), p_viewport: &vps, p_rasterization: &rs, p_multisample: core::ptr::null(), p_depth_stencil: &ds, p_color_blend: &cb,
            p_dynamic: core::ptr::null(), layout, render_pass: rp, subpass: 0, base_pipeline: 0, base_index: -1,
        };
        let mut pipe = 0;
        assert_eq!(vkCreateGraphicsPipelines(d, 0, 1, &gpi, core::ptr::null(), &mut pipe), 0);

        // commands
        let cpai = CommandBufferAllocateInfo { s_type: 40, p_next: core::ptr::null(), pool: 0, level: 0, count: 1 };
        let mut cmd: *mut c_void = core::ptr::null_mut();
        assert_eq!(vkAllocateCommandBuffers(d, &cpai, &mut cmd), 0);
        assert_eq!(vkBeginCommandBuffer(cmd, core::ptr::null()), 0);
        let clears = [[0.05f32.to_bits(), 0.05f32.to_bits(), 0.1f32.to_bits(), 1.0f32.to_bits()], [1.0f32.to_bits(), 0, 0, 0]];
        let rbi = RenderPassBeginInfo { s_type: 43, p_next: core::ptr::null(), render_pass: rp, framebuffer: fb, area: sc, clear_count: 2, p_clears: clears.as_ptr() };
        vkCmdBeginRenderPass(cmd, &rbi, 0);
        vkCmdBindPipeline(cmd, 0, pipe);
        let offs = [0u64];
        vkCmdBindVertexBuffers(cmd, 0, 1, &vbuf, offs.as_ptr());
        vkCmdBindIndexBuffer(cmd, ibuf, 0, 0);
        vkCmdBindDescriptorSets(cmd, 0, layout, 0, 1, &set, 0, core::ptr::null());
        let t = 1.25f32;
        vkCmdPushConstants(cmd, layout, 0x10, 0, 4, &t as *const f32 as *const u8);
        vkCmdDrawIndexed(cmd, 36, 1, 0, 0, 0);
        vkCmdEndRenderPass(cmd);
        assert_eq!(vkEndCommandBuffer(cmd), 0);
        let si = SubmitInfo { s_type: 4, p_next: core::ptr::null(), wait_count: 0, p_wait: core::ptr::null(), p_wait_stage: core::ptr::null(), cmd_count: 1, p_cmds: &cmd, signal_count: 0, p_signal: core::ptr::null() };
        assert_eq!(vkQueueSubmit(core::ptr::null_mut(), 1, &si, 0), 0);

        let px = core::slice::from_raw_parts(cptr as *const u32, (w * h) as usize);
        let mut ppm = std::format!("P6\n{} {}\n255\n", w, h).into_bytes();
        for p in px { ppm.extend_from_slice(&[(p >> 16) as u8, (p >> 8) as u8, *p as u8]); }
        let _ = fs::create_dir_all("/tmp/vk");
        fs::write("/tmp/vk/test.ppm", &ppm).unwrap();
        let background = px[0];
        let covered = px.iter().filter(|p| **p != background).count();
        std::println!("covered pixels: {} of {}", covered, px.len());
        assert!(covered > 3000, "the cube should cover a good part of the image");
        let _ = cimg;
    }
}

// The Doom port's presenter: a storage buffer holding a 4x2 "frame", scaled 3x by a fullscreen-triangle pipeline.
#[test]
fn blits_storage_buffer() {
    unsafe {
        let d: *mut c_void = core::ptr::null_mut();
        let (w, h) = (std::env::var("VKW").ok().and_then(|v| v.parse().ok()).unwrap_or(12u32), std::env::var("VKH").ok().and_then(|v| v.parse().ok()).unwrap_or(6u32));
        let frame: [u32; 8] = [0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF, 0x102030, 0x405060, 0x708090, 0xA0B0C0];
        let sbuf = make_buffer(core::slice::from_raw_parts(frame.as_ptr() as *const u8, 32), 0x20);
        let (_img, _m, view, cptr) = make_image(w, h, 44, 0x10);
        let att = AttachmentDescription { flags: 0, format: 44, samples: 1, load_op: 1, store_op: 0, stencil_load_op: 2, stencil_store_op: 1, initial_layout: 0, final_layout: 1000001002 };
        let cref = AttachmentReference { attachment: 0, layout: 2 };
        let sub = SubpassDescription { flags: 0, bind_point: 0, input_count: 0, p_input: core::ptr::null(), color_count: 1, p_color: &cref, p_resolve: core::ptr::null(), p_depth: core::ptr::null(), preserve_count: 0, p_preserve: core::ptr::null() };
        let rpi = RenderPassCreateInfo { s_type: 38, p_next: core::ptr::null(), flags: 0, attachment_count: 1, p_attachments: &att, subpass_count: 1, p_subpasses: &sub, dependency_count: 0, p_dependencies: core::ptr::null() };
        let mut rp = 0;
        assert_eq!(vkCreateRenderPass(d, &rpi, core::ptr::null(), &mut rp), 0);
        let fbi = FramebufferCreateInfo { s_type: 37, p_next: core::ptr::null(), flags: 0, render_pass: rp, attachment_count: 1, p_attachments: &view, width: w, height: h, layers: 1 };
        let mut fb = 0;
        assert_eq!(vkCreateFramebuffer(d, &fbi, core::ptr::null(), &mut fb), 0);

        let vs_words = words(include_bytes!("../../../tools/shaders/doom_blit.vert.spv"));
        let fs_words = words(include_bytes!("../../../tools/shaders/doom_blit.frag.spv"));
        let (mut vsm, mut fsm) = (0, 0);
        let smi = |w: &Vec<u32>| ShaderModuleCreateInfo { s_type: 16, p_next: core::ptr::null(), flags: 0, code_size: w.len() * 4, p_code: w.as_ptr() };
        assert_eq!(vkCreateShaderModule(d, &smi(&vs_words), core::ptr::null(), &mut vsm), 0);
        assert_eq!(vkCreateShaderModule(d, &smi(&fs_words), core::ptr::null(), &mut fsm), 0);

        let dsb = DescriptorSetLayoutBinding { binding: 0, descriptor_type: 7, count: 1, stage_flags: 0x10, p_immutable: core::ptr::null() };
        let dslci = DescriptorSetLayoutCreateInfo { s_type: 32, p_next: core::ptr::null(), flags: 0, binding_count: 1, p_bindings: &dsb };
        let mut dsl = 0;
        assert_eq!(vkCreateDescriptorSetLayout(d, &dslci, core::ptr::null(), &mut dsl), 0);
        let pcr = PushConstantRange { stage_flags: 0x10, offset: 0, size: 8 };
        let plci = PipelineLayoutCreateInfo { s_type: 30, p_next: core::ptr::null(), flags: 0, set_layout_count: 1, p_set_layouts: &dsl, push_range_count: 1, p_push_ranges: &pcr };
        let mut layout = 0;
        assert_eq!(vkCreatePipelineLayout(d, &plci, core::ptr::null(), &mut layout), 0);
        let dsai = DescriptorSetAllocateInfo { s_type: 34, p_next: core::ptr::null(), pool: 0, count: 1, p_layouts: &dsl };
        let mut set = 0;
        assert_eq!(vkAllocateDescriptorSets(d, &dsai, &mut set), 0);
        let dbi = DescriptorBufferInfo { buffer: sbuf, offset: 0, range: u64::MAX };
        let wds = WriteDescriptorSet { s_type: 35, p_next: core::ptr::null(), dst_set: set, dst_binding: 0, dst_array: 0, count: 1, descriptor_type: 7, p_image: core::ptr::null(), p_buffer: &dbi, p_texel: core::ptr::null() };
        vkUpdateDescriptorSets(d, 1, &wds, 0, core::ptr::null());

        let name = b"main\0";
        let stages = [
            PipelineShaderStageCreateInfo { s_type: 18, p_next: core::ptr::null(), flags: 0, stage: 1, module: vsm, p_name: name.as_ptr(), p_spec: core::ptr::null() },
            PipelineShaderStageCreateInfo { s_type: 18, p_next: core::ptr::null(), flags: 0, stage: 0x10, module: fsm, p_name: name.as_ptr(), p_spec: core::ptr::null() },
        ];
        let vi = PipelineVertexInputStateCreateInfo { s_type: 19, p_next: core::ptr::null(), flags: 0, binding_count: 0, p_bindings: core::ptr::null(), attribute_count: 0, p_attributes: core::ptr::null() };
        let ia = PipelineInputAssemblyStateCreateInfo { s_type: 20, p_next: core::ptr::null(), flags: 0, topology: 3, primitive_restart: 0 };
        let rs = PipelineRasterizationStateCreateInfo { s_type: 23, p_next: core::ptr::null(), flags: 0, depth_clamp: 0, discard: 0, polygon_mode: 0, cull_mode: 0, front_face: 0, depth_bias: 0, bias_constant: 0.0, bias_clamp: 0.0, bias_slope: 0.0, line_width: 1.0 };
        let cba = PipelineColorBlendAttachmentState { enable: 0, src_color: 1, dst_color: 0, color_op: 0, src_alpha: 1, dst_alpha: 0, alpha_op: 0, write_mask: 0xF };
        let cb = PipelineColorBlendStateCreateInfo { s_type: 26, p_next: core::ptr::null(), flags: 0, logic_op_enable: 0, logic_op: 0, attachment_count: 1, p_attachments: &cba, constants: [0.0; 4] };
        let gpi = GraphicsPipelineCreateInfo {
            s_type: 28, p_next: core::ptr::null(), flags: 0, stage_count: 2, p_stages: stages.as_ptr(), p_vertex_input: &vi, p_input_assembly: &ia,
            p_tessellation: core::ptr::null(), p_viewport: core::ptr::null(), p_rasterization: &rs, p_multisample: core::ptr::null(), p_depth_stencil: core::ptr::null(),
            p_color_blend: &cb, p_dynamic: core::ptr::null(), layout, render_pass: rp, subpass: 0, base_pipeline: 0, base_index: -1,
        };
        let mut pipe = 0;
        assert_eq!(vkCreateGraphicsPipelines(d, 0, 1, &gpi, core::ptr::null(), &mut pipe), 0);

        let cpai = CommandBufferAllocateInfo { s_type: 40, p_next: core::ptr::null(), pool: 0, level: 0, count: 1 };
        let mut cmd: *mut c_void = core::ptr::null_mut();
        assert_eq!(vkAllocateCommandBuffers(d, &cpai, &mut cmd), 0);
        assert_eq!(vkBeginCommandBuffer(cmd, core::ptr::null()), 0);
        let area = Rect2D { offset: Offset2D { x: 0, y: 0 }, extent: Extent2D { width: w, height: h } };
        let clears = [[0u32; 4]];
        let rbi = RenderPassBeginInfo { s_type: 43, p_next: core::ptr::null(), render_pass: rp, framebuffer: fb, area, clear_count: 1, p_clears: clears.as_ptr() };
        vkCmdBeginRenderPass(cmd, &rbi, 0);
        vkCmdBindPipeline(cmd, 0, pipe);
        vkCmdBindDescriptorSets(cmd, 0, layout, 0, 1, &set, 0, core::ptr::null());
        let pc: [u32; 2] = [4, 3];
        vkCmdPushConstants(cmd, layout, 0x10, 0, 8, pc.as_ptr() as *const u8);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
        assert_eq!(vkEndCommandBuffer(cmd), 0);
        let si = SubmitInfo { s_type: 4, p_next: core::ptr::null(), wait_count: 0, p_wait: core::ptr::null(), p_wait_stage: core::ptr::null(), cmd_count: 1, p_cmds: &cmd, signal_count: 0, p_signal: core::ptr::null() };
        let t0 = std::time::Instant::now();
        assert_eq!(vkQueueSubmit(core::ptr::null_mut(), 1, &si, 0), 0);
        std::println!("blit {}x{} took {:?}", w, h, t0.elapsed());
        if w != 12 { return; }

        let px = core::slice::from_raw_parts(cptr as *const u32, (w * h) as usize);
        for y in 0..h as usize {
            for x in 0..w as usize {
                let want = frame[(y / 3) * 4 + x / 3] | 0xFF00_0000;
                assert_eq!(px[y * w as usize + x], want, "pixel {},{}", x, y);
            }
        }
    }
}
