/* AMD FSR1 EASU/RCAS passes, included after synchronous Vulkan helpers.
 * Source images remain untouched. Each private pass has independent resources.
 * Both draws share one command submission and an explicit image dependency. */

#include "rsx_fsr1_spv.h"
#include "rsx_fsr1_constants.h"

typedef struct vk_fsr1_pass {
    vk_image out;
    VkRenderPass rp;
    VkFramebuffer fb;
    VkShaderModule vs, fs;
    VkSampler sampler;
    VkDescriptorSetLayout dsl;
    VkDescriptorPool pool;
    VkDescriptorSet set;
    VkPipelineLayout layout;
    VkPipeline pipe;
    VkImageView src_view;
    u32 width, height, input_width, input_height;
    int ready;
} vk_fsr1_pass;
static vk_fsr1_pass s_fsr1[2];

static void vk_fsr1_pass_shutdown(vk_fsr1_pass* p)
{
    VkDevice d = s_vk.device;
    if (d) {
        if (p->pipe) pvkDestroyPipeline(d, p->pipe, NULL);
        if (p->fb) pvkDestroyFramebuffer(d, p->fb, NULL);
        if (p->layout) pvkDestroyPipelineLayout(d, p->layout, NULL);
        if (p->rp) pvkDestroyRenderPass(d, p->rp, NULL);
        if (p->vs) pvkDestroyShaderModule(d, p->vs, NULL);
        if (p->fs) pvkDestroyShaderModule(d, p->fs, NULL);
        if (p->pool) pvkDestroyDescriptorPool(d, p->pool, NULL);
        if (p->dsl) pvkDestroyDescriptorSetLayout(d, p->dsl, NULL);
        if (p->sampler) pvkDestroySampler(d, p->sampler, NULL);
        vk_destroy_image(&p->out);
    }
    memset(p, 0, sizeof(*p));
}

