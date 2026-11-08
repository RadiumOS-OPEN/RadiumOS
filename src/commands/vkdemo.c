// vkdemo.c - Vulkan demo and device query for the software implementation (rust_lib/src/vulkan, src/vulkan/vulkan.h)
//
//   vulkaninfo            what the Vulkan implementation reports
//   vkdemo [-f FRAMES] [-s WIDTHxHEIGHT]   a rotating, depth-tested cube drawn with real SPIR-V shaders through the Vulkan API;
//                                          Esc or Q stops it (default 640x360, runs until a key is pressed)
//
// The shaders are tools/shaders/cube.vert|frag (GLSL, compiled with tools/gen_vkdemo_shaders.py into vkdemo_spv.h).

#include <stdint.h>
#include <stdbool.h>
#include "vkdemo.h"
#include "fsapp.h"
#include "../terminal/terminal.h"
#include "../vga/vga.h"
#include "../utility/utility.h"
#include "../timers/timer.h"
#include "../timers/tsc.h"
#include "../vulkan/vulkan.h"
#include "vkdemo_spv.h"

#define OUT_ERR(...) do { terminal_setcolor(VGA_COLOR_LIGHT_RED); printr(__VA_ARGS__); terminal_setcolor(VGA_COLOR_WHITE); } while (0)

// The numbers in these assertions are the offsets of the 32-bit Vulkan ABI (the Rust side asserts the same ones).
_Static_assert(offsetof(VkBufferCreateInfo, size) == 12, "VkBufferCreateInfo layout");
_Static_assert(offsetof(VkMemoryAllocateInfo, memoryTypeIndex) == 16, "VkMemoryAllocateInfo layout");
_Static_assert(offsetof(VkGraphicsPipelineCreateInfo, layout) == 56, "VkGraphicsPipelineCreateInfo layout");
_Static_assert(offsetof(VkGraphicsPipelineCreateInfo, renderPass) == 64, "VkGraphicsPipelineCreateInfo layout");
_Static_assert(offsetof(VkRenderPassBeginInfo, clearValueCount) == 40, "VkRenderPassBeginInfo layout");
_Static_assert(offsetof(VkSwapchainCreateInfoKHR, oldSwapchain) == 76, "VkSwapchainCreateInfoKHR layout");
_Static_assert(offsetof(VkWriteDescriptorSet, pBufferInfo) == 36, "VkWriteDescriptorSet layout");

#define CHECK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { OUT_ERR("vkdemo: %s failed (%d)\n", #call, (int)r_); goto fail; } } while (0)

static const char *queue_flags(VkQueueFlags f, char *out)
{
    out[0] = '\0';
    if (f & VK_QUEUE_GRAPHICS_BIT) strcat(out, "graphics ");
    if (f & VK_QUEUE_COMPUTE_BIT) strcat(out, "compute ");
    if (f & VK_QUEUE_TRANSFER_BIT) strcat(out, "transfer");
    return out;
}

