/* interpose glib's context machinery: the exe's definitions win over
 * libglib for libgio's calls; forward to the real ones via dlsym */
typedef unsigned long usize;

extern void *dlsym(void *handle, const char *name);
#define RTLD_NEXT ((void *)-1l)

extern void tr3(const char *tag, long a, long b, long c);

/* putnum/tr3 copies from interpose.c are static there; re-declare a
 * tiny local printer via the one in interpose.c (non-static there) */

int g_main_context_prepare(void *context, int *priority);
int
g_main_context_prepare(void *context, int *priority)
{
	static int (*real)(void *, int *);
	int r;

	if (!real)
		real = (int (*)(void *, int *))dlsym(RTLD_NEXT, "g_main_context_prepare");
	tr3("TRprep", (long)context, 0, 0);
	r = real ? real(context, priority) : 0;
	tr3("TRprep=", r, priority ? *priority : 0, 0);
	return r;
}

int g_main_context_check(void *context, int maxp, void *fds, int nfds);
int
g_main_context_check(void *context, int maxp, void *fds, int nfds)
{
	static int (*real)(void *, int, void *, int);
	int r;

	if (!real)
		real = (int (*)(void *, int, void *, int))dlsym(RTLD_NEXT, "g_main_context_check");
	r = real ? real(context, maxp, fds, nfds) : 0;
	tr3("TRchk", (long)context, nfds, r);
	return r;
}

int g_main_context_dispatch(void *context);
int
g_main_context_dispatch(void *context)
{
	static int (*real)(void *);
	int r;

	if (!real)
		real = (int (*)(void *))dlsym(RTLD_NEXT, "g_main_context_dispatch");
	tr3("TRdisp", (long)context, 0, 0);
	r = real ? real(context) : 0;
	return r;
}

void *g_main_context_invoke_full(void *context, int prio, void *func, void *data, void *notify);
void *
g_main_context_invoke_full(void *context, int prio, void *func, void *data, void *notify)
{
	static void *(*real)(void *, int, void *, void *, void *);

	if (!real)
		real = (void *(*)(void *, int, void *, void *, void *))dlsym(RTLD_NEXT, "g_main_context_invoke_full");
	tr3("TRinvk", (long)context, (long)func, prio);
	return real ? real(context, prio, func, data, notify) : 0;
}

void *g_source_attach(void *source, void *context);
void *
g_source_attach(void *source, void *context)
{
	static void *(*real)(void *, void *);

	if (!real)
		real = (void *(*)(void *, void *))dlsym(RTLD_NEXT, "g_source_attach");
	tr3("TRatch", (long)source, (long)context, 0);
	return real ? real(source, context) : 0;
}

int g_socket_condition_check(void *socket, int condition);
int
g_socket_condition_check(void *socket, int condition)
{
	static int (*real)(void *, int);
	int r;

	if (!real)
		real = (int (*)(void *, int))dlsym(RTLD_NEXT, "g_socket_condition_check");
	r = real ? real(socket, condition) : 0;
	tr3("TRsockchk", (long)condition, r, 0);
	return r;
}

void *g_main_context_invoke(void *context, void *func, void *data);
void *
g_main_context_invoke(void *context, void *func, void *data)
{
	static void *(*real)(void *, void *, void *);

	if (!real)
		real = (void *(*)(void *, void *, void *))dlsym(RTLD_NEXT, "g_main_context_invoke");
	tr3("TRinv0", (long)context, (long)func, 0);
	return real ? real(context, func, data) : 0;
}

int g_main_context_acquire(void *context);
int
g_main_context_acquire(void *context)
{
	static int (*real)(void *);
	int r;

	if (!real)
		real = (int (*)(void *))dlsym(RTLD_NEXT, "g_main_context_acquire");
	r = real ? real(context) : 0;
	tr3("TRacq", (long)context, r, 0);
	return r;
}

int g_main_context_is_owner(void *context);
int
g_main_context_is_owner(void *context)
{
	static int (*real)(void *);
	int r;

	if (!real)
		real = (int (*)(void *))dlsym(RTLD_NEXT, "g_main_context_is_owner");
	r = real ? real(context) : 0;
	tr3("TRowner", (long)context, r, 0);
	return r;
}

void *g_task_attach_source(void *task, void *source, void *func);
void *
g_task_attach_source(void *task, void *source, void *func)
{
	tr3("TRtasksrc", (long)task, (long)source, 0);
	return task; /* unreachable: g_task_attach_source is private */
}

void g_main_context_push_thread_default(void *context);
void
g_main_context_push_thread_default(void *context)
{
	static void (*real)(void *);

	if (!real)
		real = (void (*)(void *))dlsym(RTLD_NEXT, "g_main_context_push_thread_default");
	tr3("TRpush", (long)context, 0, 0);
	if (real)
		real(context);
}

void g_main_context_pop_thread_default(void *context);
void
g_main_context_pop_thread_default(void *context)
{
	static void (*real)(void *);

	if (!real)
		real = (void (*)(void *))dlsym(RTLD_NEXT, "g_main_context_pop_thread_default");
	tr3("TRpop", (long)context, 0, 0);
	if (real)
		real(context);
}

void *g_main_loop_new(void *context, int is_running);
void *
g_main_loop_new(void *context, int is_running)
{
	static void *(*real)(void *, int);

	if (!real)
		real = (void *(*)(void *, int))dlsym(RTLD_NEXT, "g_main_loop_new");
	tr3("TRloopnew", (long)context, (long)is_running, 0);
	return real ? real(context, is_running) : 0;
}
