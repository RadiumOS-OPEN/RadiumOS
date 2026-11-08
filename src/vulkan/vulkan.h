// vulkan.h - the Vulkan 1.0 subset implemented by rust_lib/src/vulkan (a software rasterizer with a SPIR-V interpreter).
//
// Declarations follow the Khronos header (names, values, struct member order) for what is implemented; anything not
// listed here is not available. VkPhysicalDeviceLimits only names the first members (the rest is reserved padding).
// Differences from a real driver:
//   - vkQueueSubmit runs the command buffers before it returns; fences and semaphores are only handles.
//   - one queue family (graphics + transfer), one host-visible memory type; buffers and images share vkAllocateMemory.
//   - the screen is a surface created with vkCreateRadiumSurfaceEXT (it takes the display from the text console until
//     vkDestroySurfaceKHR); presenting copies the image to the middle of the 1280x720 framebuffer.
//   - shaders: 32-bit SPIR-V (vertex + fragment, entry point "main"), uniform/storage buffers and push constants;
//     no images/samplers, no tessellation/geometry/compute, triangle topologies only, single subpass, one color target.
#ifndef RADIUM_VULKAN_H
#define RADIUM_VULKAN_H

#include <stdint.h>
#include <stddef.h>

#define VK_VERSION_1_0 1
#define VK_MAKE_VERSION(major, minor, patch) (((major) << 22) | ((minor) << 12) | (patch))
#define VK_API_VERSION_1_0 VK_MAKE_VERSION(1, 0, 0)

#define VK_DEFINE_HANDLE(object) typedef struct object##_T *object;
#define VK_DEFINE_NON_DISPATCHABLE_HANDLE(object) typedef uint64_t object;
#define VK_NULL_HANDLE 0
#define VK_WHOLE_SIZE (~0ULL)
#define VK_ATTACHMENT_UNUSED (~0U)

typedef uint32_t VkFlags;
typedef uint32_t VkBool32;
typedef uint64_t VkDeviceSize;
typedef VkFlags VkQueueFlags, VkMemoryPropertyFlags, VkBufferUsageFlags, VkImageUsageFlags, VkShaderStageFlags,
                VkCullModeFlags, VkColorComponentFlags, VkImageAspectFlags, VkPipelineStageFlags, VkSampleCountFlags;

VK_DEFINE_HANDLE(VkInstance)
VK_DEFINE_HANDLE(VkPhysicalDevice)
VK_DEFINE_HANDLE(VkDevice)
VK_DEFINE_HANDLE(VkQueue)
VK_DEFINE_HANDLE(VkCommandBuffer)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkDeviceMemory)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkBuffer)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkImage)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkImageView)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkShaderModule)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkRenderPass)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkFramebuffer)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkPipelineLayout)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkPipeline)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkPipelineCache)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkDescriptorSetLayout)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkDescriptorPool)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkDescriptorSet)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkCommandPool)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkFence)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkSemaphore)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkSurfaceKHR)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkSwapchainKHR)

typedef struct VkAllocationCallbacks VkAllocationCallbacks;   // always pass NULL

typedef enum VkResult {
    VK_SUCCESS = 0, VK_NOT_READY = 1, VK_TIMEOUT = 2, VK_INCOMPLETE = 5,
    VK_ERROR_OUT_OF_HOST_MEMORY = -1, VK_ERROR_OUT_OF_DEVICE_MEMORY = -2, VK_ERROR_INITIALIZATION_FAILED = -3,
    VK_ERROR_DEVICE_LOST = -4, VK_ERROR_MEMORY_MAP_FAILED = -5, VK_ERROR_FEATURE_NOT_PRESENT = -8,
    VK_ERROR_FORMAT_NOT_SUPPORTED = -11, VK_ERROR_UNKNOWN = -13,
} VkResult;

typedef enum VkStructureType {
    VK_STRUCTURE_TYPE_APPLICATION_INFO = 0, VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO = 1,
    VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO = 2, VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO = 3, VK_STRUCTURE_TYPE_SUBMIT_INFO = 4,
    VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO = 5, VK_STRUCTURE_TYPE_FENCE_CREATE_INFO = 8, VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO = 9,
    VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO = 12, VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO = 14, VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO = 15,
    VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO = 16, VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO = 18,
    VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO = 19, VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO = 20,
    VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO = 22, VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO = 23,
    VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO = 24, VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO = 25,
    VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO = 26, VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO = 27,
    VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO = 28, VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO = 30,
    VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO = 32, VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO = 33,
    VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO = 34, VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET = 35,
    VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO = 37, VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO = 38,
    VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO = 39, VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO = 40,
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO = 42, VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO = 43,
    VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR = 1000001000, VK_STRUCTURE_TYPE_PRESENT_INFO_KHR = 1000001001,
    VK_STRUCTURE_TYPE_RADIUM_SURFACE_CREATE_INFO_EXT = 1000999000,
} VkStructureType;

