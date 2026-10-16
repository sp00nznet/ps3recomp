/* Fullscreen AO helper: nearest-depth resolve (min over grid) and AO color composite.
 * Included after vk_eng_translate in rsx_vulkan_backend.c. All images stay in GENERAL. */

#define VK_AO_SLOT_COUNT 3

static struct {
    int base_ready;
    VkDescriptorSetLayout set_layout;
    VkDescriptorPool pool;
    VkDescriptorSet set;
    VkPipelineLayout pipe_layout;
    vk_buffer ubo;
    VkShaderModule vs, fs_resolve, fs_composite;
    VkRenderPass rp[VK_AO_SLOT_COUNT];
    VkPipeline pipe[VK_AO_SLOT_COUNT];
} s_vk_ao;

static const VkFormat k_vk_ao_formats[VK_AO_SLOT_COUNT] = {
    VK_FORMAT_R32_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT
};

static const char k_vk_ao_vs[] =
    "float4 main(uint id : SV_VertexID) : SV_Position {\n"
    "  float2 p = float2((id << 1) & 2, id & 2);\n"
    "  return float4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

static const char k_vk_ao_resolve_ps[] =
    "Texture2D<float4> source : register(t0);\n"
    "cbuffer Params : register(b0) { float4 params; };\n"
    "float4 main(float4 pos : SV_Position) : SV_Target0 {\n"
    "  int2 g = int2(params.xy);\n"
    "  int2 base = int2(pos.xy) * g;\n"
    "  float d = source.Load(int3(base, 0)).r;\n"
    "  if (g.x > 1) d = min(d, source.Load(int3(base + int2(1, 0), 0)).r);\n"
    "  if (g.y > 1) {\n"
    "    d = min(d, source.Load(int3(base + int2(0, 1), 0)).r);\n"
    "    d = min(d, source.Load(int3(base + int2(1, 1), 0)).r);\n"
    "  }\n"
    "  return float4(d, 0.0, 0.0, 1.0);\n"
    "}\n";

static const char k_vk_ao_composite_ps[] =
    "Texture2D<float4> source : register(t0);\n"
    "Texture2D<float4> ao : register(t1);\n"
    "cbuffer Params : register(b0) { float4 params; };\n"
    "float4 main(float4 pos : SV_Position) : SV_Target0 {\n"
    "  int2 p = int2(pos.xy);\n"
    "  float4 color = source.Load(int3(p, 0));\n"
    "  float visibility = saturate(ao.Load(int3(p / int2(params.xy), 0)).r);\n"
    "  float factor = lerp(1.0, visibility, params.z);\n"
    "  return float4(color.rgb * factor, color.a);\n"
    "}\n";

static void vk_ao_destroy_base(void)
{
    VkDevice dev = s_vk.device;
    if (s_vk_ao.vs) pvkDestroyShaderModule(dev, s_vk_ao.vs, NULL);
    if (s_vk_ao.pipe_layout) pvkDestroyPipelineLayout(dev, s_vk_ao.pipe_layout, NULL);
    if (s_vk_ao.pool) pvkDestroyDescriptorPool(dev, s_vk_ao.pool, NULL);
    if (s_vk_ao.set_layout) pvkDestroyDescriptorSetLayout(dev, s_vk_ao.set_layout, NULL);
    vk_destroy_buffer(&s_vk_ao.ubo);
    s_vk_ao.vs = VK_NULL_HANDLE;
    s_vk_ao.pipe_layout = VK_NULL_HANDLE;
    s_vk_ao.pool = VK_NULL_HANDLE;
    s_vk_ao.set = VK_NULL_HANDLE;
    s_vk_ao.set_layout = VK_NULL_HANDLE;
    memset(&s_vk_ao.ubo, 0, sizeof(s_vk_ao.ubo));
    s_vk_ao.base_ready = 0;
}

