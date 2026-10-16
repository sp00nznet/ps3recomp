/* Individual-sample storage packing for the Vulkan draw engine.
 * Inverse of vk_eng_expand_samples: a single-sample expanded texture
 * (2*w x h*(samples==4?2:1)) is written back into a genuine multisample
 * color attachment. Sample addressing matches rsx_vulkan_msaa.inc:
 * loc=(index%2,index/2), index=loc.x+loc.y*grid.y.
 * SPDX-License-Identifier: GPL-2.0-only
 * See the containing project's COPYING.RPCS3 and DEPENDENCIES.md.
 * Included after rsx_vulkan_msaa.inc; shares the synchronous backend queue.
 *
 * Design: no sampleRateShading is required. The fullscreen triangle is drawn
 * once per sample with a pipeline whose static pSampleMask is 1<<sample, so
 * every sample receives exactly its own stored texel (no averaging, no
 * replication). The render pass uses LOAD/STORE so samples masked out of a
 * draw survive; by the last draw every sample of the target is overwritten.
 */
static const char vk_msaa_pack_fs[] =
    "Texture2D<float4> source:register(t0);"
    "struct vk_pack_params{uint4 v;};"
    "[[vk::push_constant]] ConstantBuffer<vk_pack_params> params;"
    "float4 main(float4 pos:SV_Position):SV_Target0 {"
    "uint2 p=uint2(pos.xy)*params.v.zw+params.v.xy;"
    "return source.Load(int3(p,0));}";

/* [format: RGBA8, RGBA16F][samples: 2, 4][sample index] */
static struct {
    VkDescriptorSetLayout set_layout;
    VkDescriptorPool pool;
    VkDescriptorSet set;
    VkPipelineLayout layout;
    VkShaderModule vs, fs;
    VkRenderPass pass[2][2];
    VkPipeline pipeline[2][2][4];
} s_msaa_pack;

static void vk_eng_pack_shutdown(void)
{
    for (u32 f=0;f<2;++f) for (u32 c=0;c<2;++c) {
        for (u32 s=0;s<4;++s)
            if (s_msaa_pack.pipeline[f][c][s]) pvkDestroyPipeline(s_vk.device,s_msaa_pack.pipeline[f][c][s],NULL);
        if (s_msaa_pack.pass[f][c]) pvkDestroyRenderPass(s_vk.device,s_msaa_pack.pass[f][c],NULL);
    }
    if (s_msaa_pack.vs) pvkDestroyShaderModule(s_vk.device,s_msaa_pack.vs,NULL);
    if (s_msaa_pack.fs) pvkDestroyShaderModule(s_vk.device,s_msaa_pack.fs,NULL);
    if (s_msaa_pack.layout) pvkDestroyPipelineLayout(s_vk.device,s_msaa_pack.layout,NULL);
    if (s_msaa_pack.pool) pvkDestroyDescriptorPool(s_vk.device,s_msaa_pack.pool,NULL);
    if (s_msaa_pack.set_layout) pvkDestroyDescriptorSetLayout(s_vk.device,s_msaa_pack.set_layout,NULL);
    memset(&s_msaa_pack,0,sizeof s_msaa_pack);
}

static int vk_eng_pack_format_index(VkFormat format)
{
    if (format==VK_FORMAT_R8G8B8A8_UNORM) return 0;
    if (format==VK_FORMAT_R16G16B16A16_SFLOAT) return 1;
    return -1;
}