typedef enum VkFormat {
    VK_FORMAT_UNDEFINED = 0, VK_FORMAT_R8G8B8A8_UNORM = 37, VK_FORMAT_R8G8B8A8_SRGB = 43, VK_FORMAT_B8G8R8A8_UNORM = 44,
    VK_FORMAT_B8G8R8A8_SRGB = 50, VK_FORMAT_R32_SFLOAT = 100, VK_FORMAT_R32G32_SFLOAT = 103, VK_FORMAT_R32G32B32_SFLOAT = 106,
    VK_FORMAT_R32G32B32A32_SFLOAT = 109, VK_FORMAT_R32_UINT = 98, VK_FORMAT_R32G32_UINT = 101, VK_FORMAT_R32G32B32_UINT = 104,
    VK_FORMAT_R32G32B32A32_UINT = 107, VK_FORMAT_R32_SINT = 99, VK_FORMAT_R32G32_SINT = 102, VK_FORMAT_R32G32B32_SINT = 105,
    VK_FORMAT_R32G32B32A32_SINT = 108, VK_FORMAT_D32_SFLOAT = 126,
} VkFormat;

enum { VK_QUEUE_GRAPHICS_BIT = 1, VK_QUEUE_COMPUTE_BIT = 2, VK_QUEUE_TRANSFER_BIT = 4 };
enum { VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT = 1, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT = 2, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT = 4 };
enum { VK_BUFFER_USAGE_TRANSFER_SRC_BIT = 1, VK_BUFFER_USAGE_TRANSFER_DST_BIT = 2, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT = 0x10,
       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT = 0x20, VK_BUFFER_USAGE_INDEX_BUFFER_BIT = 0x40, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT = 0x80 };
enum { VK_IMAGE_USAGE_TRANSFER_SRC_BIT = 1, VK_IMAGE_USAGE_TRANSFER_DST_BIT = 2, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT = 0x10, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT = 0x20 };
enum { VK_SHADER_STAGE_VERTEX_BIT = 1, VK_SHADER_STAGE_FRAGMENT_BIT = 0x10 };
enum { VK_CULL_MODE_NONE = 0, VK_CULL_MODE_FRONT_BIT = 1, VK_CULL_MODE_BACK_BIT = 2 };
enum { VK_COLOR_COMPONENT_R_BIT = 1, VK_COLOR_COMPONENT_G_BIT = 2, VK_COLOR_COMPONENT_B_BIT = 4, VK_COLOR_COMPONENT_A_BIT = 8 };
enum { VK_IMAGE_ASPECT_COLOR_BIT = 1, VK_IMAGE_ASPECT_DEPTH_BIT = 2 };
enum { VK_IMAGE_TYPE_2D = 1, VK_IMAGE_VIEW_TYPE_2D = 1, VK_IMAGE_TILING_OPTIMAL = 0, VK_SAMPLE_COUNT_1_BIT = 1, VK_SHARING_MODE_EXCLUSIVE = 0 };
enum { VK_IMAGE_LAYOUT_UNDEFINED = 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL = 2, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL = 3,
       VK_IMAGE_LAYOUT_PRESENT_SRC_KHR = 1000001002 };
