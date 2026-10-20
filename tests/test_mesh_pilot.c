#include "rsx_mesh_pilot.h"
#include <assert.h>
int main(int argc,char **argv){
 assert(argc==3);rsx_mesh_pilot_state s={0};
 int ok=rsx_mesh_pilot_load(&s,argv[1]);
 if(!strcmp(argv[2],"reject")){assert(!ok);rsx_mesh_pilot_free(&s);return 0;}
 assert(ok && s.count==1);s.initialized=1;
 rsx_mesh_pilot_record *r=&s.records[0];unsigned char attrs[16]={0};
 for(unsigned i=0;i<r->na;i++)attrs[i]=(unsigned char)r->attrs[i];
 uint32_t *ix=(uint32_t*)(r->original+(size_t)r->nv*r->stride);
 assert(rsx_mesh_pilot_match(&s,4,r->original,r->nv,r->stride,ix,r->ni,attrs,r->na)==r);
 assert(!rsx_mesh_pilot_match(&s,5,r->original,r->nv,r->stride,ix,r->ni,attrs,r->na));
 assert(!rsx_mesh_pilot_match(&s,4,r->original,r->nv-1,r->stride,ix,r->ni,attrs,r->na));
 unsigned char *copy=malloc(r->original_size);assert(copy);memcpy(copy,r->original,r->original_size);copy[0]^=1;
 assert(!rsx_mesh_pilot_match(&s,4,copy,r->nv,r->stride,ix,r->ni,attrs,r->na));free(copy);
 attrs[0]^=1;assert(!rsx_mesh_pilot_match(&s,4,r->original,r->nv,r->stride,ix,r->ni,attrs,r->na));
 rsx_mesh_pilot_free(&s);assert(!s.count);puts("exact-match and rejection tests passed");return 0;
}
