/* Exact pool91 full PS raw SHA256:
 * 46e66b9feb7f93cff81ce5e9ccd84ef64c9eb35f0e78b44d73b25aedd69d4285
 * LF alias:6e59f9da9d300213eb50ab8124ca5d5d7e4b6cf177ffc7dbb96c0c544db3d2f6
 * Generated/checked by tests/test_pool_rgb_source.py. No resource bindings.
 * Include rsx_scene_alpha.glsl first for RsxSceneAlphaVertexOutput.
 * Host must qualify the exact full source, constants and original sampler
 * semantics. Callbacks must be deterministic, including repeated samples.
 * Explicit LOD is caller-supplied; raster derivative equivalence is not claimed.
 * normalize(0), NaNs, scalar splats and final per-channel sanitization retain
 * original source behavior. No guessed normal repair or early NaN rejection.
 */
#ifndef RSX_POOL_RGB_GLSL
#define RSX_POOL_RGB_GLSL
bool rsx_pool_rgb_load_constant(uint absolute_index,out vec4 value);
bool rsx_pool_rgb_sample2d(uint material,uint unit,vec2 uv,float lod,out vec4 rgba);
bool rsx_pool_rgb_sample_cube(uint material,uint unit,vec3 direction,float lod,out vec4 rgba);
struct RsxPoolRgbTerms {
    vec3 direct[4]; // order:13,12,20,22; branch products, not a regrouped composer
    vec3 reflectionDirection; // exact source-space cube lookup direction, PS:83
    vec3 diffuseProxy;vec3 diffuseDirect[4]; // authored base*(1-cube weight)
    bool transportValid,reflectionValid; // independent diffuse/direction gates
    vec3 prefloor;vec3 floored;vec3 additive;vec3 overlay;
    vec2 fog_weights;vec3 fog_colors[2];vec3 postfog_additive;
};
struct RsxPoolRgbOutput {
    vec4 originalRGBA;vec4 composedRGBA;vec4 mrt[4];RsxPoolRgbTerms source;
};
vec4 rsx_pool_rgb_sanitize(vec4 x){return mix(x,vec4(0),isnan(x));}
bool rsx_pool_rgb_transport_finite3(vec3 x){return !any(isnan(x))&&!any(isinf(x));}
// Internal exact register replay. Scales are inserted only at direct branches;
// unity scales skip multiplication so the original instruction order survives.
int rsx_pool_rgb_replay(uint material,RsxSceneAlphaVertexOutput v,
    vec4 fp_constants[43],float lod[7],vec4 visibility,vec3 indirect,
    out vec4 mrt[4],out RsxPoolRgbTerms terms){
    precise vec4 r[48];precise vec4 h[48];
    for(int i=0;i<48;++i){r[i]=vec4(0);h[i]=vec4(0);}
    precise vec4 cc0=vec4(0),cc1=vec4(0);
    terms.transportValid=false;terms.reflectionValid=false;
    // source line 41, register statement 1
    { vec4 sampled;if(!rsx_pool_rgb_sample2d(material,0u,((v.t0).zwzz).xy,lod[0],sampled))return -3;precise vec4 _v=vec4(sampled);r[0].xyz = _v.xyz; }
    // source line 42, register statement 2
    { precise vec4 _v=vec4(((r[0]).xyzw) * ((fp_constants[0]).xxxx) + ((fp_constants[0]).yyyy));r[4].xyz = _v.xyz; }
    // source line 43, register statement 3
    { precise vec4 _v=vec4((v.t0).zwzz);r[0].xy = _v.xy; }
    // source line 44, register statement 4
    { precise vec4 _v=vec4(((r[0]).xxxy) + ((fp_constants[1]).xxxy));r[1].zw = _v.zw; }
    // source line 45, register statement 5
    { precise vec4 _v=vec4(((r[0]).xyzw) + ((fp_constants[2]).xzzw));r[0].xy = _v.xy; }
    // source line 46, register statement 6
    { vec4 sampled;if(!rsx_pool_rgb_sample2d(material,0u,((r[0]).xyzw).xy,lod[0],sampled))return -3;precise vec4 _v=vec4(sampled);r[3].xyz = _v.xyz; }
    // source line 47, register statement 7
    { precise vec4 _v=vec4(vec4(normalize(((v.t4).xyzw).xyz), 1.0));h[2].xyz = _v.xyz; }
    // source line 48, register statement 8
    { vec4 sampled;if(!rsx_pool_rgb_sample2d(material,0u,((r[1]).zwzz).xy,lod[0],sampled))return -3;precise vec4 _v=vec4(sampled);r[2].xyz = _v.xyz; }
    // source line 49, register statement 9
    { precise vec4 _v=vec4(vec4(normalize(((r[4]).xyzw).xyz), 1.0));h[0].xyz = _v.xyz; }
    // source line 50, register statement 10
    { precise vec4 _v=vec4(((h[0]).xyzw) + ((r[2]).xyzw));r[2].xyz = _v.xyz; }
    // source line 51, register statement 11
    { vec4 sampled;if(!rsx_pool_rgb_sample2d(material,2u,((v.t0).zwzz).xy,lod[2],sampled))return -3;precise vec4 _v=vec4(sampled);h[8].xz = _v.xz; }
    // source line 52, register statement 12
    { precise vec4 _v=vec4((h[2]).xyzw);h[1].x = _v.x; }
    // source line 53, register statement 13
    { precise vec4 _v=vec4(((h[8]).zzzz) + ((fp_constants[3]).xxxx));h[8].w = _v.w; }
    // source line 54, register statement 14
    { precise vec4 _v=vec4(-((h[2]).xyzw));h[1].yz = _v.yz; }
    // source line 55, register statement 15
    { precise vec4 _v=vec4(min(((h[8]).xyzw), ((fp_constants[4]).xxxx)));h[1].w = _v.w; }
    // source line 56, register statement 16
    { precise vec4 _v=vec4(((r[2]).xyzw) + ((r[3]).xyzw));r[2].xyz = _v.xyz; }
    // source line 57, register statement 17
    { precise vec4 _v=vec4(((r[2]).xyzw) + ((fp_constants[5]).xxxx));h[3].xyz = _v.xyz; }
    // source line 58, register statement 18
    { precise vec4 _v=vec4(dot(((h[0]).xyzw).xyz, ((h[2]).xyzw).xyz));r[3].w = _v.w; }
    // source line 59, register statement 19
    { precise vec4 _v=vec4(vec4(normalize(((h[3]).xyzw).xyz), 1.0));h[4].xyz = _v.xyz; }
    // source line 60, register statement 20
    { precise vec4 _v=vec4(dot(((h[4]).xyzw).xyz, ((h[1]).xyzw).xyz)) * 2.0;r[2].z = _v.z; }
    // source line 61, register statement 21
    { precise vec4 _v=vec4(vec4(normalize(((v.t8).xyzw).xyz), 1.0));h[3].xyz = _v.xyz; }
    // source line 62, register statement 22
    { precise vec4 _v=vec4((-((h[4]).xyzw)) * ((r[2]).zzzz) + ((h[1]).xyzw));r[2].xyz = _v.xyz; }
    // source line 63, register statement 23
    { precise vec4 _v=vec4(vec4(normalize(((r[2]).xyzw).xyz), 1.0));h[9].xyz = _v.xyz; }
    // source line 64, register statement 24
    { precise vec4 _v=vec4(((h[9]).yyyy) * ((h[3]).xyzw));r[2].xyz = _v.xyz; }
    // source line 65, register statement 25
    { precise vec4 _v=vec4(vec4(normalize(((v.t3).xyzw).xyz), 1.0));h[1].xyz = _v.xyz; }
    // source line 66, register statement 26
    { precise vec4 _v=vec4(dot(((h[0]).xyzw).xyz, ((h[1]).xyzw).xyz)) / 2.0;r[1].z = _v.z; }
    // source line 67, register statement 27
    { precise vec4 _v=vec4((fp_constants[6]).xxxx);r[1].w = _v.w; }
    // source line 68, register statement 28
    { precise vec4 _v=vec4(((h[2]).xyzw) + ((h[1]).xyzw));h[1].xyz = _v.xyz; }
    // source line 69, register statement 29
    { precise vec4 _v=vec4(((r[1]).xyzw) + ((fp_constants[7]).xxxx));r[1].z = _v.z; }
    // source line 70, register statement 30
    { precise vec4 _v=vec4(vec4(normalize(((v.t5).xyzw).xyz), 1.0));h[6].xyz = _v.xyz; }
    // source line 71, register statement 31
    { precise vec4 _v=vec4(((h[9]).xxxx) * ((h[6]).xyzw) + ((r[2]).xyzw));r[3].xyz = _v.xyz; }
    // source line 72, register statement 32
    { precise vec4 _v=vec4(vec4(normalize(((v.t7).xyzw).xyz), 1.0));h[2].xyz = _v.xyz; }
    // source line 73, register statement 33
    { precise vec4 _v=vec4(dot(((h[0]).xyzw).xyz, ((h[2]).xyzw).xyz)) / 2.0;r[1].x = _v.x; }
    // source line 74, register statement 34
    { precise vec4 _v=vec4(vec4(normalize(((h[1]).xyzw).xyz), 1.0));h[1].xyz = _v.xyz; }
    // source line 75, register statement 35
    { precise vec4 _v=vec4(dot(((h[0]).xyzw).xyz, ((h[1]).xyzw).xyz));r[0].y = _v.y; }
    // source line 76, register statement 36
    { precise vec4 _v=vec4(((r[1]).xxxx) + ((fp_constants[8]).xxxx));r[4].z = _v.z; }
    // source line 77, register statement 37
    { vec4 sampled;if(!rsx_pool_rgb_sample2d(material,4u,((r[1]).zwzz).xy,lod[4],sampled))return -3;precise vec4 _v=vec4(sampled);h[0].x = _v.x; }
    // source line 78, register statement 38
    { precise vec4 _v=vec4(max(((r[0]).xyzw), ((fp_constants[9]).xxxx)));r[0].y = _v.y; }
    // source line 79, register statement 39
    { precise vec4 _v=vec4(vec4(normalize(((v.t2).xyzw).xyz), 1.0));h[1].xyz = _v.xyz; }
    // source line 80, register statement 40
    { precise vec4 _v=vec4(((h[9]).zzzz) * ((h[1]).xyzw) + ((r[3]).xyzw));r[3].xyz = _v.xyz; }
    // source line 81, register statement 41
    { vec4 sampled;if(!rsx_pool_rgb_sample2d(material,1u,((v.t0).xyzw).xy,lod[1],sampled))return -3;precise vec4 _v=vec4(sampled);h[2].xyzw = _v.xyzw; }
    // source line 82, register statement 42
    { precise vec4 _v=vec4(((v.col0).xyzw) * ((h[2]).xyzw)) * 2.0;h[4].xyzw = _v.xyzw; }
    // source line 83, register statement 43
    { precise vec4 _v=vec4(vec4(normalize(((r[3]).xyzw).xyz), 1.0));h[5].xyz = _v.xyz; }
    terms.reflectionDirection=h[5].xyz;
    terms.reflectionValid=rsx_pool_rgb_transport_finite3(terms.reflectionDirection)&&dot(terms.reflectionDirection,terms.reflectionDirection)>0.0;
    // source line 84, register statement 44
    { precise vec4 _v=vec4(((h[4]).xyzw) * (-((h[1]).wwww)) + ((h[4]).xyzw));h[3].xyz = _v.xyz; }
    terms.diffuseProxy=h[3].xyz;
    terms.transportValid=rsx_pool_rgb_transport_finite3(h[4].xyz)&&rsx_pool_rgb_transport_finite3(terms.diffuseProxy)&&!isnan(h[1].w)&&!isinf(h[1].w)&&h[1].w>=0.0&&h[1].w<=1.0&&all(greaterThanEqual(terms.diffuseProxy,vec3(0)))&&all(lessThanEqual(terms.diffuseProxy,vec3(1)));
    // source line 85, register statement 45
    { precise vec4 _v=vec4((r[0]).xyzw);r[0].w = _v.w; }
    // source line 86, register statement 46
    { vec4 sampled;if(!rsx_pool_rgb_sample_cube(material,3u,((h[5]).xyzw).xyz,lod[3],sampled))return -3;precise vec4 _v=vec4(sampled);h[2].xyz = _v.xyz; }
    // source line 87, register statement 47
    { precise vec4 _v=vec4(((h[8]).xxxx) * ((fp_constants[10]).yyyy));h[2].w = _v.w; }
    // source line 88, register statement 48
    { precise vec4 _v=vec4(((h[1]).wwww) * ((h[2]).xyzw) + ((h[3]).xyzw));h[1].xyz = _v.xyz; }
    // source line 89, register statement 49
    { precise vec4 _v=vec4(((h[2]).xyzw) * ((h[2]).wwww) + (-((h[2]).wwww)));h[2].xyz = _v.xyz; }
    // source line 90, register statement 50
    { precise vec4 _v=vec4(log2(((r[0]).yyyy).x));r[0].y = _v.y; }
    // source line 91, register statement 51
    { precise vec4 _v=vec4(((h[8]).xxxx) * ((h[2]).xyzw) + ((h[8]).xxxx));h[2].xyz = _v.xyz; }
    // source line 92, register statement 52
    { precise vec4 _v=vec4(((r[0]).yyyy) * ((fp_constants[11]).xxxx));r[1].w = _v.w; }
    // source line 93, register statement 53
    { precise vec4 _v=vec4(exp2(((r[1]).wwww).x));h[2].w = _v.w; }
    // source line 94, register statement 54
    { precise vec4 _v=vec4((-(abs((r[3]).wwww))) + ((fp_constants[12]).xxxx));_v=clamp(_v,vec4(0),vec4(1));r[3].z = _v.z; }
    // source line 95, register statement 55
    { precise vec4 _v=vec4(((h[2]).wwww) * ((h[2]).xyzw));h[5].xyz = _v.xyz; }
    // source line 96, register statement 56
    { precise vec4 _v=vec4((fp_constants[13]).xxxx);r[4].w = _v.w; }
    // source line 97, register statement 57
    { precise vec4 _v=vec4(((h[4]).xyzw) * ((fp_constants[14]).xyzw));h[4].xyz = _v.xyz; }
    // source line 98, register statement 58
    terms.direct[1]=(h[1].xyz*h[0].xxx)*fp_constants[17].xyz;
    terms.diffuseDirect[1]=(terms.diffuseProxy*h[0].xxx)*fp_constants[17].xyz;
    if(visibility.y!=1.0)h[0].x=h[0].x*visibility.y;
    { precise vec4 _v=vec4(((h[1]).xyzw) * ((h[0]).xxxx) + ((h[5]).xyzw));h[3].xyz = _v.xyz; }
    // source line 99, register statement 59
    { precise vec4 _v=vec4(((r[3]).zzzz) * ((fp_constants[15]).yyyy) + ((fp_constants[15]).xxxx));r[3].x = _v.x; }
    // source line 100, register statement 60
    { vec4 sampled;if(!rsx_pool_rgb_sample2d(material,4u,((r[4]).zwzz).xy,lod[4],sampled))return -3;precise vec4 _v=vec4(sampled);h[0].x = _v.x; }
    // source line 101, register statement 61
    { precise vec4 _v=vec4((v.col1).xyzw);h[2].xyzw = _v.xyzw; }
    // source line 102, register statement 62
    terms.direct[0]=(h[1].xyz*h[0].xxx)*fp_constants[16].xyz;
    terms.diffuseDirect[0]=(terms.diffuseProxy*h[0].xxx)*fp_constants[16].xyz;
    if(visibility.x!=1.0)h[0].x=h[0].x*visibility.x;
    { precise vec4 _v=vec4(((h[1]).xyzw) * ((h[0]).xxxx) + ((h[5]).xyzw));h[0].xyz = _v.xyz; }
    // source line 103, register statement 63
    { precise vec4 _v=vec4(((h[0]).xyyz) * ((fp_constants[16]).xyyz));h[0].xzw = _v.xzw; }
    // source line 104, register statement 64
    { precise vec4 _v=vec4(((h[3]).xyzw) * ((fp_constants[17]).xyzw) + ((h[0]).xzww));h[3].xyz = _v.xyz; }
    // source line 105, register statement 65
    terms.direct[3]=h[1].xyz*(h[2].www*fp_constants[18].xyz);
    terms.diffuseDirect[3]=terms.diffuseProxy*(h[2].www*fp_constants[18].xyz);
    { precise vec4 _v=vec4(((h[2]).wwww) * ((fp_constants[18]).xyzw));if(visibility.w!=1.0)_v.xyz=_v.xyz*visibility.w;h[5].xyz = _v.xyz; }
    // source line 106, register statement 66
    terms.direct[2]=h[1].xyz*(h[2].zzz*fp_constants[19].xyz);
    terms.diffuseDirect[2]=terms.diffuseProxy*(h[2].zzz*fp_constants[19].xyz);
    if(visibility.z!=1.0)h[2].z=h[2].z*visibility.z;
    { precise vec4 _v=vec4(((h[2]).zzzz) * ((fp_constants[19]).xyzw) + ((h[5]).xyzw));h[5].xyz = _v.xyz; }
    for(int i=0;i<4;++i)terms.transportValid=terms.transportValid&&rsx_pool_rgb_transport_finite3(terms.diffuseDirect[i])&&all(greaterThanEqual(terms.diffuseDirect[i],vec3(0)));
    // source line 107, register statement 67
    { precise vec4 _v=vec4(log2(((r[3]).xyzw).x));r[1].y = _v.y; }
    // source line 108, register statement 68
    { precise vec4 _v=vec4(((r[1]).yyyy) * ((fp_constants[20]).zzzz));r[3].w = _v.w; }
    // source line 109, register statement 69
    { vec4 sampled;if(!rsx_pool_rgb_sample2d(material,2u,((v.t0).xyzw).xy,lod[2],sampled))return -3;precise vec4 _v=vec4(sampled);h[0].y = _v.y; }
    // source line 110, register statement 70
    { precise vec4 _v=vec4(((h[0]).yyyy) * ((h[1]).xyzw));h[0].xyz = _v.xyz; }
    // source line 111, register statement 71
    { precise vec4 _v=vec4((fp_constants[21]).xyzw);h[6].xyz = _v.xyz; }
    // source line 112, register statement 72
    { precise vec4 _v=vec4((r[3]).xyzw);r[3].w = _v.w; }
    // source line 113, register statement 73
    { precise vec4 _v=vec4(exp2(((r[3]).wwww).x));h[2].z = _v.z; }
    // source line 114, register statement 74
    { precise vec4 _v=vec4(((h[4]).xyzw) * ((h[2]).zzzz) + ((h[3]).xyzw));h[3].xyz = _v.xyz; }
    // source line 115, register statement 75
    { precise vec4 _v=vec4((v.t4).xyzw);h[0].w = _v.w; }
    // source line 116, register statement 76
    { precise vec4 _v=vec4(((h[1]).xyzw) * ((h[5]).xyzw) + ((h[3]).xyzw));h[1].xyz = _v.xyz; }
    // source line 117, register statement 77
    { precise vec4 _v=vec4(((h[0]).xyzw) * ((fp_constants[22]).xxxx) + ((h[1]).xyzw));h[1].xyz = _v.xyz; }
    // source line 118, register statement 78
    { precise vec4 _v=vec4((fp_constants[23]).yyyy);r[3].w = _v.w; }
    // source line 119, register statement 79
    { precise vec4 _v=vec4(((h[0]).wwww) * ((fp_constants[24]).xyzw));h[0].xyz = _v.xyz; }
    // source line 120, register statement 80
    { precise vec4 _v=vec4(((h[0]).xyzw) * ((h[8]).xxxx) + ((h[1]).xyzw));h[0].xyz = _v.xyz; }
    // source line 121, register statement 81
    { precise vec4 _v=vec4((-((h[2]).xxxx)) * ((fp_constants[25]).xyzw) + ((fp_constants[25]).xyzw)) / 2.0;h[4].xyz = _v.xyz; }
    // source line 122, register statement 82
    { precise vec4 _v=vec4(((r[3]).zzzz) * ((fp_constants[26]).yyyy) + ((fp_constants[26]).xxxx));r[0].w = _v.w; }
    // source line 123, register statement 83
    { precise vec4 _v=vec4(log2(((r[0]).wwww).x));r[4].z = _v.z; }
    // source line 124, register statement 84
    { precise vec4 _v=vec4(((r[3]).zwzz) + ((fp_constants[27]).xyxx));r[4].xy = _v.xy; }
    // source line 125, register statement 85
    { precise vec4 _v=vec4(((r[4]).zzzz) * ((fp_constants[28]).zzzz));r[0].w = _v.w; }
    // source line 126, register statement 86
    { vec4 sampled;if(!rsx_pool_rgb_sample2d(material,6u,((r[4]).xyzw).xy,lod[6],sampled))return -3;precise vec4 _v=vec4(sampled);h[5].xyz = _v.xyz; }
    // source line 127, register statement 87
    { precise vec4 _v=vec4(((h[5]).xyzw) * ((fp_constants[29]).zzzz));h[5].xyz = _v.xyz; }
    terms.postfog_additive=h[5].xyz;
    // source line 128, register statement 88
    { precise vec4 _v=vec4(exp2(((r[0]).wwww).x));h[5].w = _v.w; }
    // source line 129, register statement 89
    { precise vec4 _v=vec4(((r[3]).zzzz) * ((fp_constants[30]).yyyy) + ((fp_constants[30]).xxxx));r[0].w = _v.w; }
    // source line 130, register statement 90
    terms.prefloor=h[0].xyz;
    if(any(notEqual(indirect,vec3(0))))h[0].xyz=h[0].xyz+indirect;
    { precise vec4 _v=vec4(max(((h[0]).xyzw), ((fp_constants[31]).zzzz)));h[0].xyz = _v.xyz; }
    terms.floored=h[0].xyz;
    // source line 131, register statement 91
    { precise vec4 _v=vec4(log2(((r[0]).wwww).x));r[3].z = _v.z; }
    // source line 132, register statement 92
    { precise vec4 _v=vec4(((h[5]).wwww) * ((h[6]).xyzw) + ((fp_constants[32]).xyzw));h[6].xyz = _v.xyz; }
    terms.additive=h[6].xyz;
    // source line 133, register statement 93
    { precise vec4 _v=vec4(((r[3]).zzzz) * ((fp_constants[33]).zzzz));r[3].w = _v.w; }
    // source line 134, register statement 94
    { precise vec4 _v=vec4((r[3]).xyzw);r[3].xy = _v.xy; }
    // source line 135, register statement 95
    { precise vec4 _v=vec4(exp2(((r[3]).wwww).x));h[1].w = _v.w; }
    // source line 136, register statement 96
    { precise vec4 _v=vec4(((h[0]).xyzw) + ((h[6]).xyzw));h[0].xyz = _v.xyz; }
    // source line 137, register statement 97
    { precise vec4 _v=vec4(((h[4]).xyzw) * ((h[1]).xyzw));h[0].w = _v.w; }
    // source line 138, register statement 98
    { vec4 sampled;if(!rsx_pool_rgb_sample2d(material,5u,((v.t1).zwzz).xy,lod[5],sampled))return -3;precise vec4 _v=vec4(sampled);h[1].xyz = _v.xyz; }
    terms.overlay=h[1].xyz;terms.fog_weights=v.col1.xy;terms.fog_colors[0]=fp_constants[25].xyz;terms.fog_colors[1]=fp_constants[36].xyz;
    // source line 139, register statement 99
    { precise vec4 _v=vec4(((h[4]).xyzw) + ((h[1]).xyzw));h[1].w = _v.w; }
    // source line 140, register statement 100
    { precise vec4 _v=vec4(((h[1]).xyzw) * ((h[0]).xyzw));h[0].xyz = _v.xyz; }
    // source line 141, register statement 101
    { precise vec4 _v=vec4(((h[1]).xyzw) * (-((fp_constants[34]).xyzw)) + ((h[1]).xyzw));h[1].w = _v.w; }
    // source line 142, register statement 102
    { precise vec4 _v=vec4((r[2]).xyzw);r[2].xy = _v.xy; }
    // source line 143, register statement 103
    { precise vec4 _v=vec4(((h[0]).xyzw) * ((h[2]).xxxx) + ((h[4]).xyzw)) * 2.0;h[0].xyz = _v.xyz; }
    // source line 144, register statement 104
    { precise vec4 _v=vec4(((h[0]).xyzw) * ((fp_constants[35]).xyzw) + ((h[1]).xyzw));h[0].w = _v.w; }
    // source line 145, register statement 105
    { precise vec4 _v=vec4((-((h[2]).yyyy)) * ((fp_constants[36]).xyzw) + ((fp_constants[36]).xyzw));h[3].xyz = _v.xyz; }
    // source line 146, register statement 106
    { precise vec4 _v=vec4(min(((h[0]).xyzw), ((fp_constants[37]).xyzw)));h[2].w = _v.w; }
    // source line 147, register statement 107
    { precise vec4 _v=vec4(((h[0]).xyzw) * ((h[2]).yyyy) + ((h[3]).xyzw));h[2].xyz = _v.xyz; }
    // source line 148, register statement 108
    { precise vec4 _v=vec4((r[1]).xyzw);r[1].xy = _v.xy; }
    // source line 149, register statement 109
    { precise vec4 _v=vec4(((h[2]).xyzw) * ((fp_constants[38]).xxxx) + ((h[5]).xyzw));h[0].xyz = _v.xyz; }
    // source line 150, register statement 110
    { precise vec4 _v=vec4(((h[4]).xyzw) * ((fp_constants[39]).xyzw) + ((h[2]).xyzw));h[0].w = _v.w; }
    // source line 151, register statement 111
    { precise vec4 _v=vec4(max(((h[0]).xyzw), ((fp_constants[40]).xxxx)));h[0].xyzw = _v.xyzw; }
    // source line 152, register statement 112
    { precise vec4 _v=vec4(min(((h[0]).wwww), ((fp_constants[41]).xxxx)));h[1].x = _v.x; }
    // source line 153, register statement 113
    { precise vec4 _v=vec4(((h[1]).xxxx) * ((fp_constants[42]).xxxx));r[0].w = _v.w; }
    // source line 154, register statement 114
    { precise vec4 _v=vec4((h[0]).xyzw);r[0].xyz = _v.xyz; }
    mrt[0]=rsx_pool_rgb_sanitize(r[0]);mrt[1]=rsx_pool_rgb_sanitize(r[2]);
    mrt[2]=rsx_pool_rgb_sanitize(r[3]);mrt[3]=rsx_pool_rgb_sanitize(r[4]);return 0;
}
// Failure leaves result unchanged. fp_count includes the declared alpha vec4.
// Original source uses43 constants and no discard; alpha reference is unused.
int rsx_pool_rgb_evaluate(bool qualifiedSource,bool extendedCoordinates,uint material,uint fp_base,uint fp_count,
    RsxSceneAlphaVertexOutput v,float lod[7],vec4 visibility,vec3 indirect,
    inout RsxPoolRgbOutput result){
    if(!qualifiedSource)return -1;
    if(fp_count!=44u || fp_base>0xffffffffu-43u)return -2;
    if(any(isnan(visibility))||any(isinf(visibility))||any(lessThan(visibility,vec4(0)))||
       any(greaterThan(visibility,vec4(1)))||any(isnan(indirect))||any(isinf(indirect)))return -4;
    for(int i=0;i<7;++i)if(isnan(lod[i])||isinf(lod[i]))return -4;
    vec4 constants[43];for(uint i=0u;i<43u;++i)if(!rsx_pool_rgb_load_constant(fp_base+i,constants[i]))return -2;
    if(!extendedCoordinates){v.t8=vec4(0);v.t9=vec4(0);}
    RsxPoolRgbOutput answer;int status=rsx_pool_rgb_replay(material,v,constants,lod,vec4(1),vec3(0),answer.mrt,answer.source);
    if(status!=0)return status;
    answer.originalRGBA=answer.mrt[0];answer.composedRGBA=answer.originalRGBA;
    if(any(notEqual(visibility,vec4(1)))||any(notEqual(indirect,vec3(0)))){
        vec4 modified[4];RsxPoolRgbTerms discarded;
        status=rsx_pool_rgb_replay(material,v,constants,lod,visibility,indirect,modified,discarded);
        if(status!=0)return status;answer.composedRGBA=modified[0];
    }
    result=answer;return 0;
}
#endif
