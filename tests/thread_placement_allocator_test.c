#define _GNU_SOURCE
#include "thread_placement_allocator.h"
#include <assert.h>
#include <stdio.h>
static int pick(thread_placement_allocator_t *a, pid_t p, unsigned long long ps, pid_t t, unsigned long long ts, const char *id, int n, int reserve) { cpu_set_t s; int c = -1; CPU_ZERO(&s); for (int i=0;i<n;i++) CPU_SET(i*4,&s); assert(thread_placement_allocator_select(a,p,ps,t,ts,0,&s,id,reserve,&c)); return c; }
int main(void) { thread_placement_allocator_t *a=thread_placement_allocator_create(); assert(a); for(int i=0;i<100;i++) assert(pick(a,1,10,11,12,"p",3,0)==0); assert(pick(a,1,10,11,12,"a",3,1)==0); thread_placement_allocator_finish(a,"a",1); assert(pick(a,1,10,12,13,"b",3,1)==4); thread_placement_allocator_finish(a,"b",1); assert(pick(a,1,10,13,14,"c",3,1)==8); thread_placement_allocator_finish(a,"c",0); assert(pick(a,2,20,21,22,"d",2,1)==0); thread_placement_allocator_finish(a,"d",0); assert(pick(a,2,20,22,23,"e",2,1)==0); assert(pick(a,2,21,21,22,"f",2,1)==0); assert(pick(a,2,20,22,24,"g",2,1)==4); assert(pick(a,3,30,31,32,"h",1,1)==0); assert(pick(a,3,30,32,33,"i",1,1)==0); thread_placement_allocator_destroy(a); puts("TPA01-TPA10 passed"); }
