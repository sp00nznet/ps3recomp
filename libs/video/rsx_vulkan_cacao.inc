/* Optional CACAO world-color replacement. Guest images remain unchanged.
 * Included after the image-pass helper; all submissions are synchronous. */
static struct {
    FFX_CACAO_VkContext* context;
    int context_ready, screen_ready, failed, warned;
    vk_image depth, ao;
    u32 color, width, height, color_width, color_height;
    VkFormat format;
} s_cacao;

static void vk_cacao_drop_screen(void)
{
    if (s_cacao.screen_ready) FFX_CACAO_VkDestroyScreenSizeDependentResources(s_cacao.context);
    s_cacao.screen_ready=0;
    if (s_cacao.color) vk_eng_release(NULL,s_cacao.color);
    s_cacao.color=0;
    vk_destroy_image(&s_cacao.depth);vk_destroy_image(&s_cacao.ao);
    s_cacao.width=s_cacao.height=s_cacao.color_width=s_cacao.color_height=0;
}

static void vk_eng_cacao_shutdown(void)
{
    if (s_vk.device) pvkDeviceWaitIdle(s_vk.device);
    vk_cacao_drop_screen();
    if (s_cacao.context_ready) FFX_CACAO_VkDestroyContext(s_cacao.context);
    free(s_cacao.context);
    vk_ao_pass_shutdown();
    rsx_cacao_dispatch_reset();memset(&s_cacao,0,sizeof(s_cacao));
}

static int vk_cacao_context(void)
{
    if (s_cacao.context_ready) return 1;
    if (s_cacao.failed || !s_vk.cacao_device_ok) return 0;
    const VkFormat formats[]={VK_FORMAT_R16_SFLOAT,VK_FORMAT_R8G8B8A8_SNORM,
        VK_FORMAT_R8G8_UNORM,VK_FORMAT_R8_UNORM,VK_FORMAT_R32_UINT};
    for (u32 i=0;i<sizeof(formats)/sizeof(formats[0]);++i) {
        VkFormatProperties p;pvkGetPhysicalDeviceFormatProperties(s_vk.phys,formats[i],&p);
        const VkFormatFeatureFlags need=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
        if ((p.optimalTilingFeatures&need)!=need) goto fail;
    }
    if (!rsx_cacao_dispatch_bind(s_vk.instance,s_vk.device,pvkGetInstanceProcAddr)) goto fail;
    s_cacao.context=(FFX_CACAO_VkContext*)calloc(1,FFX_CACAO_VkGetContextSize());
    if (!s_cacao.context) goto fail;
    FFX_CACAO_VkCreateInfo ci={0};ci.device=s_vk.device;ci.physicalDevice=s_vk.phys;
    if (FFX_CACAO_VkInitContext(s_cacao.context,&ci)!=FFX_CACAO_STATUS_OK) {
        /* Upstream init unwinds partial allocations itself. */
        free(s_cacao.context);s_cacao.context=NULL;goto fail;
    }
    s_cacao.context_ready=1;return 1;
fail:
    s_cacao.failed=1;return 0;
}

static int vk_cacao_screen(u32 w,u32 h,u32 cw,u32 ch,VkFormat format)
{
    if (s_cacao.screen_ready && s_cacao.width==w && s_cacao.height==h &&
        s_cacao.color_width==cw && s_cacao.color_height==ch && s_cacao.format==format) return 1;
    vk_cacao_drop_screen();
    if (vk_create_image(&s_cacao.depth,VK_FORMAT_R32_SFLOAT,w,h,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT)) goto fail;
    if (vk_create_image(&s_cacao.ao,VK_FORMAT_R8_UNORM,w,h,
        VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT)) goto fail;
    s_cacao.color=vk_eng_alloc();if(!s_cacao.color)goto fail;
    vk_eng_obj* out=&s_eobj[s_cacao.color];
    if (vk_create_image(&out->im,format,cw,ch,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT)) goto fail;
    out->kind=VK_ENG_COLOR;out->fmt=format;out->w=cw;out->h=ch;out->samples=1;
    out->bpp=format==VK_FORMAT_R16G16B16A16_SFLOAT?8:4;
    if(vk_begin())goto fail;
    vk_barrier_image(s_cacao.depth.img,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_UNDEFINED);
    vk_barrier_image(out->im.img,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_UNDEFINED);
    /* CACAO owns AO output transitions and discards its prior contents. */
    if(vk_submit_and_wait())goto fail;
    FFX_CACAO_VkScreenSizeInfo si={0};si.width=w;si.height=h;
    si.depthView=s_cacao.depth.view;si.output=s_cacao.ao.img;si.outputView=s_cacao.ao.view;
    if(FFX_CACAO_VkInitScreenSizeDependentResources(s_cacao.context,&si)!=FFX_CACAO_STATUS_OK)goto fail;
    s_cacao.screen_ready=1;s_cacao.width=w;s_cacao.height=h;
    s_cacao.color_width=cw;s_cacao.color_height=ch;s_cacao.format=format;return 1;
fail:
    pvkDeviceWaitIdle(s_vk.device);
    /* A failed image creation may own partial handles before kind is set. */
    if(s_cacao.color && s_eobj[s_cacao.color].kind==VK_ENG_FREE) {
        vk_destroy_image(&s_eobj[s_cacao.color].im);memset(&s_eobj[s_cacao.color],0,sizeof(vk_eng_obj));
    }
    vk_cacao_drop_screen();return 0;
}

