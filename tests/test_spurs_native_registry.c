/* Synthetic host snapshots only. Compile with real registry C17/pthread POSIX.
 * These tests never invoke a registered entry or qualify guest/native staging. */
#include "spurs_native_registry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define T(x) do{if(!(x)){fprintf(stderr,"NATIVE_REGISTRY_FAIL %d %s\n",__LINE__,#x);exit(1);}}while(0)
static uint8_t image[32],desc[64];static unsigned executed;
static void entry(spu_context*c){(void)c;++executed;}
static void other_entry(spu_context*c){(void)c;++executed;}
static const char sha_a[]="0123456701234567012345670123456701234567012345670123456701234567";
static const char sha_b[]="abcdef89abcdef89abcdef89abcdef89abcdef89abcdef89abcdef89abcdef89";
static uint64_t fnv(const uint8_t*p,unsigned n){uint64_t h=UINT64_C(1469598103934665603);for(unsigned i=0;i<n;i++){h^=p[i];h*=UINT64_C(1099511628211);}return h;}
static spurs_native_definition definition(void)
{
 for(unsigned i=0;i<32;i++)image[i]=(uint8_t)(i*3+1);memset(desc,0,sizeof desc);desc[9]=2;desc[28]=1;desc[63]=0xAB;
 spurs_native_definition d={0};d.image=image;d.descriptor_template=desc;d.image_size=32;d.descriptor_size=64;d.entry=entry;d.entry_pc=0x1000;
 spurs_native_layout l={0x1000,0x2000,0x3000,128,0x4000,0x1000,0x3FFF0};d.layout=l;d.source_sha256=sha_a;d.build_sha256=sha_b;return d;
}
static spurs_job_snapshot snapshot(void)
{
 spurs_job_snapshot s={0};s.job_ea=0x30000;s.descriptor_size=64;s.raw_binary_ea=s.binary_ea=0x20000;s.image_size=32;s.fingerprint=fnv(image,32);s.descriptor=desc;s.image=image;s.guest_limit=UINT64_C(0x100000000);
 memset(desc,0,8);desc[5]=2;return s;
}
static const spurs_native_profile *add(spurs_native_definition*d)
{const spurs_native_profile*p=NULL;T(spurs_native_registry_register(d,&p)==SPURS_NATIVE_OK&&p);return p;}
static void open_sealed(void)
{
 spurs_native_definition d=definition();spurs_job_snapshot s=snapshot();const spurs_native_profile*p=NULL,*sentinel=(const spurs_native_profile*)(uintptr_t)1;
 T(spurs_native_registry_resolve(&s,&sentinel)==SPURS_NATIVE_NOT_SEALED&&sentinel==(const spurs_native_profile*)(uintptr_t)1);
 p=add(&d);T(spurs_native_registry_seal()==SPURS_NATIVE_OK&&spurs_native_registry_seal()==SPURS_NATIVE_SEALED);
 T(spurs_native_registry_register(&d,&sentinel)==SPURS_NATIVE_SEALED&&sentinel==(const spurs_native_profile*)(uintptr_t)1);
 T(spurs_native_registry_resolve(&s,&sentinel)==SPURS_NATIVE_OK&&sentinel==p&&p->entry==entry&&p->entry_pc==0x1000);
}
static void exact(void)
{
 spurs_native_definition d=definition();const spurs_native_profile*p=add(&d);T(spurs_native_registry_seal()==SPURS_NATIVE_OK);spurs_job_snapshot s=snapshot();const spurs_native_profile*out=p;
 image[17]^=0x80;T(spurs_native_registry_resolve(&s,&out)==SPURS_NATIVE_INVALID&&out==p); /* spoofed original FNV is not identity */
 s.fingerprint=fnv(image,32);T(spurs_native_registry_resolve(&s,&out)==SPURS_NATIVE_NOT_FOUND&&out==p);
 image[17]^=0x80;s.fingerprint=fnv(image,32);T(spurs_native_registry_resolve(&s,&out)==SPURS_NATIVE_OK&&out==p);
 s.binary_ea=s.raw_binary_ea=0x40000;desc[5]=4;T(spurs_native_registry_resolve(&s,&out)==SPURS_NATIVE_OK); /* dynamic first8 */
}
static void descriptor(void)
{
 spurs_native_definition d=definition();const spurs_native_profile*p=add(&d);T(spurs_native_registry_seal()==SPURS_NATIVE_OK);spurs_job_snapshot s=snapshot();const spurs_native_profile*out=p;
 desc[63]^=1;T(spurs_native_registry_resolve(&s,&out)==SPURS_NATIVE_NOT_FOUND&&out==p);desc[63]^=1;
 desc[9]=3;T(spurs_native_registry_resolve(&s,&out)==SPURS_NATIVE_INVALID);desc[9]=2;
 desc[0]=1;T(spurs_native_registry_resolve(&s,&out)==SPURS_NATIVE_INVALID);desc[0]=0;
 s.raw_binary_ea+=UINT64_C(0x100000000);T(spurs_native_registry_resolve(&s,&out)==SPURS_NATIVE_INVALID);s.raw_binary_ea=s.binary_ea;
 s.binary_flags=1;T(spurs_native_registry_resolve(&s,&out)==SPURS_NATIVE_INVALID);s.binary_flags=0;
 s.guest_limit=0x20010;T(spurs_native_registry_resolve(&s,&out)==SPURS_NATIVE_INVALID);s.guest_limit=UINT64_C(0x100000000);
 s.job_ea=0x30001;T(spurs_native_registry_resolve(&s,&out)==SPURS_NATIVE_INVALID);
}
static void immutable_copy(void)
{
 spurs_native_definition d=definition();uint8_t expected_image[32],expected_desc[64];memcpy(expected_image,image,32);memcpy(expected_desc,desc,64);
 char source[65],build[65];memcpy(source,sha_a,65);memcpy(build,sha_b,65);d.source_sha256=source;d.build_sha256=build;
 const spurs_native_profile*p=add(&d);memset(image,0xEE,32);memset(desc,0xDD,64);memset(source,'f',64);memset(build,'0',64);
 T(!memcmp(p->image,expected_image,32)&&!memcmp(p->descriptor_template+8,expected_desc+8,56));
 for(unsigned i=0;i<8;i++)T(p->descriptor_template[i]==0);T(!strcmp(p->source_sha256,sha_a)&&!strcmp(p->build_sha256,sha_b));
 memcpy(image,expected_image,32);memcpy(desc,expected_desc,64);spurs_job_snapshot s=snapshot();T(spurs_native_registry_seal()==SPURS_NATIVE_OK);const spurs_native_profile*out=NULL;T(spurs_native_registry_resolve(&s,&out)==SPURS_NATIVE_OK&&out==p);
}
static void invalid_layout(void)
{
 spurs_native_definition good=definition(),d;const spurs_native_profile*out=(const spurs_native_profile*)(uintptr_t)1;
 for(unsigned i=0;i<9;i++){d=good;switch(i){case 0:d.layout.descriptor=d.layout.image;break;case 1:d.layout.context_size=48;break;case 2:d.layout.stack_size=32;break;case 3:d.layout.stack=0x3FFF0;break;case 4:d.entry_pc=0x2000;break;case 5:d.entry_pc++;break;case 6:d.layout.return_pc=0x1000;break;case 7:d.layout.image=0x3FFF0;d.entry_pc=0x3FFF0;break;case 8:d.layout.stack++;break;}
  T(spurs_native_registry_register(&d,&out)==SPURS_NATIVE_INVALID&&out==(const spurs_native_profile*)(uintptr_t)1);}
 d=good;d.entry=NULL;T(spurs_native_registry_register(&d,&out)==SPURS_NATIVE_INVALID);d=good;d.source_sha256="short";T(spurs_native_registry_register(&d,&out)==SPURS_NATIVE_INVALID);
 d=good;desc[9]=1;T(spurs_native_registry_register(&d,&out)==SPURS_NATIVE_INVALID);desc[9]=2;desc[28]=2;T(spurs_native_registry_register(&good,&out)==SPURS_NATIVE_INVALID);desc[28]=1;T(add(&good));
}
static void duplicate(void)
{
 spurs_native_definition d=definition();const spurs_native_profile*p=add(&d),*out=NULL;
 T(spurs_native_registry_register(&d,&out)==SPURS_NATIVE_DUPLICATE&&out==p);d.entry=other_entry;out=p;
 T(spurs_native_registry_register(&d,&out)==SPURS_NATIVE_CONFLICT&&out==p);d.entry=entry;d.layout.stack+=0x1000;
 T(spurs_native_registry_register(&d,&out)==SPURS_NATIVE_CONFLICT&&out==p);d.layout.stack-=0x1000;d.build_sha256=sha_a;
 T(spurs_native_registry_register(&d,&out)==SPURS_NATIVE_CONFLICT&&out==p);
}
static void capacity(void)
{
 spurs_native_definition d=definition();const spurs_native_profile*first=NULL,*out=NULL;
 for(unsigned i=0;i<16;i++){desc[63]=(uint8_t)i;const spurs_native_profile*p=add(&d);if(!i)first=p;}
 desc[63]=16;out=first;T(spurs_native_registry_register(&d,&out)==SPURS_NATIVE_FULL&&out==first);
 desc[63]=0;T(spurs_native_registry_register(&d,&out)==SPURS_NATIVE_DUPLICATE&&out==first);
 T(spurs_native_registry_seal()==SPURS_NATIVE_OK);spurs_job_snapshot s=snapshot();T(spurs_native_registry_resolve(&s,&out)==SPURS_NATIVE_OK&&out==first);
}
static void input_contract(void)
{
 spurs_native_definition d=definition();d.input.kind=SPURS_NATIVE_INPUT_EXACT_GET;d.input.lsa=0x4100;d.input.ea=0x20000;d.input.size=4;d.input.tag=2;
 d.input.expected[0]=1;d.input.expected[1]=2;d.input.expected[2]=3;d.input.expected[3]=4;
 const spurs_native_profile*p=add(&d),*out=p;T(p->input.expected[3]==4&&p->input.ea==0x20000);
 d.input.expected[3]=5;T(spurs_native_registry_register(&d,&out)==SPURS_NATIVE_CONFLICT&&out==p&&p->input.expected[3]==4);d.input.expected[3]=4;
 spurs_native_input good=d.input;
 for(unsigned i=0;i<7;i++){d.input=good;switch(i){case 0:d.input.size=3;break;case 1:d.input.tag=32;break;case 2:d.input.lsa=0x5000;break;case 3:d.input.ea=UINT64_C(0x100020000);break;case 4:d.input.ea=0xFFF0;break;case 5:d.input.expected[4]=1;break;case 6:d.input.lsa++;break;}
  T(spurs_native_registry_register(&d,&out)==SPURS_NATIVE_INVALID&&out==p);}
 memset(&d.input,0,sizeof d.input);T(spurs_native_registry_register(&d,&out)==SPURS_NATIVE_CONFLICT&&out==p);
 T(spurs_native_registry_seal()==SPURS_NATIVE_OK);spurs_job_snapshot snap=snapshot();T(spurs_native_registry_resolve(&snap,&out)==SPURS_NATIVE_OK&&out==p&&out->input.kind==SPURS_NATIVE_INPUT_EXACT_GET);
}
int main(int argc,char**argv)
{
 if(argc!=2)return 2;
 if(!strcmp(argv[1],"open-sealed"))open_sealed();else if(!strcmp(argv[1],"exact"))exact();else if(!strcmp(argv[1],"descriptor"))descriptor();
 else if(!strcmp(argv[1],"immutable-copy"))immutable_copy();else if(!strcmp(argv[1],"invalid-layout"))invalid_layout();else if(!strcmp(argv[1],"duplicate"))duplicate();else if(!strcmp(argv[1],"capacity"))capacity();else if(!strcmp(argv[1],"input-contract"))input_contract();else return 2;
 T(executed==0);puts("NATIVE_REGISTRY_PASS");return 0;
}
