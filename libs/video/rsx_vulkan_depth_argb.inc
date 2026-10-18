/* GPU-side A8R8G8B8 reading of a multisample Z24S8 depth target, for the draw
 * engine's depth_to_argb (rsx_draw_engine.h). The CPU path reads the expanded
 * depth samples back (vk_eng_depth_readback: expansion, then
 * vk_eng_scaled_read's texel choice), stores each one as rsx_depth_guest_z24
 * in guest memory as [v >> 16, v >> 8, v, 0] and decodes those bytes as
 * A8R8G8B8 (rsx_texture_decode). This pass computes the same 32-bit host texel
 * from the same expanded sample with the same two float roundings (`precise`
 * forbids contraction into a fused multiply-add) and writes it through an
 * R32_UINT view of an R8G8B8A8 image, so the texture is bit-identical while
 * nothing waits for the GPU. Single-sample depth stays on the CPU path.
 * Included after rsx_vulkan_msaa.inc; shares the synchronous backend queue. */
#include "rsx_depth_guest.h"

#define VK_DARGB_MAX_VARIANTS 16

static struct {
    VkDescriptorSetLayout set_layout;
    VkDescriptorPool pool;
    VkDescriptorSet spare[VK_ENG_MAX_OBJ];  /* sets of released textures, for reuse */
    u32 nspare;
    VkPipelineLayout layout;
    VkShaderModule vs;
    VkRenderPass pass;
    struct { u32 key; VkShaderModule fs; VkPipeline pipeline; } variant[VK_DARGB_MAX_VARIANTS];
    u32 nvariant;
} s_dargb;

static void vk_eng_depth_argb_shutdown(void)
{
    for (u32 i = 0; i < s_dargb.nvariant; ++i) {
        if (s_dargb.variant[i].pipeline) pvkDestroyPipeline(s_vk.device, s_dargb.variant[i].pipeline, NULL);
        if (s_dargb.variant[i].fs) pvkDestroyShaderModule(s_vk.device, s_dargb.variant[i].fs, NULL);
    }
    if (s_dargb.vs) pvkDestroyShaderModule(s_vk.device, s_dargb.vs, NULL);
    if (s_dargb.pass) pvkDestroyRenderPass(s_vk.device, s_dargb.pass, NULL);
    if (s_dargb.layout) pvkDestroyPipelineLayout(s_vk.device, s_dargb.layout, NULL);
    if (s_dargb.pool) pvkDestroyDescriptorPool(s_vk.device, s_dargb.pool, NULL);
    if (s_dargb.set_layout) pvkDestroyDescriptorSetLayout(s_vk.device, s_dargb.set_layout, NULL);
    memset(&s_dargb, 0, sizeof s_dargb);
}

/* key: raster scale k, sample grid gx/gy (vk_eng_scaled_read's texel choice),
 * bit 12 rsx_texture_argb_is_rgba, bit 13 D8R8G8B8 (alpha reads 255). */