void vulkaninfo_command(int argc, char *argv[])
{
    (void)argc; (void)argv;
    uint32_t version = 0, count = 1;
    VkInstance instance;
    VkPhysicalDevice device;
    vkEnumerateInstanceVersion(&version);
    if (vkCreateInstance(NULL, NULL, &instance) != VK_SUCCESS) { OUT_ERR("vulkaninfo: could not create an instance\n"); return; }
    vkEnumeratePhysicalDevices(instance, &count, &device);

    static VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(device, &props);
    terminal_setcolor(VGA_COLOR_LIGHT_CYAN);
    print("Vulkan instance\n");
    terminal_setcolor(VGA_COLOR_WHITE);
    printr("  instance version  %u.%u.%u\n", version >> 22, (version >> 12) & 0x3FF, version & 0xFFF);
    terminal_setcolor(VGA_COLOR_LIGHT_CYAN);
    print("Device 0\n");
    terminal_setcolor(VGA_COLOR_WHITE);
    printr("  name              %s\n", props.deviceName);
    printr("  type              %s (it runs on the CPU)\n", props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ? "CPU" : "other");
    printr("  api version       %u.%u.%u   driver %u\n", props.apiVersion >> 22, (props.apiVersion >> 12) & 0x3FF, props.apiVersion & 0xFFF, props.driverVersion);
    printr("  max image size    %u x %u\n", props.limits.maxImageDimension2D, props.limits.maxImageDimension2D);
    printr("  max push consts   %u bytes   max uniform range %u bytes\n", props.limits.maxPushConstantsSize, props.limits.maxUniformBufferRange);

    uint32_t qn = 1;
    VkQueueFamilyProperties q;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &qn, &q);
    char flags[40];
    printr("  queue family 0    %u queue, %s\n", q.queueCount, queue_flags(q.queueFlags, flags));

    static VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(device, &mem);
    for (uint32_t i = 0; i < mem.memoryHeapCount; i++) printr("  heap %u            %u MB\n", i, (uint32_t)(mem.memoryHeaps[i].size >> 20));
    for (uint32_t i = 0; i < mem.memoryTypeCount; i++)
        printr("  memory type %u     flags 0x%x (device-local, host-visible, host-coherent)\n", i, mem.memoryTypes[i].propertyFlags);

    terminal_setcolor(VGA_COLOR_LIGHT_CYAN);
    print("Implemented\n");
    terminal_setcolor(VGA_COLOR_WHITE);
    print("  shaders           SPIR-V 32-bit, vertex + fragment, uniform/storage buffers, push constants, GLSL.std.450\n");
    print("  rendering         triangle list/strip/fan, depth test, blending, culling, viewport/scissor, indexed and instanced draws\n");
    print("  formats           B8G8R8A8, R8G8B8A8 (color), D32_SFLOAT (depth); vertex: R32* float/uint/sint, R8G8B8A8_UNORM\n");
    print("  not implemented   images/samplers, compute, geometry/tessellation, multisampling, multiple subpasses\n");
    print("Try: vkdemo\n");
    vkDestroyInstance(instance, NULL);
}

typedef struct { float pos[3]; float color[3]; } cube_vertex_t;

static const float FACE_CORNERS[6][4][3] = {
    { {-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1} },
    { {1, -1, -1}, {-1, -1, -1}, {-1, 1, -1}, {1, 1, -1} },
    { {1, -1, 1}, {1, -1, -1}, {1, 1, -1}, {1, 1, 1} },
    { {-1, -1, -1}, {-1, -1, 1}, {-1, 1, 1}, {-1, 1, -1} },
    { {-1, 1, 1}, {1, 1, 1}, {1, 1, -1}, {-1, 1, -1} },
    { {-1, -1, -1}, {1, -1, -1}, {1, -1, 1}, {-1, -1, 1} },
};
static const float FACE_COLORS[6][3] = {
    {1.0f, 0.2f, 0.2f}, {0.2f, 1.0f, 0.2f}, {0.3f, 0.4f, 1.0f}, {1.0f, 1.0f, 0.2f}, {0.2f, 1.0f, 1.0f}, {1.0f, 0.3f, 1.0f},
};

static cube_vertex_t g_vertices[24];
static uint16_t g_indices[36];

static void build_cube(void)
{
    for (int f = 0; f < 6; f++) {
        for (int c = 0; c < 4; c++) {
            for (int k = 0; k < 3; k++) { g_vertices[f * 4 + c].pos[k] = FACE_CORNERS[f][c][k]; g_vertices[f * 4 + c].color[k] = FACE_COLORS[f][k]; }
        }
        uint16_t b = (uint16_t)(f * 4);
        uint16_t idx[6] = { b, (uint16_t)(b + 1), (uint16_t)(b + 2), (uint16_t)(b + 2), (uint16_t)(b + 3), b };
        for (int k = 0; k < 6; k++) g_indices[f * 6 + k] = idx[k];
    }
}

static VkDevice g_dev;