static int vk_ao_ensure_base(void)
{
    VkDescriptorSetLayoutBinding b[3];
    VkDescriptorSetLayoutCreateInfo lci;
    VkDescriptorPoolSize sizes[2];
    VkDescriptorPoolCreateInfo pci;
    VkDescriptorSetAllocateInfo ai;
    VkPipelineLayoutCreateInfo plci;
    VkShaderModule vs = VK_NULL_HANDLE;
    VkDevice dev = s_vk.device;

    if (s_vk_ao.base_ready) return 1;
    if (!dev) return 0;

    memset(b, 0, sizeof(b));
    b[0].binding = 0;
    b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b[1].binding = RSX_SPIRV_TEXTURE_BINDING;
    b[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    b[2].binding = RSX_SPIRV_TEXTURE_BINDING + 1;
    b[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    for (int i = 0; i < 3; ++i) {
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    memset(&lci, 0, sizeof(lci));
    lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    lci.bindingCount = 3;
    lci.pBindings = b;
    if (pvkCreateDescriptorSetLayout(dev, &lci, NULL, &s_vk_ao.set_layout) != VK_SUCCESS) {
        s_vk_ao.set_layout = VK_NULL_HANDLE;
        goto fail;
    }

    sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sizes[0].descriptorCount = 1;
    sizes[1].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    sizes[1].descriptorCount = 2;
    memset(&pci, 0, sizeof(pci));
    pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pci.maxSets = 1;
    pci.poolSizeCount = 2;
    pci.pPoolSizes = sizes;
    if (pvkCreateDescriptorPool(dev, &pci, NULL, &s_vk_ao.pool) != VK_SUCCESS) {
        s_vk_ao.pool = VK_NULL_HANDLE;
        goto fail;
    }

    memset(&ai, 0, sizeof(ai));
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = s_vk_ao.pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &s_vk_ao.set_layout;
    if (pvkAllocateDescriptorSets(dev, &ai, &s_vk_ao.set) != VK_SUCCESS) {
        s_vk_ao.set = VK_NULL_HANDLE;
        goto fail;
    }

    memset(&plci, 0, sizeof(plci));
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &s_vk_ao.set_layout;
    if (pvkCreatePipelineLayout(dev, &plci, NULL, &s_vk_ao.pipe_layout) != VK_SUCCESS) {
        s_vk_ao.pipe_layout = VK_NULL_HANDLE;
        goto fail;
    }

    if (vk_create_host_buffer(&s_vk_ao.ubo, 16, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) != 0 || !s_vk_ao.ubo.ptr || !s_vk_ao.ubo.coherent)
        goto fail;

    if (vk_eng_translate(k_vk_ao_vs, RSX_SHADER_STAGE_VERTEX, &vs) != 0 || !vs)
        goto fail;
    s_vk_ao.vs = vs;

    s_vk_ao.base_ready = 1;
    return 1;

fail:
    vk_ao_destroy_base();
    return 0;
}

static int vk_ao_create_slot(int slot, VkShaderModule fs)
{
    VkDevice dev = s_vk.device;
    VkAttachmentDescription att;
    VkAttachmentReference ref;
    VkSubpassDescription sub;
    VkRenderPassCreateInfo rpci;
    VkPipelineShaderStageCreateInfo stages[2];
    VkPipelineVertexInputStateCreateInfo vi;
    VkPipelineInputAssemblyStateCreateInfo ia;
    VkPipelineViewportStateCreateInfo vp;
    VkPipelineRasterizationStateCreateInfo rs;
    VkPipelineMultisampleStateCreateInfo ms;
    VkPipelineColorBlendAttachmentState cba;
    VkPipelineColorBlendStateCreateInfo cb;
    VkDynamicState dyn_states[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn;
    VkGraphicsPipelineCreateInfo gp;

    if (!s_vk_ao.rp[slot]) {
        memset(&att, 0, sizeof(att));
        att.format = k_vk_ao_formats[slot];
        att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
        att.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
        ref.attachment = 0;
        ref.layout = VK_IMAGE_LAYOUT_GENERAL;
        memset(&sub, 0, sizeof(sub));
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &ref;
        memset(&rpci, 0, sizeof(rpci));
        rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpci.attachmentCount = 1;
        rpci.pAttachments = &att;
        rpci.subpassCount = 1;
        rpci.pSubpasses = &sub;
        if (pvkCreateRenderPass(dev, &rpci, NULL, &s_vk_ao.rp[slot]) != VK_SUCCESS) {
            s_vk_ao.rp[slot] = VK_NULL_HANDLE;
            return 0;
        }
    }

    memset(stages, 0, sizeof(stages));
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = s_vk_ao.vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    memset(&vi, 0, sizeof(vi));
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    memset(&ia, 0, sizeof(ia));
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    memset(&vp, 0, sizeof(vp));
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    memset(&rs, 0, sizeof(rs));
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    memset(&ms, 0, sizeof(ms));
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    memset(&cba, 0, sizeof(cba));
    cba.blendEnable = VK_FALSE;
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    memset(&cb, 0, sizeof(cb));
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    memset(&dyn, 0, sizeof(dyn));
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dyn_states;

    memset(&gp, 0, sizeof(gp));
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dyn;
    gp.layout = s_vk_ao.pipe_layout;
    gp.renderPass = s_vk_ao.rp[slot];
    gp.subpass = 0;
    if (pvkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, &s_vk_ao.pipe[slot]) != VK_SUCCESS) {
        if (s_vk_ao.pipe[slot]) pvkDestroyPipeline(dev,s_vk_ao.pipe[slot],NULL);
        s_vk_ao.pipe[slot] = VK_NULL_HANDLE;
        pvkDestroyRenderPass(dev, s_vk_ao.rp[slot], NULL);
        s_vk_ao.rp[slot] = VK_NULL_HANDLE;
        return 0;
    }
    return 1;
}

static int vk_ao_ensure_slot(int slot)
{
    VkShaderModule* fs;
    if (s_vk_ao.pipe[slot]) return 1;
    if (!vk_ao_ensure_base()) return 0;
    fs = slot == 0 ? &s_vk_ao.fs_resolve : &s_vk_ao.fs_composite;
    if (!*fs) {
        VkShaderModule m = VK_NULL_HANDLE;
        const char* src = slot == 0 ? k_vk_ao_resolve_ps : k_vk_ao_composite_ps;
        if (vk_eng_translate(src, RSX_SHADER_STAGE_FRAGMENT, &m) != 0 || !m) return 0;
        *fs = m;
    }
    return vk_ao_create_slot(slot, *fs);
}

/* Global memory barrier; all images remain in GENERAL so no layout transition is needed,
 * and this stays valid regardless of the source image aspect (color or depth). */
static void vk_ao_memory_barrier(void)
{
    VkMemoryBarrier mb;
    memset(&mb, 0, sizeof(mb));
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          0, 1, &mb, 0, NULL, 0, NULL);
}

static int vk_ao_image_pass(vk_image* dst, VkFormat format, u32 width, u32 height,
                            VkImage src0, VkImageView view0, VkImage src1, VkImageView view1,
                            u32 grid_x, u32 grid_y, float strength, int reduce_depth)
{
    VkDevice dev = s_vk.device;
    VkFramebuffer fb = VK_NULL_HANDLE;
    VkFramebufferCreateInfo fci;
    VkDescriptorBufferInfo bi;
    VkDescriptorImageInfo ii[2];
    VkWriteDescriptorSet w[3];
    VkRenderPassBeginInfo rbi;
    VkViewport viewport;
    VkRect2D scissor;
    float params[4];
    int slot = -1;

    if (!dev || !dst || !dst->img || !dst->view || !src0 || !view0) return 0;
    if (width == 0 || height == 0) return 0;
    if (!((grid_x == 1 && grid_y == 1) || (grid_x == 2 && grid_y == 1) || (grid_x == 2 && grid_y == 2)))
        return 0;
    if (!isfinite(strength) || strength < 0.0f || strength > 1.0f) return 0;

    if (reduce_depth) {
        if (format == VK_FORMAT_R32_SFLOAT) slot = 0;
        src1 = src0; /* descriptor 1 is unused: bind the source as a safe dummy */
        view1 = view0;
    } else {
        if (format == VK_FORMAT_R8G8B8A8_UNORM) slot = 1;
        else if (format == VK_FORMAT_R16G16B16A16_SFLOAT) slot = 2;
        if (!src1 || !view1) return 0;
        if (width % grid_x != 0 || height % grid_y != 0) return 0;
    }
    if (slot < 0) return 0;
    if (dst->img == src0 || dst->img == src1 || dst->view == view0 || dst->view == view1) return 0;

    if (!vk_ao_ensure_slot(slot)) return 0;

    memset(&fci, 0, sizeof(fci));
    fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fci.renderPass = s_vk_ao.rp[slot];
    fci.attachmentCount = 1;
    fci.pAttachments = &dst->view;
    fci.width = width;
    fci.height = height;
    fci.layers = 1;
    if (pvkCreateFramebuffer(dev, &fci, NULL, &fb) != VK_SUCCESS) return 0;

    /* Every prior submission was waited, so the set is not in use. */
    bi.buffer = s_vk_ao.ubo.buf;
    bi.offset = 0;
    bi.range = 16;
    ii[0].sampler = VK_NULL_HANDLE;
    ii[0].imageView = view0;
    ii[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    ii[1].sampler = VK_NULL_HANDLE;
    ii[1].imageView = view1;
    ii[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    memset(w, 0, sizeof(w));
    for (int i = 0; i < 3; ++i) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = s_vk_ao.set;
        w[i].descriptorCount = 1;
    }
    w[0].dstBinding = 0;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w[0].pBufferInfo = &bi;
    w[1].dstBinding = RSX_SPIRV_TEXTURE_BINDING;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w[1].pImageInfo = &ii[0];
    w[2].dstBinding = RSX_SPIRV_TEXTURE_BINDING + 1;
    w[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w[2].pImageInfo = &ii[1];
    pvkUpdateDescriptorSets(dev, 3, w, 0, NULL);

    params[0] = (float)grid_x;
    params[1] = (float)grid_y;
    params[2] = strength;
    params[3] = 0.0f;
    memcpy(s_vk_ao.ubo.ptr, params, sizeof(params));

    if (vk_begin() != 0) {
        pvkDestroyFramebuffer(dev, fb, NULL);
        return 0;
    }

    vk_ao_memory_barrier(); /* src0, src1 and dst, all GENERAL */

    memset(&rbi, 0, sizeof(rbi));
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = s_vk_ao.rp[slot];
    rbi.framebuffer = fb;
    rbi.renderArea.extent.width = width;
    rbi.renderArea.extent.height = height;
    pvkCmdBeginRenderPass(s_vk.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    pvkCmdBindPipeline(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_vk_ao.pipe[slot]);
    pvkCmdBindDescriptorSets(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_vk_ao.pipe_layout,
                             0, 1, &s_vk_ao.set, 0, NULL);
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = (float)width;
    viewport.height = (float)height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    pvkCmdSetViewport(s_vk.cmd, 0, 1, &viewport);
    scissor.offset.x = 0;
    scissor.offset.y = 0;
    scissor.extent.width = width;
    scissor.extent.height = height;
    pvkCmdSetScissor(s_vk.cmd, 0, 1, &scissor);
    pvkCmdDraw(s_vk.cmd, 3, 1, 0, 0);
    pvkCmdEndRenderPass(s_vk.cmd);

    vk_ao_memory_barrier(); /* make dst writes visible to later work */

    if (vk_submit_and_wait() != 0) {
        pvkDeviceWaitIdle(dev);
        pvkDestroyFramebuffer(dev, fb, NULL);
        return 0;
    }
    pvkDestroyFramebuffer(dev, fb, NULL);
    return 1;
}

static void vk_ao_pass_shutdown(void)
{
    VkDevice dev = s_vk.device;
    if (dev) {
        for (int i = 0; i < VK_AO_SLOT_COUNT; ++i) {
            if (s_vk_ao.pipe[i]) pvkDestroyPipeline(dev, s_vk_ao.pipe[i], NULL);
            if (s_vk_ao.rp[i]) pvkDestroyRenderPass(dev, s_vk_ao.rp[i], NULL);
        }
        if (s_vk_ao.fs_resolve) pvkDestroyShaderModule(dev, s_vk_ao.fs_resolve, NULL);
        if (s_vk_ao.fs_composite) pvkDestroyShaderModule(dev, s_vk_ao.fs_composite, NULL);
        vk_ao_destroy_base();
    }
    memset(&s_vk_ao, 0, sizeof(s_vk_ao));
}