static int vk_fsr1_pass_init(vk_fsr1_pass* p, const vk_image* src,
    u32 iw, u32 ih, u32 w, u32 h, const uint32_t* fragment, size_t fragment_bytes,
    u32 push_bytes)
{
    VkDevice d = s_vk.device;
    const VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT;
    VkFormatProperties fp;
    VkPhysicalDeviceProperties props;
    VkResult r;
    if (!src) return -1;

    if (p->ready) {
        if (p->width == w && p->height == h && p->src_view == src->view &&
            p->input_width == iw && p->input_height == ih)
            return 0;
        VK_LOG("fsr1: init with changed target %ux%u (have %ux%u); shutdown first\n",
               w, h, p->width, p->height);
        return -1;
    }
    vk_fsr1_pass_shutdown(p);

    if (!d || !s_vk.phys || !src->view || !src->img) {
        VK_LOG("fsr1: missing device or input image\n");
        return -1;
    }
    if (w == 0 || h == 0 || iw == 0 || ih == 0) {
        VK_LOG("fsr1: zero dimensions %ux%u\n", w, h);
        return -1;
    }
    pvkGetPhysicalDeviceProperties(s_vk.phys, &props);
    if (w > props.limits.maxImageDimension2D || h > props.limits.maxImageDimension2D ||
        iw > props.limits.maxImageDimension2D || ih > props.limits.maxImageDimension2D ||
        push_bytes > props.limits.maxPushConstantsSize) {
        VK_LOG("fsr1: %ux%u exceeds maxImageDimension2D %u\n", w, h,
               props.limits.maxImageDimension2D);
        return -1;
    }
    pvkGetPhysicalDeviceFormatProperties(s_vk.phys, fmt, &fp);
    if ((fp.optimalTilingFeatures & need) != need) {
        VK_LOG("fsr1: RGBA8_UNORM lacks required optimal tiling features (0x%x)\n",
               (unsigned)fp.optimalTilingFeatures);
        return -1;
    }

    if (vk_create_image(&p->out, fmt, w, h,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT) != 0) {
        VK_LOG("fsr1: output image creation failed\n");
        goto fail;
    }

    {
        VkAttachmentDescription att = {0};
        VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sub = {0};
        VkSubpassDependency deps[2];
        VkRenderPassCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        att.format = fmt;
        att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
        att.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &ref;
        memset(deps, 0, sizeof(deps));
        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass = 0;
        deps[0].srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        deps[0].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        deps[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].srcSubpass = 0;
        deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT |
                                VK_ACCESS_MEMORY_READ_BIT;
        ci.attachmentCount = 1;
        ci.pAttachments = &att;
        ci.subpassCount = 1;
        ci.pSubpasses = &sub;
        ci.dependencyCount = 2;
        ci.pDependencies = deps;
        if ((r = pvkCreateRenderPass(d, &ci, NULL, &p->rp)) != VK_SUCCESS) {
            VK_LOG("fsr1: vkCreateRenderPass failed (%d)\n", (int)r);
            goto fail;
        }
    }
    {
        VkFramebufferCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        ci.renderPass = p->rp;
        ci.attachmentCount = 1;
        ci.pAttachments = &p->out.view;
        ci.width = w;
        ci.height = h;
        ci.layers = 1;
        if ((r = pvkCreateFramebuffer(d, &ci, NULL, &p->fb)) != VK_SUCCESS) {
            VK_LOG("fsr1: vkCreateFramebuffer failed (%d)\n", (int)r);
            goto fail;
        }
    }
    {
        VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        ci.codeSize = sizeof(rsx_fsr1_vs_spv);
        ci.pCode = rsx_fsr1_vs_spv;
        if ((r = pvkCreateShaderModule(d, &ci, NULL, &p->vs)) != VK_SUCCESS) {
            VK_LOG("fsr1: vertex vkCreateShaderModule failed (%d)\n", (int)r);
            goto fail;
        }
        ci.codeSize = fragment_bytes;
        ci.pCode = fragment;
        if ((r = pvkCreateShaderModule(d, &ci, NULL, &p->fs)) != VK_SUCCESS) {
            VK_LOG("fsr1: fragment vkCreateShaderModule failed (%d)\n", (int)r);
            goto fail;
        }
    }
    {
        VkSamplerCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        ci.magFilter = VK_FILTER_LINEAR;
        ci.minFilter = VK_FILTER_LINEAR;
        ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.anisotropyEnable = VK_FALSE;
        ci.maxAnisotropy = 1.0f;
        ci.compareEnable = VK_FALSE;
        ci.compareOp = VK_COMPARE_OP_ALWAYS;
        ci.minLod = 0.0f;
        ci.maxLod = 0.0f;
        ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        ci.unnormalizedCoordinates = VK_FALSE;
        if ((r = pvkCreateSampler(d, &ci, NULL, &p->sampler)) != VK_SUCCESS) {
            VK_LOG("fsr1: vkCreateSampler failed (%d)\n", (int)r);
            goto fail;
        }
    }
    {
        VkDescriptorSetLayoutBinding b = {0};
        VkDescriptorSetLayoutCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
        VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        VkDescriptorImageInfo ii;
        VkWriteDescriptorSet wr = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        b.binding = 0;
        b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        ci.bindingCount = 1;
        ci.pBindings = &b;
        if ((r = pvkCreateDescriptorSetLayout(d, &ci, NULL, &p->dsl)) != VK_SUCCESS) {
            VK_LOG("fsr1: vkCreateDescriptorSetLayout failed (%d)\n", (int)r);
            goto fail;
        }
        pci.maxSets = 1;
        pci.poolSizeCount = 1;
        pci.pPoolSizes = &ps;
        if ((r = pvkCreateDescriptorPool(d, &pci, NULL, &p->pool)) != VK_SUCCESS) {
            VK_LOG("fsr1: vkCreateDescriptorPool failed (%d)\n", (int)r);
            goto fail;
        }
        ai.descriptorPool = p->pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &p->dsl;
        if ((r = pvkAllocateDescriptorSets(d, &ai, &p->set)) != VK_SUCCESS) {
            VK_LOG("fsr1: vkAllocateDescriptorSets failed (%d)\n", (int)r);
            goto fail;
        }
        ii.sampler = p->sampler;
        ii.imageView = src->view;
        ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        wr.dstSet = p->set;
        wr.dstBinding = 0;
        wr.descriptorCount = 1;
        wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr.pImageInfo = &ii;
        pvkUpdateDescriptorSets(d, 1, &wr, 0, NULL);
    }
    {
        VkPushConstantRange pc = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, push_bytes };
        VkPipelineLayoutCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        ci.setLayoutCount = 1;
        ci.pSetLayouts = &p->dsl;
        ci.pushConstantRangeCount = 1;
        ci.pPushConstantRanges = &pc;
        if ((r = pvkCreatePipelineLayout(d, &ci, NULL, &p->layout)) != VK_SUCCESS) {
            VK_LOG("fsr1: vkCreatePipelineLayout failed (%d)\n", (int)r);
            goto fail;
        }
    }
    {
        VkPipelineShaderStageCreateInfo st[2];
        VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        VkPipelineColorBlendAttachmentState cba = {0};
        VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        VkGraphicsPipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        memset(st, 0, sizeof(st));
        st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        st[0].module = p->vs;
        st[0].pName = "main";
        st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        st[1].module = p->fs;
        st[1].pName = "main";
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        cba.blendEnable = VK_FALSE;
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        cb.attachmentCount = 1;
        cb.pAttachments = &cba;
        ds.dynamicStateCount = 2;
        ds.pDynamicStates = dyn;
        ci.stageCount = 2;
        ci.pStages = st;
        ci.pVertexInputState = &vi;
        ci.pInputAssemblyState = &ia;
        ci.pViewportState = &vp;
        ci.pRasterizationState = &rs;
        ci.pMultisampleState = &ms;
        ci.pColorBlendState = &cb;
        ci.pDynamicState = &ds;
        ci.layout = p->layout;
        ci.renderPass = p->rp;
        ci.subpass = 0;
        if ((r = pvkCreateGraphicsPipelines(d, vk_pcache(), 1, &ci, NULL, &p->pipe)) != VK_SUCCESS) {
            VK_LOG("fsr1: vkCreateGraphicsPipelines failed (%d)\n", (int)r);
            goto fail;
        }
    }

    if (vk_begin() != 0) {
        VK_LOG("fsr1: begin for output layout init failed\n");
        goto fail;
    }
    vk_barrier_image(p->out.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    if (vk_submit_and_wait() != 0) {
        VK_LOG("fsr1: output layout init submit failed\n");
        goto fail;
    }

    p->src_view = src->view;
    p->input_width = iw;
    p->input_height = ih;
    p->width = w;
    p->height = h;
    p->ready = 1;
    return 0;

fail:
    vk_fsr1_pass_shutdown(p);
    return -1;
}