// Milliseconds since the first call. The timer interrupt does not run while a command does, so this counts TSC cycles
// (32-bit steps, which is fine as long as it is called more often than every second).
static uint32_t elapsed_ms(void)
{
    static uint32_t last_lo, ms, rem;
    static bool first = true;
    uint32_t lo, hi, per_ms = tsc_cycles_per_ms();
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    if (first) { first = false; last_lo = lo; ms = 0; rem = 0; return 0; }
    rem += lo - last_lo;
    last_lo = lo;
    if (per_ms) { ms += rem / per_ms; rem %= per_ms; }
    return ms;
}

// A buffer with its own memory, mapped; `data` (if any) is copied in.
static VkResult make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, const void *data, VkBuffer *buf, void **mapped)
{
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
    VkMemoryRequirements req;
    VkDeviceMemory mem;
    VkResult r = vkCreateBuffer(g_dev, &bi, NULL, buf);
    if (r != VK_SUCCESS) return r;
    vkGetBufferMemoryRequirements(g_dev, *buf, &req);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, 0 };
    if ((r = vkAllocateMemory(g_dev, &ai, NULL, &mem)) != VK_SUCCESS) return r;
    if ((r = vkBindBufferMemory(g_dev, *buf, mem, 0)) != VK_SUCCESS) return r;
    void *p = NULL;
    if ((r = vkMapMemory(g_dev, mem, 0, req.size, 0, &p)) != VK_SUCCESS) return r;
    if (data) memcpy(p, data, (size_t)size);
    if (mapped) *mapped = p;
    return VK_SUCCESS;
}

static VkResult make_image_view(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect, VkImageView *view)
{
    VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, format, w, h, 1, 1, 1, VK_SAMPLE_COUNT_1_BIT,
                             VK_IMAGE_TILING_OPTIMAL, usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED };
    VkImage image;
    VkMemoryRequirements req;
    VkDeviceMemory mem;
    VkResult r = vkCreateImage(g_dev, &ci, NULL, &image);
    if (r != VK_SUCCESS) return r;
    vkGetImageMemoryRequirements(g_dev, image, &req);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, 0 };
    if ((r = vkAllocateMemory(g_dev, &ai, NULL, &mem)) != VK_SUCCESS) return r;
    if ((r = vkBindImageMemory(g_dev, image, mem, 0)) != VK_SUCCESS) return r;
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, image, VK_IMAGE_VIEW_TYPE_2D, format, {0, 0, 0, 0}, aspect, 0, 1, 0, 1 };
    return vkCreateImageView(g_dev, &vi, NULL, view);
}

static VkResult make_view_of(VkImage image, VkFormat format, VkImageView *view)
{
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, image, VK_IMAGE_VIEW_TYPE_2D, format, {0, 0, 0, 0}, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    return vkCreateImageView(g_dev, &vi, NULL, view);
}

