// A software implementation of a subset of Vulkan 1.0 for RadiumOS. Shaders are real SPIR-V, run by the interpreter in
// spirv.rs; triangles are rasterized on the CPU (raster.rs) into images that vkQueuePresentKHR copies to the screen.
// The C declarations are in src/vulkan/vulkan.h. Entry points not listed there are not implemented.
//
// Everything is single-threaded and synchronous: vkQueueSubmit executes the command buffers before it returns, so
// fences and semaphores only exist as handles.

use alloc::{boxed::Box, vec, vec::Vec};
use core::ffi::c_void;

pub mod raster;
pub mod spirv;

use raster::{Attribute, Blend, Draw, Pipe, Target, VBuf, VertexBinding, MAX_VERTEX_BINDINGS};
use spirv::{Machine, Module};

pub const MAX_VARYING_WORDS: usize = 64;

type H = u64;

const VK_SUCCESS: i32 = 0;
const VK_NOT_READY: i32 = 1;
const VK_ERROR_OUT_OF_HOST_MEMORY: i32 = -1;
const VK_ERROR_INITIALIZATION_FAILED: i32 = -3;
const VK_ERROR_MEMORY_MAP_FAILED: i32 = -5;
const VK_ERROR_FEATURE_NOT_PRESENT: i32 = -8;
const VK_ERROR_FORMAT_NOT_SUPPORTED: i32 = -11;
const VK_ERROR_UNKNOWN: i32 = -13;
const VK_INCOMPLETE: i32 = 5;

extern "C" {
    fn fbcon_suspend();
    fn fbcon_resume();
    fn fbcon_blit(src: *const u32, stride: i32, x: i32, y: i32, w: i32, h: i32);
}

// ---- C structures (32-bit layout) ----------------------------------------------------------------------------

#[repr(C)] #[derive(Clone, Copy)] struct Extent2D { width: u32, height: u32 }
#[repr(C)] #[derive(Clone, Copy)] struct Offset2D { x: i32, y: i32 }
#[repr(C)] #[derive(Clone, Copy)] struct Rect2D { offset: Offset2D, extent: Extent2D }
#[repr(C)] #[derive(Clone, Copy)] pub struct Viewport { x: f32, y: f32, width: f32, height: f32, min_depth: f32, max_depth: f32 }

#[repr(C)] struct BufferCreateInfo { s_type: u32, p_next: *const c_void, flags: u32, size: u64, usage: u32, sharing_mode: u32, qfi_count: u32, p_qfi: *const u32 }
#[repr(C)] struct MemoryAllocateInfo { s_type: u32, p_next: *const c_void, size: u64, memory_type_index: u32 }
#[repr(C)] struct MemoryRequirements { size: u64, alignment: u64, memory_type_bits: u32 }
#[repr(C)] struct ImageCreateInfo {
    s_type: u32, p_next: *const c_void, flags: u32, image_type: u32, format: u32, width: u32, height: u32, depth: u32,
    mip_levels: u32, array_layers: u32, samples: u32, tiling: u32, usage: u32, sharing_mode: u32, qfi_count: u32, p_qfi: *const u32, initial_layout: u32,
}
#[repr(C)] struct ImageViewCreateInfo {
    s_type: u32, p_next: *const c_void, flags: u32, image: H, view_type: u32, format: u32, components: [u32; 4], aspect: u32, base_mip: u32, level_count: u32, base_layer: u32, layer_count: u32,
}
#[repr(C)] struct ShaderModuleCreateInfo { s_type: u32, p_next: *const c_void, flags: u32, code_size: usize, p_code: *const u32 }
#[repr(C)] struct AttachmentDescription { flags: u32, format: u32, samples: u32, load_op: u32, store_op: u32, stencil_load_op: u32, stencil_store_op: u32, initial_layout: u32, final_layout: u32 }
#[repr(C)] struct AttachmentReference { attachment: u32, layout: u32 }
#[repr(C)] struct SubpassDescription {
    flags: u32, bind_point: u32, input_count: u32, p_input: *const AttachmentReference, color_count: u32, p_color: *const AttachmentReference,
    p_resolve: *const AttachmentReference, p_depth: *const AttachmentReference, preserve_count: u32, p_preserve: *const u32,
}
#[repr(C)] struct RenderPassCreateInfo {
    s_type: u32, p_next: *const c_void, flags: u32, attachment_count: u32, p_attachments: *const AttachmentDescription,
    subpass_count: u32, p_subpasses: *const SubpassDescription, dependency_count: u32, p_dependencies: *const c_void,
}
#[repr(C)] struct FramebufferCreateInfo { s_type: u32, p_next: *const c_void, flags: u32, render_pass: H, attachment_count: u32, p_attachments: *const H, width: u32, height: u32, layers: u32 }
#[repr(C)] struct PushConstantRange { stage_flags: u32, offset: u32, size: u32 }
#[repr(C)] struct PipelineLayoutCreateInfo { s_type: u32, p_next: *const c_void, flags: u32, set_layout_count: u32, p_set_layouts: *const H, push_range_count: u32, p_push_ranges: *const PushConstantRange }
#[repr(C)] struct DescriptorSetLayoutBinding { binding: u32, descriptor_type: u32, count: u32, stage_flags: u32, p_immutable: *const H }
#[repr(C)] struct DescriptorSetLayoutCreateInfo { s_type: u32, p_next: *const c_void, flags: u32, binding_count: u32, p_bindings: *const DescriptorSetLayoutBinding }
#[repr(C)] struct DescriptorPoolCreateInfo { s_type: u32, p_next: *const c_void, flags: u32, max_sets: u32, pool_size_count: u32, p_pool_sizes: *const c_void }
#[repr(C)] struct DescriptorSetAllocateInfo { s_type: u32, p_next: *const c_void, pool: H, count: u32, p_layouts: *const H }
#[repr(C)] struct DescriptorBufferInfo { buffer: H, offset: u64, range: u64 }
#[repr(C)] struct WriteDescriptorSet {
    s_type: u32, p_next: *const c_void, dst_set: H, dst_binding: u32, dst_array: u32, count: u32, descriptor_type: u32,
    p_image: *const c_void, p_buffer: *const DescriptorBufferInfo, p_texel: *const c_void,
}
#[repr(C)] struct PipelineShaderStageCreateInfo { s_type: u32, p_next: *const c_void, flags: u32, stage: u32, module: H, p_name: *const u8, p_spec: *const c_void }
#[repr(C)] struct VertexInputBindingDescription { binding: u32, stride: u32, input_rate: u32 }
#[repr(C)] struct VertexInputAttributeDescription { location: u32, binding: u32, format: u32, offset: u32 }
#[repr(C)] struct PipelineVertexInputStateCreateInfo {
    s_type: u32, p_next: *const c_void, flags: u32, binding_count: u32, p_bindings: *const VertexInputBindingDescription,
    attribute_count: u32, p_attributes: *const VertexInputAttributeDescription,
}
#[repr(C)] struct PipelineInputAssemblyStateCreateInfo { s_type: u32, p_next: *const c_void, flags: u32, topology: u32, primitive_restart: u32 }
#[repr(C)] struct PipelineViewportStateCreateInfo { s_type: u32, p_next: *const c_void, flags: u32, viewport_count: u32, p_viewports: *const Viewport, scissor_count: u32, p_scissors: *const Rect2D }
#[repr(C)] struct PipelineRasterizationStateCreateInfo {
    s_type: u32, p_next: *const c_void, flags: u32, depth_clamp: u32, discard: u32, polygon_mode: u32, cull_mode: u32, front_face: u32,
    depth_bias: u32, bias_constant: f32, bias_clamp: f32, bias_slope: f32, line_width: f32,
}
#[repr(C)] struct StencilOpState { a: [u32; 7] }
#[repr(C)] struct PipelineDepthStencilStateCreateInfo {
    s_type: u32, p_next: *const c_void, flags: u32, depth_test: u32, depth_write: u32, depth_op: u32, bounds_test: u32, stencil_test: u32,
    front: StencilOpState, back: StencilOpState, min_bounds: f32, max_bounds: f32,
}
#[repr(C)] struct PipelineColorBlendAttachmentState {
    enable: u32, src_color: u32, dst_color: u32, color_op: u32, src_alpha: u32, dst_alpha: u32, alpha_op: u32, write_mask: u32,
}
#[repr(C)] struct PipelineColorBlendStateCreateInfo {
    s_type: u32, p_next: *const c_void, flags: u32, logic_op_enable: u32, logic_op: u32, attachment_count: u32,
    p_attachments: *const PipelineColorBlendAttachmentState, constants: [f32; 4],
}
#[repr(C)] struct PipelineDynamicStateCreateInfo { s_type: u32, p_next: *const c_void, flags: u32, count: u32, p_states: *const u32 }
#[repr(C)] struct GraphicsPipelineCreateInfo {
    s_type: u32, p_next: *const c_void, flags: u32, stage_count: u32, p_stages: *const PipelineShaderStageCreateInfo,
    p_vertex_input: *const PipelineVertexInputStateCreateInfo, p_input_assembly: *const PipelineInputAssemblyStateCreateInfo,
    p_tessellation: *const c_void, p_viewport: *const PipelineViewportStateCreateInfo, p_rasterization: *const PipelineRasterizationStateCreateInfo,
    p_multisample: *const c_void, p_depth_stencil: *const PipelineDepthStencilStateCreateInfo, p_color_blend: *const PipelineColorBlendStateCreateInfo,
    p_dynamic: *const PipelineDynamicStateCreateInfo, layout: H, render_pass: H, subpass: u32, base_pipeline: H, base_index: i32,
}
#[repr(C)] struct CommandBufferAllocateInfo { s_type: u32, p_next: *const c_void, pool: H, level: u32, count: u32 }
#[repr(C)] struct RenderPassBeginInfo { s_type: u32, p_next: *const c_void, render_pass: H, framebuffer: H, area: Rect2D, clear_count: u32, p_clears: *const [u32; 4] }
#[repr(C)] struct SubmitInfo {
    s_type: u32, p_next: *const c_void, wait_count: u32, p_wait: *const H, p_wait_stage: *const u32, cmd_count: u32, p_cmds: *const *mut c_void,
    signal_count: u32, p_signal: *const H,
}
#[repr(C)] struct SwapchainCreateInfo {
    s_type: u32, p_next: *const c_void, flags: u32, surface: H, min_images: u32, format: u32, color_space: u32, width: u32, height: u32,
    layers: u32, usage: u32, sharing: u32, qfi_count: u32, p_qfi: *const u32, pre_transform: u32, composite_alpha: u32, present_mode: u32, clipped: u32, old: H,
}
#[repr(C)] struct PresentInfo { s_type: u32, p_next: *const c_void, wait_count: u32, p_wait: *const H, swapchain_count: u32, p_swapchains: *const H, p_indices: *const u32, p_results: *mut i32 }
#[repr(C)] pub struct MemoryType { property_flags: u32, heap_index: u32 }
#[repr(C)] pub struct MemoryHeap { size: u64, flags: u32 }
#[repr(C)] pub struct PhysicalDeviceMemoryProperties { type_count: u32, types: [MemoryType; 32], heap_count: u32, heaps: [MemoryHeap; 16] }
#[repr(C)] pub struct QueueFamilyProperties { flags: u32, count: u32, timestamp_bits: u32, granularity: [u32; 3] }
#[repr(C)] pub struct PhysicalDeviceProperties {
    api_version: u32, driver_version: u32, vendor_id: u32, device_id: u32, device_type: u32, device_name: [u8; 256], uuid: [u8; 16],
    limits: [u32; 126], sparse: [u32; 5],
}

