/* GPU-resident batch versus the separately qualified synchronous reference. */
#define main rsx_vertex_outputs_reference_main
#include "test_scene_vertex_outputs.c"
#undef main
static int batch_checks(void)
{
    rsx_vertex_layout_plan layout;rsx_vertex_layout_plan_init(&layout,(1u<<0)|(1u<<8));
    float vertices[3][8]={{-1,-2,3,1,1,2,3,4},{2,3,4,1,5,6,7,8},{3,-1,5,1,9,10,11,12}};
    float constants[514][4]={{0}};rsx_scene_vertex_output expected[4];
    for(unsigned k=0;k<4;++k){constants[0][k]=1;constants[1][k]=(float)k;}
    for(unsigned i=2;i<43;++i)for(unsigned k=0;k<4;++k)constants[i][k]=(float)(i+k);
    CHECK(!rsx_vulkan_backend_replay_outputs(synthetic_shader,&layout,vertices,3,constants,sizeof constants,expected,3));
    uint64_t token=0;uint32_t base=99;
    CHECK(!rsx_vulkan_scene_batch_begin(1,s_vk.rt_enabled,&token));
    rsx_scene_batch_input input={synthetic_shader,layout,vertices,sizeof vertices,3,constants,sizeof constants};
    CHECK(!rsx_vulkan_scene_batch_append(token,&input,&base) && base==0);
    constants[0][0]=2;input.vertex_count=1;input.vertex_bytes=layout.stride;
    CHECK(!rsx_vulkan_backend_replay_outputs(synthetic_shader,&layout,vertices,1,constants,sizeof constants,expected+3,1));
    CHECK(!rsx_vulkan_scene_batch_append(token,&input,&base) && base==3);
    CHECK(s_scene_batch->cache_count==1);
    memset(vertices,0,sizeof vertices);memset(constants,0,sizeof constants);
    vk_buffer readback={0};CHECK(!vk_create_host_buffer(&readback,sizeof expected+16,VK_BUFFER_USAGE_TRANSFER_DST_BIT) && readback.coherent);
    CHECK(!vk_begin());CHECK(pvkResetFences(s_vk.device,1,&s_vk.fence)==VK_SUCCESS);
    rsx_scene_batch_output output={0};CHECK(!rsx_vulkan_scene_batch_record(token,s_vk.cmd,s_vk.fence,&output));
    CHECK(output.vertex_count==4 && output.draw_count==2 && output.vertex_bytes==sizeof expected);
    CHECK(!s_vk.rt_enabled || (output.vertex_address && output.validity_address));
    CHECK(rsx_vulkan_scene_batch_reset(token)==RSX_SCENE_BATCH_BUSY);
    VkMemoryBarrier transfer={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT};
    pvkCmdPipelineBarrier(s_vk.cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&transfer,0,NULL,0,NULL);
    PFN_vkCmdCopyBuffer copy_buffer=(PFN_vkCmdCopyBuffer)pvkGetDeviceProcAddr(s_vk.device,"vkCmdCopyBuffer");CHECK(copy_buffer);
    VkBufferCopy copy={0,0,sizeof expected};copy_buffer(s_vk.cmd,output.vertices,readback.buf,1,&copy);
    copy.srcOffset=0;copy.dstOffset=sizeof expected;copy.size=4;copy_buffer(s_vk.cmd,output.validity,readback.buf,1,&copy);
    VkMemoryBarrier host={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
    pvkCmdPipelineBarrier(s_vk.cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,NULL,0,NULL);
    CHECK(!vk_submit_and_wait());CHECK(!rsx_vulkan_scene_batch_mark_submitted(token));
    CHECK(!memcmp(readback.ptr,expected,sizeof expected));
    CHECK(*(uint32_t*)((unsigned char*)readback.ptr+sizeof expected)==0);
    CHECK(!rsx_vulkan_scene_batch_reset(token));
    CHECK(rsx_vulkan_scene_batch_append(token,&input,&base)==RSX_SCENE_BATCH_BUSY);
    vk_destroy_buffer(&readback);CHECK(!rsx_vulkan_scene_batch_shutdown());
    return 1;
}
int main(void)
{
    SDL_SetMainReady();
#ifdef _WIN32
    _putenv_s("PS3RECOMP_VK_GUEST_PROGRAMS","1");
    _putenv_s("PS3RECOMP_RSX_AO","rayquery");
#else
    setenv("PS3RECOMP_VK_GUEST_PROGRAMS","1",1);
    setenv("PS3RECOMP_RSX_AO","rayquery",1);
#endif
    if(rsx_vulkan_backend_init(8,8,"GPU scene batch qualification"))return 1;
    int ok=batch_checks();int enabled=s_vk.rt_enabled;rsx_vulkan_backend_shutdown();
    printf("%s %u GPU scene-batch checks; RT logical features %s\n",ok?"PASS":"FAIL",checks,enabled?"enabled":"unavailable");
    return ok?0:1;
}
