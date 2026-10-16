/* Native Vulkan FXAA postprocess pass. Text-included by rsx_vulkan_backend.c
 * after the resource/command helpers. Renders s_vk.color into a private
 * persistent RGBA8 output image; the source image is never written. */

#include "rsx_fxaa_spv.h"

static struct {
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
    u32 width, height;
    int ready;
} s_fxaa;

static void vk_fxaa_shutdown(void)
{
    VkDevice d = s_vk.device;
    if (d) {
        if (s_fxaa.pipe) pvkDestroyPipeline(d, s_fxaa.pipe, NULL);
        if (s_fxaa.fb) pvkDestroyFramebuffer(d, s_fxaa.fb, NULL);
        if (s_fxaa.layout) pvkDestroyPipelineLayout(d, s_fxaa.layout, NULL);
        if (s_fxaa.rp) pvkDestroyRenderPass(d, s_fxaa.rp, NULL);
        if (s_fxaa.vs) pvkDestroyShaderModule(d, s_fxaa.vs, NULL);
        if (s_fxaa.fs) pvkDestroyShaderModule(d, s_fxaa.fs, NULL);
        if (s_fxaa.pool) pvkDestroyDescriptorPool(d, s_fxaa.pool, NULL);
        if (s_fxaa.dsl) pvkDestroyDescriptorSetLayout(d, s_fxaa.dsl, NULL);
        if (s_fxaa.sampler) pvkDestroySampler(d, s_fxaa.sampler, NULL);
        vk_destroy_image(&s_fxaa.out);
    }
    memset(&s_fxaa, 0, sizeof(s_fxaa));
}