macro_rules! check_layout { ($t:ty, $f:ident, $o:expr) => { #[cfg(target_pointer_width = "32")] const _: () = assert!(core::mem::offset_of!($t, $f) == $o); }; }
check_layout!(BufferCreateInfo, size, 12);
check_layout!(BufferCreateInfo, usage, 20);
check_layout!(MemoryAllocateInfo, memory_type_index, 16);
check_layout!(GraphicsPipelineCreateInfo, layout, 56);
check_layout!(GraphicsPipelineCreateInfo, render_pass, 64);
check_layout!(RenderPassBeginInfo, clear_count, 40);
check_layout!(SwapchainCreateInfo, old, 76);
check_layout!(WriteDescriptorSet, p_buffer, 36);

fn slice<'a, T>(p: *const T, n: usize) -> &'a [T] {
    if p.is_null() || n == 0 { &[] } else { unsafe { core::slice::from_raw_parts(p, n) } }
}

// ---- objects --------------------------------------------------------------------------------------------------

struct Table<T> { items: Vec<Option<T>> }

impl<T> Table<T> {
    const fn new() -> Self { Table { items: Vec::new() } }
    fn insert(&mut self, v: T) -> H {
        for (i, s) in self.items.iter_mut().enumerate() {
            if s.is_none() { *s = Some(v); return i as H + 1; }
        }
        self.items.push(Some(v));
        self.items.len() as H
    }
    fn get(&mut self, h: H) -> Option<&mut T> {
        if h == 0 { return None; }
        self.items.get_mut(h as usize - 1).and_then(|s| s.as_mut())
    }
    fn remove(&mut self, h: H) { if h != 0 { if let Some(s) = self.items.get_mut(h as usize - 1) { *s = None; } } }
}

struct Memory { data: Vec<u8> }
struct Buffer { size: u64, mem: H, off: u64 }
struct Image { width: u32, height: u32, format: u32, mem: H, off: u64 }
struct View { image: H }
struct Shader { words: Vec<u32> }
struct Att { format: u32, load_op: u32 }
struct RenderPass { atts: Vec<Att>, color: Vec<u32>, depth: Option<u32> }
struct Framebuffer { views: Vec<H>, width: u32, height: u32 }
struct PipeLayout { push_size: u32 }
struct DescSet { entries: Vec<(u32, H, u64, u64)> }   // binding, buffer, offset, range
struct Swapchain { images: Vec<H>, mems: Vec<H>, width: u32, height: u32, next: u32, scale: u32 }

#[derive(Clone)]
enum Cmd {
    BeginRenderPass { rp: H, fb: H, area: [i32; 4], clears: Vec<[u32; 4]> },
    EndRenderPass,
    BindPipeline(H),
    BindVertexBuffers { first: u32, bufs: Vec<(H, u64)> },
    BindIndexBuffer { buf: H, off: u64, wide: bool },
    BindDescriptorSets { first: u32, sets: Vec<H> },
    PushConstants { off: u32, data: Vec<u8> },
    SetViewport(Viewport),
    SetScissor([i32; 4]),
    Draw { vertices: u32, instances: u32, first_vertex: u32, first_instance: u32 },
    DrawIndexed { indices: u32, instances: u32, first_index: u32, vertex_offset: i32, first_instance: u32 },
    CopyBuffer { src: H, dst: H, regions: Vec<[u64; 3]> },
    CopyBufferToImage { buf: H, img: H, regions: Vec<BufferImageCopy> },
    ClearAttachments { color: [u32; 4], rects: Vec<[i32; 4]> },
}

#[repr(C)] #[derive(Clone, Copy)]
struct BufferImageCopy {
    buffer_offset: u64, row_length: u32, image_height: u32,
    aspect: u32, mip: u32, base_layer: u32, layer_count: u32,
    ox: i32, oy: i32, oz: i32, w: u32, h: u32, d: u32,
}
const _: () = assert!(core::mem::size_of::<BufferImageCopy>() == 56);
#[repr(C)] struct ClearAttachment { aspect: u32, color_attachment: u32, value: [u32; 4] }
#[repr(C)] struct ClearRect { x: i32, y: i32, w: u32, h: u32, base_layer: u32, layer_count: u32 }

struct CmdBuf { cmds: Vec<Cmd>, recording: bool }

struct State {
    memories: Table<Memory>, buffers: Table<Buffer>, images: Table<Image>, views: Table<View>, shaders: Table<Shader>,
    rpasses: Table<RenderPass>, fbs: Table<Framebuffer>, pipe_layouts: Table<PipeLayout>, dsets: Table<DescSet>,
    pipes: Table<Box<Pipe>>, cmdbufs: Table<CmdBuf>, swapchains: Table<Swapchain>, misc: Table<u8>,
    display_taken: bool,
    present_scale: u32,
    scaled: Vec<u32>,
}

static mut STATE: Option<State> = None;

fn st() -> &'static mut State {
    unsafe {
        let s = &mut *core::ptr::addr_of_mut!(STATE);
        if s.is_none() {
            *s = Some(State {
                memories: Table::new(), buffers: Table::new(), images: Table::new(), views: Table::new(), shaders: Table::new(),
                rpasses: Table::new(), fbs: Table::new(), pipe_layouts: Table::new(), dsets: Table::new(), pipes: Table::new(),
                cmdbufs: Table::new(), swapchains: Table::new(), misc: Table::new(), display_taken: false,
                present_scale: 1, scaled: Vec::new(),
            });
        }
        s.as_mut().unwrap()
    }
}

fn dispatchable(id: H) -> *mut c_void { id as usize as *mut c_void }
fn from_dispatchable(p: *mut c_void) -> H { p as usize as H }

// ---- instance, devices, queues ----------------------------------------------------------------------------------

#[no_mangle] pub unsafe extern "C" fn vkEnumerateInstanceVersion(v: *mut u32) -> i32 { *v = (1 << 22) | (0 << 12); VK_SUCCESS }

#[no_mangle] pub unsafe extern "C" fn vkCreateInstance(_ci: *const c_void, _alloc: *const c_void, out: *mut *mut c_void) -> i32 {
    *out = dispatchable(1);
    VK_SUCCESS
}
#[no_mangle] pub unsafe extern "C" fn vkDestroyInstance(_i: *mut c_void, _a: *const c_void) {}