enum { VK_ATTACHMENT_LOAD_OP_LOAD = 0, VK_ATTACHMENT_LOAD_OP_CLEAR = 1, VK_ATTACHMENT_LOAD_OP_DONT_CARE = 2 };
enum { VK_ATTACHMENT_STORE_OP_STORE = 0, VK_ATTACHMENT_STORE_OP_DONT_CARE = 1 };
enum { VK_PIPELINE_BIND_POINT_GRAPHICS = 0 };
enum { VK_COMMAND_BUFFER_LEVEL_PRIMARY = 0 };
enum { VK_SUBPASS_CONTENTS_INLINE = 0 };
enum { VK_INDEX_TYPE_UINT16 = 0, VK_INDEX_TYPE_UINT32 = 1 };
enum { VK_VERTEX_INPUT_RATE_VERTEX = 0, VK_VERTEX_INPUT_RATE_INSTANCE = 1 };
enum { VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST = 3, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP = 4, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN = 5 };
enum { VK_POLYGON_MODE_FILL = 0 };
enum { VK_FRONT_FACE_COUNTER_CLOCKWISE = 0, VK_FRONT_FACE_CLOCKWISE = 1 };
enum { VK_COMPARE_OP_NEVER = 0, VK_COMPARE_OP_LESS = 1, VK_COMPARE_OP_EQUAL = 2, VK_COMPARE_OP_LESS_OR_EQUAL = 3,
       VK_COMPARE_OP_GREATER = 4, VK_COMPARE_OP_NOT_EQUAL = 5, VK_COMPARE_OP_GREATER_OR_EQUAL = 6, VK_COMPARE_OP_ALWAYS = 7 };
enum { VK_BLEND_FACTOR_ZERO = 0, VK_BLEND_FACTOR_ONE = 1, VK_BLEND_FACTOR_SRC_COLOR = 2, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR = 3,
       VK_BLEND_FACTOR_DST_COLOR = 4, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR = 5, VK_BLEND_FACTOR_SRC_ALPHA = 6,
       VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA = 7, VK_BLEND_FACTOR_DST_ALPHA = 8, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA = 9 };
enum { VK_BLEND_OP_ADD = 0, VK_BLEND_OP_SUBTRACT = 1, VK_BLEND_OP_REVERSE_SUBTRACT = 2, VK_BLEND_OP_MIN = 3, VK_BLEND_OP_MAX = 4 };
enum { VK_DYNAMIC_STATE_VIEWPORT = 0, VK_DYNAMIC_STATE_SCISSOR = 1 };
enum { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER = 6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER = 7 };
enum { VK_PHYSICAL_DEVICE_TYPE_CPU = 4 };
enum { VK_PRESENT_MODE_FIFO_KHR = 2 };

typedef struct VkExtent2D { uint32_t width, height; } VkExtent2D;
typedef struct VkOffset2D { int32_t x, y; } VkOffset2D;
typedef struct VkRect2D { VkOffset2D offset; VkExtent2D extent; } VkRect2D;
typedef struct VkViewport { float x, y, width, height, minDepth, maxDepth; } VkViewport;
typedef union VkClearColorValue { float float32[4]; int32_t int32[4]; uint32_t uint32[4]; } VkClearColorValue;
typedef struct VkClearDepthStencilValue { float depth; uint32_t stencil; } VkClearDepthStencilValue;
typedef union VkClearValue { VkClearColorValue color; VkClearDepthStencilValue depthStencil; } VkClearValue;

typedef struct VkPhysicalDeviceLimits {
    uint32_t maxImageDimension1D, maxImageDimension2D, maxImageDimension3D, maxImageDimensionCube, maxImageArrayLayers,
             maxTexelBufferElements, maxUniformBufferRange, maxStorageBufferRange, maxPushConstantsSize;
    uint32_t reserved[117];
} VkPhysicalDeviceLimits;
typedef struct VkPhysicalDeviceProperties {
    uint32_t apiVersion, driverVersion, vendorID, deviceID, deviceType;
    char deviceName[256];
    uint8_t pipelineCacheUUID[16];
    VkPhysicalDeviceLimits limits;
    VkBool32 sparseProperties[5];
} VkPhysicalDeviceProperties;
typedef struct VkQueueFamilyProperties { VkQueueFlags queueFlags; uint32_t queueCount, timestampValidBits; uint32_t minImageTransferGranularity[3]; } VkQueueFamilyProperties;
typedef struct VkMemoryType { VkMemoryPropertyFlags propertyFlags; uint32_t heapIndex; } VkMemoryType;
typedef struct VkMemoryHeap { VkDeviceSize size; VkFlags flags; } VkMemoryHeap;
typedef struct VkPhysicalDeviceMemoryProperties {
    uint32_t memoryTypeCount; VkMemoryType memoryTypes[32]; uint32_t memoryHeapCount; VkMemoryHeap memoryHeaps[16];
} VkPhysicalDeviceMemoryProperties;
typedef struct VkMemoryRequirements { VkDeviceSize size, alignment; uint32_t memoryTypeBits; } VkMemoryRequirements;