/* Shared objects; each step is adopted only on success so retries resume. */
static int vk_eng_pack_common(void)
{
    if (!s_msaa_pack.set_layout) {
        VkDescriptorSetLayoutBinding binding={RSX_SPIRV_TEXTURE_BINDING,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_FRAGMENT_BIT,NULL};
        VkDescriptorSetLayoutCreateInfo sci={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=1,.pBindings=&binding};
        VkDescriptorSetLayout set_layout=VK_NULL_HANDLE;
        if (pvkCreateDescriptorSetLayout(s_vk.device,&sci,NULL,&set_layout)!=VK_SUCCESS) return 0;
        s_msaa_pack.set_layout=set_layout;
    }
    if (!s_msaa_pack.pool) {
        VkDescriptorPoolSize size={VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1};
        VkDescriptorPoolCreateInfo pci={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=1,.pPoolSizes=&size};
        VkDescriptorPool pool=VK_NULL_HANDLE;
        if (pvkCreateDescriptorPool(s_vk.device,&pci,NULL,&pool)!=VK_SUCCESS) return 0;
        s_msaa_pack.pool=pool;
    }
    if (!s_msaa_pack.set) {
        VkDescriptorSetAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=s_msaa_pack.pool,.descriptorSetCount=1,.pSetLayouts=&s_msaa_pack.set_layout};
        VkDescriptorSet set=VK_NULL_HANDLE;
        if (pvkAllocateDescriptorSets(s_vk.device,&ai,&set)!=VK_SUCCESS) return 0;
        s_msaa_pack.set=set;
    }
    if (!s_msaa_pack.layout) {
        VkPushConstantRange range={VK_SHADER_STAGE_FRAGMENT_BIT,0,16};
        VkPipelineLayoutCreateInfo lci={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&s_msaa_pack.set_layout,
            .pushConstantRangeCount=1,.pPushConstantRanges=&range};
        VkPipelineLayout layout=VK_NULL_HANDLE;
        if (pvkCreatePipelineLayout(s_vk.device,&lci,NULL,&layout)!=VK_SUCCESS) return 0;
        s_msaa_pack.layout=layout;
    }
    if (!s_msaa_pack.vs) {
        VkShaderModule module=VK_NULL_HANDLE;
        if (vk_eng_translate(vk_msaa_expand_vs,RSX_SHADER_STAGE_VERTEX,&module) || !module) return 0;
        s_msaa_pack.vs=module;
    }
    if (!s_msaa_pack.fs) {
        VkShaderModule module=VK_NULL_HANDLE;
        if (vk_eng_translate(vk_msaa_pack_fs,RSX_SHADER_STAGE_FRAGMENT,&module) || !module) return 0;
        s_msaa_pack.fs=module;
    }
    return 1;
}

static int vk_eng_pack_pass(VkFormat format, u32 fi, u32 samples, u32 ci)
{
    if (s_msaa_pack.pass[fi][ci]) return 1;
    /* LOAD keeps samples outside the current draw's static mask intact. */
    VkAttachmentDescription attachment={.format=format,
        .samples=samples==4?VK_SAMPLE_COUNT_4_BIT:VK_SAMPLE_COUNT_2_BIT,
        .loadOp=VK_ATTACHMENT_LOAD_OP_LOAD,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout=VK_IMAGE_LAYOUT_GENERAL,.finalLayout=VK_IMAGE_LAYOUT_GENERAL};
    VkAttachmentReference color={0,VK_IMAGE_LAYOUT_GENERAL};
    VkSubpassDescription sub={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,.colorAttachmentCount=1,.pColorAttachments=&color};
    VkRenderPassCreateInfo rci={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.attachmentCount=1,.pAttachments=&attachment,.subpassCount=1,.pSubpasses=&sub};
    VkRenderPass pass=VK_NULL_HANDLE;
    if (pvkCreateRenderPass(s_vk.device,&rci,NULL,&pass)!=VK_SUCCESS) return 0;
    s_msaa_pack.pass[fi][ci]=pass;
    return 1;
}

static int vk_eng_pack_pipeline(u32 fi, u32 samples, u32 ci, u32 sample)
{
    if (s_msaa_pack.pipeline[fi][ci][sample]) return 1;
    const VkSampleMask mask=1u<<sample;
    VkPipelineShaderStageCreateInfo stages[2]={
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=s_msaa_pack.vs,.pName="main"},
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=s_msaa_pack.fs,.pName="main"}};
    VkPipelineVertexInputStateCreateInfo vi={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.scissorCount=1};
    VkPipelineRasterizationStateCreateInfo raster={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.polygonMode=VK_POLYGON_MODE_FILL,.cullMode=VK_CULL_MODE_NONE,.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE,.lineWidth=1.0f};
    /* Per-pixel shading; the static mask restricts coverage to one sample. */
    VkPipelineMultisampleStateCreateInfo ms={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples=samples==4?VK_SAMPLE_COUNT_4_BIT:VK_SAMPLE_COUNT_2_BIT,.sampleShadingEnable=VK_FALSE,.pSampleMask=&mask};
    VkPipelineColorBlendAttachmentState blend={.colorWriteMask=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT};
    VkPipelineColorBlendStateCreateInfo cb={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=1,.pAttachments=&blend};
    VkDynamicState dynamic[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy={.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,.dynamicStateCount=2,.pDynamicStates=dynamic};
    VkGraphicsPipelineCreateInfo gci={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.stageCount=2,.pStages=stages,
        .pVertexInputState=&vi,.pInputAssemblyState=&ia,.pViewportState=&vp,.pRasterizationState=&raster,
        .pMultisampleState=&ms,.pColorBlendState=&cb,.pDynamicState=&dy,.layout=s_msaa_pack.layout,.renderPass=s_msaa_pack.pass[fi][ci]};
    VkPipeline pipeline=VK_NULL_HANDLE;
    if (pvkCreateGraphicsPipelines(s_vk.device,VK_NULL_HANDLE,1,&gci,NULL,&pipeline)!=VK_SUCCESS) return 0;
    s_msaa_pack.pipeline[fi][ci][sample]=pipeline;
    return 1;
}