#[no_mangle] pub unsafe extern "C" fn vkEnumeratePhysicalDevices(_i: *mut c_void, count: *mut u32, devices: *mut *mut c_void) -> i32 {
    if devices.is_null() { *count = 1; return VK_SUCCESS; }
    if *count == 0 { return VK_INCOMPLETE; }
    *devices = dispatchable(1);
    *count = 1;
    VK_SUCCESS
}

#[no_mangle] pub unsafe extern "C" fn vkGetPhysicalDeviceProperties(_d: *mut c_void, p: *mut PhysicalDeviceProperties) {
    core::ptr::write_bytes(p as *mut u8, 0, core::mem::size_of::<PhysicalDeviceProperties>());
    let p = &mut *p;
    p.api_version = 1 << 22;
    p.driver_version = 1;
    p.vendor_id = 0x1234;
    p.device_id = 0x0001;
    p.device_type = 4;   // VK_PHYSICAL_DEVICE_TYPE_CPU
    let name = b"RadiumOS software rasterizer";
    p.device_name[..name.len()].copy_from_slice(name);
    // VkPhysicalDeviceLimits, the first members in their declared order
    let l = &mut p.limits;
    l[0] = 4096; l[1] = 4096; l[2] = 256; l[3] = 4096; l[4] = 256; l[5] = 65536;
    l[6] = 65536; l[7] = 1 << 27; l[8] = 128;
}

#[no_mangle] pub unsafe extern "C" fn vkGetPhysicalDeviceQueueFamilyProperties(_d: *mut c_void, count: *mut u32, props: *mut QueueFamilyProperties) {
    if props.is_null() { *count = 1; return; }
    if *count == 0 { return; }
    *props = QueueFamilyProperties { flags: 0x1 | 0x2 | 0x4, count: 1, timestamp_bits: 0, granularity: [1, 1, 1] };
    *count = 1;
}

#[no_mangle] pub unsafe extern "C" fn vkGetPhysicalDeviceMemoryProperties(_d: *mut c_void, p: *mut PhysicalDeviceMemoryProperties) {
    core::ptr::write_bytes(p as *mut u8, 0, core::mem::size_of::<PhysicalDeviceMemoryProperties>());
    let p = &mut *p;
    p.type_count = 1;
    p.types[0] = MemoryType { property_flags: 0x1 | 0x2 | 0x4, heap_index: 0 };
    p.heap_count = 1;
    p.heaps[0] = MemoryHeap { size: 64 << 20, flags: 1 };
}

#[no_mangle] pub unsafe extern "C" fn vkCreateDevice(_d: *mut c_void, _ci: *const c_void, _a: *const c_void, out: *mut *mut c_void) -> i32 {
    *out = dispatchable(1);
    VK_SUCCESS
}
#[no_mangle] pub unsafe extern "C" fn vkDestroyDevice(_d: *mut c_void, _a: *const c_void) {}
#[no_mangle] pub unsafe extern "C" fn vkGetDeviceQueue(_d: *mut c_void, _family: u32, _index: u32, out: *mut *mut c_void) { *out = dispatchable(1); }
#[no_mangle] pub unsafe extern "C" fn vkDeviceWaitIdle(_d: *mut c_void) -> i32 { VK_SUCCESS }
#[no_mangle] pub unsafe extern "C" fn vkQueueWaitIdle(_q: *mut c_void) -> i32 { VK_SUCCESS }

// ---- memory, buffers, images ------------------------------------------------------------------------------------

#[no_mangle] pub unsafe extern "C" fn vkAllocateMemory(_d: *mut c_void, ci: *const MemoryAllocateInfo, _a: *const c_void, out: *mut H) -> i32 {
    let size = (*ci).size as usize;
    if size == 0 || size > 48 << 20 { return VK_ERROR_OUT_OF_HOST_MEMORY; }
    let mut data = Vec::new();
    if data.try_reserve_exact(size + 16).is_err() { return VK_ERROR_OUT_OF_HOST_MEMORY; }
    data.resize(size + 16, 0u8);
    *out = st().memories.insert(Memory { data });
    VK_SUCCESS
}
#[no_mangle] pub unsafe extern "C" fn vkFreeMemory(_d: *mut c_void, m: H, _a: *const c_void) { st().memories.remove(m); }

#[no_mangle] pub unsafe extern "C" fn vkMapMemory(_d: *mut c_void, m: H, offset: u64, _size: u64, _flags: u32, out: *mut *mut c_void) -> i32 {
    match st().memories.get(m) {
        Some(mem) if (offset as usize) < mem.data.len() => { *out = mem.data.as_mut_ptr().add(offset as usize) as *mut c_void; VK_SUCCESS }
        _ => VK_ERROR_MEMORY_MAP_FAILED,
    }
}
#[no_mangle] pub unsafe extern "C" fn vkUnmapMemory(_d: *mut c_void, _m: H) {}
#[no_mangle] pub unsafe extern "C" fn vkFlushMappedMemoryRanges(_d: *mut c_void, _n: u32, _r: *const c_void) -> i32 { VK_SUCCESS }

#[no_mangle] pub unsafe extern "C" fn vkCreateBuffer(_d: *mut c_void, ci: *const BufferCreateInfo, _a: *const c_void, out: *mut H) -> i32 {
    *out = st().buffers.insert(Buffer { size: (*ci).size, mem: 0, off: 0 });
    VK_SUCCESS
}
#[no_mangle] pub unsafe extern "C" fn vkDestroyBuffer(_d: *mut c_void, b: H, _a: *const c_void) { st().buffers.remove(b); }
#[no_mangle] pub unsafe extern "C" fn vkGetBufferMemoryRequirements(_d: *mut c_void, b: H, out: *mut MemoryRequirements) {
    let size = st().buffers.get(b).map_or(0, |b| b.size);
    *out = MemoryRequirements { size: (size + 15) & !15, alignment: 16, memory_type_bits: 1 };
}
#[no_mangle] pub unsafe extern "C" fn vkBindBufferMemory(_d: *mut c_void, b: H, m: H, off: u64) -> i32 {
    match st().buffers.get(b) { Some(buf) => { buf.mem = m; buf.off = off; VK_SUCCESS } None => VK_ERROR_UNKNOWN }
}

fn format_ok(f: u32) -> bool { matches!(f, 37 | 43 | 44 | 50 | 126) }