typedef struct VkMemoryAllocateInfo { VkStructureType sType; const void *pNext; VkDeviceSize allocationSize; uint32_t memoryTypeIndex; } VkMemoryAllocateInfo;
typedef struct VkBufferCreateInfo {
    VkStructureType sType; const void *pNext; VkFlags flags; VkDeviceSize size; VkBufferUsageFlags usage; uint32_t sharingMode;
    uint32_t queueFamilyIndexCount; const uint32_t *pQueueFamilyIndices;
} VkBufferCreateInfo;
typedef struct VkImageCreateInfo {
    VkStructureType sType; const void *pNext; VkFlags flags; uint32_t imageType; VkFormat format; uint32_t width, height, depth;
    uint32_t mipLevels, arrayLayers, samples, tiling; VkImageUsageFlags usage; uint32_t sharingMode;
    uint32_t queueFamilyIndexCount; const uint32_t *pQueueFamilyIndices; uint32_t initialLayout;
} VkImageCreateInfo;   // (extent is spelled out as width, height, depth)
typedef struct VkImageViewCreateInfo {
    VkStructureType sType; const void *pNext; VkFlags flags; VkImage image; uint32_t viewType; VkFormat format; uint32_t components[4];
    VkImageAspectFlags aspectMask; uint32_t baseMipLevel, levelCount, baseArrayLayer, layerCount;
} VkImageViewCreateInfo;   // (components and subresourceRange are spelled out)
typedef struct VkShaderModuleCreateInfo { VkStructureType sType; const void *pNext; VkFlags flags; size_t codeSize; const uint32_t *pCode; } VkShaderModuleCreateInfo;

typedef struct VkAttachmentDescription {
    VkFlags flags; VkFormat format; uint32_t samples, loadOp, storeOp, stencilLoadOp, stencilStoreOp, initialLayout, finalLayout;
} VkAttachmentDescription;
typedef struct VkAttachmentReference { uint32_t attachment, layout; } VkAttachmentReference;
typedef struct VkSubpassDescription {
    VkFlags flags; uint32_t pipelineBindPoint; uint32_t inputAttachmentCount; const VkAttachmentReference *pInputAttachments;
    uint32_t colorAttachmentCount; const VkAttachmentReference *pColorAttachments; const VkAttachmentReference *pResolveAttachments;
    const VkAttachmentReference *pDepthStencilAttachment; uint32_t preserveAttachmentCount; const uint32_t *pPreserveAttachments;
} VkSubpassDescription;
typedef struct VkRenderPassCreateInfo {
    VkStructureType sType; const void *pNext; VkFlags flags; uint32_t attachmentCount; const VkAttachmentDescription *pAttachments;
    uint32_t subpassCount; const VkSubpassDescription *pSubpasses; uint32_t dependencyCount; const void *pDependencies;
} VkRenderPassCreateInfo;
typedef struct VkFramebufferCreateInfo {
    VkStructureType sType; const void *pNext; VkFlags flags; VkRenderPass renderPass; uint32_t attachmentCount;
    const VkImageView *pAttachments; uint32_t width, height, layers;
} VkFramebufferCreateInfo;

typedef struct VkPushConstantRange { VkShaderStageFlags stageFlags; uint32_t offset, size; } VkPushConstantRange;
typedef struct VkDescriptorSetLayoutBinding { uint32_t binding, descriptorType, descriptorCount; VkShaderStageFlags stageFlags; const void *pImmutableSamplers; } VkDescriptorSetLayoutBinding;
typedef struct VkDescriptorSetLayoutCreateInfo { VkStructureType sType; const void *pNext; VkFlags flags; uint32_t bindingCount; const VkDescriptorSetLayoutBinding *pBindings; } VkDescriptorSetLayoutCreateInfo;
typedef struct VkPipelineLayoutCreateInfo {
    VkStructureType sType; const void *pNext; VkFlags flags; uint32_t setLayoutCount; const VkDescriptorSetLayout *pSetLayouts;
    uint32_t pushConstantRangeCount; const VkPushConstantRange *pPushConstantRanges;
} VkPipelineLayoutCreateInfo;
typedef struct VkDescriptorPoolCreateInfo { VkStructureType sType; const void *pNext; VkFlags flags; uint32_t maxSets, poolSizeCount; const void *pPoolSizes; } VkDescriptorPoolCreateInfo;
typedef struct VkDescriptorSetAllocateInfo { VkStructureType sType; const void *pNext; VkDescriptorPool descriptorPool; uint32_t descriptorSetCount; const VkDescriptorSetLayout *pSetLayouts; } VkDescriptorSetAllocateInfo;
typedef struct VkDescriptorBufferInfo { VkBuffer buffer; VkDeviceSize offset, range; } VkDescriptorBufferInfo;
typedef struct VkWriteDescriptorSet {
    VkStructureType sType; const void *pNext; VkDescriptorSet dstSet; uint32_t dstBinding, dstArrayElement, descriptorCount, descriptorType;
    const void *pImageInfo; const VkDescriptorBufferInfo *pBufferInfo; const void *pTexelBufferView;
} VkWriteDescriptorSet;

