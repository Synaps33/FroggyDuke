
#define LIBCO_C
#include <libco.h>
#include <stdlib.h>
#include <setjmp.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
	jmp_buf context;
	void (*coentry)(void);
	char* stack;
} cothread_context_t;

static cothread_context_t co_primary = {0};
static cothread_context_t *co_creating, *co_running = NULL;

static void cothread_proc()
{
	// cothread will be created in a suspended state,
	// so save the context and return immediately.
	if (setjmp(co_creating->context) == 1)
	{// execution will continue here after a call to co_switch
		co_running->coentry();
		// cothread should never exit!
		abort();
	}
}

cothread_t co_active(void)
{
	if (!co_running)
		co_running = &co_primary;

	return (cothread_t)co_running;
}

cothread_t co_create(unsigned int stacksize, void (*coentry)(void))
{
	if (!co_running)
		co_running = &co_primary;

	cothread_context_t *ctx = (cothread_context_t *)malloc(sizeof(cothread_context_t));
	ctx->stack = malloc(stacksize);
	ctx->coentry = coentry;

	co_creating = ctx;

	// these must be static because can't access local vars when the stack changes
	static void* original_sp;
	static void* modified_sp; 
	modified_sp = ctx->stack + stacksize;

	// save the current stack pointer
	asm volatile (
		"move %0, $sp"
		: "=r" (original_sp)
	);

	// set the stack for the new cothread
	asm volatile (
		"move $sp, %0"
		:
		: "r" (modified_sp)
	);

	// call cothread entry proc to only setup its context, but return immediately here.
	// cothread's coentry function will actually begin its execution after specifically calling co_switch.
	cothread_proc();

	// restore the stack pointer to the original stack
	asm volatile (
		"move $sp, %0"
		:
		: "r" (original_sp)
	);

	return (cothread_t)ctx;
}

void co_delete(cothread_t cothread)
{
	cothread_context_t *ctx = (cothread_context_t *)cothread;
	if (ctx->stack)
		free(ctx->stack);
	free(ctx);
}

void co_switch(cothread_t cothread)
{
	if (setjmp(co_running->context) == 0)
	{
		co_running = (cothread_context_t *)cothread;
		longjmp(co_running->context, 1);
	}
}

#ifdef __cplusplus
}
#endif