#[no_mangle] pub unsafe extern "C" fn vkCreateImage(_d: *mut c_void, ci: *const ImageCreateInfo, _a: *const c_void, out: *mut H) -> i32 {
    let ci = &*ci;
    if !format_ok(ci.format) || ci.image_type != 1 || ci.depth != 1 || ci.mip_levels != 1 || ci.array_layers != 1 { return VK_ERROR_FORMAT_NOT_SUPPORTED; }
    *out = st().images.insert(Image { width: ci.width, height: ci.height, format: ci.format, mem: 0, off: 0 });
    VK_SUCCESS
}
#[no_mangle] pub unsafe extern "C" fn vkDestroyImage(_d: *mut c_void, i: H, _a: *const c_void) { st().images.remove(i); }
#[no_mangle] pub unsafe extern "C" fn vkGetImageMemoryRequirements(_d: *mut c_void, i: H, out: *mut MemoryRequirements) {
    let size = st().images.get(i).map_or(0, |i| i.width as u64 * i.height as u64 * 4);
    *out = MemoryRequirements { size, alignment: 16, memory_type_bits: 1 };
}
#[no_mangle] pub unsafe extern "C" fn vkBindImageMemory(_d: *mut c_void, i: H, m: H, off: u64) -> i32 {
    match st().images.get(i) { Some(img) => { img.mem = m; img.off = off; VK_SUCCESS } None => VK_ERROR_UNKNOWN }
}
#[no_mangle] pub unsafe extern "C" fn vkCreateImageView(_d: *mut c_void, ci: *const ImageViewCreateInfo, _a: *const c_void, out: *mut H) -> i32 {
    *out = st().views.insert(View { image: (*ci).image });
    VK_SUCCESS
}
#[no_mangle] pub unsafe extern "C" fn vkDestroyImageView(_d: *mut c_void, v: H, _a: *const c_void) { st().views.remove(v); }

// ---- shaders, render passes, layouts, descriptors, pipelines -----------------------------------------------------

#[no_mangle] pub unsafe extern "C" fn vkCreateShaderModule(_d: *mut c_void, ci: *const ShaderModuleCreateInfo, _a: *const c_void, out: *mut H) -> i32 {
    let ci = &*ci;
    let words = core::slice::from_raw_parts(ci.p_code, ci.code_size / 4).to_vec();
    if Module::parse(&words).is_err() { return VK_ERROR_FEATURE_NOT_PRESENT; }
    *out = st().shaders.insert(Shader { words });
    VK_SUCCESS
}
#[no_mangle] pub unsafe extern "C" fn vkDestroyShaderModule(_d: *mut c_void, s: H, _a: *const c_void) { st().shaders.remove(s); }

#[no_mangle] pub unsafe extern "C" fn vkCreateRenderPass(_d: *mut c_void, ci: *const RenderPassCreateInfo, _a: *const c_void, out: *mut H) -> i32 {
    let ci = &*ci;
    if ci.subpass_count != 1 { return VK_ERROR_FEATURE_NOT_PRESENT; }
    let atts = core::slice::from_raw_parts(ci.p_attachments, ci.attachment_count as usize)
        .iter().map(|a| Att { format: a.format, load_op: a.load_op }).collect();
    let sp = &*ci.p_subpasses;
    let color = if sp.color_count > 0 { core::slice::from_raw_parts(sp.p_color, sp.color_count as usize).iter().map(|r| r.attachment).collect() } else { Vec::new() };
    let depth = if sp.p_depth.is_null() || (*sp.p_depth).attachment == u32::MAX { None } else { Some((*sp.p_depth).attachment) };
    *out = st().rpasses.insert(RenderPass { atts, color, depth });
    VK_SUCCESS
}
#[no_mangle] pub unsafe extern "C" fn vkDestroyRenderPass(_d: *mut c_void, r: H, _a: *const c_void) { st().rpasses.remove(r); }

#[no_mangle] pub unsafe extern "C" fn vkCreateFramebuffer(_d: *mut c_void, ci: *const FramebufferCreateInfo, _a: *const c_void, out: *mut H) -> i32 {
    let ci = &*ci;
    let views = core::slice::from_raw_parts(ci.p_attachments, ci.attachment_count as usize).to_vec();
    *out = st().fbs.insert(Framebuffer { views, width: ci.width, height: ci.height });
    VK_SUCCESS
}
#[no_mangle] pub unsafe extern "C" fn vkDestroyFramebuffer(_d: *mut c_void, f: H, _a: *const c_void) { st().fbs.remove(f); }

#[no_mangle] pub unsafe extern "C" fn vkCreateDescriptorSetLayout(_d: *mut c_void, _ci: *const DescriptorSetLayoutCreateInfo, _a: *const c_void, out: *mut H) -> i32 {
    *out = st().misc.insert(0);
    VK_SUCCESS
}
#[no_mangle] pub unsafe extern "C" fn vkDestroyDescriptorSetLayout(_d: *mut c_void, l: H, _a: *const c_void) { st().misc.remove(l); }

#[no_mangle] pub unsafe extern "C" fn vkCreatePipelineLayout(_d: *mut c_void, ci: *const PipelineLayoutCreateInfo, _a: *const c_void, out: *mut H) -> i32 {
    let ci = &*ci;
    let mut push = 0;
    if ci.push_range_count > 0 {
        for r in core::slice::from_raw_parts(ci.p_push_ranges, ci.push_range_count as usize) { push = push.max(r.offset + r.size); }
    }
    *out = st().pipe_layouts.insert(PipeLayout { push_size: push });
    VK_SUCCESS
}
#[no_mangle] pub unsafe extern "C" fn vkDestroyPipelineLayout(_d: *mut c_void, l: H, _a: *const c_void) { st().pipe_layouts.remove(l); }

#[no_mangle] pub unsafe extern "C" fn vkCreateDescriptorPool(_d: *mut c_void, _ci: *const DescriptorPoolCreateInfo, _a: *const c_void, out: *mut H) -> i32 {
    *out = st().misc.insert(0);
    VK_SUCCESS
}
#[no_mangle] pub unsafe extern "C" fn vkDestroyDescriptorPool(_d: *mut c_void, p: H, _a: *const c_void) { st().misc.remove(p); }

#[no_mangle] pub unsafe extern "C" fn vkAllocateDescriptorSets(_d: *mut c_void, ai: *const DescriptorSetAllocateInfo, out: *mut H) -> i32 {
    for k in 0..(*ai).count as usize { *out.add(k) = st().dsets.insert(DescSet { entries: Vec::new() }); }
    VK_SUCCESS
}

#[no_mangle] pub unsafe extern "C" fn vkUpdateDescriptorSets(_d: *mut c_void, write_count: u32, writes: *const WriteDescriptorSet, _copy_count: u32, _copies: *const c_void) {
    for w in core::slice::from_raw_parts(writes, write_count as usize) {
        if !matches!(w.descriptor_type, 6 | 7 | 8 | 9) || w.p_buffer.is_null() { continue; }
        let Some(set) = st().dsets.get(w.dst_set) else { continue };
        for k in 0..w.count as usize {
            let info = &*w.p_buffer.add(k);
            let binding = w.dst_binding + k as u32;
            set.entries.retain(|e| e.0 != binding);
            set.entries.push((binding, info.buffer, info.offset, info.range));
        }
    }
}