typedef struct VkPipelineShaderStageCreateInfo {
    VkStructureType sType; const void *pNext; VkFlags flags; uint32_t stage; VkShaderModule module; const char *pName; const void *pSpecializationInfo;
} VkPipelineShaderStageCreateInfo;
typedef struct VkVertexInputBindingDescription { uint32_t binding, stride, inputRate; } VkVertexInputBindingDescription;
typedef struct VkVertexInputAttributeDescription { uint32_t location, binding; VkFormat format; uint32_t offset; } VkVertexInputAttributeDescription;
typedef struct VkPipelineVertexInputStateCreateInfo {
    VkStructureType sType; const void *pNext; VkFlags flags; uint32_t vertexBindingDescriptionCount; const VkVertexInputBindingDescription *pVertexBindingDescriptions;
    uint32_t vertexAttributeDescriptionCount; const VkVertexInputAttributeDescription *pVertexAttributeDescriptions;
} VkPipelineVertexInputStateCreateInfo;
typedef struct VkPipelineInputAssemblyStateCreateInfo { VkStructureType sType; const void *pNext; VkFlags flags; uint32_t topology; VkBool32 primitiveRestartEnable; } VkPipelineInputAssemblyStateCreateInfo;
typedef struct VkPipelineViewportStateCreateInfo {
    VkStructureType sType; const void *pNext; VkFlags flags; uint32_t viewportCount; const VkViewport *pViewports; uint32_t scissorCount; const VkRect2D *pScissors;
} VkPipelineViewportStateCreateInfo;
typedef struct VkPipelineRasterizationStateCreateInfo {
    VkStructureType sType; const void *pNext; VkFlags flags; VkBool32 depthClampEnable, rasterizerDiscardEnable; uint32_t polygonMode;
    VkCullModeFlags cullMode; uint32_t frontFace; VkBool32 depthBiasEnable; float depthBiasConstantFactor, depthBiasClamp, depthBiasSlopeFactor, lineWidth;
} VkPipelineRasterizationStateCreateInfo;
typedef struct VkStencilOpState { uint32_t failOp, passOp, depthFailOp, compareOp, compareMask, writeMask, reference; } VkStencilOpState;
typedef struct VkPipelineDepthStencilStateCreateInfo {
    VkStructureType sType; const void *pNext; VkFlags flags; VkBool32 depthTestEnable, depthWriteEnable; uint32_t depthCompareOp;
    VkBool32 depthBoundsTestEnable, stencilTestEnable; VkStencilOpState front, back; float minDepthBounds, maxDepthBounds;
} VkPipelineDepthStencilStateCreateInfo;
typedef struct VkPipelineColorBlendAttachmentState {
    VkBool32 blendEnable; uint32_t srcColorBlendFactor, dstColorBlendFactor, colorBlendOp, srcAlphaBlendFactor, dstAlphaBlendFactor, alphaBlendOp;
    VkColorComponentFlags colorWriteMask;
} VkPipelineColorBlendAttachmentState;
typedef struct VkPipelineColorBlendStateCreateInfo {
    VkStructureType sType; const void *pNext; VkFlags flags; VkBool32 logicOpEnable; uint32_t logicOp; uint32_t attachmentCount;
    const VkPipelineColorBlendAttachmentState *pAttachments; float blendConstants[4];
} VkPipelineColorBlendStateCreateInfo;
typedef struct VkPipelineDynamicStateCreateInfo { VkStructureType sType; const void *pNext; VkFlags flags; uint32_t dynamicStateCount; const uint32_t *pDynamicStates; } VkPipelineDynamicStateCreateInfo;
typedef struct VkGraphicsPipelineCreateInfo {
    VkStructureType sType; const void *pNext; VkFlags flags; uint32_t stageCount; const VkPipelineShaderStageCreateInfo *pStages;
    const VkPipelineVertexInputStateCreateInfo *pVertexInputState; const VkPipelineInputAssemblyStateCreateInfo *pInputAssemblyState;
    const void *pTessellationState; const VkPipelineViewportStateCreateInfo *pViewportState;
    const VkPipelineRasterizationStateCreateInfo *pRasterizationState; const void *pMultisampleState;
    const VkPipelineDepthStencilStateCreateInfo *pDepthStencilState; const VkPipelineColorBlendStateCreateInfo *pColorBlendState;
    const VkPipelineDynamicStateCreateInfo *pDynamicState; VkPipelineLayout layout; VkRenderPass renderPass; uint32_t subpass;
    VkPipeline basePipelineHandle; int32_t basePipelineIndex;
} VkGraphicsPipelineCreateInfo;

