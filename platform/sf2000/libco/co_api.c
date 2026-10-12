/*
 * One coroutine API over the two libco flavours this port needs.
 *
 * The console build uses libco_sf2000.c -- a setjmp/longjmp implementation that
 * switches by writing $sp with inline MIPS asm, because the console's newlib
 * has no usable makecontext. The host test model cannot run that asm, so it
 * links the multicore's stock libco.c instead (sjlj.c on x86).
 *
 * Both expose the same three primitives -- co_create/co_switch/co_delete --
 * which is not the API this port wants to program against. eduke32's shim wants
 * "run one frame" and "hand control back", so this file adds co_resume() and
 * co_yield() on top and hides the difference. Nothing above this line has to
 * know which coroutine implementation it got.
 */
#include "co_api.h"

#include <libco.h>

static cothread_t s_primary;

void sf2k_co_init(void)
{
    /* Captured before the game coroutine exists, so it *is* the frontend's
     * own context -- that is what co_yield() switches back to. */
    s_primary = co_active();
}

cothread_t sf2k_co_create(unsigned int stacksize, void (*entry)(void))
{
    return co_create(stacksize, entry);
}

void sf2k_co_resume(cothread_t t)
{
    co_switch(t);
}

void sf2k_co_yield(void)
{
    if (s_primary)
        co_switch(s_primary);
}

void sf2k_co_destroy(cothread_t t)
{
    if (t)
        co_delete(t);
}