#[no_mangle] pub unsafe extern "C" fn vkCreateGraphicsPipelines(_d: *mut c_void, _cache: H, count: u32, cis: *const GraphicsPipelineCreateInfo, _a: *const c_void, out: *mut H) -> i32 {
    for n in 0..count as usize {
        let ci = &*cis.add(n);
        let mut vs: Option<Module> = None;
        let mut fs: Option<Module> = None;
        for s in core::slice::from_raw_parts(ci.p_stages, ci.stage_count as usize) {
            let Some(sh) = st().shaders.get(s.module) else { return VK_ERROR_UNKNOWN };
            let Ok(mut m) = Module::parse(&sh.words) else { return VK_ERROR_FEATURE_NOT_PRESENT };
            let mut name = Vec::new();
            let mut at = 0;
            while *s.p_name.add(at) != 0 { name.push(*s.p_name.add(at)); at += 1; }
            let model = match s.stage { 0x1 => 0, 0x10 => 4, _ => return VK_ERROR_FEATURE_NOT_PRESENT };
            if !m.select_entry(&name, model) { return VK_ERROR_FEATURE_NOT_PRESENT; }
            if model == 0 { if !m.has_position() { return VK_ERROR_FEATURE_NOT_PRESENT; } vs = Some(m); } else { fs = Some(m); }
        }
        let (Some(vs), Some(fs)) = (vs, fs) else { return VK_ERROR_FEATURE_NOT_PRESENT };

        let mut attrs = Vec::new();
        let mut bindings = [VertexBinding::default(); MAX_VERTEX_BINDINGS];
        if !ci.p_vertex_input.is_null() {
            let vi = &*ci.p_vertex_input;
            for b in slice(vi.p_bindings, vi.binding_count as usize) {
                if (b.binding as usize) < MAX_VERTEX_BINDINGS { bindings[b.binding as usize] = VertexBinding { stride: b.stride, per_instance: b.input_rate == 1 }; }
            }
            for a in slice(vi.p_attributes, vi.attribute_count as usize) {
                attrs.push(Attribute { location: a.location, binding: a.binding, format: a.format, offset: a.offset });
            }
        }
        let topology = if ci.p_input_assembly.is_null() { 3 } else { (*ci.p_input_assembly).topology };
        if !matches!(topology, 3 | 4 | 5) { return VK_ERROR_FEATURE_NOT_PRESENT; }

        let (mut cull, mut front_ccw) = (0, true);
        if !ci.p_rasterization.is_null() {
            let r = &*ci.p_rasterization;
            if r.polygon_mode != 0 { return VK_ERROR_FEATURE_NOT_PRESENT; }
            cull = r.cull_mode;
            front_ccw = r.front_face == 0;
        }
        let (mut depth_test, mut depth_write, mut depth_op) = (false, false, 1);
        if !ci.p_depth_stencil.is_null() {
            let ds = &*ci.p_depth_stencil;
            depth_test = ds.depth_test != 0; depth_write = ds.depth_write != 0; depth_op = ds.depth_op;
        }
        let mut blend = Blend { enable: false, src_color: 1, dst_color: 0, color_op: 0, src_alpha: 1, dst_alpha: 0, alpha_op: 0, write_mask: 0xF };
        if !ci.p_color_blend.is_null() && (*ci.p_color_blend).attachment_count > 0 {
            let a = &*(*ci.p_color_blend).p_attachments;
            blend = Blend { enable: a.enable != 0, src_color: a.src_color, dst_color: a.dst_color, color_op: a.color_op, src_alpha: a.src_alpha, dst_alpha: a.dst_alpha, alpha_op: a.alpha_op, write_mask: a.write_mask };
        }
        let (mut dyn_viewport, mut dyn_scissor) = (false, false);
        if !ci.p_dynamic.is_null() {
            for s in core::slice::from_raw_parts((*ci.p_dynamic).p_states, (*ci.p_dynamic).count as usize) {
                if *s == 0 { dyn_viewport = true; } else if *s == 1 { dyn_scissor = true; }
            }
        }
        let (mut viewport, mut scissor) = (None, None);
        if !ci.p_viewport.is_null() {
            let v = &*ci.p_viewport;
            if v.viewport_count > 0 && !v.p_viewports.is_null() { let p = &*v.p_viewports; viewport = Some([p.x, p.y, p.width, p.height, p.min_depth, p.max_depth]); }
            if v.scissor_count > 0 && !v.p_scissors.is_null() { let s = &*v.p_scissors; scissor = Some([s.offset.x, s.offset.y, s.extent.width as i32, s.extent.height as i32]); }
        }
        let vsm = Machine::new(&vs, 1);
        let fsm = Machine::new(&fs, if fs.straight { 64 } else { 1 });
        let pipe = Pipe { vs, fs, vsm, fsm, attrs, bindings, topology, cull, front_ccw, depth_test, depth_write, depth_op, blend, viewport, scissor, dyn_viewport, dyn_scissor };
        *out.add(n) = st().pipes.insert(Box::new(pipe));
    }
    VK_SUCCESS
}
#[no_mangle] pub unsafe extern "C" fn vkDestroyPipeline(_d: *mut c_void, p: H, _a: *const c_void) { st().pipes.remove(p); }

// ---- sync objects (everything runs synchronously) ----------------------------------------------------------------

#[no_mangle] pub unsafe extern "C" fn vkCreateFence(_d: *mut c_void, _ci: *const c_void, _a: *const c_void, out: *mut H) -> i32 { *out = st().misc.insert(0); VK_SUCCESS }
#[no_mangle] pub unsafe extern "C" fn vkDestroyFence(_d: *mut c_void, f: H, _a: *const c_void) { st().misc.remove(f); }
#[no_mangle] pub unsafe extern "C" fn vkWaitForFences(_d: *mut c_void, _n: u32, _f: *const H, _all: u32, _t: u64) -> i32 { VK_SUCCESS }
#[no_mangle] pub unsafe extern "C" fn vkResetFences(_d: *mut c_void, _n: u32, _f: *const H) -> i32 { VK_SUCCESS }
#[no_mangle] pub unsafe extern "C" fn vkGetFenceStatus(_d: *mut c_void, _f: H) -> i32 { VK_SUCCESS }
#[no_mangle] pub unsafe extern "C" fn vkCreateSemaphore(_d: *mut c_void, _ci: *const c_void, _a: *const c_void, out: *mut H) -> i32 { *out = st().misc.insert(0); VK_SUCCESS }
#[no_mangle] pub unsafe extern "C" fn vkDestroySemaphore(_d: *mut c_void, s: H, _a: *const c_void) { st().misc.remove(s); }

// ---- command buffers ---------------------------------------------------------------------------------------------

#[no_mangle] pub unsafe extern "C" fn vkCreateCommandPool(_d: *mut c_void, _ci: *const c_void, _a: *const c_void, out: *mut H) -> i32 { *out = st().misc.insert(0); VK_SUCCESS }
#[no_mangle] pub unsafe extern "C" fn vkDestroyCommandPool(_d: *mut c_void, p: H, _a: *const c_void) { st().misc.remove(p); }
#[no_mangle] pub unsafe extern "C" fn vkResetCommandPool(_d: *mut c_void, _p: H, _f: u32) -> i32 { VK_SUCCESS }

#[no_mangle] pub unsafe extern "C" fn vkAllocateCommandBuffers(_d: *mut c_void, ai: *const CommandBufferAllocateInfo, out: *mut *mut c_void) -> i32 {
    for k in 0..(*ai).count as usize { *out.add(k) = dispatchable(st().cmdbufs.insert(CmdBuf { cmds: Vec::new(), recording: false })); }
    VK_SUCCESS
}
#[no_mangle] pub unsafe extern "C" fn vkFreeCommandBuffers(_d: *mut c_void, _p: H, n: u32, cbs: *const *mut c_void) {
    for k in 0..n as usize { st().cmdbufs.remove(from_dispatchable(*cbs.add(k))); }
}
#[no_mangle] pub unsafe extern "C" fn vkBeginCommandBuffer(cb: *mut c_void, _bi: *const c_void) -> i32 {
    match st().cmdbufs.get(from_dispatchable(cb)) { Some(c) => { c.cmds.clear(); c.recording = true; VK_SUCCESS } None => VK_ERROR_UNKNOWN }
}
#[no_mangle] pub unsafe extern "C" fn vkEndCommandBuffer(cb: *mut c_void) -> i32 {
    match st().cmdbufs.get(from_dispatchable(cb)) { Some(c) => { c.recording = false; VK_SUCCESS } None => VK_ERROR_UNKNOWN }
}
#[no_mangle] pub unsafe extern "C" fn vkResetCommandBuffer(cb: *mut c_void, _f: u32) -> i32 {
    match st().cmdbufs.get(from_dispatchable(cb)) { Some(c) => { c.cmds.clear(); VK_SUCCESS } None => VK_ERROR_UNKNOWN }
}