typedef struct VkCommandBufferAllocateInfo { VkStructureType sType; const void *pNext; VkCommandPool commandPool; uint32_t level, commandBufferCount; } VkCommandBufferAllocateInfo;
typedef struct VkRenderPassBeginInfo {
    VkStructureType sType; const void *pNext; VkRenderPass renderPass; VkFramebuffer framebuffer; VkRect2D renderArea;
    uint32_t clearValueCount; const VkClearValue *pClearValues;
} VkRenderPassBeginInfo;
typedef struct VkBufferCopy { VkDeviceSize srcOffset, dstOffset, size; } VkBufferCopy;
typedef struct VkOffset3D { int32_t x, y, z; } VkOffset3D;
typedef struct VkExtent3D { uint32_t width, height, depth; } VkExtent3D;
typedef struct VkImageSubresourceLayers { VkImageAspectFlags aspectMask; uint32_t mipLevel, baseArrayLayer, layerCount; } VkImageSubresourceLayers;
typedef struct VkBufferImageCopy {
    VkDeviceSize bufferOffset; uint32_t bufferRowLength, bufferImageHeight;   // row length in texels, 0 = tightly packed
    VkImageSubresourceLayers imageSubresource; VkOffset3D imageOffset; VkExtent3D imageExtent;
} VkBufferImageCopy;
typedef struct VkClearAttachment { VkImageAspectFlags aspectMask; uint32_t colorAttachment; VkClearValue clearValue; } VkClearAttachment;
typedef struct VkClearRect { VkRect2D rect; uint32_t baseArrayLayer, layerCount; } VkClearRect;
typedef struct VkSubmitInfo {
    VkStructureType sType; const void *pNext; uint32_t waitSemaphoreCount; const VkSemaphore *pWaitSemaphores; const VkPipelineStageFlags *pWaitDstStageMask;
    uint32_t commandBufferCount; const VkCommandBuffer *pCommandBuffers; uint32_t signalSemaphoreCount; const VkSemaphore *pSignalSemaphores;
} VkSubmitInfo;

// flags & 0xF: integer factor (1 to 4) by which presented images are enlarged (nearest neighbour)
typedef struct VkRadiumSurfaceCreateInfoEXT { VkStructureType sType; const void *pNext; VkFlags flags; } VkRadiumSurfaceCreateInfoEXT;
typedef struct VkSwapchainCreateInfoKHR {
    VkStructureType sType; const void *pNext; VkFlags flags; VkSurfaceKHR surface; uint32_t minImageCount; VkFormat imageFormat; uint32_t imageColorSpace;
    VkExtent2D imageExtent; uint32_t imageArrayLayers; VkImageUsageFlags imageUsage; uint32_t imageSharingMode; uint32_t queueFamilyIndexCount;
    const uint32_t *pQueueFamilyIndices; uint32_t preTransform, compositeAlpha, presentMode; VkBool32 clipped; VkSwapchainKHR oldSwapchain;
} VkSwapchainCreateInfoKHR;
typedef struct VkPresentInfoKHR {
    VkStructureType sType; const void *pNext; uint32_t waitSemaphoreCount; const VkSemaphore *pWaitSemaphores; uint32_t swapchainCount;
    const VkSwapchainKHR *pSwapchains; const uint32_t *pImageIndices; VkResult *pResults;
} VkPresentInfoKHR;