static void vk_fsr1_shutdown(void)
{
    vk_fsr1_pass_shutdown(&s_fsr1[1]);
    vk_fsr1_pass_shutdown(&s_fsr1[0]);
}

/* Called only after successful init and vk_begin, on the renderer owner thread. */
static void vk_fsr1_record(vk_fsr1_pass* p, VkImage source,
    const void* constants, u32 bytes)
{
    VkRenderPassBeginInfo rb = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    VkViewport vp = {0.f, 0.f, (float)p->width, (float)p->height, 0.f, 1.f};
    vk_barrier_image(source, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    vk_barrier_image(p->out.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    rb.renderPass = p->rp;
    rb.framebuffer = p->fb;
    rb.renderArea.extent.width = p->width;
    rb.renderArea.extent.height = p->height;
    pvkCmdBeginRenderPass(s_vk.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    pvkCmdBindPipeline(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pipe);
    pvkCmdBindDescriptorSets(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->layout,
        0, 1, &p->set, 0, NULL);
    pvkCmdSetViewport(s_vk.cmd, 0, 1, &vp);
    pvkCmdSetScissor(s_vk.cmd, 0, 1, &rb.renderArea);
    pvkCmdPushConstants(s_vk.cmd, p->layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, bytes, constants);
    pvkCmdDraw(s_vk.cmd, 3, 1, 0, 0);
    pvkCmdEndRenderPass(s_vk.cmd);
}

/* Point a ready pass at another source of the same size. The game flips
 * between display buffers, so the source alternates every frame; rebuilding
 * every pass object for that cost a pipeline and a GPU wait per frame. The
 * backend waits for each submission, so the set is not in use here. */
static void vk_fsr1_rebind(vk_fsr1_pass* p, const vk_image* src)
{
    VkDescriptorImageInfo ii = { p->sampler, src->view, VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet wr = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = p->set,
        .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &ii };
    pvkUpdateDescriptorSets(s_vk.device, 1, &wr, 0, NULL);
    p->src_view = src->view;
}

/* Returns -1 without changing *output on failure. Caller may use the original
 * image and its ordinary presentation filter. No guest image is overwritten. */
static int vk_fsr1_apply(const vk_image* source, u32 iw, u32 ih, u32 ow, u32 oh,
    float sharpness, VkImage* output)
{
    rsx_fsr1_constants constants;
    if (!output || !source || !source->img || !source->view || !s_vk.device ||
        source->img == s_fsr1[0].out.img || source->img == s_fsr1[1].out.img ||
        rsx_fsr1_make_constants(&constants, iw, ih, ow, oh, sharpness))
        return -1;
    vk_fsr1_pass* p = &s_fsr1[0];
    if (p->ready && s_fsr1[1].ready && p->width == ow && p->height == oh &&
        p->input_width == iw && p->input_height == ih && p->src_view != source->view)
        vk_fsr1_rebind(p, source);
    if (!p->ready || !s_fsr1[1].ready || p->width != ow || p->height != oh ||
        p->input_width != iw || p->input_height != ih || p->src_view != source->view) {
        /* The backend waits for every draw/blit submission before returning.
         * Its old postprocess resources are therefore no longer in flight. */
        vk_fsr1_shutdown();
        if (vk_fsr1_pass_init(&s_fsr1[0], source, iw, ih, ow, oh,
                rsx_fsr1_easu_spv, sizeof(rsx_fsr1_easu_spv), sizeof(constants.easu)) ||
            vk_fsr1_pass_init(&s_fsr1[1], &s_fsr1[0].out, ow, oh, ow, oh,
                rsx_fsr1_rcas_spv, sizeof(rsx_fsr1_rcas_spv), sizeof(constants.rcas))) {
            vk_fsr1_shutdown();
            return -1;
        }
    }
    if (vk_begin()) return -1;
    vk_fsr1_record(&s_fsr1[0], source->img, constants.easu, sizeof(constants.easu));
    vk_fsr1_record(&s_fsr1[1], s_fsr1[0].out.img, constants.rcas, sizeof(constants.rcas));
    if (vk_submit_and_wait()) return -1;
    *output = s_fsr1[1].out.img;
    return 0;
}

/* RCAS alone at display resolution (PS3RECOMP_SHARPEN), for frames that need
 * no FSR1 upscale: native output, a downscaled render scale, or DLSS output
 * stretched by the ordinary blit. Same AMD RCAS kernel as FSR1's second pass. */
static vk_fsr1_pass s_rcas;
static void vk_rcas_shutdown(void) { vk_fsr1_pass_shutdown(&s_rcas); }
static int vk_rcas_apply(const vk_image* source, u32 w, u32 h, float sharpness, VkImage* output)
{
    rsx_fsr1_constants constants;
    if (!output || !source || !source->img || !source->view || !s_vk.device ||
        source->img == s_rcas.out.img || rsx_fsr1_make_constants(&constants, w, h, w, h, sharpness))
        return -1;
    if (s_rcas.ready && s_rcas.width == w && s_rcas.height == h && s_rcas.src_view != source->view)
        vk_fsr1_rebind(&s_rcas, source);
    if (!s_rcas.ready || s_rcas.width != w || s_rcas.height != h || s_rcas.src_view != source->view) {
        vk_rcas_shutdown();
        if (vk_fsr1_pass_init(&s_rcas, source, w, h, w, h,
                rsx_fsr1_rcas_spv, sizeof(rsx_fsr1_rcas_spv), sizeof(constants.rcas))) {
            vk_rcas_shutdown();
            return -1;
        }
    }
    if (vk_begin()) return -1;
    vk_fsr1_record(&s_rcas, source->img, constants.rcas, sizeof(constants.rcas));
    if (vk_submit_and_wait()) return -1;
    *output = s_rcas.out.img;
    return 0;
}

/* PS3RECOMP_SHARPEN strength 0..1 as RCAS stops (0 strongest): 1 -> 0,
 * 0.9 -> 0.2 (FSR1's default here), 0.5 -> 1.0. Unset: FSR1 keeps 0.2 and
 * no separate RCAS pass runs. Returns the stops, or -1 for off. */
static float vk_sharpen_stops(void)
{
    static int parsed; static float stops = -1.0f;
    if (!parsed) {
        parsed = 1;
        const char* v = getenv("PS3RECOMP_SHARPEN");
        if (v && *v) {
            char* end = NULL; float s = strtof(v, &end);
            if (!end || *end || !isfinite(s) || s < 0.0f || s > 1.0f)
                VK_LOG("PS3RECOMP_SHARPEN must be 0..1; sharpening off\n");
            else if (s > 0.0f) stops = 2.0f * (1.0f - s);
            VK_LOG("sharpening: %s (RCAS %.2f stops)\n", stops >= 0 ? "on" : "off", stops >= 0 ? stops : 0.0f);
        }
    }
    return stops;
}