fn record(cb: *mut c_void, cmd: Cmd) {
    if let Some(c) = st().cmdbufs.get(from_dispatchable(cb)) { if c.recording { c.cmds.push(cmd); } }
}

#[no_mangle] pub unsafe extern "C" fn vkCmdBeginRenderPass(cb: *mut c_void, bi: *const RenderPassBeginInfo, _contents: u32) {
    let b = &*bi;
    let clears = if b.clear_count > 0 { core::slice::from_raw_parts(b.p_clears, b.clear_count as usize).to_vec() } else { Vec::new() };
    record(cb, Cmd::BeginRenderPass { rp: b.render_pass, fb: b.framebuffer, area: [b.area.offset.x, b.area.offset.y, b.area.extent.width as i32, b.area.extent.height as i32], clears });
}
#[no_mangle] pub unsafe extern "C" fn vkCmdEndRenderPass(cb: *mut c_void) { record(cb, Cmd::EndRenderPass); }
#[no_mangle] pub unsafe extern "C" fn vkCmdBindPipeline(cb: *mut c_void, _bp: u32, p: H) { record(cb, Cmd::BindPipeline(p)); }
#[no_mangle] pub unsafe extern "C" fn vkCmdBindVertexBuffers(cb: *mut c_void, first: u32, count: u32, bufs: *const H, offs: *const u64) {
    let v = (0..count as usize).map(|k| (*bufs.add(k), *offs.add(k))).collect();
    record(cb, Cmd::BindVertexBuffers { first, bufs: v });
}
#[no_mangle] pub unsafe extern "C" fn vkCmdBindIndexBuffer(cb: *mut c_void, buf: H, off: u64, index_type: u32) { record(cb, Cmd::BindIndexBuffer { buf, off, wide: index_type == 1 }); }
#[no_mangle] pub unsafe extern "C" fn vkCmdBindDescriptorSets(cb: *mut c_void, _bp: u32, _layout: H, first: u32, count: u32, sets: *const H, _dyn_count: u32, _dyn_off: *const u32) {
    record(cb, Cmd::BindDescriptorSets { first, sets: core::slice::from_raw_parts(sets, count as usize).to_vec() });
}
#[no_mangle] pub unsafe extern "C" fn vkCmdPushConstants(cb: *mut c_void, _layout: H, _stages: u32, off: u32, size: u32, data: *const u8) {
    record(cb, Cmd::PushConstants { off, data: core::slice::from_raw_parts(data, size as usize).to_vec() });
}
#[no_mangle] pub unsafe extern "C" fn vkCmdSetViewport(cb: *mut c_void, _first: u32, _count: u32, v: *const Viewport) { record(cb, Cmd::SetViewport(*v)); }
#[no_mangle] pub unsafe extern "C" fn vkCmdSetScissor(cb: *mut c_void, _first: u32, _count: u32, r: *const Rect2D) {
    let r = &*r;
    record(cb, Cmd::SetScissor([r.offset.x, r.offset.y, r.extent.width as i32, r.extent.height as i32]));
}
#[no_mangle] pub unsafe extern "C" fn vkCmdDraw(cb: *mut c_void, vertices: u32, instances: u32, first_vertex: u32, first_instance: u32) {
    record(cb, Cmd::Draw { vertices, instances, first_vertex, first_instance });
}
#[no_mangle] pub unsafe extern "C" fn vkCmdDrawIndexed(cb: *mut c_void, indices: u32, instances: u32, first_index: u32, vertex_offset: i32, first_instance: u32) {
    record(cb, Cmd::DrawIndexed { indices, instances, first_index, vertex_offset, first_instance });
}
#[no_mangle] pub unsafe extern "C" fn vkCmdCopyBuffer(cb: *mut c_void, src: H, dst: H, count: u32, regions: *const [u64; 3]) {
    record(cb, Cmd::CopyBuffer { src, dst, regions: core::slice::from_raw_parts(regions, count as usize).to_vec() });
}

/// Row-by-row copy of buffer memory into an image (any 4-byte format; the texels are copied as they are).
#[no_mangle] pub unsafe extern "C" fn vkCmdCopyBufferToImage(cb: *mut c_void, buf: H, img: H, _layout: u32, count: u32, regions: *const BufferImageCopy) {
    record(cb, Cmd::CopyBufferToImage { buf, img, regions: core::slice::from_raw_parts(regions, count as usize).to_vec() });
}

/// Fills rectangles of the first colour attachment of the current render pass.
#[no_mangle] pub unsafe extern "C" fn vkCmdClearAttachments(cb: *mut c_void, count: u32, atts: *const ClearAttachment, rect_count: u32, rects: *const ClearRect) {
    let mut color = None;
    for a in core::slice::from_raw_parts(atts, count as usize) { if a.aspect & 1 != 0 { color = Some(a.value); break; } }
    let Some(color) = color else { return };
    let rects = core::slice::from_raw_parts(rects, rect_count as usize).iter().map(|r| [r.x, r.y, r.w as i32, r.h as i32]).collect();
    record(cb, Cmd::ClearAttachments { color, rects });
}

// ---- execution ----------------------------------------------------------------------------------------------------

fn buffer_view(buf: H, offset: u64) -> VBuf {
    let s = st();
    let Some(b) = s.buffers.get(buf) else { return VBuf { ptr: core::ptr::null(), len: 0 } };
    let (mem, off, size) = (b.mem, b.off, b.size);
    let Some(m) = s.memories.get(mem) else { return VBuf { ptr: core::ptr::null(), len: 0 } };
    let start = (off + offset) as usize;
    if start > m.data.len() || offset > size { return VBuf { ptr: core::ptr::null(), len: 0 }; }
    VBuf { ptr: unsafe { m.data.as_ptr().add(start) }, len: (size - offset) as usize }
}

fn image_target(view: H) -> Option<(*mut u8, u32, u32, u32)> {
    let s = st();
    let image = s.views.get(view)?.image;
    let img = s.images.get(image)?;
    let (w, h, fmt, mem, off) = (img.width, img.height, img.format, img.mem, img.off);
    let m = s.memories.get(mem)?;
    if off as usize + (w * h * 4) as usize > m.data.len() { return None; }
    Some((unsafe { m.data.as_mut_ptr().add(off as usize) }, w, h, fmt))
}

#[derive(Default)]
struct Exec {
    pipe: H,
    vbs: [(H, u64); MAX_VERTEX_BINDINGS],
    ib: Option<(H, u64, bool)>,
    sets: [H; 4],
    push: Vec<u8>,
    viewport: Option<[f32; 6]>,
    scissor: Option<[i32; 4]>,
    rp: H,
    fb: H,
    area: [i32; 4],
}