// instance, devices, queues
VkResult vkEnumerateInstanceVersion(uint32_t *pApiVersion);
VkResult vkCreateInstance(const void *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkInstance *pInstance);
void vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks *pAllocator);
VkResult vkEnumeratePhysicalDevices(VkInstance instance, uint32_t *pCount, VkPhysicalDevice *pDevices);
void vkGetPhysicalDeviceProperties(VkPhysicalDevice device, VkPhysicalDeviceProperties *pProperties);
void vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice device, uint32_t *pCount, VkQueueFamilyProperties *pProperties);
void vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice device, VkPhysicalDeviceMemoryProperties *pProperties);
VkResult vkCreateDevice(VkPhysicalDevice device, const void *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkDevice *pDevice);
void vkDestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator);
void vkGetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue *pQueue);
VkResult vkDeviceWaitIdle(VkDevice device);
VkResult vkQueueWaitIdle(VkQueue queue);

// memory, buffers, images
VkResult vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo *pAllocateInfo, const VkAllocationCallbacks *pAllocator, VkDeviceMemory *pMemory);
void vkFreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks *pAllocator);
VkResult vkMapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size, VkFlags flags, void **ppData);
void vkUnmapMemory(VkDevice device, VkDeviceMemory memory);
VkResult vkFlushMappedMemoryRanges(VkDevice device, uint32_t count, const void *pRanges);
VkResult vkCreateBuffer(VkDevice device, const VkBufferCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkBuffer *pBuffer);
void vkDestroyBuffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks *pAllocator);
void vkGetBufferMemoryRequirements(VkDevice device, VkBuffer buffer, VkMemoryRequirements *pRequirements);
VkResult vkBindBufferMemory(VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset);
VkResult vkCreateImage(VkDevice device, const VkImageCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkImage *pImage);
void vkDestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks *pAllocator);
void vkGetImageMemoryRequirements(VkDevice device, VkImage image, VkMemoryRequirements *pRequirements);
VkResult vkBindImageMemory(VkDevice device, VkImage image, VkDeviceMemory memory, VkDeviceSize offset);
VkResult vkCreateImageView(VkDevice device, const VkImageViewCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkImageView *pView);
void vkDestroyImageView(VkDevice device, VkImageView view, const VkAllocationCallbacks *pAllocator);

// shaders, render passes, pipelines
VkResult vkCreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkShaderModule *pModule);
void vkDestroyShaderModule(VkDevice device, VkShaderModule module, const VkAllocationCallbacks *pAllocator);
VkResult vkCreateRenderPass(VkDevice device, const VkRenderPassCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkRenderPass *pRenderPass);
void vkDestroyRenderPass(VkDevice device, VkRenderPass renderPass, const VkAllocationCallbacks *pAllocator);
VkResult vkCreateFramebuffer(VkDevice device, const VkFramebufferCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkFramebuffer *pFramebuffer);
void vkDestroyFramebuffer(VkDevice device, VkFramebuffer framebuffer, const VkAllocationCallbacks *pAllocator);
VkResult vkCreateDescriptorSetLayout(VkDevice device, const VkDescriptorSetLayoutCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkDescriptorSetLayout *pLayout);
void vkDestroyDescriptorSetLayout(VkDevice device, VkDescriptorSetLayout layout, const VkAllocationCallbacks *pAllocator);
VkResult vkCreatePipelineLayout(VkDevice device, const VkPipelineLayoutCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkPipelineLayout *pLayout);
void vkDestroyPipelineLayout(VkDevice device, VkPipelineLayout layout, const VkAllocationCallbacks *pAllocator);
VkResult vkCreateDescriptorPool(VkDevice device, const VkDescriptorPoolCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkDescriptorPool *pPool);
void vkDestroyDescriptorPool(VkDevice device, VkDescriptorPool pool, const VkAllocationCallbacks *pAllocator);
VkResult vkAllocateDescriptorSets(VkDevice device, const VkDescriptorSetAllocateInfo *pAllocateInfo, VkDescriptorSet *pSets);
void vkUpdateDescriptorSets(VkDevice device, uint32_t writeCount, const VkWriteDescriptorSet *pWrites, uint32_t copyCount, const void *pCopies);
VkResult vkCreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t count, const VkGraphicsPipelineCreateInfo *pCreateInfos, const VkAllocationCallbacks *pAllocator, VkPipeline *pPipelines);
void vkDestroyPipeline(VkDevice device, VkPipeline pipeline, const VkAllocationCallbacks *pAllocator);