static VkPipeline vk_eng_depth_argb_pipeline(u32 key)
{
    for (u32 i = 0; i < s_dargb.nvariant; ++i)
        if (s_dargb.variant[i].key == key) return s_dargb.variant[i].pipeline;
    if (s_dargb.nvariant == VK_DARGB_MAX_VARIANTS) return VK_NULL_HANDLE;
    if (!s_dargb.layout) {
        VkDescriptorSetLayoutBinding binding = { RSX_SPIRV_TEXTURE_BINDING, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                                 VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
        VkDescriptorSetLayoutCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                .bindingCount = 1, .pBindings = &binding };
        if (pvkCreateDescriptorSetLayout(s_vk.device, &sci, NULL, &s_dargb.set_layout) != VK_SUCCESS) return VK_NULL_HANDLE;
        VkDescriptorPoolSize size = { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_ENG_MAX_OBJ };
        VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                           .maxSets = VK_ENG_MAX_OBJ, .poolSizeCount = 1, .pPoolSizes = &size };
        if (pvkCreateDescriptorPool(s_vk.device, &pci, NULL, &s_dargb.pool) != VK_SUCCESS) return VK_NULL_HANDLE;
        VkPipelineLayoutCreateInfo lci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                           .setLayoutCount = 1, .pSetLayouts = &s_dargb.set_layout };
        if (pvkCreatePipelineLayout(s_vk.device, &lci, NULL, &s_dargb.layout) != VK_SUCCESS) return VK_NULL_HANDLE;
        if (vk_eng_translate(vk_msaa_expand_vs, RSX_SHADER_STAGE_VERTEX, &s_dargb.vs)) return VK_NULL_HANDLE;
        VkAttachmentDescription attachment = { .format = VK_FORMAT_R32_UINT, .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL };
        VkAttachmentReference color = { 0, VK_IMAGE_LAYOUT_GENERAL };
        VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                     .colorAttachmentCount = 1, .pColorAttachments = &color };
        VkRenderPassCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1,
                                       .pAttachments = &attachment, .subpassCount = 1, .pSubpasses = &sub };
        if (pvkCreateRenderPass(s_vk.device, &rci, NULL, &s_dargb.pass) != VK_SUCCESS) return VK_NULL_HANDLE;
    }
    const u32 k = key & 15u, gx = (key >> 4) & 15u, gy = (key >> 8) & 15u;
    const int rgba = (key >> 12) & 1u, d8 = (key >> 13) & 1u;
    /* Guest bytes A,R,G,B = v>>16, v>>8, v, 0; the decoder's host order. */
    const char* order = rgba ? (d8 ? "A,R,G,255u" : "A,R,G,B") : (d8 ? "R,G,B,255u" : "R,G,B,A");
    char fs[1024];
    snprintf(fs, sizeof fs,
        "Texture2D<float> source:register(t0);"
        "uint main(float4 pos:SV_Position):SV_Target0 {"
        "uint2 l=uint2(pos.xy);"
        "uint hx=(l.x/%uu)*%uu+%uu+l.x%%%uu;"
        "uint hy=(l.y/%uu)*%uu+%uu+l.y%%%uu;"
        "float d=source.Load(int3(hx,hy,0));"
        "if(!(d>0.0f)) d=0.0f;"
        "if(d>1.0f) d=1.0f;"
        "precise float s=d*16777215.0f;"
        "precise float r=s+0.5f;"
        "uint v=(uint)r;"
        "uint A=(v>>16)&255u,R=(v>>8)&255u,G=v&255u,B=0u;"
        "uint4 h=uint4(%s);"
        "return h.x|(h.y<<8)|(h.z<<16)|(h.w<<24);}",
        gx, gx * k, (k / 2u) * gx, gx, gy, gy * k, (k / 2u) * gy, gy, order);
    VkShaderModule module = VK_NULL_HANDLE;
    if (vk_eng_translate(fs, RSX_SHADER_STAGE_FRAGMENT, &module)) return VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = s_dargb.vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = module, .pName = "main" } };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                  .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                             .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo raster = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState blend = { .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                               .attachmentCount = 1, .pAttachments = &blend };
    VkDynamicState dynamic[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                            .dynamicStateCount = 2, .pDynamicStates = dynamic };
    VkGraphicsPipelineCreateInfo gci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2,
        .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp,
        .pRasterizationState = &raster, .pMultisampleState = &ms, .pColorBlendState = &cb, .pDynamicState = &dy,
        .layout = s_dargb.layout, .renderPass = s_dargb.pass };
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (pvkCreateGraphicsPipelines(s_vk.device, vk_pcache(), 1, &gci, NULL, &pipeline) != VK_SUCCESS) {
        pvkDestroyShaderModule(s_vk.device, module, NULL);
        return VK_NULL_HANDLE;
    }
    vk_pcache_created();
    s_dargb.variant[s_dargb.nvariant].key = key;
    s_dargb.variant[s_dargb.nvariant].fs = module;
    s_dargb.variant[s_dargb.nvariant].pipeline = pipeline;
    s_dargb.nvariant++;
    return pipeline;
}

