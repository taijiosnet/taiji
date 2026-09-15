/* Minimal GDBus sync harness - links against the rootfs's own i386
 * glib.  Reproduces the exact call path that wedges in xfwm4:
 * g_bus_get_sync on the session bus, then one sync method call. */
typedef unsigned long ulong_t;
typedef int bool_t;

extern void *g_bus_get_sync(int bus_type, void *cancellable, void **error);
extern void g_object_unref(void *obj);
extern void g_error_free(void *err);
extern void g_print(const char *fmt, ...);
extern void *g_dbus_connection_call_sync(void *conn, const char *name,
	const char *path, const char *iface, const char *method,
	void *params, void **reply_type, int flags, int timeout,
	void *cancellable, void **error);

static int
streq_n(void)
{
	return 0;
}

static const char hexdig[] = "0123456789abcdef";

static void
putptr(const char *tag, void *p)
{
	char b[32];
	int i;
	ulong_t v;

	for (i = 0; i < 31; i++)
		b[i] = ' ';
	b[31] = 0;
	/* copy tag */
	for (i = 0; tag[i] && i < 12; i++)
		b[i] = tag[i];
	b[i++] = '=';
	v = (ulong_t)p;
	b[i++] = '0';
	b[i++] = 'x';
	if (v == 0) {
		b[i++] = '0';
	} else {
		char t[20];
		int n = 0;
		while (v && n < 16) {
			t[n++] = hexdig[v & 0xf];
			v >>= 4;
		}
		while (n)
			b[i++] = t[--n];
	}
	b[i++] = '\n';
	__asm__ __volatile__(
		"movl $4, %%eax\n"
		"movl $1, %%ebx\n"
		"movl %0, %%ecx\n"
		"movl %1, %%edx\n"
		"int $0x80\n"
		: : "r"(b), "r"(i) : "eax", "ebx", "ecx", "edx");
	streq_n();
}


int
main(void)
{
	void *err = 0;
	void *conn;

	putptr("harness-start", (void*)0x1234);
	conn = g_bus_get_sync(2, 0, &err);
	putptr("conn", conn);
	putptr("err", err);
	if (conn == 0)
		return 2;
	g_object_unref(conn);
	putptr("done", (void*)0x77);
	return 0;
}
