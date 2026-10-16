/* Individual-sample storage expansion for the Vulkan draw engine.
 * Sample addressing follows RPCS3 ColorResolvePass.glsl and
 * DepthResolvePass.glsl at 4d5def5880217135554ed434cd5055cceebe9cc2.
 * SPDX-License-Identifier: GPL-2.0-only
 * See the containing project's COPYING.RPCS3 and DEPENDENCIES.md.
 * Included after vk_eng_translate; shares the synchronous backend queue.
 */
static const char vk_msaa_expand_vs[] =
    "float4 main(uint id:SV_VertexID):SV_Position {"
    "float2 p=float2((id<<1)&2,id&2);return float4(p*2-1,0,1);}";
static const char vk_msaa_expand_fs[] =
    "Texture2DMS<float4> source:register(t0);"
    "float4 main(float4 pos:SV_Position):SV_Target0 {"
    "uint w,h,n;source.GetDimensions(w,h,n);"
    "uint2 grid=uint2(2,n==4?2:1);uint2 p=uint2(pos.xy);"
    "uint2 loc=p%grid;uint sample=loc.x+loc.y*grid.y;"
    "return source.Load(int2(p/grid),sample);}";

static struct {
    VkDescriptorSetLayout set_layout;
    VkDescriptorPool pool;
    VkDescriptorSet set;
    VkPipelineLayout layout;
    VkShaderModule vs, fs;
    VkRenderPass pass[3];
    VkPipeline pipeline[3];
} s_msaa_expand;

static void vk_eng_expansion_shutdown(void)
{
    for (u32 i=0;i<3;++i) {
        if (s_msaa_expand.pipeline[i]) pvkDestroyPipeline(s_vk.device,s_msaa_expand.pipeline[i],NULL);
        if (s_msaa_expand.pass[i]) pvkDestroyRenderPass(s_vk.device,s_msaa_expand.pass[i],NULL);
    }
    if (s_msaa_expand.vs) pvkDestroyShaderModule(s_vk.device,s_msaa_expand.vs,NULL);
    if (s_msaa_expand.fs) pvkDestroyShaderModule(s_vk.device,s_msaa_expand.fs,NULL);
    if (s_msaa_expand.layout) pvkDestroyPipelineLayout(s_vk.device,s_msaa_expand.layout,NULL);
    if (s_msaa_expand.pool) pvkDestroyDescriptorPool(s_vk.device,s_msaa_expand.pool,NULL);
    if (s_msaa_expand.set_layout) pvkDestroyDescriptorSetLayout(s_vk.device,s_msaa_expand.set_layout,NULL);
    memset(&s_msaa_expand,0,sizeof s_msaa_expand);
}

