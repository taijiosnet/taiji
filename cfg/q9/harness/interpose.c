/* interpose the libc I/O surface: definitions in the executable win
 * the global symbol lookup, so libgio's write/writev/poll/read calls
 * come here - a userspace-level trace of glib's decisions, forwarded
 * via raw int80 (no libc recursion) */
typedef unsigned long usize;

static void
putnum(char *out, int *n, usize v)
{
	char t[20];
	int k = 0;
	const char *hex = "0123456789abcdef";

	if (v == 0)
		t[k++] = '0';
	while (v && k < 16) {
		t[k++] = hex[v & 0xf];
		v >>= 4;
	}
	out[(*n)++] = '0';
	out[(*n)++] = 'x';
	while (k)
		out[(*n)++] = t[--k];
}

void
tr3(const char *tag, long a, long b, long c)
{
	char buf[96];
	int n = 0;

	while (tag[n] && n < 12) { buf[n] = tag[n]; n++; }
	buf[n++] = ' ';
	putnum(buf, &n, (usize)a);
	buf[n++] = '/';
	putnum(buf, &n, (usize)b);
	buf[n++] = '/';
	putnum(buf, &n, (usize)c);
	buf[n++] = '\n';
	__asm__ __volatile__(
		"movl $4, %%eax\n movl $1, %%ebx\n movl %0, %%ecx\n movl %1, %%edx\n int $0x80\n"
		: : "r"(buf), "r"((long)n) : "eax", "ebx", "ecx", "edx");
}

static long
raw3(long nr, long a, long b, long c)
{
	long ret;

	__asm__ __volatile__(
		"int $0x80\n"
		: "=a"(ret)
		: "a"(nr), "b"(a), "c"(b), "d"(c)
		: "memory");
	return ret;
}

long write(int fd, const void *buf, usize n);
long
write(int fd, const void *buf, usize n)
{
	long r;

	tr3("TRwr", fd, (long)n, 0);
	r = raw3(4, fd, (long)buf, (long)n);
	tr3("TRwr=", r, 0, 0);
	return r;
}

struct iov { void *base; usize len; };
long writev(int fd, const struct iov *v, int cnt);
long
writev(int fd, const struct iov *v, int cnt)
{
	long total = 0;
	int i;

	tr3("TRwrv", fd, cnt, cnt > 0 ? (long)v[0].len : 0);
	for (i = 0; i < cnt; i++) {
		long r = raw3(4, fd, (long)v[i].base, (long)v[i].len);
		if (r < 0)
			return r;
		total += r;
	}
	tr3("TRwrv=", total, 0, 0);
	return total;
}

long poll(void *ufds, unsigned long nfds, int timeout);
long
poll(void *ufds, unsigned long nfds, int timeout)
{
	tr3("TRpoll", nfds, timeout, ((int*)ufds)[0]);
	return raw3(168, (long)ufds, (long)nfds, timeout);
}

long read(int fd, void *buf, usize n);
long
read(int fd, void *buf, usize n)
{
	long r;

	tr3("TRrd", fd, (long)n, 0);
	r = raw3(3, fd, (long)buf, (long)n);
	tr3("TRrd=", r, 0, 0);
	return r;
}

long sendmsg(int fd, void *msg, int flags);
long
sendmsg(int fd, void *msg, int flags)
{
	/* msg: msghdr with iov at +8, iovlen at +12 */
	struct iov *v = *(struct iov **)((char *)msg + 8);
	int cnt = *(int *)((char *)msg + 12);

	tr3("TRsendmsg", fd, cnt, cnt > 0 ? (long)v[0].len : 0);
	{
		long total = 0;
		int i;

		for (i = 0; i < cnt; i++) {
			long r = raw3(4, fd, (long)v[i].base, (long)v[i].len);
			if (r < 0)
				return r;
			total += r;
		}
		tr3("TRsmsg=", total, 0, 0);
		return total;
	}
}

long recvmsg(int fd, void *msg, int flags);
long
recvmsg(int fd, void *msg, int flags)
{
	struct iov *v = *(struct iov **)((char *)msg + 8);
	int cnt = *(int *)((char *)msg + 12);
	long r;

	tr3("TRrecvmsg", fd, cnt, flags);
	if (cnt < 1)
		return raw3(102 * 256 + 17, 0, 0, 0);
	r = raw3(3, fd, (long)v[0].base, (long)v[0].len);
	tr3("TRrmsg=", r, 0, 0);
	return r;
}

long send(int fd, const void *buf, usize n, int flags);
long
send(int fd, const void *buf, usize n, int flags)
{
	long r;

	tr3("TRsend", fd, (long)n, flags);
	r = raw3(4, fd, (long)buf, (long)n);
	tr3("TRsend=", r, 0, 0);
	return r;
}
