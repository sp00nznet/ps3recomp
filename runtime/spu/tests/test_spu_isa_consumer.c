/* Synthetic C/C++ SDK consumer: architectural oracle uses word shifts only,
 * independently of SPU_W, SIMD intrinsics, and the helper's byte loop. */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "../spu_helpers.h"
#include <stdio.h>
#include <stdlib.h>

#if defined(_MSC_VER)
#define CONSUMER_NOINLINE __declspec(noinline)
#else
#define CONSUMER_NOINLINE __attribute__((noinline))
#endif
#ifdef __cplusplus
extern "C"
#endif
CONSUMER_NOINLINE u128 spu_consumer_shuffle(u128 a,u128 b,u128 c) { return spu_shufb(a,b,c); }
static unsigned cases;
static u32 random_state=0x72BC409Du;
static u128 cleared(void) { u128 value;memset(&value,0,sizeof(value));return value; }
static u32 next_word(void) { random_state^=random_state<<13;random_state^=random_state>>17;random_state^=random_state<<5;return random_state; }
static unsigned byte_at(u128 value,unsigned byte) { return (value._u32[byte/4]>>(24-8*(byte%4)))&255u; }
static void put_byte(u128* value,unsigned byte,unsigned bits) {
    const unsigned shift=24-8*(byte%4);
    value->_u32[byte/4]=(value->_u32[byte/4]&~(255u<<shift))|((bits&255u)<<shift);
}
static u128 uniform(unsigned byte) { u128 value=cleared();for(unsigned i=0;i<16;i++)put_byte(&value,i,byte);return value; }
static void check(u128 a,u128 b,u128 control) {
    u128 expected=cleared();
    for(unsigned i=0;i<16;i++) {
        const unsigned selector=byte_at(control,i);
        const unsigned value=selector>=224?128:selector>=192?255:selector>=128?0:
            (selector&16)?byte_at(b,selector&15):byte_at(a,selector&15);
        put_byte(&expected,i,value);
    }
    u128 got=spu_consumer_shuffle(a,b,control);
    u128 aa=a,bb=b,cc=control;
    aa=spu_shufb(aa,b,control);bb=spu_shufb(a,bb,control);cc=spu_shufb(a,b,cc);
    if(memcmp(&expected,&got,16)||memcmp(&expected,&aa,16)||memcmp(&expected,&bb,16)||memcmp(&expected,&cc,16)) {
        fprintf(stderr,"SHUFB consumer semantic/alias failure case=%u\n",cases);exit(2);
    }
    ++cases;
}
int main(void) {
    int sse41=0,ssse3=0;
#if defined(__SSE4_1__)
    sse41=1;
#endif
#if defined(__SSSE3__)
    ssse3=1;
#endif
    u128 a=cleared(),b=cleared(),c=cleared();
    for(unsigned i=0;i<16;i++){put_byte(&a,i,i+1);put_byte(&b,i,i+65);put_byte(&c,i,i);}
    for(unsigned selector=0;selector<256;selector++) {
        check(a,b,uniform(selector));
        for(unsigned lane=0;lane<16;lane++){u128 mixed=c;put_byte(&mixed,lane,selector);check(a,b,mixed);}
    }
    for(unsigned bit=0;bit<128;bit++)for(unsigned selector=0;selector<256;selector++) {
        u128 value=cleared(),zero=cleared();value._u32[bit/32]=1u<<(bit%32);
        check(value,zero,uniform(selector));check(zero,value,uniform(selector));
    }
    for(unsigned iteration=0;iteration<16384;iteration++) {
        for(unsigned word=0;word<4;word++){a._u32[word]=next_word();b._u32[word]=next_word();c._u32[word]=next_word();}
        check(a,b,c);check(a,a,c);check(a,b,a);check(a,b,b);
    }
    printf("SPU consumer semantics PASS cases=%u SSE4.1=%d SSSE3=%d\n",cases,sse41,ssse3);
#if defined(SPU_CONSUMER_REQUIRE_SSE41)
    if(!sse41||!ssse3) { fputs("SDK SSE4.1 probe succeeded but linked consumer lost its ISA usage requirement\n",stderr);return 41; }
#endif
    return 0;
}
