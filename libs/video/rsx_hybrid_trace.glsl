// Hybrid transport: raster primary visibility, captured post-VS geometry for
// secondary rays. Unknown shaders use an explicitly approximate base material.
//
// Lighting is traced once per logical pixel (sample 0 of the MSAA grid) and
// stored demodulated as out = raster*M + A, so the spatial/temporal filters
// never blur texture detail and every MSAA sample reuses the matching pixel.
// One stable key light owns all traced direct shadows; the CPU selects it with
// hysteresis so shadows do not change owner when draw order or culling does.
//
// Trace-grid planes (uint, pixelCount() each): 0..2 packed M/A, 3 octahedral
// normal, 4 view depth, 5..7 packed M/A ping-pong and final temporal result.
// History: two banks of uvec4(packed M/A, depth) per pixel plus a sky slot.
#define HYBRID_FP (uint(p.settings.w))
vec4 hybridFp(uint offset){return fp.value[HYBRID_FP+offset];}
ivec2 hybridExtent(){return ivec2(int(p.dims.z*p.counts.z),int(p.dims.w));}
ivec2 hybridTexel(uvec2 pixel){return ivec2(int(pixel.x*p.counts.z),int(pixel.y));}
vec3 hybridTexelPosition(ivec2 texel){
    texel=clamp(texel,ivec2(0),hybridExtent()-1);
    float z=texelFetch(rasterDepth,texel,0).r;
    if(isnan(z)||isinf(z)||z<0.0||z>=1.0)return vec3(0);
    float depth=p.projection.w/(z-p.projection.z);
    if(isnan(depth)||isinf(depth)||depth<=0.0)return vec3(0);
    float offset=p.counts.z==2u?(texel.x%2==0?.75:.25):.5;
    vec2 uv=(vec2(texel.x/int(p.counts.z),texel.y)+offset)/vec2(p.dims.zw);
    uv-=hybridFp(1u).xy/vec2(p.dims.zw);
    return vec3((2.0*uv.x-1.0)/p.projection.x,(1.0-2.0*uv.y)/p.projection.y,1.0)*depth;
}
float hybridViewDepth(ivec2 texel){
    float z=texelFetch(rasterDepth,texel,0).r;
    if(isnan(z)||isinf(z)||z<0.0||z>=1.0)return 0.0;
    float depth=p.projection.w/(z-p.projection.z);
    return isnan(depth)||isinf(depth)||depth<=0.0?0.0:depth;
}
vec2 hybridOctEncode(vec3 n){
    n/=abs(n.x)+abs(n.y)+abs(n.z);
    return n.z>=0.0?n.xy:(1.0-abs(n.yx))*vec2(n.x>=0.0?1.0:-1.0,n.y>=0.0?1.0:-1.0);
}
vec3 hybridOctDecode(vec2 e){
    vec3 n=vec3(e,1.0-abs(e.x)-abs(e.y));
    if(n.z<0.0)n.xy=(1.0-abs(n.yx))*vec2(n.x>=0.0?1.0:-1.0,n.y>=0.0?1.0:-1.0);
    return normalize(n);
}
uvec3 hybridPack(vec3 m,vec3 a){return uvec3(packHalf2x16(m.rg),packHalf2x16(vec2(m.b,a.r)),packHalf2x16(a.gb));}
void hybridUnpack(uvec3 v,out vec3 m,out vec3 a){
    vec2 x=unpackHalf2x16(v.x),y=unpackHalf2x16(v.y),z=unpackHalf2x16(v.z);m=vec3(x,y.x);a=vec3(y.y,z);
}
void hybridStore(uint first,uint index,vec3 m,vec3 a){
    uint n=pixelCount();uvec3 v=hybridPack(m,a);
    ao.value[n*first+index]=v.x;ao.value[n*(first+1u)+index]=v.y;ao.value[n*(first+2u)+index]=v.z;
}
void hybridLoad(uint first,uint index,out vec3 m,out vec3 a){
    uint n=pixelCount();
    hybridUnpack(uvec3(ao.value[n*first+index],ao.value[n*(first+1u)+index],ao.value[n*(first+2u)+index]),m,a);
}
float hybridGuideDepth(uint index){return uintBitsToFloat(ao.value[pixelCount()*4u+index]);}
vec3 hybridGuideNormal(uint index){return hybridOctDecode(unpackSnorm2x16(ao.value[pixelCount()*3u+index]));}
uint hybridSkySlot(){return 2u*pixelCount();}
vec3 hybridSky(){
    // Sky radiance is estimated once per frame and smoothed over time, so
    // ambient light does not flicker as sky pixels enter or leave the view.
    uvec4 s=history.value[hybridSkySlot()];
    vec3 sky=vec3(uintBitsToFloat(s.x),uintBitsToFloat(s.y),uintBitsToFloat(s.z));
    return s.w==1u && finite3(sky)?sky:clamp(hybridFp(0u).rgb,vec3(.025),vec3(1));
}
void hybridSkyUpdate(){
    ivec2 e=hybridExtent();vec3 sky=vec3(0);float weight=0.0;
    for(int j=0;j<4;j++)for(int i=0;i<16;i++){
        ivec2 q=ivec2((float(i)+.5)*float(e.x)/16.0,(float(j)+.5)*float(e.y)*.05);
        if(texelFetch(rasterDepth,q,0).r>=.99999){
            vec3 c=texelFetch(worldColor,q,0).rgb;
            if(finite3(c)){sky+=c;weight+=1.0;}
        }
    }
    // The CPU appends a decoded authored fog color after the draw constants.
    vec3 current=weight>0.0?clamp(sky/weight,vec3(.025),vec3(1)):clamp(hybridFp(0u).rgb,vec3(.025),vec3(1));
    uvec4 s=history.value[hybridSkySlot()];
    vec3 previous=vec3(uintBitsToFloat(s.x),uintBitsToFloat(s.y),uintBitsToFloat(s.z));
    bool reset=hybridFp(8u).z!=0.0 || s.w!=1u || !finite3(previous);
    vec3 value=reset?current:mix(previous,current,.08);
    history.value[hybridSkySlot()]=uvec4(floatBitsToUint(value.r),floatBitsToUint(value.g),floatBitsToUint(value.b),1u);
}
bool hybridNearest(vec3 origin,vec3 direction,float minimum,float maximum,
                   bool colorOwner,out uint hit,out vec2 bary,out float distance){
    countRay(2);hit=0xffffffffu;distance=maximum;bary=vec2(0);
    rayQueryEXT q;
    if(!colorOwner){
        // Plain closest hit (bounce, reflection): opaque hardware traversal.
        rayQueryInitializeEXT(q,scene,gl_RayFlagsOpaqueEXT,255,origin,minimum,direction,maximum);
        while(rayQueryProceedEXT(q)){}
        if(rayQueryGetIntersectionTypeEXT(q,true)!=gl_RayQueryCommittedIntersectionTriangleEXT)return false;
        hit=rayQueryGetIntersectionPrimitiveIndexEXT(q,true);distance=rayQueryGetIntersectionTEXT(q,true);
        bary=rayQueryGetIntersectionBarycentricsEXT(q,true);
        return true;
    }
    rayQueryInitializeEXT(q,scene,0,255,origin,minimum,direction,maximum);
    while(rayQueryProceedEXT(q))if(rayQueryGetIntersectionTypeEXT(q,false)==gl_RayQueryCandidateIntersectionTriangleEXT){
        uint candidate=rayQueryGetIntersectionPrimitiveIndexEXT(q,false);
        Material candidateMaterial=materials.value[indices.triangle[candidate].w];
        if(colorOwner && candidateMaterial.authored.y==0u)continue;
        float t=rayQueryGetIntersectionTEXT(q,false);
        float target=(minimum+maximum)*.5;
        bool choose=colorOwner?(hit==0xffffffffu || abs(t-target)<abs(distance-target)):(t<distance);
        if(hit!=0xffffffffu && abs(t-distance)<.00001){
            uint known=materials.value[indices.triangle[hit].w].authored.x;
            choose=(candidateMaterial.authored.x!=0u && known==0u) ||
                ((candidateMaterial.authored.x!=0u)==(known!=0u) && candidate<hit);
        }
        if(choose){
            hit=candidate;distance=t;bary=rayQueryGetIntersectionBarycentricsEXT(q,false);
            if(!colorOwner)rayQueryConfirmIntersectionEXT(q);
        }
    }
    return hit!=0xffffffffu;
}
vec3 hybridBase(uint primitive,vec2 bary,vec3 direction,float radius,out bool generic){
    Material m=materials.value[indices.triangle[primitive].w];
    generic=true;vec3 w;
    if(!homogeneousWeights(primitive,bary,w))w=vec3(1-bary.x-bary.y,bary);
    if(m.authored.x!=0u){
        RsxPoolRgbOutput source;vec3 exactWeights;
        if(homogeneousWeights(primitive,bary,exactWeights) &&
           authoredEvaluate(primitive,bary,direction,radius,vec4(1),vec3(0),source) && source.source.transportValid){
            generic=false;return clamp(source.source.diffuseProxy,vec3(0),vec3(.95));
        }
    }
    uvec3 ids=indices.triangle[primitive].xyz;
    vec2 uv=vs.value[ids.x*14u+4u].xy*w.x+vs.value[ids.y*14u+4u].xy*w.y+vs.value[ids.z*14u+4u].xy*w.z;
    vec3 vertex=vs.value[ids.x*14u+1u].rgb*w.x+vs.value[ids.y*14u+1u].rgb*w.y+vs.value[ids.z*14u+1u].rgb*w.z;
    if(!finite3(vertex))return vec3(.5);
    vec4 texel=vec4(1);uint slot=m.textures.z;
    float lod=0;
    rsx_ray_footprint_lod(point(primitive,0),point(primitive,1),point(primitive,2),
        vs.value[ids.x*14u+4u].xy,vs.value[ids.y*14u+4u].xy,vs.value[ids.z*14u+4u].xy,
        direction,radius,alphaTextureSize(slot),lod);
    if(isnan(lod)||isinf(lod))lod=0;
#define HYBRID_TEX(N) case N:texel=textureLod(alphaTexture##N,uv,lod);break;
    switch(slot){
    HYBRID_TEX(0) HYBRID_TEX(1) HYBRID_TEX(2) HYBRID_TEX(3) HYBRID_TEX(4) HYBRID_TEX(5) HYBRID_TEX(6) HYBRID_TEX(7)
    HYBRID_TEX(8) HYBRID_TEX(9) HYBRID_TEX(10) HYBRID_TEX(11) HYBRID_TEX(12) HYBRID_TEX(13) HYBRID_TEX(14) HYBRID_TEX(15)
    HYBRID_TEX(16) HYBRID_TEX(17) HYBRID_TEX(18) HYBRID_TEX(19) HYBRID_TEX(20) HYBRID_TEX(21) HYBRID_TEX(22) HYBRID_TEX(23)
    HYBRID_TEX(24) HYBRID_TEX(25) HYBRID_TEX(26) HYBRID_TEX(27) HYBRID_TEX(28) HYBRID_TEX(29) HYBRID_TEX(30) HYBRID_TEX(31)
    }
#undef HYBRID_TEX
    vec3 base=texel.rgb*vertex;
    return finite3(base)?clamp(base,vec3(0),vec3(.95)):vec3(.5);
}
// Alpha-tested cutouts (foliage, fences) keep their holes in shadow rays
// instead of casting solid quads. Coarse LOD: shadows are filtered anyway.
bool hybridCutoutKeep(uint primitive,vec2 bary){
    Material m=materials.value[indices.triangle[primitive].w];
    if((m.textures.w&0x200u)==0u || m.textures.z>=32u)return true;
    uvec3 ids=indices.triangle[primitive].xyz;vec3 w=vec3(1.0-bary.x-bary.y,bary);
    vec2 uv=vs.value[ids.x*14u+4u].xy*w.x+vs.value[ids.y*14u+4u].xy*w.y+vs.value[ids.z*14u+4u].xy*w.z;
    if(any(isnan(uv))||any(isinf(uv)))return true;
    float alpha=1.0;
#define HYBRID_ALPHA(N) case N:alpha=textureLod(alphaTexture##N,uv,1.0).a;break;
    switch(m.textures.z){
    HYBRID_ALPHA(0) HYBRID_ALPHA(1) HYBRID_ALPHA(2) HYBRID_ALPHA(3) HYBRID_ALPHA(4) HYBRID_ALPHA(5) HYBRID_ALPHA(6) HYBRID_ALPHA(7)
    HYBRID_ALPHA(8) HYBRID_ALPHA(9) HYBRID_ALPHA(10) HYBRID_ALPHA(11) HYBRID_ALPHA(12) HYBRID_ALPHA(13) HYBRID_ALPHA(14) HYBRID_ALPHA(15)
    HYBRID_ALPHA(16) HYBRID_ALPHA(17) HYBRID_ALPHA(18) HYBRID_ALPHA(19) HYBRID_ALPHA(20) HYBRID_ALPHA(21) HYBRID_ALPHA(22) HYBRID_ALPHA(23)
    HYBRID_ALPHA(24) HYBRID_ALPHA(25) HYBRID_ALPHA(26) HYBRID_ALPHA(27) HYBRID_ALPHA(28) HYBRID_ALPHA(29) HYBRID_ALPHA(30) HYBRID_ALPHA(31)
    }
#undef HYBRID_ALPHA
    return !(alpha<=m.depth_range.z);
}
float hybridVisibility(vec3 position,vec3 normal,vec3 direction,float maximum){
    countRay(2);rayQueryEXT q;
    // Depth-reconstructed positions lose precision with distance; a bias that
    // grows with depth avoids self-shadow speckle that flickers with motion.
    float bias=max(p.settings.y,.0004*abs(position.z));
    rayQueryInitializeEXT(q,scene,gl_RayFlagsTerminateOnFirstHitEXT,255,
                          position+normal*bias,bias,direction,maximum);
    while(rayQueryProceedEXT(q))
        if(rayQueryGetIntersectionTypeEXT(q,false)==gl_RayQueryCandidateIntersectionTriangleEXT &&
           hybridCutoutKeep(rayQueryGetIntersectionPrimitiveIndexEXT(q,false),rayQueryGetIntersectionBarycentricsEXT(q,false)))
            rayQueryConfirmIntersectionEXT(q);
    bool blocked=rayQueryGetIntersectionTypeEXT(q,true)==gl_RayQueryCommittedIntersectionTriangleEXT;
    if(blocked)countRay(28);
    return blocked?0.0:1.0;
}
// View distance of the far plane: z_ndc=1 at p14/(1-p10).
float hybridFar(){float f=p.projection.w/(1.0-p.projection.z);return f>0.0&&!isinf(f)?f:10000.0;}
// CPU-selected key light: fp+2 view-space vector toward the light (w=0
// directional), fp+3 radiance with w=1 when valid.
// A directional shadow ray reaches fp+8.w world units (at most half the far
// plane). Folklore's areas are enclosed by hills and tree walls that the
// game's own shadows never include; with a low sun an unbounded ray hit them
// from nearly every pixel and the whole frame went dark. Local casters
// (trees, rocks, characters) stay inside the range, and a fixed world range
// never moves a shadow in or out as the camera moves.
bool hybridKey(vec3 position,out vec3 direction,out float distance,out vec3 radiance){
    vec4 l=hybridFp(2u),c=hybridFp(3u);
    direction=vec3(0,0,1);distance=0.0;radiance=vec3(0);
    if(c.w!=1.0)return false;
    vec3 v=l.w==0.0?l.xyz:l.xyz/l.w-position;float d=length(v);
    if(isnan(d)||isinf(d)||d<=1e-5)return false;
    float range=hybridFp(8u).w>0.0?hybridFp(8u).w:150.0;
    direction=v/d;distance=l.w==0.0?min(range,.5*hybridFar()):d;radiance=clamp(c.rgb,vec3(0),vec3(4));
    return finite3(direction)&&finite3(radiance);
}
// Shadowed and unshadowed irradiance share the same ambient term; only the key
// light is traced, so every receiver agrees on which light casts shadows.
void hybridIrradiance(vec3 position,vec3 normal,bool traced,out vec3 lit,out vec3 unshadowed){
    vec3 ambient=hybridSky()*.35;lit=unshadowed=ambient;
    vec3 key,radiance;float distance;
    if(hybridKey(position,key,distance,radiance)){
        float cosine=max(dot(normal,key),0.0);
        if(cosine>0.0){
            vec3 direct=radiance*cosine;unshadowed+=direct;
            lit+=traced?direct*hybridVisibility(position,normal,key,distance):direct;
        }
    }
}
vec3 hybridDirect(uint hit,vec2 bary,vec3 origin,vec3 direction,float t,float radius){
    bool generic;vec3 base=hybridBase(hit,bary,direction,radius,generic);
    countRay(5);if(generic)countRay(4);
    vec3 normal=cross(point(hit,1)-point(hit,0),point(hit,2)-point(hit,0));
    float n=length(normal);if(n<=1e-10||!finite3(normal))return vec3(0);
    normal/=n;if(dot(normal,direction)>0)normal=-normal;
    vec3 lit,unshadowed;hybridIrradiance(origin+direction*t,normal,true,lit,unshadowed);
    return base*lit;
}
vec3 hybridRadiance(uint hit,vec2 bary,vec3 origin,vec3 direction,float t,float radius,bool allowReflection){
    vec3 radiance=hybridDirect(hit,bary,origin,direction,t,radius);
    Material material=materials.value[indices.triangle[hit].w];
    if(allowReflection && (material.textures.w&0x100u)!=0u){
        vec3 normal=cross(point(hit,1)-point(hit,0),point(hit,2)-point(hit,0));
        float len=length(normal);if(len<=1e-10||!finite3(normal))return radiance;
        normal/=len;if(dot(normal,direction)>0)normal=-normal;
        vec3 position=origin+direction*t;
        float bias=max(p.settings.y,.00005*abs(position.z));
        vec3 reflected=reflect(direction,normal),start=position+normal*bias;
        uint reflectedHit;vec2 reflectedBary;float reflectedDistance;countRay(29);
        vec3 incoming=hybridSky();
        if(hybridNearest(start,reflected,bias,10000.0,false,reflectedHit,reflectedBary,reflectedDistance)){
            countRay(30);
            incoming=hybridDirect(reflectedHit,reflectedBary,start,reflected,reflectedDistance,radius+reflectedDistance*.0007);
        }
        float fresnel=.04+.96*pow(1-max(dot(-direction,normal),0),5);
        radiance=mix(radiance,incoming,min(.6,fresnel));
    }
    return radiance;
}
void hybridTrace(uvec2 pixel){
    uint index=pixel.y*p.dims.x+pixel.x,n=pixelCount();
    ivec2 texel=hybridTexel(pixel);
    vec3 position=hybridTexelPosition(texel);if(position.z<=0)return;
    countRay(0);
    ivec2 step=ivec2(p.counts.z,1);
    vec3 left=hybridTexelPosition(texel-ivec2(step.x,0));
    vec3 right=hybridTexelPosition(texel+ivec2(step.x,0));
    vec3 up=hybridTexelPosition(texel-ivec2(0,1));
    vec3 down=hybridTexelPosition(texel+ivec2(0,1));
    vec3 dx=(left.z>0 && abs(position.z-left.z)<abs(right.z-position.z))?position-left:right-position;
    vec3 dy=(up.z>0 && abs(position.z-up.z)<abs(down.z-position.z))?position-up:down-position;
    vec3 normal=cross(dx,dy);float len=length(normal);
    if(!finite3(normal)||len<1e-10)return;
    normal/=len;if(dot(normal,position)>0)normal=-normal;
    vec3 view=normalize(position);float distance=length(position);
    uint primitive;vec2 bary;float t;
    // The ray only chooses a material near the raster surface; failure never
    // rejects raster primary visibility. Equal-distance candidates use index order.
    float tolerance=max(.01,distance*.005);
    bool owner=hybridNearest(vec3(0),view,max(.001,distance-tolerance),distance+tolerance,true,primitive,bary,t);
    uint material=owner?indices.triangle[primitive].w:0u;
    if(owner){countRay(8);uint family=materials.value[material].authored.x;if(family<=3u)countRay(9u+family);}
    vec3 raster=texelFetch(worldColor,texel,0).rgb;
    if(!finite3(raster)){countRay(6);return;}
    vec3 lit,unshadowed;hybridIrradiance(position,normal,true,lit,unshadowed);
    // 13: primary pixels the key light reaches, 14: primary pixels it shadows.
    if(any(greaterThan(unshadowed-lit,vec3(1e-4))))countRay(14);else countRay(13);
    float luminance=max(dot(unshadowed,vec3(.2126,.7152,.0722)),.15);
    // Preserve the game's primary shading and apply traced light visibility.
    vec3 M=mix(vec3(1),clamp(lit/max(unshadowed,vec3(.025)),vec3(0),vec3(1)),p.settings.z),A=vec3(0);
    bool generic=true;vec3 known=vec3(0);
    if(owner)known=hybridBase(primitive,bary,view,distance*.0007,generic);
    bool primaryGloss=owner && (materials.value[material].textures.w&0x100u)!=0u;
    uint seed=pathHash(index+uint(materials.value[0].depth_range.w)*0x9e3779b9u);
    vec3 bounce;float pdf;
    float bias=max(p.settings.y,.00005*position.z);
    vec3 origin=position+normal*bias;
    if(rsx_pt_cosine_sample(normal,pathRandom(seed),pathRandom(seed),bounce,pdf)){
        uint hit;vec2 b;float d;vec3 incoming;
        if(hybridNearest(origin,bounce,bias,10000.0,false,hit,b,d)){
            countRay(26);incoming=hybridRadiance(hit,b,origin,bounce,d,d*.001,!primaryGloss)*.5;
        }else incoming=hybridSky()*.15;
        // Generic base is raster/luminance: the bounce is then a pure
        // modulation. A decoded base stays additive, independent of texels.
        if(generic)M+=incoming/luminance;else A+=known*incoming;
    }
    // Roughness 0.8 for generic diffuse materials; explicit source cube
    // materials use a single glossy reflection ray (roughness 0.18).
    if(primaryGloss){
        vec3 reflection=reflect(view,normal);countRay(29);
        uint hit;vec2 b;float d;vec3 radiance=hybridSky();
        if(hybridNearest(origin,reflection,bias,10000.0,false,hit,b,d)){
            countRay(30);radiance=hybridRadiance(hit,b,origin,reflection,d,d*.0007,false);
        }
        float fresnel=min(.6,.04+.96*pow(1-max(dot(-view,normal),0),5));
        M*=1.0-fresnel;A=A*(1.0-fresnel)+radiance*fresnel;
    }
    if(!finite3(M)||!finite3(A)){countRay(6);return;}
    M=clamp(M,vec3(0),vec3(8));A=clamp(A,vec3(0),vec3(1));
    hybridStore(0u,index,M,A);
    ao.value[n*3u+index]=packSnorm2x16(hybridOctEncode(normal));
    ao.value[n*4u+index]=floatBitsToUint(position.z);
    countRay(1);countRay(24);
    vec3 delta=clamp(raster*M+A,vec3(0),vec3(1))-raster;
    if(any(greaterThan(abs(delta),vec3(1.0/255.0))))countRay(25);
}
// Edge-stopping a-trous over the demodulated lighting only (steps 1 and 2).
void hybridFilter(uvec2 pixel){
    uint index=pixel.y*p.dims.x+pixel.x;
    uint source=phase.mode==2u?0u:5u,destination=phase.mode==2u?5u:0u;
    vec3 M,A;hybridLoad(source,index,M,A);
    float depth=hybridGuideDepth(index);
    if(depth<=0.0){hybridStore(destination,index,M,A);return;}
    vec3 normal=hybridGuideNormal(index);
    vec3 sm=vec3(0),sa=vec3(0);float weights=0.0;int stepSize=1<<int(phase.mode-2u);
    for(int y=-1;y<=1;y++)for(int x=-1;x<=1;x++){
        ivec2 q=ivec2(pixel)+ivec2(x,y)*stepSize;
        if(any(lessThan(q,ivec2(0)))||any(greaterThanEqual(q,ivec2(p.dims.xy))))continue;
        uint qi=uint(q.y)*p.dims.x+uint(q.x);float d=hybridGuideDepth(qi);
        if(d<=0.0||abs(d-depth)>max(.02,depth*.01))continue;
        float alignment=dot(normal,hybridGuideNormal(qi));
        if(alignment<.9)continue;
        float weight=(x==0?2.0:1.0)*(y==0?2.0:1.0)*pow(max(alignment,0.0),16.0);
        vec3 qm,qa;hybridLoad(source,qi,qm,qa);
        if(!finite3(qm)||!finite3(qa))continue;
        sm+=qm*weight;sa+=qa*weight;weights+=weight;
    }
    if(weights>0.0){M=sm/weights;A=sa/weights;}
    hybridStore(destination,index,M,A);
}
// Temporal accumulation with camera reprojection (fp+4..7 map the current
// view position to the previous frame's clip space; the identity camera
// degenerates to same-pixel history). History is clamped to the current 3x3
// neighbourhood to limit ghosting from moving objects.
void hybridTemporal(uvec2 pixel){
    uint index=pixel.y*p.dims.x+pixel.x,n=pixelCount();
    vec4 flags=hybridFp(8u);uint bank=flags.y!=0.0?1u:0u,prior=1u-bank;
    vec3 position=hybridTexelPosition(hybridTexel(pixel));
    // A frame whose gather or trace was rejected keeps the previous frame's
    // reprojected lighting instead of dropping every traced shadow for a
    // frame (the visible pop). Raster depth still guides the composite.
    bool hold=(validity.flags&~2u)!=0u;
    if(hold){
        vec3 M=vec3(1),A=vec3(0);float depth=position.z;
        if(depth>0.0 && flags.z==0.0){
            vec4 world=vec4(position,1.0);
            vec4 clip=vec4(dot(hybridFp(4u),world),dot(hybridFp(5u),world),dot(hybridFp(6u),world),dot(hybridFp(7u),world));
            if(clip.w>1e-5 && finite3(clip.xyz)){
                vec2 ndc=clip.xy/clip.w;vec2 uv=vec2(ndc.x*.5+.5,.5-ndc.y*.5);
                float offset=p.counts.z==2u?.75:.5;
                ivec2 q=ivec2(floor(uv*vec2(p.dims.xy)-offset+.5));
                if(all(greaterThanEqual(q,ivec2(0)))&&all(lessThan(q,ivec2(p.dims.xy)))){
                    uvec4 h=history.value[prior*n+uint(q.y)*p.dims.x+uint(q.x)];
                    float previous=uintBitsToFloat(h.w);
                    if(previous>0.0 && abs(previous-clip.w)<max(.02,clip.w*.02)){
                        vec3 hm,ha;hybridUnpack(h.xyz,hm,ha);
                        if(finite3(hm)&&finite3(ha)){M=hm;A=ha;}
                    }
                }
            }
        }
        if(depth<=0.0)depth=0.0;
        hybridStore(5u,index,M,A);
        ao.value[n*4u+index]=floatBitsToUint(depth);
        history.value[bank*n+index]=depth>0.0?uvec4(hybridPack(M,A),floatBitsToUint(depth)):uvec4(0);
        return;
    }
    float depth=hybridGuideDepth(index);
    vec3 M,A;hybridLoad(0u,index,M,A);
    if(depth<=0.0){hybridStore(5u,index,M,A);history.value[bank*n+index]=uvec4(0);return;}
    vec3 lo=M,hi=M,alo=A,ahi=A;
    for(int y=-1;y<=1;y++)for(int x=-1;x<=1;x++){
        ivec2 q=ivec2(pixel)+ivec2(x,y);
        if((x==0&&y==0)||any(lessThan(q,ivec2(0)))||any(greaterThanEqual(q,ivec2(p.dims.xy))))continue;
        uint qi=uint(q.y)*p.dims.x+uint(q.x);float d=hybridGuideDepth(qi);
        if(d<=0.0||abs(d-depth)>max(.02,depth*.02))continue;
        vec3 qm,qa;hybridLoad(0u,qi,qm,qa);
        if(!finite3(qm)||!finite3(qa))continue;
        lo=min(lo,qm);hi=max(hi,qm);alo=min(alo,qa);ahi=max(ahi,qa);
    }
    if(flags.z==0.0 && position.z>0.0){
        vec4 world=vec4(position,1.0);
        vec4 clip=vec4(dot(hybridFp(4u),world),dot(hybridFp(5u),world),dot(hybridFp(6u),world),dot(hybridFp(7u),world));
        if(clip.w>1e-5 && finite3(clip.xyz)){
            vec2 ndc=clip.xy/clip.w;vec2 uv=vec2(ndc.x*.5+.5,.5-ndc.y*.5);
            float offset=p.counts.z==2u?.75:.5;
            ivec2 q=ivec2(floor(uv*vec2(p.dims.xy)-offset+.5));
            if(all(greaterThanEqual(q,ivec2(0)))&&all(lessThan(q,ivec2(p.dims.xy)))){
                uvec4 h=history.value[prior*n+uint(q.y)*p.dims.x+uint(q.x)];
                float previous=uintBitsToFloat(h.w);
                if(previous>0.0 && abs(previous-clip.w)<max(.02,clip.w*.01)){
                    vec3 hm,ha;hybridUnpack(h.xyz,hm,ha);
                    if(finite3(hm)&&finite3(ha)){
                        hm=clamp(hm,lo-vec3(.02),hi+vec3(.02));ha=clamp(ha,alo-vec3(.01),ahi+vec3(.01));
                        M=mix(M,hm,.85);A=mix(A,ha,.85);
                    }
                }
            }
        }
    }
    hybridStore(5u,index,M,A);
    uvec3 v=hybridPack(M,A);
    history.value[bank*n+index]=uvec4(v,floatBitsToUint(depth));
}
// Asynchronous transport: this frame's raster takes the lighting the previous
// frame's trace finished (the history bank it wrote, 1-fp+8.y), reprojected
// through fp+4..7 as in hybridTemporal. Missing or mismatched history keeps
// the raster unchanged for this frame.
void hybridLateComposite(uvec2 texel,inout vec4 original,out vec4 raw){
    raw=original;
    vec4 flags=hybridFp(8u);if(flags.z!=0.0)return;
    vec3 position=hybridTexelPosition(ivec2(texel));if(position.z<=0.0)return;
    vec4 world=vec4(position,1.0);
    vec4 clip=vec4(dot(hybridFp(4u),world),dot(hybridFp(5u),world),dot(hybridFp(6u),world),dot(hybridFp(7u),world));
    if(!(clip.w>1e-5)||!finite3(clip.xyz))return;
    vec2 ndc=clip.xy/clip.w;vec2 uv=vec2(ndc.x*.5+.5,.5-ndc.y*.5);
    float offset=p.counts.z==2u?.75:.5;
    ivec2 c=ivec2(floor(uv*vec2(p.dims.xy)-offset+.5));
    uint n=pixelCount(),bank=flags.y!=0.0?0u:1u;
    const ivec2 offsets[5]=ivec2[5](ivec2(0),ivec2(-1,0),ivec2(1,0),ivec2(0,-1),ivec2(0,1));
    float best=1e30;uvec4 chosen=uvec4(0);
    for(int k=0;k<5;k++){
        ivec2 q=c+offsets[k];
        if(any(lessThan(q,ivec2(0)))||any(greaterThanEqual(q,ivec2(p.dims.xy))))continue;
        uvec4 h=history.value[bank*n+uint(q.y)*p.dims.x+uint(q.x)];
        float d=uintBitsToFloat(h.w);if(!(d>0.0))continue;
        float e=abs(d-clip.w);if(e<best){best=e;chosen=h;}
    }
    if(best>max(.02,clip.w*.02))return;
    vec3 M,A;hybridUnpack(chosen.xyz,M,A);
    if(!finite3(M)||!finite3(A)){countRay(6);return;}
    original.rgb=clamp(original.rgb*M+A,vec3(0),vec3(1));raw=original;
    if(max(max(original.r,original.g),original.b)>.001)countRay(7);
}
void hybridComposite(uvec2 texel,inout vec4 original,out vec4 raw){
    raw=original;
    float depth=hybridViewDepth(ivec2(texel));
    if(depth<=0.0)return;
    // Each MSAA sample takes the traced pixel (centre or 4-neighbour) whose
    // surface depth matches, so edges never borrow lighting across surfaces.
    ivec2 c=ivec2(int(texel.x/p.counts.z),int(texel.y));
    const ivec2 offsets[5]=ivec2[5](ivec2(0),ivec2(-1,0),ivec2(1,0),ivec2(0,-1),ivec2(0,1));
    float best=1e30;uint chosen=0xffffffffu;
    for(int k=0;k<5;k++){
        ivec2 q=c+offsets[k];
        if(any(lessThan(q,ivec2(0)))||any(greaterThanEqual(q,ivec2(p.dims.xy))))continue;
        uint qi=uint(q.y)*p.dims.x+uint(q.x);float d=hybridGuideDepth(qi);
        if(d<=0.0)continue;
        float e=abs(d-depth);if(e<best){best=e;chosen=qi;}
    }
    if(chosen==0xffffffffu || best>max(.02,depth*.01))return;
    vec3 M,A;hybridLoad(5u,chosen,M,A);
    if(!finite3(M)||!finite3(A)){countRay(6);return;}
    original.rgb=clamp(original.rgb*M+A,vec3(0),vec3(1));raw=original;
    if(max(max(original.r,original.g),original.b)>.001)countRay(7);
}