static void vk_cacao_layout(VkImage image,VkImageLayout from,VkImageLayout to)
{
    VkImageMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout=from,.newLayout=to,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=image,
        .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    pvkCmdPipelineBarrier(s_vk.cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0,0,NULL,0,NULL,1,&b);
}

/* Returns a private replacement view or0 (caller retains the original).
 * The engine must have qualified the matching depth projection beforehand. */
static u32 vk_eng_cacao_world(void* user,u32 color,u32 depth,u32 logical_w,u32 logical_h,
    const float projection[16],u32 remap,u32 rsx_format,float radius,float strength)
{
    (void)user;
    if(s_cacao.failed)return 0;
    if(!projection || !logical_w || !logical_h || !isfinite(radius) || radius<=0 ||
        radius>1000 || !isfinite(strength) || strength<0 || strength>1) return 0;
    for(u32 i=0;i<16;++i)if(!isfinite(projection[i]))return 0;
    if(!s_vk.cacao_device_ok || !vk_cacao_context())goto unavailable;
    vk_eng_obj* c=vk_eng_get(color,VK_ENG_COLOR);
    vk_eng_obj* z=vk_eng_get(depth,VK_ENG_DEPTH);
    if(!c || !z || (c->samples?c->samples:1)!=(z->samples?z->samples:1) || c->scale!=z->scale) return 0;
    const u32 samples=c->samples?c->samples:1;
    const u32 scale=c->scale?c->scale:1, gx=samples>1?2:1,gy=samples==4?2:1;
    if(samples!=1 && samples!=2 && samples!=4)return 0;
    const u64 pw64=(u64)logical_w*scale,ph64=(u64)logical_h*scale;
    if(pw64>UINT32_MAX/gx || ph64>UINT32_MAX/gy || !pw64 || !ph64)return 0;
    const u32 pw=(u32)pw64,ph=(u32)ph64;
    if(c->w!=pw || c->h!=ph || z->w<pw || z->h<ph)return 0;
    if(c->fmt!=VK_FORMAT_R8G8B8A8_UNORM && c->fmt!=VK_FORMAT_R16G16B16A16_SFLOAT)return 0;
    u32 src_color=c->samples>1?vk_eng_expand_samples(color):color;
    u32 src_depth=vk_eng_depth_snapshot(NULL,depth,logical_w*gx,logical_h*gy);
    if(!src_color || !src_depth)return 0;
    vk_eng_obj* source=&s_eobj[src_color];vk_eng_obj* ds=vk_eng_get(src_depth,VK_ENG_TEXTURE);
    if(!ds || ds->fmt!=VK_FORMAT_R32_SFLOAT || ds->w!=pw*gx || ds->h!=ph*gy ||
        source->w!=pw*gx || source->h!=ph*gy)return 0;
    if(!vk_cacao_screen(pw,ph,source->w,source->h,source->fmt))goto unavailable;
    if(!vk_ao_image_pass(&s_cacao.depth,VK_FORMAT_R32_SFLOAT,pw,ph,
        ds->im.img,ds->im.view,VK_NULL_HANDLE,VK_NULL_HANDLE,gx,gy,1,1))goto unavailable;
    FFX_CACAO_Settings settings=FFX_CACAO_DEFAULT_SETTINGS;
    settings.radius=radius;settings.generateNormals=FFX_CACAO_TRUE;
    settings.qualityLevel=FFX_CACAO_QUALITY_HIGH;settings.blurPassCount=2;settings.detailShadowStrength=.5f;
    if(FFX_CACAO_VkUpdateSettings(s_cacao.context,&settings)!=FFX_CACAO_STATUS_OK)goto unavailable;
    FFX_CACAO_Matrix4x4 p,n={0};memcpy(p.elements,projection,sizeof(p.elements));
    for(u32 i=0;i<4;++i)n.elements[i][i]=1;
    if(vk_begin())goto unavailable;
    vk_cacao_layout(s_cacao.depth.img,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if(FFX_CACAO_VkDraw(s_cacao.context,s_vk.cmd,&p,&n)!=FFX_CACAO_STATUS_OK)goto unavailable;
    vk_cacao_layout(s_cacao.depth.img,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL);
    vk_cacao_layout(s_cacao.ao.img,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL);
    if(vk_submit_and_wait())goto unavailable;
    vk_eng_obj* output=vk_eng_get(s_cacao.color,VK_ENG_COLOR);if(!output)goto unavailable;
    output->scale=scale;output->grid_x=gx;output->grid_y=gy;
    if(!vk_ao_image_pass(&output->im,output->fmt,output->w,output->h,
        source->im.img,source->im.view,s_cacao.ao.img,s_cacao.ao.view,gx,gy,strength,0))goto unavailable;
    return vk_eng_surface_view(NULL,s_cacao.color,remap,rsx_format);
unavailable:
    s_cacao.failed=1;
    if(!s_cacao.warned) { VK_LOG("CACAO unavailable or failed; keeping original world color\n");s_cacao.warned=1; }
    return 0;
}