static int vk_eng_expansion_pipeline(VkFormat format, u32 index)
{
    if (s_msaa_expand.pipeline[index]) return 1;
    if (!s_msaa_expand.layout) {
        VkDescriptorSetLayoutBinding binding={RSX_SPIRV_TEXTURE_BINDING,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_FRAGMENT_BIT,NULL};
        VkDescriptorSetLayoutCreateInfo sci={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=1,.pBindings=&binding};
        if (pvkCreateDescriptorSetLayout(s_vk.device,&sci,NULL,&s_msaa_expand.set_layout)!=VK_SUCCESS) return 0;
        VkDescriptorPoolSize size={VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1};
        VkDescriptorPoolCreateInfo pci={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=1,.pPoolSizes=&size};
        if (pvkCreateDescriptorPool(s_vk.device,&pci,NULL,&s_msaa_expand.pool)!=VK_SUCCESS) return 0;
        VkDescriptorSetAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=s_msaa_expand.pool,.descriptorSetCount=1,.pSetLayouts=&s_msaa_expand.set_layout};
        if (pvkAllocateDescriptorSets(s_vk.device,&ai,&s_msaa_expand.set)!=VK_SUCCESS) return 0;
        VkPipelineLayoutCreateInfo lci={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&s_msaa_expand.set_layout};
        if (pvkCreatePipelineLayout(s_vk.device,&lci,NULL,&s_msaa_expand.layout)!=VK_SUCCESS) return 0;
        if (vk_eng_translate(vk_msaa_expand_vs,RSX_SHADER_STAGE_VERTEX,&s_msaa_expand.vs) ||
            vk_eng_translate(vk_msaa_expand_fs,RSX_SHADER_STAGE_FRAGMENT,&s_msaa_expand.fs)) return 0;
    }
    VkAttachmentDescription attachment={.format=format,.samples=VK_SAMPLE_COUNT_1_BIT,
        .loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout=VK_IMAGE_LAYOUT_GENERAL,.finalLayout=VK_IMAGE_LAYOUT_GENERAL};
    VkAttachmentReference color={0,VK_IMAGE_LAYOUT_GENERAL};
    VkSubpassDescription sub={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,.colorAttachmentCount=1,.pColorAttachments=&color};
    VkRenderPassCreateInfo rci={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.attachmentCount=1,.pAttachments=&attachment,.subpassCount=1,.pSubpasses=&sub};
    if (pvkCreateRenderPass(s_vk.device,&rci,NULL,&s_msaa_expand.pass[index])!=VK_SUCCESS) return 0;
    VkPipelineShaderStageCreateInfo stages[2]={
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=s_msaa_expand.vs,.pName="main"},
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=s_msaa_expand.fs,.pName="main"}};
    VkPipelineVertexInputStateCreateInfo vi={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.scissorCount=1};
    VkPipelineRasterizationStateCreateInfo raster={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.polygonMode=VK_POLYGON_MODE_FILL,.cullMode=VK_CULL_MODE_NONE,.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE,.lineWidth=1.0f};
    VkPipelineMultisampleStateCreateInfo ms={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState blend={.colorWriteMask=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT};
    VkPipelineColorBlendStateCreateInfo cb={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=1,.pAttachments=&blend};
    VkDynamicState dynamic[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy={.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,.dynamicStateCount=2,.pDynamicStates=dynamic};
    VkGraphicsPipelineCreateInfo gci={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.stageCount=2,.pStages=stages,
        .pVertexInputState=&vi,.pInputAssemblyState=&ia,.pViewportState=&vp,.pRasterizationState=&raster,
        .pMultisampleState=&ms,.pColorBlendState=&cb,.pDynamicState=&dy,.layout=s_msaa_expand.layout,.renderPass=s_msaa_expand.pass[index]};
    return pvkCreateGraphicsPipelines(s_vk.device,VK_NULL_HANDLE,1,&gci,NULL,&s_msaa_expand.pipeline[index])==VK_SUCCESS;
}

static u32 vk_eng_expand_samples(u32 surface)
{
    if (!surface || surface>=VK_ENG_MAX_OBJ) return 0;
    vk_eng_obj* src=&s_eobj[surface];
    if ((src->kind!=VK_ENG_COLOR && src->kind!=VK_ENG_DEPTH) || (src->samples!=2 && src->samples!=4)) return 0;
    if (src->expansion_valid && vk_eng_get(src->expanded,VK_ENG_TEXTURE)) return src->expanded;
    const int depth=src->kind==VK_ENG_DEPTH;
    const VkFormat format=depth?VK_FORMAT_R32_SFLOAT:src->fmt;
    const u32 index=depth?2u:(format==VK_FORMAT_R16G16B16A16_SFLOAT?1u:0u);
    const u32 w=src->w*2u,h=src->h*(src->samples==4?2u:1u);
    VkImageView source_view=VK_NULL_HANDLE;
    VkFramebuffer framebuffer=VK_NULL_HANDLE;
    if (!vk_eng_expansion_pipeline(format,index)) goto fail;
    if (!src->expanded) {
        const u32 hd=vk_eng_alloc();
        if (!hd) goto fail;
        vk_eng_obj* dst=&s_eobj[hd];
        if (vk_create_image(&dst->im,format,w,h,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT)) {
            vk_destroy_image(&dst->im); memset(dst,0,sizeof *dst); goto fail;
        }
        dst->kind=VK_ENG_TEXTURE;dst->fmt=format;dst->bpp=depth?4u:src->bpp;dst->w=w;dst->h=h;
        src->expanded=hd;
        if (vk_begin()) goto fail;
        vk_barrier_image(dst->im.img,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_UNDEFINED);
        if (vk_submit_and_wait()) goto fail;
    }
    vk_eng_obj* dst=vk_eng_get(src->expanded,VK_ENG_TEXTURE);
    if (!dst) goto fail;
    VkImageViewCreateInfo vci={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=src->im.img,
        .viewType=VK_IMAGE_VIEW_TYPE_2D,.format=src->fmt,
        .subresourceRange={depth?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    if (pvkCreateImageView(s_vk.device,&vci,NULL,&source_view)!=VK_SUCCESS) goto fail;
    VkFramebufferCreateInfo fci={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass=s_msaa_expand.pass[index],.attachmentCount=1,.pAttachments=&dst->im.view,.width=w,.height=h,.layers=1};
    if (pvkCreateFramebuffer(s_vk.device,&fci,NULL,&framebuffer)!=VK_SUCCESS) goto fail;
    VkDescriptorImageInfo info={VK_NULL_HANDLE,source_view,VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet write={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=s_msaa_expand.set,
        .dstBinding=RSX_SPIRV_TEXTURE_BINDING,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,.pImageInfo=&info};
    pvkUpdateDescriptorSets(s_vk.device,1,&write,0,NULL);
    if (vk_begin()) goto fail;
    vk_barrier_image(src->im.img,depth?s_vk.depth_aspect:VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_GENERAL);
    vk_barrier_image(dst->im.img,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_GENERAL);
    VkRenderPassBeginInfo bi={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=s_msaa_expand.pass[index],
        .framebuffer=framebuffer,.renderArea={{0,0},{w,h}}};
    pvkCmdBeginRenderPass(s_vk.cmd,&bi,VK_SUBPASS_CONTENTS_INLINE);
    pvkCmdBindPipeline(s_vk.cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,s_msaa_expand.pipeline[index]);
    VkViewport viewport={0,0,(float)w,(float)h,0,1};VkRect2D scissor={{0,0},{w,h}};
    pvkCmdSetViewport(s_vk.cmd,0,1,&viewport);pvkCmdSetScissor(s_vk.cmd,0,1,&scissor);
    pvkCmdBindDescriptorSets(s_vk.cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,s_msaa_expand.layout,0,1,&s_msaa_expand.set,0,NULL);
    pvkCmdDraw(s_vk.cmd,3,1,0,0);
    pvkCmdEndRenderPass(s_vk.cmd);
    if (vk_submit_and_wait()) goto fail;
    pvkDestroyFramebuffer(s_vk.device,framebuffer,NULL);
    pvkDestroyImageView(s_vk.device,source_view,NULL);
    src->expansion_valid=1;
    return src->expanded;
fail:
    if (framebuffer) pvkDestroyFramebuffer(s_vk.device,framebuffer,NULL);
    if (source_view) pvkDestroyImageView(s_vk.device,source_view,NULL);
    ps3_strict_boot_guard("vulkan-msaa-expand","cannot preserve individual samples surface=%u samples=%u",surface,src->samples);
    return 0;
}