static int vk_eng_pack_samples(u32 surface, u32 expanded)
{
    vk_eng_obj* dst=vk_eng_get(surface,VK_ENG_COLOR);
    if (!dst) return 0;
    vk_eng_obj* src=vk_eng_get(expanded,VK_ENG_TEXTURE);
    VkFramebuffer framebuffer=VK_NULL_HANDLE;
    /* Validate everything before touching any resource. */
    if (!src || surface==expanded) goto fail;
    if (dst->samples!=2 && dst->samples!=4) goto fail;
    if (src->samples>1) goto fail;
    const int fi=vk_eng_pack_format_index(dst->fmt);
    if (fi<0 || src->fmt!=dst->fmt || src->bpp!=dst->bpp) goto fail;
    const u32 samples=dst->samples,ci=samples==4?1u:0u;
    const u32 grid_y=samples==4?2u:1u;
    const u32 w=dst->w,h=dst->h;
    if (!w || !h || src->w!=w*2u || src->h!=h*grid_y) goto fail;
    if (!dst->im.img || !dst->im.view || !src->im.img || !src->im.view) goto fail;

    if (!vk_eng_pack_common()) goto fail;
    if (!vk_eng_pack_pass(dst->fmt,(u32)fi,samples,ci)) goto fail;
    for (u32 s=0;s<samples;++s)
        if (!vk_eng_pack_pipeline((u32)fi,samples,ci,s)) goto fail;

    /* Framebuffer covers the logical surface size. */
    VkFramebufferCreateInfo fci={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass=s_msaa_pack.pass[fi][ci],.attachmentCount=1,.pAttachments=&dst->im.view,.width=w,.height=h,.layers=1};
    if (pvkCreateFramebuffer(s_vk.device,&fci,NULL,&framebuffer)!=VK_SUCCESS) { framebuffer=VK_NULL_HANDLE; goto fail; }
    /* Previous submissions completed synchronously, so the set is idle. */
    VkDescriptorImageInfo info={VK_NULL_HANDLE,src->im.view,VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet write={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=s_msaa_pack.set,
        .dstBinding=RSX_SPIRV_TEXTURE_BINDING,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,.pImageInfo=&info};
    pvkUpdateDescriptorSets(s_vk.device,1,&write,0,NULL);

    if (vk_begin()) goto fail;
    vk_barrier_image(src->im.img,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_GENERAL);
    vk_barrier_image(dst->im.img,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_GENERAL);
    VkRenderPassBeginInfo bi={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=s_msaa_pack.pass[fi][ci],
        .framebuffer=framebuffer,.renderArea={{0,0},{w,h}}};
    pvkCmdBeginRenderPass(s_vk.cmd,&bi,VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport={0,0,(float)w,(float)h,0,1};VkRect2D scissor={{0,0},{w,h}};
    pvkCmdSetViewport(s_vk.cmd,0,1,&viewport);pvkCmdSetScissor(s_vk.cmd,0,1,&scissor);
    pvkCmdBindDescriptorSets(s_vk.cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,s_msaa_pack.layout,0,1,&s_msaa_pack.set,0,NULL);
    /* One draw per sample; disjoint static masks write each sample once. */
    for (u32 s=0;s<samples;++s) {
        const u32 params[4]={s%2u,s/2u,2u,grid_y};
        pvkCmdBindPipeline(s_vk.cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,s_msaa_pack.pipeline[fi][ci][s]);
        pvkCmdPushConstants(s_vk.cmd,s_msaa_pack.layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof params,params);
        pvkCmdDraw(s_vk.cmd,3,1,0,0);
    }
    pvkCmdEndRenderPass(s_vk.cmd);
    if (vk_submit_and_wait()) goto fail;
    pvkDestroyFramebuffer(s_vk.device,framebuffer,NULL);
    /* Surface now equals this texture; only its own expansion stays valid. */
    dst->expansion_valid=expanded==dst->expanded?1:0;
    return 1;
fail:
    if (framebuffer) pvkDestroyFramebuffer(s_vk.device,framebuffer,NULL);
    dst->expansion_valid=0;
    return 0;
}
