/*
 * One coroutine API over libco's two flavours -- see co_api.c.
 */
#ifndef SF2K_CO_API_H
#define SF2K_CO_API_H

#include <libco.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Records the frontend's context. Must be called before sf2k_co_create(). */
void        sf2k_co_init(void);
cothread_t  sf2k_co_create(unsigned int stacksize, void (*entry)(void));

/* Runs the coroutine until it yields back (one frame). */
void        sf2k_co_resume(cothread_t t);

/* Called from inside the coroutine: hands control back to the frontend. */
void        sf2k_co_yield(void);

void        sf2k_co_destroy(cothread_t t);

#ifdef __cplusplus
}
#endif

#endif /* SF2K_CO_API_H */