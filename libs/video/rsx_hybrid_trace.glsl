// Hybrid transport: raster primary visibility, captured post-VS geometry for
// secondary rays. Unknown shaders use an explicitly approximate base material.
vec3 hybridPosition(ivec2 pixel){
    ivec2 extent=ivec2(p.dims.x,p.dims.y);
    pixel=clamp(pixel,ivec2(0),extent-1);
    float z=texelFetch(rasterDepth,pixel,0).r;
    if(isnan(z)||isinf(z)||z<0.0||z>=1.0)return vec3(0);
    float depth=p.projection.w/(z-p.projection.z);
    if(isnan(depth)||isinf(depth)||depth<=0.0)return vec3(0);
    float offset=p.counts.z==2u?(pixel.x%2==0?.75:.25):.5;
    vec2 uv=(vec2(pixel/ivec2(p.counts.z,1))+offset)/vec2(p.dims.zw);
    uv-=fp.value[uint(p.settings.w)+1u].xy/vec2(p.dims.zw);
    return vec3((2.0*uv.x-1.0)/p.projection.x,(1.0-2.0*uv.y)/p.projection.y,1.0)*depth;
}
vec3 hybridSky(){
    // Only depth-clear pixels contribute to the sky estimate. An enclosed view
    // falls back to low ambient, explicitly not an invented directional light.
    vec3 sky=vec3(0);float weight=0;
    for(int i=0;i<8;i++){
        ivec2 q=ivec2((float(i)+.5)*float(p.dims.x)/8.0,float(p.dims.y)*.08);
        if(texelFetch(rasterDepth,q,0).r>=.99999){sky+=texelFetch(worldColor,q,0).rgb;weight+=1;}
    }
    // The CPU appends a decoded authored fog color after the draw constants.
    vec3 fog=fp.value[uint(p.settings.w)].rgb;
    return weight>0?clamp(sky/weight,vec3(.025),vec3(1)):clamp(fog,vec3(.025),vec3(1));
}
bool hybridNearest(vec3 origin,vec3 direction,float minimum,float maximum,
                   bool colorOwner,out uint hit,out vec2 bary,out float distance){
    countRay(2);hit=0xffffffffu;distance=maximum;bary=vec2(0);
    rayQueryEXT q;rayQueryInitializeEXT(q,scene,0,255,origin,minimum,direction,maximum);
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
float hybridVisibility(vec3 position,vec3 normal,vec3 direction,float maximum){
    countRay(2);rayQueryEXT q;
    float bias=max(p.settings.y,.00005*abs(position.z));
    rayQueryInitializeEXT(q,scene,gl_RayFlagsTerminateOnFirstHitEXT|gl_RayFlagsOpaqueEXT,255,
                          position+normal*bias,bias,direction,maximum);
    while(rayQueryProceedEXT(q)){}
    bool blocked=rayQueryGetIntersectionTypeEXT(q,true)==gl_RayQueryCommittedIntersectionTriangleEXT;
    if(blocked)countRay(28);
    return blocked?0.0:1.0;
}
vec3 hybridLighting(uint material,vec3 position,vec3 normal,bool shadows){
    Material m=materials.value[material];vec3 light=hybridSky()*.35;
    for(uint i=0;i<4;i++)if(m.light_rgb[i].w==1.0){
        vec4 l=m.light_view[i];vec3 vector=l.w==0?l.xyz:l.xyz/l.w-position;
        float d=length(vector);if(d<=.00001||isnan(d)||isinf(d))continue;
        vec3 direction=vector/d;float cosine=max(dot(normal,direction),0);
        if(cosine>0)light+=clamp(m.light_rgb[i].rgb,vec3(0),vec3(4))*cosine*
            (shadows?hybridVisibility(position,normal,direction,l.w==0?10000.0:d):1.0);
    }
    return light;
}
vec3 hybridDirect(uint hit,vec2 bary,vec3 origin,vec3 direction,float t,float radius){
    bool generic;vec3 base=hybridBase(hit,bary,direction,radius,generic);
    countRay(5);if(generic)countRay(4);
    vec3 normal=cross(point(hit,1)-point(hit,0),point(hit,2)-point(hit,0));
    float n=length(normal);if(n<=1e-10||!finite3(normal))return vec3(0);
    normal/=n;if(dot(normal,direction)>0)normal=-normal;
    return base*hybridLighting(indices.triangle[hit].w,origin+direction*t,normal,true);
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
    vec3 position=hybridPosition(ivec2(pixel));if(position.z<=0)return;
    countRay(0);
    ivec2 step=ivec2(p.counts.z,1);
    vec3 left=hybridPosition(ivec2(pixel)-ivec2(step.x,0));
    vec3 right=hybridPosition(ivec2(pixel)+ivec2(step.x,0));
    vec3 up=hybridPosition(ivec2(pixel)-ivec2(0,1));
    vec3 down=hybridPosition(ivec2(pixel)+ivec2(0,1));
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
    vec3 raster=texelFetch(worldColor,ivec2(pixel),0).rgb;
    if(!finite3(raster)){countRay(6);return;}
    vec3 unshadowed=hybridLighting(material,position,normal,false);
    vec3 shadowed=hybridLighting(material,position,normal,true);
    float luminance=max(dot(unshadowed,vec3(.2126,.7152,.0722)),.15);
    vec3 base=clamp(raster/luminance,vec3(0),vec3(.95));
    bool generic=true;
    if(owner){vec3 known=hybridBase(primitive,bary,view,distance*.0007,generic);if(!generic)base=known;}
    // Preserve the game's primary shading and apply traced light visibility.
    vec3 modified=raster*mix(vec3(1),clamp(shadowed/max(unshadowed,vec3(.025)),vec3(0),vec3(1)),p.settings.z);
    bool primaryGloss=owner && (materials.value[material].textures.w&0x100u)!=0u;
    uint seed=pathHash(index+uint(materials.value[0].depth_range.w)*0x9e3779b9u);
    vec3 bounce;float pdf;
    float bias=max(p.settings.y,.00005*position.z);
    vec3 origin=position+normal*bias;
    if(rsx_pt_cosine_sample(normal,pathRandom(seed),pathRandom(seed),bounce,pdf)){
        uint hit;vec2 b;float d;
        if(hybridNearest(origin,bounce,bias,10000.0,false,hit,b,d)){
            countRay(26);modified+=base*hybridRadiance(hit,b,origin,bounce,d,d*.001,!primaryGloss)*.5;
        }else modified+=base*hybridSky()*.15;
    }
    // Roughness 0.8 for generic diffuse materials; explicit source cube
    // materials use a single glossy reflection ray (roughness 0.18).
    if(primaryGloss){
        vec3 reflection=reflect(view,normal);countRay(29);
        uint hit;vec2 b;float d;vec3 radiance=hybridSky();
        if(hybridNearest(origin,reflection,bias,10000.0,false,hit,b,d)){
            countRay(30);radiance=hybridRadiance(hit,b,origin,reflection,d,d*.0007,false);
        }
        float fresnel=.04+.96*pow(1-max(dot(-view,normal),0),5);
        modified=mix(modified,radiance,min(.6,fresnel));
    }
    if(!finite3(modified)){countRay(6);return;}
    vec3 delta=clamp(modified,vec3(0),vec3(1))-raster;
    for(uint c=0;c<3;c++){
        ao.value[n*(3u+c)+index]=floatBitsToUint(normal[c]);
        ao.value[n*(7u+c)+index]=floatBitsToUint(delta[c]);
    }
    ao.value[n*6u+index]=floatBitsToUint(position.z);
    countRay(1);countRay(24);
    if(any(greaterThan(abs(delta),vec3(1.0/255.0))))countRay(25);
}
vec3 hybridDelta(uint index,uint first){
    uint n=pixelCount();return vec3(uintBitsToFloat(ao.value[n*first+index]),uintBitsToFloat(ao.value[n*(first+1u)+index]),uintBitsToFloat(ao.value[n*(first+2u)+index]));
}
void hybridFilter(uvec2 pixel){
    uint index=pixel.y*p.dims.x+pixel.x,n=pixelCount();
    uint source=phase.mode==3u?0u:7u,destination=phase.mode==3u?7u:0u;
    vec3 center=hybridDelta(index,source),normal=hybridDelta(index,3u);
    float depth=uintBitsToFloat(ao.value[n*6u+index]);
    vec3 sum=vec3(0);float weights=0;int stepSize=1<<int(phase.mode-2u);
    if(depth>0)for(int y=-1;y<=1;y++)for(int x=-1;x<=1;x++){
        ivec2 q=ivec2(pixel)+ivec2(x*int(p.counts.z),y)*stepSize;
        if(any(lessThan(q,ivec2(0)))||any(greaterThanEqual(q,ivec2(p.dims.xy))))continue;
        uint qi=uint(q.y)*p.dims.x+uint(q.x);float d=uintBitsToFloat(ao.value[n*6u+qi]);
        float alignment=dot(normal,hybridDelta(qi,3u));
        if(d<=0||alignment<.9||abs(d-depth)>max(.02,depth*.005))continue;
        float weight=(x==0?2:1)*(y==0?2:1)*pow(max(alignment,0),16);
        sum+=hybridDelta(qi,source)*weight;weights+=weight;
    }
    vec3 result=weights>0?sum/weights:center;
    for(uint c=0;c<3;c++)ao.value[n*(destination+c)+index]=floatBitsToUint(result[c]);
}
void hybridComposite(uvec2 pixel,inout vec4 original,out vec4 raw){
    uint index=pixel.y*p.dims.x+pixel.x,n=pixelCount();
    float depth=uintBitsToFloat(ao.value[n*6u+index]);
    raw=original;
    if(depth<=0){history.value[index*2u]=history.value[index*2u+1u]=vec4(0);return;}
    vec3 delta=hybridDelta(index,(RSX_TRANSPORT_FLAGS&2u)!=0u?0u:7u);
    vec3 normal=hybridDelta(index,3u);
    raw.rgb=clamp(original.rgb+delta,vec3(0),vec3(1));
    vec4 previous=history.value[index*2u],guide=history.value[index*2u+1u];
    bool accept=previous.w>0 && abs(previous.w-depth)<max(.01,depth*.002) && dot(normal,guide.xyz)>.97;
    // Same-pixel depth/normal rejection, with local transport clamp. Camera
    // reprojection is not claimed by this hybrid accumulator.
    if(accept && finite3(previous.xyz))delta=mix(delta,clamp(previous.xyz,delta-vec3(.06),delta+vec3(.06)),.8);
    history.value[index*2u]=vec4(delta,depth);history.value[index*2u+1u]=vec4(normal,1);
    original.rgb=clamp(original.rgb+delta,vec3(0),vec3(1));
    if(!finite3(original.rgb))countRay(6);
    if(max(max(original.r,original.g),original.b)>.001)countRay(7);
}