/* A texture's conversion objects go with it. Pending passes may use them. */
static void vk_eng_depth_argb_forget(vk_eng_obj* o)
{
    if (!o->argb_fb && !o->argb_view && !o->argb_set) return;
    vk_flush_pending(1);
    if (o->argb_fb) pvkDestroyFramebuffer(s_vk.device, o->argb_fb, NULL);
    if (o->argb_view) pvkDestroyImageView(s_vk.device, o->argb_view, NULL);
    /* After vk_eng_depth_argb_shutdown the pool (and with it the set) is gone. */
    if (o->argb_set && s_dargb.pool && s_dargb.nspare < VK_ENG_MAX_OBJ)
        s_dargb.spare[s_dargb.nspare++] = o->argb_set;
    o->argb_fb = VK_NULL_HANDLE; o->argb_view = VK_NULL_HANDLE; o->argb_set = VK_NULL_HANDLE; o->argb_src = 0;
}

/* An R8G8B8A8 texture with the unit's crossbar for sampling and an R32_UINT
 * view the conversion pass renders into. */
static u32 vk_eng_depth_argb_texture(u32 w, u32 h, u32 remap, u32 rsx_fmt)
{
    const u32 hd = vk_eng_alloc();
    if (!hd) return 0;
    vk_eng_obj* o = &s_eobj[hd];
    VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { w, h, 1 }, .mipLevels = 1,
        .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkMemoryRequirements req;
    u32 type;
    if (pvkCreateImage(s_vk.device, &ici, NULL, &o->im.img) != VK_SUCCESS) goto fail;
    pvkGetImageMemoryRequirements(s_vk.device, o->im.img, &req);
    if (vk_find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type) &&
        vk_find_memory_type(req.memoryTypeBits, 0, &type)) goto fail;
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size, .memoryTypeIndex = type };
    if (pvkAllocateMemory(s_vk.device, &mai, NULL, &o->im.mem) != VK_SUCCESS ||
        pvkBindImageMemory(s_vk.device, o->im.img, o->im.mem, 0) != VK_SUCCESS ||
        vk_eng_make_view(o->im.img, VK_FORMAT_R8G8B8A8_UNORM, 1, vk_remap(remap, rsx_fmt), 0, &o->im.view))
        goto fail;
    VkComponentMapping identity = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                    VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
    if (vk_eng_make_view(o->im.img, VK_FORMAT_R32_UINT, 1, identity, 0, &o->argb_view)) goto fail;
    VkFramebufferCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = s_dargb.pass,
                                    .attachmentCount = 1, .pAttachments = &o->argb_view, .width = w, .height = h, .layers = 1 };
    if (pvkCreateFramebuffer(s_vk.device, &fci, NULL, &o->argb_fb) != VK_SUCCESS) goto fail;
    if (s_dargb.nspare) o->argb_set = s_dargb.spare[--s_dargb.nspare];
    else {
        VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                           .descriptorPool = s_dargb.pool, .descriptorSetCount = 1,
                                           .pSetLayouts = &s_dargb.set_layout };
        if (pvkAllocateDescriptorSets(s_vk.device, &ai, &o->argb_set) != VK_SUCCESS) { o->argb_set = VK_NULL_HANDLE; goto fail; }
    }
    o->kind = VK_ENG_TEXTURE; o->fmt = VK_FORMAT_R8G8B8A8_UNORM; o->bpp = 4; o->w = w; o->h = h;
    o->remap = remap; o->rsx_fmt = rsx_fmt;
    if (vk_begin()) goto fail;
    vk_barrier_image(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    if (vk_submit_or_defer()) goto fail;
    return hd;
fail:
    vk_eng_depth_argb_forget(o);
    vk_destroy_image(&o->im);
    memset(o, 0, sizeof *o);
    return 0;
}