static int vk_fxaa_init(void)
{
    VkDevice d = s_vk.device;
    const VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
    VkFormatProperties fp;
    VkPhysicalDeviceProperties props;
    VkResult r;
    u32 w = s_vk.width, h = s_vk.height;

    if (s_fxaa.ready) {
        if (s_fxaa.width == w && s_fxaa.height == h && s_fxaa.src_view == s_vk.color.view)
            return 0;
        VK_LOG("fxaa: init with changed target %ux%u (have %ux%u); shutdown first\n",
               w, h, s_fxaa.width, s_fxaa.height);
        return -1;
    }
    vk_fxaa_shutdown();

    if (!d || !s_vk.phys || !s_vk.color.view || !s_vk.color.img) {
        VK_LOG("fxaa: missing device or input image\n");
        return -1;
    }
    if (w == 0 || h == 0) {
        VK_LOG("fxaa: zero dimensions %ux%u\n", w, h);
        return -1;
    }
    pvkGetPhysicalDeviceProperties(s_vk.phys, &props);
    if (w > props.limits.maxImageDimension2D || h > props.limits.maxImageDimension2D) {
        VK_LOG("fxaa: %ux%u exceeds maxImageDimension2D %u\n", w, h,
               props.limits.maxImageDimension2D);
        return -1;
    }
    pvkGetPhysicalDeviceFormatProperties(s_vk.phys, fmt, &fp);
    if ((fp.optimalTilingFeatures & need) != need) {
        VK_LOG("fxaa: RGBA8_UNORM lacks required optimal tiling features (0x%x)\n",
               (unsigned)fp.optimalTilingFeatures);
        return -1;
    }

    if (vk_create_image(&s_fxaa.out, fmt, w, h,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT) != 0) {
        VK_LOG("fxaa: output image creation failed\n");
        goto fail;
    }

    {
        VkAttachmentDescription att = {0};
        VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sub = {0};
        VkSubpassDependency deps[2];
        VkRenderPassCreateInfo ci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
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
        if ((r = pvkCreateRenderPass(d, &ci, NULL, &s_fxaa.rp)) != VK_SUCCESS) {
            VK_LOG("fxaa: vkCreateRenderPass failed (%d)\n", (int)r);
            goto fail;
        }
    }
    {
        VkFramebufferCreateInfo ci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        ci.renderPass = s_fxaa.rp;
        ci.attachmentCount = 1;
        ci.pAttachments = &s_fxaa.out.view;
        ci.width = w;
        ci.height = h;
        ci.layers = 1;
        if ((r = pvkCreateFramebuffer(d, &ci, NULL, &s_fxaa.fb)) != VK_SUCCESS) {
            VK_LOG("fxaa: vkCreateFramebuffer failed (%d)\n", (int)r);
            goto fail;
        }
    }
    {
        VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        ci.codeSize = sizeof(rsx_fxaa_vs_spv);
        ci.pCode = rsx_fxaa_vs_spv;
        if ((r = pvkCreateShaderModule(d, &ci, NULL, &s_fxaa.vs)) != VK_SUCCESS) {
            VK_LOG("fxaa: vertex vkCreateShaderModule failed (%d)\n", (int)r);
            goto fail;
        }
        ci.codeSize = sizeof(rsx_fxaa_fs_spv);
        ci.pCode = rsx_fxaa_fs_spv;
        if ((r = pvkCreateShaderModule(d, &ci, NULL, &s_fxaa.fs)) != VK_SUCCESS) {
            VK_LOG("fxaa: fragment vkCreateShaderModule failed (%d)\n", (int)r);
            goto fail;
        }
    }
    {
        VkSamplerCreateInfo ci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
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
        if ((r = pvkCreateSampler(d, &ci, NULL, &s_fxaa.sampler)) != VK_SUCCESS) {
            VK_LOG("fxaa: vkCreateSampler failed (%d)\n", (int)r);
            goto fail;
        }
    }
    {
        VkDescriptorSetLayoutBinding b = {0};
        VkDescriptorSetLayoutCreateInfo ci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
        VkDescriptorPoolCreateInfo pci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        VkDescriptorImageInfo ii;
        VkWriteDescriptorSet wr = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        b.binding = 0;
        b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        ci.bindingCount = 1;
        ci.pBindings = &b;
        if ((r = pvkCreateDescriptorSetLayout(d, &ci, NULL, &s_fxaa.dsl)) != VK_SUCCESS) {
            VK_LOG("fxaa: vkCreateDescriptorSetLayout failed (%d)\n", (int)r);
            goto fail;
        }
        pci.maxSets = 1;
        pci.poolSizeCount = 1;
        pci.pPoolSizes = &ps;
        if ((r = pvkCreateDescriptorPool(d, &pci, NULL, &s_fxaa.pool)) != VK_SUCCESS) {
            VK_LOG("fxaa: vkCreateDescriptorPool failed (%d)\n", (int)r);
            goto fail;
        }
        ai.descriptorPool = s_fxaa.pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &s_fxaa.dsl;
        if ((r = pvkAllocateDescriptorSets(d, &ai, &s_fxaa.set)) != VK_SUCCESS) {
            VK_LOG("fxaa: vkAllocateDescriptorSets failed (%d)\n", (int)r);
            goto fail;
        }
        ii.sampler = s_fxaa.sampler;
        ii.imageView = s_vk.color.view;
        ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        wr.dstSet = s_fxaa.set;
        wr.dstBinding = 0;
        wr.descriptorCount = 1;
        wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr.pImageInfo = &ii;
        pvkUpdateDescriptorSets(d, 1, &wr, 0, NULL);
    }
    {
        VkPushConstantRange pc = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8 };
        VkPipelineLayoutCreateInfo ci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        ci.setLayoutCount = 1;
        ci.pSetLayouts = &s_fxaa.dsl;
        ci.pushConstantRangeCount = 1;
        ci.pPushConstantRanges = &pc;
        if ((r = pvkCreatePipelineLayout(d, &ci, NULL, &s_fxaa.layout)) != VK_SUCCESS) {
            VK_LOG("fxaa: vkCreatePipelineLayout failed (%d)\n", (int)r);
            goto fail;
        }
    }
    {
        VkPipelineShaderStageCreateInfo st[2];
        VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        VkPipelineColorBlendAttachmentState cba = {0};
        VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        VkGraphicsPipelineCreateInfo ci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        memset(st, 0, sizeof(st));
        st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        st[0].module = s_fxaa.vs;
        st[0].pName = "main";
        st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        st[1].module = s_fxaa.fs;
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
        ci.layout = s_fxaa.layout;
        ci.renderPass = s_fxaa.rp;
        ci.subpass = 0;
        if ((r = pvkCreateGraphicsPipelines(d, VK_NULL_HANDLE, 1, &ci, NULL, &s_fxaa.pipe)) != VK_SUCCESS) {
            VK_LOG("fxaa: vkCreateGraphicsPipelines failed (%d)\n", (int)r);
            goto fail;
        }
    }

    if (vk_begin() != 0) {
        VK_LOG("fxaa: begin for output layout init failed\n");
        goto fail;
    }
    vk_barrier_image(s_fxaa.out.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    if (vk_submit_and_wait() != 0) {
        VK_LOG("fxaa: output layout init submit failed\n");
        goto fail;
    }

    s_fxaa.src_view = s_vk.color.view;
    s_fxaa.width = w;
    s_fxaa.height = h;
    s_fxaa.ready = 1;
    return 0;

fail:
    vk_fxaa_shutdown();
    return -1;
}