void vkdemo_command(int argc, char *argv[])
{
    uint32_t width = 640, height = 360, max_frames = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) max_frames = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            const char *s = argv[++i];
            width = (uint32_t)atoi(s);
            const char *x = strchr(s, 'x');
            height = x ? (uint32_t)atoi(x + 1) : width * 9 / 16;
        } else { OUT_ERR("Usage: vkdemo [-f FRAMES] [-s WIDTHxHEIGHT]\n"); return; }
    }
    if (width < 64 || height < 64 || width > 1280 || height > 720) { OUT_ERR("vkdemo: the size must be between 64x64 and 1280x720\n"); return; }
    build_cube();

    VkInstance instance = NULL; VkPhysicalDevice phys; VkQueue queue;
    VkSurfaceKHR surface = 0; VkSwapchainKHR swapchain = 0;
    uint32_t n = 1, image_count = 0;
    VkImage images[4];
    VkImageView color_views[4], depth_view;
    VkFramebuffer framebuffers[4];
    VkRenderPass render_pass; VkShaderModule vs, fs; VkDescriptorSetLayout dsl; VkPipelineLayout layout;
    VkDescriptorPool pool; VkDescriptorSet set; VkPipeline pipeline;
    VkBuffer vbuf, ibuf, ubuf; float *ubo = NULL;
    VkCommandBuffer cmd;
    uint32_t frames = 0;
    uint32_t start_ms = 0, end_ms = 0;
    bool started = false;

    CHECK(vkCreateInstance(NULL, NULL, &instance));
    CHECK(vkEnumeratePhysicalDevices(instance, &n, &phys));
    CHECK(vkCreateDevice(phys, NULL, NULL, &g_dev));
    vkGetDeviceQueue(g_dev, 0, 0, &queue);

    CHECK(make_buffer(sizeof(g_vertices), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, g_vertices, &vbuf, NULL));
    CHECK(make_buffer(sizeof(g_indices), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, g_indices, &ibuf, NULL));
    CHECK(make_buffer(64, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, NULL, &ubuf, (void **)&ubo));

    VkShaderModuleCreateInfo smi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(vkdemo_cube_vert_spv), vkdemo_cube_vert_spv };
    CHECK(vkCreateShaderModule(g_dev, &smi, NULL, &vs));
    smi.codeSize = sizeof(vkdemo_cube_frag_spv); smi.pCode = vkdemo_cube_frag_spv;
    CHECK(vkCreateShaderModule(g_dev, &smi, NULL, &fs));

    // the screen
    VkRadiumSurfaceCreateInfoEXT sci = { VK_STRUCTURE_TYPE_RADIUM_SURFACE_CREATE_INFO_EXT, NULL, 0 };
    CHECK(vkCreateRadiumSurfaceEXT(instance, &sci, NULL, &surface));
    VkSwapchainCreateInfoKHR swi = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, NULL, 0, surface, 2, VK_FORMAT_B8G8R8A8_UNORM, 0, { width, height },
                                     1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, 0, 0, VK_PRESENT_MODE_FIFO_KHR, 1, 0 };
    CHECK(vkCreateSwapchainKHR(g_dev, &swi, NULL, &swapchain));
    image_count = 4;
    CHECK(vkGetSwapchainImagesKHR(g_dev, swapchain, &image_count, images));
    for (uint32_t i = 0; i < image_count; i++) CHECK(make_view_of(images[i], VK_FORMAT_B8G8R8A8_UNORM, &color_views[i]));
    CHECK(make_image_view(width, height, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, &depth_view));

    VkAttachmentDescription atts[2] = {
        { 0, VK_FORMAT_B8G8R8A8_UNORM, 1, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR },
        { 0, VK_FORMAT_D32_SFLOAT, 1, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL },
    };
    VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL }, dref = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = { 0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 1, &cref, NULL, &dref, 0, NULL };
    VkRenderPassCreateInfo rpi = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 2, atts, 1, &sub, 0, NULL };
    CHECK(vkCreateRenderPass(g_dev, &rpi, NULL, &render_pass));
    for (uint32_t i = 0; i < image_count; i++) {
        VkImageView views[2] = { color_views[i], depth_view };
        VkFramebufferCreateInfo fbi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, render_pass, 2, views, width, height, 1 };
        CHECK(vkCreateFramebuffer(g_dev, &fbi, NULL, &framebuffers[i]));
    }

    VkDescriptorSetLayoutBinding dsb = { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo dslci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsb };
    CHECK(vkCreateDescriptorSetLayout(g_dev, &dslci, NULL, &dsl));
    VkPushConstantRange pcr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4 };
    VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl, 1, &pcr };
    CHECK(vkCreatePipelineLayout(g_dev, &plci, NULL, &layout));
    VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 0, NULL };
    CHECK(vkCreateDescriptorPool(g_dev, &dpci, NULL, &pool));
    VkDescriptorSetAllocateInfo dsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, pool, 1, &dsl };
    CHECK(vkAllocateDescriptorSets(g_dev, &dsai, &set));
    VkDescriptorBufferInfo dbi = { ubuf, 0, 64 };
    VkWriteDescriptorSet wds = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, NULL, &dbi, NULL };
    vkUpdateDescriptorSets(g_dev, 1, &wds, 0, NULL);

    VkPipelineShaderStageCreateInfo stages[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL },
    };
    VkVertexInputBindingDescription vib = { 0, sizeof(cube_vertex_t), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription via[2] = { { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 }, { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12 } };
    VkPipelineVertexInputStateCreateInfo vii = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0, 1, &vib, 2, via };
    VkPipelineInputAssemblyStateCreateInfo iai = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0 };
    VkViewport vp = { 0, 0, (float)width, (float)height, 0, 1 };
    VkRect2D scissor = { { 0, 0 }, { width, height } };
    VkPipelineViewportStateCreateInfo vpi = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, &vp, 1, &scissor };
    VkPipelineRasterizationStateCreateInfo rsi = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, NULL, 0, 0, 0, VK_POLYGON_MODE_FILL,
                                                   VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_CLOCKWISE, 0, 0, 0, 0, 1.0f };
    VkPipelineDepthStencilStateCreateInfo dsi = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, NULL, 0, 1, 1, VK_COMPARE_OP_LESS, 0, 0 };
    VkPipelineColorBlendAttachmentState cba = { 0, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, 0xF };
    VkPipelineColorBlendStateCreateInfo cbi = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, 0, 0, 1, &cba, { 0, 0, 0, 0 } };
    VkGraphicsPipelineCreateInfo gpi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL, 0, 2, stages, &vii, &iai, NULL, &vpi, &rsi, NULL, &dsi, &cbi, NULL,
                                         layout, render_pass, 0, 0, -1 };
    CHECK(vkCreateGraphicsPipelines(g_dev, 0, 1, &gpi, NULL, &pipeline));

    VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, 0, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    CHECK(vkAllocateCommandBuffers(g_dev, &cbai, &cmd));

    VkClearValue clears[2];
    clears[0].color.float32[0] = 0.05f; clears[0].color.float32[1] = 0.05f; clears[0].color.float32[2] = 0.10f; clears[0].color.float32[3] = 1.0f;
    clears[1].depthStencil.depth = 1.0f; clears[1].depthStencil.stencil = 0;
    VkDeviceSize zero = 0;

    elapsed_ms();
    start_ms = 0;
    started = true;
    for (;;) {
        uint32_t now = elapsed_ms();
        float t = (float)(now - start_ms) / 1000.0f;
        vkRadiumDemoMatrix(ubo, t, (float)width / (float)height);

        uint32_t index;
        CHECK(vkAcquireNextImageKHR(g_dev, swapchain, ~0ULL, 0, 0, &index));
        CHECK(vkBeginCommandBuffer(cmd, NULL));
        VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, render_pass, framebuffers[index], { { 0, 0 }, { width, height } }, 2, clears };
        vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindVertexBuffers(cmd, 0, 1, &vbuf, &zero);
        vkCmdBindIndexBuffer(cmd, ibuf, 0, VK_INDEX_TYPE_UINT16);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, NULL);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4, &t);
        vkCmdDrawIndexed(cmd, 36, 1, 0, 0, 0);
        vkCmdEndRenderPass(cmd);
        CHECK(vkEndCommandBuffer(cmd));
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cmd, 0, NULL };
        CHECK(vkQueueSubmit(queue, 1, &si, 0));
        VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, 0, NULL, 1, &swapchain, &index, NULL };
        CHECK(vkQueuePresentKHR(queue, &pi));
        frames++;

        uint8_t key = fs_poll_key();
        if (key == FS_KEY_ESC || key == FS_KEY_Q) break;
        if (max_frames && frames >= max_frames) break;
    }
    end_ms = elapsed_ms();

fail:
    if (swapchain) vkDestroySwapchainKHR(g_dev, swapchain, NULL);
    if (surface) vkDestroySurfaceKHR(instance, surface, NULL);   // gives the screen back to the console
    if (started && end_ms > start_ms) {
        uint32_t ms = end_ms - start_ms;
        printr("vkdemo: %ux%u, %u frames in %u ms (%u.%u frames/s)\n", width, height, frames, ms, frames * 1000 / ms, (frames * 10000 / ms) % 10);
    }
    if (instance) vkDestroyInstance(instance, NULL);
}