fn execute(cmds: &[Cmd]) {
    let mut e = Exec::default();
    e.push = vec![0u8; 128];
    for c in cmds {
        match c {
            Cmd::BeginRenderPass { rp, fb, area, clears } => {
                e.rp = *rp; e.fb = *fb; e.area = *area;
                let s = st();
                let Some(pass) = s.rpasses.get(*rp) else { continue };
                let loads: Vec<(u32, u32)> = pass.atts.iter().map(|a| (a.format, a.load_op)).collect();
                let Some(frame) = s.fbs.get(*fb) else { continue };
                let views = frame.views.clone();
                for (k, view) in views.iter().enumerate() {
                    let Some((ptr, w, h, fmt)) = image_target(*view) else { continue };
                    if loads.get(k).map_or(true, |l| l.1 != 1) { continue; }
                    let clear = clears.get(k).copied().unwrap_or([0; 4]);
                    let t = Target { color: ptr as *mut u32, depth: if fmt == 126 { ptr as *mut f32 } else { core::ptr::null_mut() }, width: w as i32, height: h as i32, swap_rb: matches!(fmt, 37 | 43), depth_w: w as i32 };
                    let rect = [area[0], area[1], area[2], area[3]];
                    if fmt == 126 { raster::clear_depth(&t, rect, f32::from_bits(clear[0])); }
                    else { raster::clear_color(&t, rect, [f32::from_bits(clear[0]), f32::from_bits(clear[1]), f32::from_bits(clear[2]), f32::from_bits(clear[3])]); }
                }
            }
            Cmd::EndRenderPass => { e.rp = 0; e.fb = 0; }
            Cmd::BindPipeline(p) => e.pipe = *p,
            Cmd::BindVertexBuffers { first, bufs } => {
                for (k, b) in bufs.iter().enumerate() { if (*first as usize + k) < MAX_VERTEX_BINDINGS { e.vbs[*first as usize + k] = *b; } }
            }
            Cmd::BindIndexBuffer { buf, off, wide } => e.ib = Some((*buf, *off, *wide)),
            Cmd::BindDescriptorSets { first, sets } => {
                for (k, s) in sets.iter().enumerate() { if (*first as usize + k) < 4 { e.sets[*first as usize + k] = *s; } }
            }
            Cmd::PushConstants { off, data } => {
                let (o, n) = (*off as usize, data.len());
                if o + n <= 128 { e.push[o..o + n].copy_from_slice(data); }
            }
            Cmd::SetViewport(v) => e.viewport = Some([v.x, v.y, v.width, v.height, v.min_depth, v.max_depth]),
            Cmd::SetScissor(r) => e.scissor = Some(*r),
            Cmd::Draw { vertices, instances, first_vertex, first_instance } => {
                do_draw(&e, *vertices, *instances, *first_vertex, *first_instance, None);
            }
            Cmd::DrawIndexed { indices, instances, first_index, vertex_offset, first_instance } => {
                let Some((buf, off, wide)) = e.ib else { continue };
                let view = buffer_view(buf, off);
                do_draw(&e, *indices, *instances, 0, *first_instance, Some((view, wide, *first_index, *vertex_offset)));
            }
            Cmd::CopyBuffer { src, dst, regions } => {
                for r in regions {
                    let (s, d) = (buffer_view(*src, r[0]), buffer_view(*dst, r[1]));
                    let n = (r[2] as usize).min(s.len).min(d.len);
                    if n > 0 { unsafe { core::ptr::copy(s.ptr, d.ptr as *mut u8, n); } }
                }
            }
            Cmd::CopyBufferToImage { buf, img, regions } => {
                let s = st();
                let Some(image) = s.images.get(*img) else { continue };
                let (iw, ih, imem, ioff) = (image.width as i64, image.height as i64, image.mem, image.off);
                let Some(m) = s.memories.get(imem) else { continue };
                if ioff as usize + (iw * ih * 4) as usize > m.data.len() { continue; }
                let dst = unsafe { m.data.as_mut_ptr().add(ioff as usize) };
                for r in regions {
                    let src = buffer_view(*buf, r.buffer_offset);
                    let row_len = if r.row_length == 0 { r.w as i64 } else { r.row_length as i64 };
                    let (x0, y0) = ((r.ox as i64).max(0), (r.oy as i64).max(0));
                    let (cx, cy) = (x0 - r.ox as i64, y0 - r.oy as i64);
                    let w = (r.w as i64 - cx).min(iw - x0);
                    let h = (r.h as i64 - cy).min(ih - y0);
                    if w <= 0 || h <= 0 || src.ptr.is_null() { continue; }
                    for row in 0..h {
                        let so = ((cy + row) * row_len + cx) * 4;
                        if (so + w * 4) as usize > src.len { break; }
                        unsafe { core::ptr::copy_nonoverlapping(src.ptr.add(so as usize), dst.add((((y0 + row) * iw + x0) * 4) as usize), (w * 4) as usize); }
                    }
                }
            }
            Cmd::ClearAttachments { color, rects } => {
                let s = st();
                let Some(frame) = s.fbs.get(e.fb) else { continue };
                let Some(view) = frame.views.first().copied() else { continue };
                let Some((ptr, w, h, fmt)) = image_target(view) else { continue };
                let t = Target { color: ptr as *mut u32, depth: core::ptr::null_mut(), width: w as i32, height: h as i32, swap_rb: matches!(fmt, 37 | 43), depth_w: w as i32 };
                let c = [f32::from_bits(color[0]), f32::from_bits(color[1]), f32::from_bits(color[2]), f32::from_bits(color[3])];
                for r in rects { raster::clear_color(&t, *r, c); }
            }
        }
    }
}

fn do_draw(e: &Exec, count: u32, instances: u32, first_vertex: u32, first_instance: u32, index: Option<(VBuf, bool, u32, i32)>) {
    let s = st();
    let Some(pass) = s.rpasses.get(e.rp) else { return };
    let (color_idx, depth_idx) = (pass.color.first().copied(), pass.depth);
    let Some(frame) = s.fbs.get(e.fb) else { return };
    let views = frame.views.clone();
    let Some(ci) = color_idx else { return };
    let Some((cptr, w, h, fmt)) = views.get(ci as usize).and_then(|v| image_target(*v)) else { return };
    let mut depth_ptr: *mut f32 = core::ptr::null_mut();
    let mut depth_w = w;
    if let Some(di) = depth_idx {
        if let Some((dptr, dw, _dh, _)) = views.get(di as usize).and_then(|v| image_target(*v)) { depth_ptr = dptr as *mut f32; depth_w = dw; }
    }
    let target = Target { color: cptr as *mut u32, depth: depth_ptr, width: w as i32, height: h as i32, swap_rb: matches!(fmt, 37 | 43), depth_w: depth_w as i32 };

    let pipe: *mut Pipe = match s.pipes.get(e.pipe) { Some(p) => &mut **p as *mut Pipe, None => return };
    let pipe = unsafe { &mut *pipe };

    let mut vbufs = [(VBuf { ptr: core::ptr::null(), len: 0 }, 0u32); MAX_VERTEX_BINDINGS];
    for k in 0..MAX_VERTEX_BINDINGS { if e.vbs[k].0 != 0 { vbufs[k].0 = buffer_view(e.vbs[k].0, e.vbs[k].1); } }

    // descriptor sets and push constants for both stages
    for (module, machine) in [(&pipe.vs, &mut pipe.vsm), (&pipe.fs, &mut pipe.fsm)] {
        for b in module.bindings.iter() {
            let mut view = (core::ptr::null::<u8>(), 0usize);
            if let Some(set) = st().dsets.get(e.sets.get(b.set as usize).copied().unwrap_or(0)) {
                if let Some(entry) = set.entries.iter().find(|x| x.0 == b.binding).copied() {
                    let v = buffer_view(entry.1, entry.2);
                    view = (v.ptr, if entry.3 == u64::MAX { v.len } else { v.len.min(entry.3 as usize) });
                }
            }
            machine.ext[b.slot as usize] = view;
        }
        machine.push = (e.push.as_ptr(), e.push.len());
    }

    let viewport = e.viewport.filter(|_| pipe.dyn_viewport).or(pipe.viewport).unwrap_or([0.0, 0.0, w as f32, h as f32, 0.0, 1.0]);
    let scissor = e.scissor.filter(|_| pipe.dyn_scissor).or(pipe.scissor).unwrap_or([0, 0, w as i32, h as i32]);
    let draw = Draw { first_vertex, vertex_count: count, first_instance, instance_count: instances, index, vbufs: &vbufs, viewport, scissor };
    raster::draw(pipe, &target, &draw);
}

#[no_mangle] pub unsafe extern "C" fn vkQueueSubmit(_q: *mut c_void, count: u32, submits: *const SubmitInfo, _fence: H) -> i32 {
    for s in core::slice::from_raw_parts(submits, count as usize) {
        for k in 0..s.cmd_count as usize {
            let id = from_dispatchable(*s.p_cmds.add(k));
            let cmds = match st().cmdbufs.get(id) { Some(c) => c.cmds.clone(), None => return VK_ERROR_UNKNOWN };
            execute(&cmds);
        }
    }
    VK_SUCCESS
}

// ---- surface, swapchain, present ------------------------------------------------------------------------------------

#[repr(C)] pub struct RadiumSurfaceCreateInfo { s_type: u32, p_next: *const c_void, flags: u32 }