static u32 vk_eng_depth_to_argb(void* user, u32 depth, u32 w, u32 h, u32 remap, u32 rsx_fmt, u32 flags)
{
    vk_batch_configure();
    /* Everything here is recorded behind the pending draws. */
    if (!s_batch.defer) vk_flush_pending(0);
    vk_eng_obj* z = vk_eng_get(depth, VK_ENG_DEPTH);
    const u32 base = rsx_fmt & RSX_TEX_FMT_BASE_MASK;
    if (!z || (z->samples != 2 && z->samples != 4) || !w || !h || (base != 0x85u && base != 0x9Eu)) return 0;
    const u32 src_handle = vk_eng_expand_samples(depth);
    vk_eng_obj* src = vk_eng_get(src_handle, VK_ENG_TEXTURE);
    if (!src) return 0;
    z = vk_eng_get(depth, VK_ENG_DEPTH);
    const u32 k = src->scale ? src->scale : 1u, gx = src->grid_x ? src->grid_x : 1u, gy = src->grid_y ? src->grid_y : 1u;
    if (k > 15u || gx > 2u || gy > 2u || w > src->w / k || h > src->h / k) return 0;
    const u32 key = k | (gx << 4) | (gy << 8) | ((flags & 1u) << 12) | ((base == 0x9Eu ? 1u : 0u) << 13);
    const VkPipeline pipeline = vk_eng_depth_argb_pipeline(key);
    if (!pipeline) {
        static int warned;
        if (!warned) { VK_LOG("engine: depth-as-ARGB pass unavailable; storing through guest memory\n"); warned = 1; }
        return 0;
    }
    vk_eng_obj* t = vk_eng_get(z->argb, VK_ENG_TEXTURE);
    if (!t || t->w != w || t->h != h || t->remap != remap || t->rsx_fmt != rsx_fmt) {
        if (t) vk_eng_release(user, z->argb);
        z->argb = vk_eng_depth_argb_texture(w, h, remap, rsx_fmt);
        t = vk_eng_get(z->argb, VK_ENG_TEXTURE);
        if (!t) { z->argb = 0; return 0; }
    }
    if (t->argb_src != src_handle) {
        VkDescriptorImageInfo info = { VK_NULL_HANDLE, src->im.view, VK_IMAGE_LAYOUT_GENERAL };
        VkWriteDescriptorSet write = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = t->argb_set,
            .dstBinding = RSX_SPIRV_TEXTURE_BINDING, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &info };
        /* The set may still be used by a pending pass. */
        if (t->argb_src) vk_flush_pending(0);
        pvkUpdateDescriptorSets(s_vk.device, 1, &write, 0, NULL);
        t->argb_src = src_handle;
    }
    if (vk_begin()) return 0;
    vk_barrier_image(src->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    vk_barrier_image(t->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    VkRenderPassBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = s_dargb.pass,
                                 .framebuffer = t->argb_fb, .renderArea = { { 0, 0 }, { w, h } } };
    pvkCmdBeginRenderPass(s_vk.cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
    pvkCmdBindPipeline(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    VkViewport viewport = { 0, 0, (float)w, (float)h, 0, 1 };
    VkRect2D scissor = { { 0, 0 }, { w, h } };
    pvkCmdSetViewport(s_vk.cmd, 0, 1, &viewport);
    pvkCmdSetScissor(s_vk.cmd, 0, 1, &scissor);
    pvkCmdBindDescriptorSets(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_dargb.layout, 0, 1, &t->argb_set, 0, NULL);
    pvkCmdDraw(s_vk.cmd, 3, 1, 0, 0);
    pvkCmdEndRenderPass(s_vk.cmd);
    if (vk_submit_or_defer()) return 0;
    return z->argb;
}