// synchronization (everything is synchronous)
VkResult vkCreateFence(VkDevice device, const void *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkFence *pFence);
void vkDestroyFence(VkDevice device, VkFence fence, const VkAllocationCallbacks *pAllocator);
VkResult vkWaitForFences(VkDevice device, uint32_t count, const VkFence *pFences, VkBool32 waitAll, uint64_t timeout);
VkResult vkResetFences(VkDevice device, uint32_t count, const VkFence *pFences);
VkResult vkGetFenceStatus(VkDevice device, VkFence fence);
VkResult vkCreateSemaphore(VkDevice device, const void *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkSemaphore *pSemaphore);
void vkDestroySemaphore(VkDevice device, VkSemaphore semaphore, const VkAllocationCallbacks *pAllocator);

// command buffers
VkResult vkCreateCommandPool(VkDevice device, const void *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkCommandPool *pPool);
void vkDestroyCommandPool(VkDevice device, VkCommandPool pool, const VkAllocationCallbacks *pAllocator);
VkResult vkResetCommandPool(VkDevice device, VkCommandPool pool, VkFlags flags);
VkResult vkAllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo *pAllocateInfo, VkCommandBuffer *pCommandBuffers);
void vkFreeCommandBuffers(VkDevice device, VkCommandPool pool, uint32_t count, const VkCommandBuffer *pCommandBuffers);
VkResult vkBeginCommandBuffer(VkCommandBuffer cb, const void *pBeginInfo);
VkResult vkEndCommandBuffer(VkCommandBuffer cb);
VkResult vkResetCommandBuffer(VkCommandBuffer cb, VkFlags flags);
void vkCmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo *pBegin, uint32_t contents);
void vkCmdEndRenderPass(VkCommandBuffer cb);
void vkCmdBindPipeline(VkCommandBuffer cb, uint32_t bindPoint, VkPipeline pipeline);
void vkCmdBindVertexBuffers(VkCommandBuffer cb, uint32_t first, uint32_t count, const VkBuffer *pBuffers, const VkDeviceSize *pOffsets);
void vkCmdBindIndexBuffer(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize offset, uint32_t indexType);
void vkCmdBindDescriptorSets(VkCommandBuffer cb, uint32_t bindPoint, VkPipelineLayout layout, uint32_t first, uint32_t count, const VkDescriptorSet *pSets, uint32_t dynCount, const uint32_t *pDynOffsets);
void vkCmdPushConstants(VkCommandBuffer cb, VkPipelineLayout layout, VkShaderStageFlags stages, uint32_t offset, uint32_t size, const void *pValues);
void vkCmdSetViewport(VkCommandBuffer cb, uint32_t first, uint32_t count, const VkViewport *pViewports);
void vkCmdSetScissor(VkCommandBuffer cb, uint32_t first, uint32_t count, const VkRect2D *pScissors);
void vkCmdDraw(VkCommandBuffer cb, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance);
void vkCmdDrawIndexed(VkCommandBuffer cb, uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance);
void vkCmdCopyBuffer(VkCommandBuffer cb, VkBuffer src, VkBuffer dst, uint32_t regionCount, const VkBufferCopy *pRegions);
// Plain row copies of 4-byte texels, outside a render pass (the layout argument is ignored).
void vkCmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer src, VkImage dst, uint32_t dstLayout, uint32_t regionCount, const VkBufferImageCopy *pRegions);
// Inside a render pass: fills rectangles of the first colour attachment.
void vkCmdClearAttachments(VkCommandBuffer cb, uint32_t attachmentCount, const VkClearAttachment *pAttachments, uint32_t rectCount, const VkClearRect *pRects);
VkResult vkQueueSubmit(VkQueue queue, uint32_t submitCount, const VkSubmitInfo *pSubmits, VkFence fence);

// presentation
VkResult vkCreateRadiumSurfaceEXT(VkInstance instance, const VkRadiumSurfaceCreateInfoEXT *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkSurfaceKHR *pSurface);
void vkDestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks *pAllocator);
void vkRadiumForgetDisplay(void);   // after a program was stopped without destroying its surface
VkResult vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkSwapchainKHR *pSwapchain);
void vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks *pAllocator);
VkResult vkGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain, uint32_t *pCount, VkImage *pImages);
VkResult vkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t *pImageIndex);
VkResult vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo);

// demo helper (not Vulkan): column-major model-view-projection matrix for a cube turning by `angle`
void vkRadiumDemoMatrix(float *out16, float angle, float aspect);

#endif // RADIUM_VULKAN_H