/// The screen as a surface: takes the display from the text console until the surface is destroyed. `flags` & 0xF is an
/// integer factor (1 to 4) by which every presented image is enlarged (nearest neighbour), so a small swapchain can fill the screen.
#[no_mangle] pub unsafe extern "C" fn vkCreateRadiumSurfaceEXT(_i: *mut c_void, ci: *const RadiumSurfaceCreateInfo, _a: *const c_void, out: *mut H) -> i32 {
    let s = st();
    s.present_scale = ((*ci).flags & 0xF).max(1).min(4);
    if !s.display_taken { fbcon_suspend(); s.display_taken = true; let black = vec![0u32; 1280 * 720]; fbcon_blit(black.as_ptr(), 1280, 0, 0, 1280, 720); }
    *out = s.misc.insert(0);
    VK_SUCCESS
}
/// For a program that was stopped without destroying its surface: the display is no longer held (the console is not resumed).
#[no_mangle] pub unsafe extern "C" fn vkRadiumForgetDisplay() { st().display_taken = false; }

#[no_mangle] pub unsafe extern "C" fn vkDestroySurfaceKHR(_i: *mut c_void, surf: H, _a: *const c_void) {
    let s = st();
    s.misc.remove(surf);
    if s.display_taken { s.display_taken = false; fbcon_resume(); }
}

#[no_mangle] pub unsafe extern "C" fn vkCreateSwapchainKHR(_d: *mut c_void, ci: *const SwapchainCreateInfo, _a: *const c_void, out: *mut H) -> i32 {
    let ci = &*ci;
    let scale = st().present_scale;
    if !matches!(ci.format, 37 | 44) || ci.width == 0 || ci.height == 0 || ci.width * scale > 1280 || ci.height * scale > 720 { return VK_ERROR_FORMAT_NOT_SUPPORTED; }
    let count = ci.min_images.max(2).min(4);
    let s = st();
    let (mut images, mut mems) = (Vec::new(), Vec::new());
    for _ in 0..count {
        let mut data = Vec::new();
        let bytes = (ci.width * ci.height * 4) as usize;
        if data.try_reserve_exact(bytes + 16).is_err() { return VK_ERROR_OUT_OF_HOST_MEMORY; }
        data.resize(bytes + 16, 0u8);
        let mem = s.memories.insert(Memory { data });
        let img = s.images.insert(Image { width: ci.width, height: ci.height, format: ci.format, mem, off: 0 });
        mems.push(mem);
        images.push(img);
    }
    *out = s.swapchains.insert(Swapchain { images, mems, width: ci.width, height: ci.height, next: 0, scale });
    VK_SUCCESS
}
#[no_mangle] pub unsafe extern "C" fn vkDestroySwapchainKHR(_d: *mut c_void, sc: H, _a: *const c_void) {
    let s = st();
    if let Some(chain) = s.swapchains.get(sc) {
        let (images, mems) = (chain.images.clone(), chain.mems.clone());
        for i in images { s.images.remove(i); }
        for m in mems { s.memories.remove(m); }
    }
    s.swapchains.remove(sc);
}
#[no_mangle] pub unsafe extern "C" fn vkGetSwapchainImagesKHR(_d: *mut c_void, sc: H, count: *mut u32, images: *mut H) -> i32 {
    let Some(chain) = st().swapchains.get(sc) else { return VK_ERROR_UNKNOWN };
    if images.is_null() { *count = chain.images.len() as u32; return VK_SUCCESS; }
    let n = (*count as usize).min(chain.images.len());
    for k in 0..n { *images.add(k) = chain.images[k]; }
    *count = n as u32;
    if n < chain.images.len() { VK_INCOMPLETE } else { VK_SUCCESS }
}
#[no_mangle] pub unsafe extern "C" fn vkAcquireNextImageKHR(_d: *mut c_void, sc: H, _timeout: u64, _sem: H, _fence: H, index: *mut u32) -> i32 {
    let Some(chain) = st().swapchains.get(sc) else { return VK_ERROR_UNKNOWN };
    *index = chain.next % chain.images.len() as u32;
    chain.next += 1;
    VK_SUCCESS
}
#[no_mangle] pub unsafe extern "C" fn vkQueuePresentKHR(_q: *mut c_void, pi: *const PresentInfo) -> i32 {
    let pi = &*pi;
    for k in 0..pi.swapchain_count as usize {
        let s = st();
        let Some(chain) = s.swapchains.get(*pi.p_swapchains.add(k)) else { return VK_ERROR_UNKNOWN };
        let (w, h, img_h, chain_scale) = (chain.width, chain.height, chain.images[(*pi.p_indices.add(k) as usize).min(chain.images.len() - 1)], chain.scale);
        let Some(img) = s.images.get(img_h) else { return VK_ERROR_UNKNOWN };
        let (fmt, mem, off) = (img.format, img.mem, img.off);
        let Some(m) = s.memories.get(mem) else { return VK_ERROR_UNKNOWN };
        let px = m.data.as_ptr().add(off as usize) as *const u32;
        let sc = chain_scale as usize;
        let (ow, oh) = (w as usize * sc, h as usize * sc);
        let (x, y) = ((1280 - ow as i32) / 2, (720 - oh as i32) / 2);
        let src = core::slice::from_raw_parts(px, (w * h) as usize);
        if sc == 1 && fmt == 44 {
            fbcon_blit(px, w as i32, x, y, w as i32, h as i32);
        } else {
            let out = &mut s.scaled;
            out.clear();
            out.resize(ow * oh, 0);
            for row in 0..h as usize {
                let base = row * sc * ow;
                for col in 0..w as usize {
                    let p = src[row * w as usize + col];
                    let v = if fmt == 44 { p } else { (p & 0xFF00_FF00) | ((p & 0xFF) << 16) | ((p >> 16) & 0xFF) };
                    for k in 0..sc { out[base + col * sc + k] = v; }
                }
                for k in 1..sc { out.copy_within(base..base + ow, base + k * ow); }
            }
            fbcon_blit(out.as_ptr(), ow as i32, x, y, ow as i32, oh as i32);
        }
        if !pi.p_results.is_null() { *pi.p_results.add(k) = VK_SUCCESS; }
    }
    VK_SUCCESS
}

#[allow(dead_code)]
const _UNUSED: i32 = VK_NOT_READY + VK_ERROR_INITIALIZATION_FAILED;

/// Demo helper (not part of Vulkan): writes a column-major model-view-projection matrix for a cube turning by `angle`.
#[no_mangle] pub unsafe extern "C" fn vkRadiumDemoMatrix(out: *mut f32, angle: f32, aspect: f32) {
    let (sa, ca) = (libm::sinf(angle), libm::cosf(angle));
    let (sb, cb) = (libm::sinf(angle * 0.6), libm::cosf(angle * 0.6));
    // model = Ry(angle) * Rx(angle * 0.6), column-major
    let ry = [ca, 0.0, -sa, 0.0, 0.0, 1.0, 0.0, 0.0, sa, 0.0, ca, 0.0, 0.0, 0.0, 0.0, 1.0];
    let rx = [1.0, 0.0, 0.0, 0.0, 0.0, cb, sb, 0.0, 0.0, -sb, cb, 0.0, 0.0, 0.0, 0.0, 1.0];
    let mul = |a: &[f32; 16], b: &[f32; 16]| -> [f32; 16] {
        let mut r = [0.0f32; 16];
        for c in 0..4 { for row in 0..4 { for k in 0..4 { r[c * 4 + row] += a[k * 4 + row] * b[c * 4 + k]; } } }
        r
    };
    let model = mul(&ry, &rx);
    let view = [1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, -4.5, 1.0];
    let f = 1.0 / libm::tanf(0.5236);   // 60 degree field of view
    let (n, fa) = (0.1f32, 20.0f32);
    // Vulkan clip space: y points down, depth 0..1
    let proj = [f / aspect, 0.0, 0.0, 0.0, 0.0, -f, 0.0, 0.0, 0.0, 0.0, fa / (n - fa), -1.0, 0.0, 0.0, n * fa / (n - fa), 0.0];
    let m = mul(&proj, &mul(&view, &model));
    for k in 0..16 { *out.add(k) = m[k]; }
}

#[cfg(test)]
mod tests;
