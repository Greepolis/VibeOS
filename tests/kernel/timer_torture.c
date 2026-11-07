/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "vibeos/timer.h"
static uint64_t rng_state;
static uint64_t next_random(void) {
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7; rng_state ^= rng_state << 17;
    return rng_state;
}
static int fail(uint64_t seed, uint64_t round, const char *what) {
    fprintf(stderr, "FAIL seed=%" PRIu64 " round=%" PRIu64 ": %s\n", seed, round, what);
    return 1;
}
/* Independent arithmetic oracle: quotient/remainder, not multiply-first. */
static uint64_t model_units(uint64_t ticks, uint32_t hz, uint64_t scale) {
    uint64_t q=ticks/hz, r=ticks%hz, whole, frac;
    if (q > UINT64_MAX/scale) return UINT64_MAX;
    whole=q*scale; frac=(r*scale)/hz;
    return whole > UINT64_MAX-frac ? UINT64_MAX : whole+frac;
}
static int conversions(uint64_t seed, vibeos_timer_t *t) {
    const uint64_t v[]={0,1,999999,UINT32_MAX,UINT64_MAX/1000000000ull,UINT64_MAX-1,UINT64_MAX};
    for (unsigned i=0;i<sizeof(v)/sizeof(v[0]);++i)
        if (vibeos_timer_ticks_to_ns(t,v[i])!=model_units(v[i],t->tick_hz,1000000000ull) ||
            vibeos_timer_ticks_to_ms(t,v[i])!=model_units(v[i],t->tick_hz,1000ull))
            return fail(seed,i,"conversion overflow");
    return 0;
}
static int boundaries(uint64_t seed) {
    vibeos_timer_t t; const uint32_t a=0x31,b=0x47;
    if (vibeos_timer_init(&t,3)) return fail(seed,0,"init");
    if (vibeos_timer_arm_deadline(&t,42) || vibeos_timer_deadline_expired(&t,41)!=0 ||
        vibeos_timer_deadline_expired(&t,42)!=1) return fail(seed,0,"deadline equality");
    if (conversions(seed,&t)) return 1;
    if (vibeos_timer_bind_backend(&t,VIBEOS_TIMER_BACKEND_IRQ,a,3)) return fail(seed,0,"bind");
    if (vibeos_timer_on_irq(&t,b)!=0 || vibeos_timer_on_irq(&t,b)!=0 ||
        vibeos_timer_on_irq(&t,b)!=0 || t.ticks!=0) return fail(seed,0,"wrong IRQ vector");
    if (vibeos_timer_on_irq(&t,a)!=0) return fail(seed,0,"first matching IRQ");
    if (vibeos_timer_bind_backend(&t,VIBEOS_TIMER_BACKEND_IRQ,b,2)) return fail(seed,0,"rebind");
    if (vibeos_timer_on_irq(&t,b)!=0 || vibeos_timer_on_irq(&t,b)!=1 || t.ticks!=1)
        return fail(seed,0,"divider phase reset");
    t.ticks=UINT64_MAX; vibeos_timer_tick(&t);
    if(t.ticks!=0) return fail(seed,0,"tick wrap");
    return 0;
}
int main(int argc,char **argv) {
    uint64_t seed=argc>1?strtoull(argv[1],NULL,0):1, rounds=argc>2?strtoull(argv[2],NULL,0):10000;
    vibeos_timer_t t; uint64_t ticks=0, deadline=0, matches=0, round;
    uint32_t hz=(uint32_t)(2u+(seed%999999u)), backend=0, vector=0, divider=1;
    if(!seed) seed=1; rng_state=seed; printf("seed=%" PRIu64 " rounds=%" PRIu64 "\n",seed,rounds);
    if(boundaries(seed)) return 1;
    if(vibeos_timer_init(&t,hz)) return fail(seed,0,"init");
    for(round=0;round<rounds;++round) {
        uint64_t r=next_random();
        switch(r%7u) {
        case 0: vibeos_timer_tick(&t); ++ticks; break;
        case 1: { uint32_t d=(uint32_t)(r>>16); if(!d)d=1;
            if(vibeos_timer_arm_deadline(&t,d)) return fail(seed,round,"arm"); deadline=d; break; }
        case 2: { uint32_t v=(uint32_t)((r>>24)|1u), d=(uint32_t)(((r>>40)%31u)+1u);
            if(vibeos_timer_bind_backend(&t,VIBEOS_TIMER_BACKEND_IRQ,v,d)) return fail(seed,round,"bind");
            backend=1; vector=v; divider=d; matches=0; break; }
        case 3: if(vibeos_timer_bind_backend(&t,VIBEOS_TIMER_BACKEND_HOST,0,0)) return fail(seed,round,"host");
            backend=0; vector=0; divider=1; matches=0; break;
        case 4: { uint32_t irq=backend?((r&3u)?vector:(vector^0x80000000u)):(uint32_t)(r>>32);
            int expected=0,got;
            if(!backend) { if(vibeos_timer_on_irq(&t,irq)!=-1) return fail(seed,round,"host irq"); break; }
            if(irq==vector && (++matches%divider)==0) { ++ticks; expected=1; }
            got=vibeos_timer_on_irq(&t,irq); if(got!=expected) return fail(seed,round,"IRQ model"); break; }
        case 5: if(conversions(seed,&t)) return 1; break;
        default: { uint64_t now=r>>1; int e=deadline?(now>=deadline): -1;
            if(vibeos_timer_deadline_expired(&t,now)!=e) return fail(seed,round,"deadline"); break; }
        }
        if(t.ticks!=ticks) return fail(seed,round,"tick count");
    }
    puts("timer-torture=ok"); return 0;
}