static int vk_fxaa_apply(VkImage* output)
{
    VkRenderPassBeginInfo rb = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    VkViewport vp;
    VkRect2D sc;
    float rcp[2];

    if (!output) {
        VK_LOG("fxaa: apply with null output\n");
        return -1;
    }
    if (!s_fxaa.ready || !s_vk.device || !s_vk.color.img) {
        VK_LOG("fxaa: apply before successful init\n");
        return -1;
    }
    if (s_fxaa.width != s_vk.width || s_fxaa.height != s_vk.height ||
        s_fxaa.src_view != s_vk.color.view) {
        VK_LOG("fxaa: target changed (%ux%u, have %ux%u); reinit required\n",
               s_vk.width, s_vk.height, s_fxaa.width, s_fxaa.height);
        return -1;
    }

    if (vk_begin() != 0) {
        VK_LOG("fxaa: vk_begin failed\n");
        return -1;
    }
    vk_barrier_image(s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    vk_barrier_image(s_fxaa.out.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);

    rb.renderPass = s_fxaa.rp;
    rb.framebuffer = s_fxaa.fb;
    rb.renderArea.offset.x = 0;
    rb.renderArea.offset.y = 0;
    rb.renderArea.extent.width = s_fxaa.width;
    rb.renderArea.extent.height = s_fxaa.height;
    pvkCmdBeginRenderPass(s_vk.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    pvkCmdBindPipeline(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_fxaa.pipe);
    pvkCmdBindDescriptorSets(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_fxaa.layout,
                             0, 1, &s_fxaa.set, 0, NULL);
    vp.x = 0.0f;
    vp.y = 0.0f;
    vp.width = (float)s_fxaa.width;
    vp.height = (float)s_fxaa.height;
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    pvkCmdSetViewport(s_vk.cmd, 0, 1, &vp);
    sc = rb.renderArea;
    pvkCmdSetScissor(s_vk.cmd, 0, 1, &sc);
    rcp[0] = 1.0f / (float)s_fxaa.width;
    rcp[1] = 1.0f / (float)s_fxaa.height;
    pvkCmdPushConstants(s_vk.cmd, s_fxaa.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                        sizeof(rcp), rcp);
    pvkCmdDraw(s_vk.cmd, 3, 1, 0, 0);
    pvkCmdEndRenderPass(s_vk.cmd);

    if (vk_submit_and_wait() != 0) {
        VK_LOG("fxaa: submit failed\n");
        return -1;
    }
    *output = s_fxaa.out.img;
    return 0;
}
