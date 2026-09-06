#include <u.h>
#include <libc.h>
#include </386/include/ureg.h>
typedef struct Ureg Ureg;

/*
 * linuxrun - execute static Linux i386 binaries on the Plan 9 kernel.
 *
 * The ELF is mapped with segattach("memory") at its program-header
 * addresses (the 386 kernel has no NX, so the pages execute), Linux
 * syscall sites (int $0x80) are rewritten to ud2, and the program is
 * entered by bouncing off a ud2 trap whose note handler installs the
 * initial registers.  Every later ud2 fault is a syscall: the handler
 * emulates it from the Ureg registers and resumes.  Raw-syscall static
 * binaries (no libc, no TLS) are the target; dynamic ELF programs need
 * the interpreter, TLS and futex work that is not built yet.
 */

enum {
	Elfident = 16,
	EiClass = 4,
	EiData = 5,
	Elfclass32 = 1,
	Elfdata2lsb = 1,
	Em386 = 3,
	EtExec = 2,
	EtDyn = 3,
	PtLoad = 1,
	PtInterp = 3,

	Piebase = 0x08000000,	/* where PIE mains land */
	Interpbase = 0x68000000,/* where ld-linux lands: clear of the
				 * map segment and the guest stack */

	LcVmnul = 0x100,	/* CLONE_VM */
	Enoexec = 8,

	Maxph = 64,
	Rungap = 64*1024,	/* merge PT_LOADs closer than this */

	Pgsz = 4096,

	/* fixed arenas: brk is carved from the low end of the map
	 * segment to stay inside the kernel's small per-process
	 * segment-slot budget */
	Brkbase = 0x40000000,
	Brksize = 8*1024*1024,
	Mapbase = 0x40000000,
	/* big enough for a GTK program's arenas and thread stacks */
	/* big enough for a GTK program's arenas, fontconfig and
	 * the pango worker-thread stacks */
	Mapsize = 512*1024*1024,
	Scratchva = 0x70000000,
	Scratchsize = 96*1024*1024,
	Stackbase = 0x60000000,
	Stacksize = 128*1024,

	/* Linux errnos, returned negative */
	Enoent = 2,
	Ebadf = 9,
	Enomem = 12,
	Eacces = 13,
	Efault = 14,
	Einval = 22,
	Enotty = 25,
	Espipe = 29,
	Enosys = 38,

	/* Linux open flags */
	LoCreat = 0x40,
	LoExcl = 0x80,
	LoTrunc = 0x200,
	LoAppend = 0x400,

	TrapUD = 6,		/* x86 #UD */
};

typedef struct Ehdr Ehdr;
typedef struct Phdr Phdr;
struct Ehdr {
	uchar ident[Elfident];
	ushort type;
	ushort machine;
	ulong entry;
	ulong phoff;
	ushort phentsize;
	ushort phnum;
};
struct Phdr {
	ulong type;
	ulong offset;
	ulong vaddr;
	ulong filesz;
	ulong memsz;
	ulong flags;
	ulong align;
};

struct Liovec {		/* Linux struct iovec */
	void *base;
	ulong len;
};

int verbose;
int analyzeonly;
int started;
ulong entrypc;
ulong mainentry;	/* AT_ENTRY: the main program's entry */
ulong stacktop;
uchar trapinsn[2] = {0x0f, 0x0b};
/* fork/clone child resume state: the child cannot noted() after rfork,
 * so it bounces off the ud2 starter and the handler installs these */
Ureg forkregs;
int forkpending;
int forksnap;
int forkready[2];
Phdr ph[Maxph];
int nph;
ulong brkcur;
char exitstr[16];
int ldtfd = -1;
/* the guest's %fs must not stay the plan9 flat data selector (base 0):
 * fs-relative accesses would hit low linear memory - this loader's own
 * text.  Point it at the TLS entry like %gs. */
ulong tlsselector = 0x33;

/* Tell the kernel the note-stack top.  Without it, notify() builds
 * note frames below the guest sp, and the lazy PLT resolver - which
 * keeps live working data below its sp across the syscalls it makes -
 * reads back kernel/host pointers and jumps to them. */
static ulong notestackva;

static ulong
registernotestack(void)
{
	int fd;
	uchar buf[4];
	ulong top;

	if(notestackva == 0)
		return 0;
	top = notestackva + 32*1024;
	buf[0] = top & 0xFF;
	buf[1] = (top>>8) & 0xFF;
	buf[2] = (top>>16) & 0xFF;
	buf[3] = (top>>24) & 0xFF;
	fd = open("/dev/notestack", OWRITE);
	if(fd < 0){
		if(bind("#z", "/dev", MAFTER) >= 0)
			fd = open("/dev/notestack", OWRITE);
	}
	if(fd < 0)
		return 0;
	if(write(fd, buf, 4) < 4)
		return 0;
	close(fd);
	return top;
}

/* Attach the private note stack (once per process tree: fork children
 * inherit the segment COW and only re-register it - segment slots are
 * scarce and the child's slots are already full of guest memory). */
static ulong
attachnotestack(void)
{
	ulong va;
	extern void* segattach(int, char*, void*, ulong);

	/* "memory", never "shared": a shared note-stack segment would
	 * let a forked child (which resumes at our exact sp inside the
	 * rfork) push frames over our live handler frames.  Private
	 * pages give each child a COW copy instead. */
	va = (ulong)segattach(0, "memory", nil, 32*1024);
	if(va == (ulong)-1){
		fprint(2, "linuxrun: notestack segattach: %r\n");
		return 0;
	}
	notestackva = va;
	return registernotestack();
}
int tlsfsokay;
int initedtls;

ulong guestsegs[16][2];
int nguestsegs;

/* AF_UNIX sockets bridged over plan9 pipes: the listener publishes
 * "spid fd" at the socket path; connect opens /proc/spid/fd and hands
 * over its own two data pipes; accept reads the request. */
int listenerfd = -1;	/* unused: accept polls a queue file now */
int lisfds[8];		/* every bound listener fd (epoll gating) */
int nlis;
int inotifywd;		/* watch-descriptor counter for the inotify stub */
void *forkscratch;	/* pre-attached RAM scratch for fork snapshots */
int cloexecfd[1024/32];	/* guest FD_CLOEXEC marks */
ulong sysring[32][4];	/* last syscalls: crash forensics */
int sysri;

/* Fake ptys: a master and a slave file descriptor joined by two
 * pipes, one per direction, wired through the socket table's packed
 * (read<<16|write) pairs.  xterm gets a real bidirectional terminal
 * out of it; there is no job control, but none is emulated anyway. */
#define NPTY 8
static struct {
	int inuse;
	int m2s[2];	/* master writes, slave reads */
	int s2m[2];	/* slave writes, master reads */
	int masterg;	/* guest fd of the master side */
} ptytab[NPTY];

enum { NRMAX = 512 };
char boundpath[256];	/* socket path of the active listener */
char connpath[256];	/* path this process connected to as a client */
int listenfd = -1;	/* guest fd of the listener socket */
int nepollsets;

char *genv[] = {
	"LD_BIND_NOW=1",
	"PATH=/bin:/usr/bin:/sbin:/usr/sbin",
	"HOME=/root",
	"DISPLAY=:0",
	nil,};

/* epoll: a table of registered fds per epoll fd; wait reports every
 * registered fd ready (the single-threaded server then blocks in the
 * read that matters). */
enum { Maxep = 256 };
struct Epev {
	int epfd;
	int fd;
	ulong events;
	ulong data;
};
struct Epev eptab[Maxep];
int epinit;
ulong phdrva;
ulong randva;
ulong sysinfova;
int phdrmode;	/* 0: phdr array, 1: ehdr, 2: omit */
ulong execfnva;
ulong platformva;
int nphhdrs;
char interppath[256];
ulong interpbase;
int dynamic;

static ulong loadelf(int, Ehdr*, ulong);
static void initsysinfo(void);
static long syssetthreadarea(ulong);
static long sysexecve(char*, char**);
static int countargs(char**);
static long dosocketcall(ulong, ulong);
static long sysgetdents64(int, ulong, ulong);
static long dofutex(ulong, ulong, ulong, ulong);

static void
fatal(char *fmt, ...)
{
	char buf[256];
	va_list arg;

	va_start(arg, fmt);
	vseprint(buf, buf+sizeof buf, fmt, arg);
	va_end(arg);
	fprint(2, "linuxrun: %s\n", buf);
	exits("linuxrun");
}

static ushort
le16(uchar *p)
{
	return p[0] | p[1]<<8;
}

static ulong
le32(uchar *p)
{
	return p[0] | (ulong)p[1]<<8 | (ulong)p[2]<<16 | (ulong)p[3]<<24;
}

static void
dumpsegments(void)
{
	char path[64], buf[4096];
	int fd, n;

	snprint(path, sizeof path, "/proc/%d/segment", getpid());
	fd = open(path, OREAD);
	if(fd < 0)
		return;
	n = readn(fd, buf, sizeof buf-1);
	close(fd);
	if(n <= 0)
		return;
	buf[n] = 0;
	fprint(2, "linuxrun: segments:\n%s", buf);
}

static void *
segat(ulong va, ulong len)
{
	void *p;

	if(nguestsegs < 16){
		guestsegs[nguestsegs][0] = va;
		guestsegs[nguestsegs][1] = len;
		nguestsegs++;
	}

	/* "shared": guest memory must survive rfork whole (clone threads,
	 * fork+exec children) rather than copy-on-write */
	p = segattach(0, "shared", (void*)va, len);
	if(p == (void*)-1)
		p = segattach(0, "memory", (void*)va, len);
	if(p == (void*)-1){
		fprint(2, "linuxrun: segattach %#lux %#lux: %r\n", va, len);
		dumpsegments();
		fatal("cannot attach guest memory");
	}
	return p;
}

static int
readat(int fd, void *buf, long n, vlong off)
{
	if(seek(fd, off, 0) < 0)
		return -1;
	return readn(fd, buf, n);
}

/*
 * Map the PT_LOAD segments at their addresses, rewriting int $0x80
 * (cd 80) into ud2 (0f 0b) in the executable bytes.  Fresh segment
 * pages are zero, so bss needs no extra work.  A byte pair inside
 * some other instruction would be rewritten too; that only matters
 * for binaries that never execute an int 80, which is not a thing we
 * care to run.
 */
static ulong
loadelf(int fd, Ehdr *eh, ulong base)
{
	uchar buf[Maxph*sizeof(Phdr)];
	uchar *seg;
	ulong va, lo, hi, off;
	int i, j, k, patched;

	if(readat(fd, buf, eh->phnum*eh->phentsize, eh->phoff) < 0)
		fatal("read program headers: %r");
	nph = 0;
	for(i = 0; i < eh->phnum; i++){
		ph[nph].type = le32(buf+i*eh->phentsize+0);
		ph[nph].offset = le32(buf+i*eh->phentsize+4);
		ph[nph].vaddr = le32(buf+i*eh->phentsize+8);
		ph[nph].filesz = le32(buf+i*eh->phentsize+16);
		ph[nph].memsz = le32(buf+i*eh->phentsize+20);
		ph[nph].flags = le32(buf+i*eh->phentsize+24);
		ph[nph].align = le32(buf+i*eh->phentsize+28);
		ph[nph].vaddr += base;
		if(ph[nph].type == PtInterp){
			uchar ipath[256];
			int n;

			n = ph[nph].filesz;
			if(n > 255)
				n = 255;
			if(readat(fd, ipath, n, ph[nph].offset) < 0)
				fatal("read interp: %r");
			ipath[n] = 0;
			memmove(interppath, ipath, n+1);
			dynamic = 1;
		}
		nph++;
	}

	patched = 0;
	i = 0;
	while(i < nph){
		/* collect a run of PT_LOADs that fit in one segment
		 * (the kernel allows only a handful of attachable
		 * segments per process) */
		if(ph[i].type != PtLoad || ph[i].memsz == 0){
			i++;
			continue;
		}
		lo = ph[i].vaddr & ~(Pgsz-1);
		hi = ph[i].vaddr + ph[i].memsz;
		for(j = i+1; j < nph; j++){
			if(ph[j].type != PtLoad || ph[j].memsz == 0)
				break;
			if((ph[j].vaddr & ~(Pgsz-1)) - hi > Rungap)
				break;
			hi = ph[j].vaddr + ph[j].memsz;
		}
		seg = segat(lo, ((hi - lo + Pgsz-1) / Pgsz) * Pgsz);
		while(i < j){
			off = ph[i].vaddr - lo;
			if(readat(fd, seg+off, ph[i].filesz, ph[i].offset) < 0)
				fatal("read segment at %#lux: %r", ph[i].offset);
			if(ph[i].flags & 1){	/* PF_X: rewrite to ud2 so
						 * the trap carries a clean
						 * register frame; the kernel
						 * gate covers the int $0x80
						 * sites we cannot rewrite
						 * (ld.so bootstrap, mmap'd
						 * libraries) */
				for(k = 0; k+1 < (int)ph[i].filesz; k++){
					uchar *q = seg + off + k;

					if(q[0] == 0xcd && q[1] == 0x80){
						q[0] = 0x0f;
						q[1] = 0x0b;
						patched++;
					}
				}
			}
			if(verbose)
				fprint(2, "linuxrun: load vaddr %#lux filesz %#lux memsz %#lux%s\n",
					ph[i].vaddr, ph[i].filesz, ph[i].memsz,
					(ph[i].flags & 1) ? " x" : "");
			i++;
		}
	}
	if(patched == 0 && !dynamic)
		fprint(2, "linuxrun: warning: no int 80 sites patched\n");
	else if(verbose)
		fprint(2, "linuxrun: patched %d syscall sites\n", patched);
	return eh->entry + base;
}


/*
 * Build the Linux initial stack: the argv0 string near the top, then
 * argc/argv/envp/auxv below it, stack pointer 16-byte aligned.
 */
static ulong
buildstack(int nargs, char **args)
{
	uchar *st;
	ulong sp, strp;
	ulong *vec;
	ulong argv[16];
	ulong enva[16];
	int i, ne, naux, nvec;

	if(nargs > 15)
		nargs = 15;
	st = segat(Stackbase, Stacksize);
	strp = Stackbase + Stacksize - 512;
	for(i = nargs-1; i >= 0; i--){
		int l;

		l = strlen(args[i]) + 1;
		strp -= l;
		strcpy((char*)st + (strp - Stackbase), args[i]);
		argv[i] = strp;
	}
	for(ne = 0; genv[ne] != nil; ne++){
		int l;

		l = strlen(genv[ne]) + 1;
		strp -= l;
		strcpy((char*)st + (strp - Stackbase), genv[ne]);
		enva[ne] = strp;
	}
	enva[ne] = 0;

	execfnva = argv[0];
	platformva = strp - 8;
	strcpy((char*)st + (platformva - Stackbase), "i686");
	/* the vsyscall thunk lives in the stack segment: it survives
	 * every exec (buildstack rewrites it) and nothing unmaps it */
	sysinfova = Stackbase + Stacksize - 2048;
	st[sysinfova - Stackbase] = 0x0f;
	st[sysinfova - Stackbase + 1] = 0x0b;
	st[sysinfova - Stackbase + 2] = 0xc3;
	randva = platformva - 32;
	for(i = 0; i < 16; i++)
		st[randva - Stackbase + i] = nsec() >> (i*3);

	/* PHDR PAGESZ PHNUM BASE ENTRY CLKTCK PHENT FLAGS UID EUID
	 * GID EGID HWCAP SECURE SYSINFO RANDOM EXECFN PLATFORM NULL */
	naux = 19;
	nvec = 1 + nargs + 1 + 1 + ne + 2*naux;
	sp = (randva - 16) & ~15UL;
	sp = (sp - nvec*4) & ~15UL;
	vec = (ulong*)(st + (sp - Stackbase));
	vec[0] = nargs;
	for(i = 0; i < nargs; i++)
		vec[1+i] = argv[i];
	vec[1+nargs] = 0;
	for(i = 0; i < ne; i++)
		vec[2+nargs+i] = enva[i];
	vec[2+nargs+ne] = 0;
	i = 3+nargs+ne;
	if(phdrmode != 2){
		vec[i++] = 3; vec[i++] = phdrva;
		vec[i++] = 5; vec[i++] = nphhdrs;
	}
	vec[i++] = 6; vec[i++] = Pgsz;
	vec[i++] = 7; vec[i++] = interpbase;
	vec[i++] = 9; vec[i++] = mainentry;
	vec[i++] = 17; vec[i++] = 100;
	vec[i++] = 4; vec[i++] = 32;
	vec[i++] = 8; vec[i++] = 0;
	vec[i++] = 11; vec[i++] = 0;
	vec[i++] = 12; vec[i++] = 0;
	vec[i++] = 13; vec[i++] = 0;
	vec[i++] = 14; vec[i++] = 0;
	vec[i++] = 16; vec[i++] = 0;
	vec[i++] = 23; vec[i++] = 0;
	vec[i++] = 32; vec[i++] = sysinfova;
	vec[i++] = 25; vec[i++] = randva;
	vec[i++] = 31; vec[i++] = execfnva;
	vec[i++] = 15; vec[i++] = platformva;
	vec[i++] = 0; vec[i++] = 0;
	return sp;
}

/* Linux struct utsname: six 65-byte fields */
static long
sysuname(ulong addr)
{
	char *p;
	static char *fields[6] = {
		"Linux", "taiji", "5.15.0-taiji", "#1 SMP Tue Sep 1 12:00:00 UTC 2026",
		"i686", "(none)"
	};
	int i;

	if(addr == 0)
		return -Efault;
	p = (char*)addr;
	memset(p, 0, 6*65);
	for(i = 0; i < 6; i++)
		strncpy(p+i*65, fields[i], 64);
	return 0;
}

/* substring search over a length-delimited, not NUL-safe, buffer */
static char*
memfind(char *b, int n, char *sub)
{
	int i, sl;

	sl = strlen(sub);
	for(i = 0; i + sl <= n; i++)
		if(memcmp(b+i, sub, sl) == 0)
			return b+i;
	return nil;
}

static int sockreadfd(int);
static int sockwritefd(int);
static long syswritev(ulong, ulong, ulong);
static long sockread(int, void*, ulong);
static void sockopc(int, void*, long, int);
static void dumpopc(void);
static int sockslot(int);
static int socknewslot(ulong);
static int sockinready(int);
extern int sockmap[16][4];

/* sendmsg: a msghdr carries msg_iov at offset 8, msg_iovlen at 12 */
static long
syssendmsg(ulong fd, ulong mh)
{
	ulong *m;

	if(mh < 0x10000)
		return -Efault;
	m = (ulong*)mh;
	if(m[2] < 0x10000)
		return -Efault;
	return syswritev(fd, m[2], m[3]);
}

/* recvmsg: one read into the first iovec; spreading the read over
 * several vectors would block on a partial buffer and deadlock X
 * clients that expect short reads to return */
static long
sysrecvmsg(ulong fd, ulong mh)
{
	ulong *m;
	struct Liovec *v;

	if(mh < 0x10000)
		return -Efault;
	m = (ulong*)mh;
	if(m[2] < 0x10000)
		return -Efault;
	v = (struct Liovec*)m[2];
	/* the kernel reports "no ancillary data and no special flags"
	 * through these fields; leaving the caller's values in place
	 * makes CMSG_FIRSTHDR walk stale stack as a cmsghdr - xcb then
	 * copies a garbage fd-count into its fd array and dies of the
	 * resulting wild pointers */
	m[5] = 0;			/* msg_controllen */
	((int*)m)[6] = 0;		/* msg_flags */
	if(v[0].len == 0)
		return 0;
	return sockread((int)fd, v[0].base, v[0].len);
}

static long
syswritev(ulong fd, ulong iov, ulong cnt)
{
	struct Liovec *v;
	long total, n;
	int slot;
	ulong i;

	v = (struct Liovec*)iov;
	slot = sockslot((int)fd);
	fd = sockwritefd((int)fd);
	if(cnt > 0 && v[0].len > 0)
		sockopc(slot, v[0].base, v[0].len, 1);
	total = 0;
	for(i = 0; i < cnt; i++)
		total += v[i].len;
	for(i = 0; i < cnt; i++){
		if(v[i].len == 0)
			continue;
		n = write((int)fd, v[i].base, v[i].len);
		if(n < 0)
			return -Ebadf;
	}
	return total;
}

static long
sysreadv(ulong fd, ulong iov, ulong cnt)
{
	struct Liovec *v;
	long total, n;
	ulong i;
	int slot;

	slot = sockslot((int)fd);
	if(slot >= 0 && sockmap[slot][3] && sockinready((int)fd) <= 0)
		return -11;		/* -EAGAIN */
	v = (struct Liovec*)iov;
	fd = sockreadfd((int)fd);
	total = 0;
	for(i = 0; i < cnt; i++){
		if(v[i].len == 0)
			continue;
		n = read((int)fd, v[i].base, v[i].len);
		if(n < 0)
			return -Ebadf;
		total += n;
		if(n < (long)v[i].len)
			break;
	}
	return total;
}

/* open the fake pty master: two pipe pairs joined through the
 * socket table give both sides bidirectional streams */
static int
ptymaster(void)
{
	int i;

	for(i = 0; i < NPTY; i++){
		if(!ptytab[i].inuse){
			if(pipe(ptytab[i].m2s) < 0 || pipe(ptytab[i].s2m) < 0)
				return -1;
			ptytab[i].inuse = 1;
			ptytab[i].masterg = socknewslot((ptytab[i].s2m[0]<<16) | ptytab[i].m2s[1]);
			if(ptytab[i].masterg < 0){
				ptytab[i].inuse = 0;
				return -1;
			}
			return ptytab[i].masterg;
		}
	}
	return -1;
}

static int
ptyslave(int n)
{
	if(n < 0 || n >= NPTY || !ptytab[n].inuse)
		return -1;
	return socknewslot((ptytab[n].m2s[0]<<16) | ptytab[n].s2m[1]);
}

static int
ptyindex(int masterg)
{
	int i;

	for(i = 0; i < NPTY; i++)
		if(ptytab[i].inuse && ptytab[i].masterg == masterg)
			return i;
	return -1;
}

static long
sysopen(ulong path, ulong flags, ulong mode)
{
	int pmode, fd;
	char *p;

	USED(mode);
	if(path == 0)
		return -Efault;
	p = (char*)path;
	if(strcmp(p, "/dev/ptmx") == 0){
		int m;

		m = ptymaster();
		return m < 0 ? -Enomem : m;
	}
	if(strncmp(p, "/dev/pts/", 9) == 0){
		int s;

		s = ptyslave(strtol(p+9, nil, 10));
		return s < 0 ? -Enoent : s;
	}
	if(strcmp(p, "/dev/tty") == 0 || strcmp(p, "/dev/console") == 0){
		int c;

		c = open("#c/cons", ORDWR);
		return c < 0 ? -Enoent : c;
	}
	switch(flags & 3){
	default:
		return -Einval;
	case 0: pmode = OREAD; break;
	case 1: pmode = OWRITE; break;
	case 2: pmode = ORDWR; break;
	}
	if(flags & LoTrunc)
		pmode |= OTRUNC;
	if(flags & LoCreat){
		if(access(p, AEXIST) < 0){
			fd = create(p, pmode, 0666);
			if(fd < 0)
				return -Enoent;
			return fd;
		}
		if(flags & LoExcl)
			return -Enoent;
	}
	fd = open(p, pmode);
	if(fd < 0){
		if(verbose)
			fprint(2, "linuxrun: open %s: %r\n", p);
		return -Enoent;
	}
	return fd;
}

static ulong mapbump = Mapbase + Brksize + Pgsz;

static void
zerorange(ulong va, ulong len)
{
	ulong a;

	for(a = va & ~(Pgsz-1); a < va+len; a += Pgsz)
		memset((void*)a, 0, Pgsz);
}

static long
sysmmap(ulong addr, ulong len, ulong prot, ulong flags, ulong fd, ulong off)
{
	ulong va;

	USED(prot);
	if(verbose)
		fprint(2, "linuxrun: mmap? addr=%#lux len=%#lux flags=%#lux fd=%d off=%#lux\n",
			addr, len, flags, (int)fd, off);
	if(len == 0)
		return -Einval;
	len = ((len + Pgsz-1) / Pgsz) * Pgsz;
	if(flags & 0x10){	/* MAP_FIXED: the caller chose the address */
		if(addr == 0 || addr + len > Mapbase + Mapsize || addr < Mapbase)
			return -Enomem;
		va = addr;
		zerorange(va, len);
	}else{
		if(addr != 0 && addr >= Mapbase && addr + len <= Mapbase + Mapsize)
			va = addr;	/* hint we can honour */
		else{
			if(mapbump + len > Mapbase + Mapsize)
				return -Enomem;
			va = mapbump;
			mapbump += len;
		}
	}
	if(!(flags & 0x20)){	/* not MAP_ANONYMOUS: fill from the file */
		uchar *p;
		ulong k;

		if(fd > 0x10000)
			return -Ebadf;
		if(readat((int)fd, (void*)va, len, off) < 0)
			return -Ebadf;
		/* no rewriting: the kernel gates int $0x80 from guest procs */
	}
	if(verbose)
		fprint(2, "linuxrun: mmap addr=%#lux len=%#lux flags=%#lux fd=%d off=%#lux -> %#lux\n",
			addr, len, flags, (int)fd, off, va);
	return va;
}

/* fill an i386 struct stat64 so ld.so accepts our files */
static long
fillstat64(ulong addr, int fd)
{
	uchar *b;
	Dir *d;

	if(addr == 0)
		return 0;
	d = dirfstat(fd);
	if(d == nil)
		return -Ebadf;
	b = (uchar*)addr;
	memset(b, 0, 96);
	if(d->mode & DMDIR)
		*(ulong*)(b+16) = 0x41ed;	/* S_IFDIR|0755 */
	else
		*(ulong*)(b+16) = (d->mode & 0111) ?
			0x81ed : 0x81a4;	/* S_IFREG|0755 : |0444;
					 * the x bits gate xterm's shell
					 * check and glib's program search */
	*(ulong*)(b+20) = 1;			/* nlink */
	*(ulong*)(b+12) = (ulong)d->qid.path;	/* __st_ino */
	*(vlong*)(b+88) = d->qid.path;	/* st_ino: ld.so dedups libraries
					 * by dev:ino, so every file must
					 * be unique */
	*(vlong*)(b+44) = d->length;		/* st_size */
	if(verbose)
		fprint(2, "linuxrun: fstat64 fd=%d size=%llud\n", fd, d->length);
	*(ulong*)(b+52) = 4096;			/* blksize */
	free(d);
	return 0;
}

/* Linux i386 struct user_desc */
typedef struct Userdesc Userdesc;
struct Userdesc {
	int entry_number;
	ulong base_addr;
	int limit;
	int flags;
};

#define UDSeg32 (1<<0)
#define UDLimpages (1<<4)

/*
 * set_thread_area: install the TLS descriptor in the process' foreign
 * descriptor page (entry 0 of the page = GDT slot TLSSEG, which is
 * selector 0x33 = entry_number 6 - exactly how Linux numbers i386
 * TLS).  glibc loads %gs itself once the entry exists.
 */
static long
syssetthreadarea(ulong udesc)
{
	Userdesc *ud;
	ulong d0, d1, base;
	static uchar desc[8];

	if(udesc == 0)
		return -Efault;
	ud = (Userdesc*)udesc;
	base = ud->base_addr;
	d0 = (base & 0xFFFF)<<16 | 0xFFFF;
	d1 = (base & 0xFF000000) | ((base>>16) & 0xFF);
	d1 |= 0x8000;		/* present */
	d1 |= 3<<13;		/* DPL 3 */
	d1 |= 0x12<<8;		/* data, writable */
	d1 |= 1<<23;		/* granularity 4k */
	d1 |= 1<<22;		/* 32-bit */
	desc[0] = d0 & 0xFF;
	desc[1] = (d0>>8) & 0xFF;
	desc[2] = (d0>>16) & 0xFF;
	desc[3] = (d0>>24) & 0xFF;
	desc[4] = d1 & 0xFF;
	desc[5] = (d1>>8) & 0xFF;
	desc[6] = (d1>>16) & 0xFF;
	desc[7] = (d1>>24) & 0xFF;
	if(!initedtls){
		initedtls = 1;
		ldtfd = open("/dev/ldt", OWRITE);
		if(ldtfd < 0){
			if(bind("#z", "/dev", MAFTER) >= 0)
				ldtfd = open("/dev/ldt", OWRITE);
		}
	}
	if(ldtfd < 0){
		fprint(2, "linuxrun: ldt open: %r\n");
		return -Enosys;
	}
	if(seek(ldtfd, 0, 0) < 0){
		fprint(2, "linuxrun: ldt seek: %r\n");
		return -Enosys;
	}
	if(write(ldtfd, desc, 8) != 8){
		fprint(2, "linuxrun: ldt write: %r\n");
		return -Enosys;
	}
	ud->entry_number = 6;
	tlsfsokay = 1;
	return 0;
}

/* glibc's i386 syscall stub calls through the TCB sysinfo pointer
 * (AT_SYSINFO) rather than int $0x80 for some syscalls; give it a
 * ud2;ret trampoline so those funnel into the same emulator. */
static void
initsysinfo(void)
{
	uchar *p;

	p = (uchar*)(Mapbase + Brksize);
	p[0] = 0x0f;
	p[1] = 0x0b;
	p[2] = 0xc3;
	sysinfova = (ulong)p;
}


/* ---- AF_UNIX over plan9 pipes ---- */
/* Guest socket descriptors are synthetic ids mapped here to the
 * bridge's packed pipe pair (read<<16)|write.  The guest reads and
 * writes its own descriptor number; handing it the packed value
 * directly never works because libc passes the socket() return to
 * poll() and recv() unchanged. */
#define NSOCK 16
int sockmap[NSOCK][4];	/* [i]: guest fd, active, packed, nonblocking */
/* X-protocol forensics: first byte of every write (request opcode) and
 * read (event/reply type) through a bridge socket, counted per slot */
int wopc[NSOCK][256];
int ropc[NSOCK][256];

static void
sockopc(int slot, void *buf, long n, int wr)
{
	static int npr, nseq;
	int op, seq;

	if(slot < 0 || n <= 0 || buf == nil)
		return;
	op = ((uchar*)buf)[0];
	seq = ((uchar*)buf)[2] | (((uchar*)buf)[3]<<8);
	/* sequence tracking: requests carry their seq at bytes 2-3,
	 * replies and errors too (events do not - skip those) */
	if(nseq < 150){
		if(wr){
			nseq++;
			fprint(2, "linuxrun: SEQ g%d wr op=%d seq=%d n=%ld\n",
				sockmap[slot][0], op, seq, n);
		}else if(op <= 1){
			nseq++;
			fprint(2, "linuxrun: SEQ g%d rd op=%d seq=%d n=%ld\n",
				sockmap[slot][0], op, seq, n);
		}
	}
	if(wr)
		wopc[slot][op]++;
	else
		ropc[slot][op]++;
	/* live frame-lifecycle trace: the requests that map and
	 * reparent, and the events that acknowledge them */
	if(npr < 120){
		char what[64];

		what[0] = 0;
		if(wr){
			if(op == 8) snprint(what, sizeof what, "MapWindow");
			if(op == 9) snprint(what, sizeof what, "MapSubwindows");
			if(op == 64) snprint(what, sizeof what, "ReparentWindow");
		}else{
			if(op == 12) snprint(what, sizeof what, "Expose");
			if(op == 19) snprint(what, sizeof what, "MapNotify");
			if(op == 21) snprint(what, sizeof what, "ReparentNotify");
			if(op == 22) snprint(what, sizeof what, "ConfigureNotify");
		}
		if(what[0]){
			npr++;
			fprint(2, "linuxrun: XOP g%d %s %s\n",
				sockmap[slot][0], wr ? "send" : "recv", what);
		}
	}
}

static void
dumpopc(void)
{
	int i, k, n;

	for(i = 0; i < NSOCK; i++){
		if(!sockmap[i][1])
			continue;
		n = 0;
		for(k = 0; k < 256; k++){
			if(wopc[i][k] || ropc[i][k]){
				fprint(2, "linuxrun: OPC g%d %s %d=%d/%d\n",
					sockmap[i][0], k < 128 ? "req" : "evt",
					k, wopc[i][k], ropc[i][k]);
				if(++n > 24)
					break;
			}
		}
	}
}

static int
sockslot(int fd)
{
	int i;

	for(i = 0; i < NSOCK; i++)
		if(sockmap[i][1] && sockmap[i][0] == fd)
			return i;
	return -1;
}


/* Allocate a socket-map slot whose guest-visible descriptor is a real
 * plan9 fd number (a #c/pid placeholder): synthetic high ids leak into
 * guest pointer slots and crash later writes through them. */
static int
sockmapfd(int fd, ulong packed)
{
	int i, j;

	j = -1;
	for(i = 0; i < NSOCK; i++){
		if(sockmap[i][1] && sockmap[i][0] == fd){
			sockmap[i][2] = packed;
			return i;
		}
		if(!sockmap[i][1] && j < 0)
			j = i;
	}
	if(j < 0)
		return -1;
	sockmap[j][0] = fd;
	sockmap[j][1] = 1;
	sockmap[j][2] = packed;
	sockmap[j][3] = 0;
	return j;
}

static int
socknewslot(ulong packed)
{
	int i, fd;

	for(i = 0; i < NSOCK; i++){
		if(sockmap[i][1])
			continue;
		fd = open("#c/pid", OREAD);
		if(fd < 0)
			return -1;
		/* never let the placeholder land on stdio: fd 0 would
		 * hijack every stdin read through the map */
		while(fd < 3){
			int fd2;

			fd2 = open("#c/pid", OREAD);
			if(fd2 < 0)
				break;
			close(fd);
			fd = fd2;
		}
		if(fd < 3){
			close(fd);
			return -1;
		}
		sockmap[i][0] = fd;
		sockmap[i][1] = 1;
		sockmap[i][2] = packed;
		return fd;
	}
	return -1;
}

static int
sockreadfd(int fd)
{
	int i;

	i = sockslot(fd);
	if(i >= 0)
		return sockmap[i][2] >> 16;
	return (fd >= 0x10000) ? (fd >> 16) : fd;
}

static int
sockwritefd(int fd)
{
	int i;

	i = sockslot(fd);
	if(i >= 0)
		return sockmap[i][2] & 0xffff;
	return (fd >= 0x10000) ? (fd & 0xffff) : fd;
}

/* read through a bridge socket, honoring the guest's O_NONBLOCK:
 * an empty pipe must give EAGAIN immediately or XCB's nonblocking
 * recv wedges the connection forever */
static long
sockread(int gfd, void *buf, ulong n)
{
	int i;

	i = sockslot(gfd);
	if(i >= 0 && sockmap[i][3] && sockinready(gfd) <= 0)
		return -11;		/* -EAGAIN */
	i = read(sockreadfd(gfd), buf, n);
	if(i > 0)
		sockopc(sockslot(gfd), buf, i, 0);
	return i;
}


/* Publish an fd as /srv/<name>: writing the decimal fd to a fresh
 * /srv file makes the kernel share that channel with any process
 * that opens the name (devsrv).  This is how the AF_UNIX bridge
 * hands pipe ends between linuxrun processes without /proc. */
static int
postsrvfd(char *name, int fd)
{
	char buf[16];
	int sfd;

	snprint(buf, sizeof buf, "%d", fd);
	sfd = create(name, OWRITE, 0666);
	if(sfd < 0){
			return -1;
	}
	if(write(sfd, buf, strlen(buf)) < 0){
		close(sfd);
		return -1;
	}
	close(sfd);
	return 0;
}

static long
sysbindlisten(ulong path)
{
	char req[512];
	int fd;

	if(path == 0)
		return -Efault;
	if(((uchar*)path)[0] == 0){
		/* abstract sockets have no pathname to publish, but
		 * failing the bind makes libxtrans tear the transport
		 * down and Xorg ends up accepting on a dangling
		 * connection record - report success instead; clients
		 * reach the pathname listener */
		return 0;
	}
	strncpy(boundpath, (char*)path, sizeof boundpath - 1);
	boundpath[sizeof boundpath - 1] = 0;
	/* clients queue connection requests at <path>.q; accept drains
	 * that file, so a stale one from a previous server must go */
	snprint(req, sizeof req, "%s.q", boundpath);
	remove(req);
	snprint(req, sizeof req, "%s.req", (char*)path);
	fd = create(req, OWRITE|OTRUNC, 0666);
	if(fd < 0)
		fd = create(req, OWRITE, 0666);
	if(fd < 0)
		return -Enoent;
	close(fd);	/* the .req file existing is the bind mark */
	return 0;
}

static int sscanf3(char*, int*, int*, int*);
static int sscanf2(char*, char*, int, char*, int);
static int traphandler(void*, char*);

static long
sysconnect(ulong path)
{
	char buf[512], line[128];
	int sfd, c2s[2], s2c[2];
	char target[128];

	if(path == 0)
		return -Efault;
	if(((uchar*)path)[0] == 0)	/* abstract sockets: no pathname;
					 * ECONNREFUSED makes Xlib fall
					 * back to the pathname socket */
		return -111;
	/* the .req file existing is the server's bind mark */
	snprint(buf, sizeof buf, "%s.req", (char*)path);
	if(access(buf, AEXIST) < 0)
		return -Enoent;
	if(pipe(c2s) < 0 || pipe(s2c) < 0)
		return -Enomem;
	/* server reads what we write: publish c2s[0]; it writes back on
	 * s2c[1].  We keep c2s[1] (write) and s2c[0] (read). */
	snprint(target, sizeof target, "/srv/x.c.%d.a", getpid());
	if(postsrvfd(target, c2s[0]) < 0)
		return -Enomem;
	snprint(target, sizeof target, "/srv/x.c.%d.b", getpid());
	if(postsrvfd(target, s2c[1]) < 0)
		return -Enomem;
	/* the two /srv posts ARE the queue: the (nonblocking)
	 * accept scans /srv for pending x.c.<pid>.a entries */
	strncpy(connpath, (char*)path, sizeof connpath - 1);
	connpath[sizeof connpath - 1] = 0;
	return (s2c[0]<<16) | c2s[1];
}

/* minimal "%%d %%d[ %%d]" line parser for the socket bridge */
static int
sscanf3(char *s, int *a, int *b, int *c)
{
	int n, v;

	n = 0;
	while(*s){
		while(*s == ' ' || *s == '\t' || *s == '\n')
			s++;
		if(*s == 0)
			break;
		v = 0;
		if(*s < '0' || *s > '9')
			break;
		while(*s >= '0' && *s <= '9')
			v = v*10 + *s++ - '0';
		if(n == 0)
			*a = v;
		else if(n == 1)
			*b = v;
		else
			*c = v;
		n++;
	}
	return n;
}

/* split "nameA nameB\n" */
static int
sscanf2(char *s, char *a, int na, char *b, int nb)
{
	int i;

	while(*s == ' ')
		s++;
	for(i = 0; s[i] && s[i] != ' ' && s[i] != '\n' && i < na-1; i++)
		a[i] = s[i];
	a[i] = 0;
	s += i;
	while(*s == ' ')
		s++;
	for(i = 0; s[i] && s[i] != ' ' && s[i] != '\n' && i < nb-1; i++)
		b[i] = s[i];
	b[i] = 0;
	return (a[0] && b[0]) ? 2 : 0;
}

static int servedpids[32];
static int nserved;

static long
sysaccept(void)
{
	char buf[4096], target[64];
	int fd, n, i, cpid, rf, wf;
	char *p;

	if(boundpath[0] == 0)
		return -Ebadf;
	/* scan /srv for the client's posted pipe ends: the entries are
	 * kernel-global and listing never blocks, so this accept is
	 * naturally nonblocking */
	fd = open("/srv", OREAD);
	if(fd < 0)
		return -11;	/* -EAGAIN */
	n = readn(fd, buf, sizeof buf-1);
	close(fd);
	if(n <= 0)
		return -11;
	buf[n] = 0;
	/* the marshaled directory stream carries names as plain
	 * strings: look for x.c.<pid>.a with its x.c.<pid>.b twin */
	for(p = memfind(buf, n, "x.c."); p != nil; p = memfind(p+4, n-(int)(p+4-buf), "x.c.")){
		char *q;

		cpid = strtol(p+4, &q, 10);
		if(cpid <= 0 || q[0] != '.' || q[1] != 'a')
			continue;	/* only the .a twin queues */
		for(i = 0; i < nserved; i++)
			if(servedpids[i] == cpid)
				goto next;
		snprint(target, sizeof target, "x.c.%d.b", cpid);
		if(memfind(buf, n, target) == nil)
			continue;
		snprint(target, sizeof target, "/srv/x.c.%d.a", cpid);
		rf = open(target, OREAD);
		snprint(target, sizeof target, "/srv/x.c.%d.b", cpid);
		wf = open(target, OWRITE);
		if(rf < 0 || wf < 0)
			continue;
		if(nserved < 32)
			servedpids[nserved++] = cpid;
		return (rf<<16) | wf;
	next: ;
	}
	return -11;	/* -EAGAIN */
}

/* true when a client's pipe ends sit queued in /srv and no accept
 * has served them yet: posted /srv entries outlive the connection,
 * so a raw name match would keep the listener "ready" forever */
static int
listenqueued(void)
{
	char buf[4096];
	char *p, *q;
	int fd, n, i, cpid;

	if(nlis == 0)
		return 0;
	fd = open("/srv", OREAD);
	if(fd < 0)
		return 0;
	n = readn(fd, buf, sizeof buf-1);
	close(fd);
	if(n <= 0)
		return 0;
	for(p = memfind(buf, n, "x.c."); p != nil; p = memfind(p+4, n-(int)(p+4-buf), "x.c.")){
		cpid = strtol(p+4, &q, 10);
		if(cpid <= 0 || q[0] != '.' || q[1] != 'a')
			continue;
		for(i = 0; i < nserved; i++)
			if(servedpids[i] == cpid)
				goto nextq;
		return 1;
	nextq: ;
	}
	return 0;
}

/* honest readiness for a bridge socket: a plan9 pipe's stat length
 * is the queue length (pipestat reports qlen), so POLLIN is exactly
 * "the pipe holds data"; POLLOUT means the far end is still open */
static int
sockinready(int fd)
{
	int i, rdy;
	Dir *d;

	i = sockslot(fd);
	if(i < 0 || sockmap[i][2] == 0)
		return -1;		/* not a connected socket */
	rdy = 0;
	if((d = dirfstat(sockmap[i][2] >> 16)) != nil){
		rdy = d->length;
		free(d);
	}
	return rdy;
}

static int
islistener(int fd)
{
	int i;

	for(i = 0; i < nlis; i++)
		if(lisfds[i] == fd)
			return 1;
	return 0;
}

/* getsockname/getpeername: a unix-domain address is the bound path;
 * the client knows the path it connected to, the server its own */
static long
syssockname(ulong name, ulong namelen)
{
	char *p;
	int nl;

	if(name < 0x10000 || namelen < 0x10000)
		return -Efault;
	p = connpath[0] != 0 ? connpath : boundpath;
	*(ushort*)name = 1;		/* sun_family = AF_UNIX */
	strncpy((char*)name+2, p, 104);
	((char*)name)[2+104-1] = 0;
	nl = 2 + strlen(p) + 1;
	*(int*)namelen = nl;
	return 0;
}

static long
dosocketcall(ulong subop, ulong argsp)
{
	/* i386 socketcall convention: ebx is the operation, ecx points
	 * at that operation's argument array.  Passing them crossed
	 * made the handler read address 1 (SOCKOP_socket) as the array
	 * and die as a nested note. */
	ulong *a;
	long r;


	if(argsp == 0 || argsp > 0xffff0000UL)
		return -Efault;
	a = (ulong*)argsp;
	r = -Enosys;
	switch(subop){
	case 1:		/* socket(a0=domain,a1=type,a2=proto): AF_UNIX only */
		if(a[0] != 1){
			r = -Eacces;
			break;
		}
		r = socknewslot(0);
		if(r < 0)
			r = -Enomem;
		break;
	case 2:		/* bind(fd,a1=addr,a2=len): sockaddr+2 = sun_path */
		listenfd = a[0];
		if(nlis < 8)
			lisfds[nlis++] = a[0];
		r = sysbindlisten(a[1] + 2);
		break;
	case 3:		/* connect(fd,a1=addr,a2=len): sockaddr+2 = sun_path */
		r = sysconnect(a[1] + 2);
		if(r >= 0){
			int i;

			i = sockslot(a[0]);
			if(i >= 0)
				sockmap[i][2] = r;
		}
		break;
	case 6:		/* getsockname(fd,name,namelen) */
	case 7:		/* getpeername(fd,name,namelen) */
		r = syssockname(a[1], a[2]);
		break;
	case 4:		/* listen */
	case 13:	/* shutdown */
	case 14:	/* setsockopt: options are ignored */
		r = 0;
		break;
	case 15:	/* getsockopt: report success with a zeroed optval -
			 * Xlib polls POLLOUT then reads SO_ERROR to learn
			 * whether a nonblocking connect finished */
		if(a[3] > 0x10000)
			*(int*)a[3] = 0;
		if(a[4] > 0x10000)
			*(int*)a[4] = 4;
		r = 0;
		break;
	case 5:		/* accept */
		r = sysaccept();
		if(r >= 0x10000){
			int ns;

			ns = socknewslot(r);
			if(ns < 0)
				r = -Enomem;
			else
				r = ns;
		}
		break;
	case 8:		/* socketpair: two pipes is close enough; never
		 * store through a pointer that is actually one of our
		 * synthetic descriptors come back in a stale arg slot */
		{
			int p[2];

			if(a[3] >= 0x51000000UL && a[3] < 0x51010000UL){
				r = 0;
				break;
			}
			if(pipe(p) < 0)
				r = -Enomem;
			else{
				((ulong*)a[3])[0] = p[0];
				((ulong*)a[3])[1] = p[1];
				r = 0;
			}
		}
		break;
	case 9:		/* send(fd,a1=buf,a2=len) */
	case 11:	/* sendto(fd,a1,a2,a3,a4) */
		r = write(sockwritefd((int)a[0]), (void*)a[1], a[2]);
		if(r > 0)
			sockopc(sockslot((int)a[0]), (void*)a[1], r, 1);
		if(r < 0)
			r = -Ebadf;
		break;
	case 10:	/* recv(fd,a1=buf,a2=len) */
	case 12:	/* recvfrom(fd,a1,a2,a3,a4) */
		r = sockread((int)a[0], (void*)a[1], a[2]);
		if(r < 0 && r != -11)	/* keep EAGAIN */
			r = -Ebadf;
		break;
	case 16:	/* sendmsg: first iovec only; the arg is a msghdr,
			 * whose msg_iov (offset 8) points at the vectors */
		r = syssendmsg(a[0], a[1]);
		break;
	case 17:	/* recvmsg: msg_iov at msghdr offset 8 */
		r = sysrecvmsg(a[0], a[1]);
		break;
	}
	return r;
}

/* Linux dirent64: ino(8) off(8) reclen(2) type(1) pad(5) name */
static long
sysgetdents64(int fd, ulong buf, ulong len)
{
	Dir *d;
	int n, i;
	long total;
	uchar *out;

	n = dirread(fd, &d);
	if(n < 0)
		return -Ebadf;
	if(n == 0){
		free(d);
		return 0;
	}
	out = (uchar*)buf;
	total = 0;
	for(i = 0; i < n; i++){
		int dl, rl;

		dl = strlen(d[i].name)+1;
		rl = (19+dl+7) & ~8+1-1;
		rl = (19+dl+7) & ~7;
		if(total+rl > (long)len)
			break;
		memset(out, 0, rl);
		*(vlong*)(out) = d[i].qid.path;
		*(vlong*)(out+8) = total+rl;
		*(ushort*)(out+16) = rl;
		out[18] = (d[i].mode&DMDIR) ? 4 : 8;
		memmove(out+19, d[i].name, dl);
		out += rl;
		total += rl;
	}
	free(d);
	return total;
}

static long
dofutex(ulong addr, ulong op, ulong val, ulong utime)
{
	switch(op & 127){
	case 0:		/* WAIT: poll; the waiters re-check shared memory */
		{
			int i;

			for(i = 0; i < 1000; i++){
				if(*(int*)addr != (int)val)
					return 0;
				if(utime != 0)
					return -110;	/* -ETIMEDOUT */
				sleep(1);
			}
			/* Give up with a spurious wakeup (legal futex
			 * behavior): a forked child waits on a private
			 * copy of the word that nobody else maps, so an
			 * honest wait would hang it forever.  The caller
			 * re-checks and re-waits if it disagrees. */
			return 0;
		}
	case 1:		/* WAKE */
	case 3:		/* REQUEUE */
	case 4:		/* CMP_REQUEUE */
	case 5:		/* WAKE_OP */
		return 0;
	}
	return -Enosys;
}

static int
countargs(char **a)
{
	int n;

	for(n = 0; a != nil && a[n] != nil; n++)
		;
	return n;
}

/* execve in place: drop the guest image, load the new one; the trap
 * handler installs the registers. */
static long
sysexecve(char *path, char **gargv)
{
	int fd, i, j;
	Ehdr eh;
	uchar hdr[64];

	if(path == nil || path[0] == 0)
		return -Efault;
	fd = open(path, OREAD);
	if(fd < 0)
		return -Enoent;
	if(readat(fd, hdr, sizeof hdr, 0) < 0){
		close(fd);
		return -Enoent;
	}
	memset(&eh, 0, sizeof eh);
	memmove(eh.ident, hdr, Elfident);
	if(eh.ident[EiClass] != Elfclass32 || eh.ident[EiData] != Elfdata2lsb){
		close(fd);
		return -Enoexec;
	}
	eh.type = le16(hdr+16);
	eh.machine = le16(hdr+18);
	eh.entry = le32(hdr+24);
	eh.phoff = le32(hdr+28);
	eh.phentsize = le16(hdr+42);
	eh.phnum = le16(hdr+44);
	if(eh.machine != Em386 || (eh.type != EtExec && eh.type != EtDyn)){
		close(fd);
		return -Enoexec;
	}
	for(i = 0; i < nguestsegs; i++)
		segdetach((void*)guestsegs[i][0]);
	nguestsegs = 0;
	/* Linux programs mark their X sockets CLOEXEC and expect exec
	 * to drop them (xfwm4's dbus/at-spi children otherwise inherit
	 * the connection and race reads with the parent); only the fds
	 * the guest actually marked close, though - a blanket close
	 * broke captures */
	{
		int q;

		for(q = 0; q < NSOCK; q++){
			if(!sockmap[q][1])
				continue;
			if(cloexecfd[sockmap[q][0] >> 5] & (1 << (sockmap[q][0] & 31))){
				close(sockmap[q][2] >> 16);
				close(sockmap[q][2] & 0xffff);
				close(sockmap[q][0]);
				sockmap[q][1] = 0;
				sockmap[q][2] = 0;
				sockmap[q][3] = 0;
			}
		}
	}
	nph = 0;
	nphhdrs = eh.phnum;
	dynamic = 0;
	interppath[0] = 0;
	interpbase = 0;
	mapbump = Mapbase + Brksize + Pgsz;
	entrypc = loadelf(fd, &eh, eh.type == EtDyn ? Piebase : 0);
	mainentry = entrypc;
	phdrva = eh.phoff;
	if(eh.type == EtDyn)
		phdrva += Piebase;
	close(fd);
	if(dynamic){
		int ifd;
		Ehdr ieh;
		uchar ihdr[64];

		if(interppath[0] == 0)
			return -Enoexec;
		ifd = open(interppath, OREAD);
		if(ifd < 0)
			return -Enoent;
		if(readat(ifd, ihdr, sizeof ihdr, 0) < 0){
			close(ifd);
			return -Enoent;
		}
		memset(&ieh, 0, sizeof ieh);
		memmove(ieh.ident, ihdr, Elfident);
		ieh.type = le16(ihdr+16);
		ieh.machine = le16(ihdr+18);
		ieh.entry = le32(ihdr+24);
		ieh.phoff = le32(ihdr+28);
		ieh.phentsize = le16(ihdr+42);
		ieh.phnum = le16(ihdr+44);
		interpbase = Interpbase;
		loadelf(ifd, &ieh, interpbase);
		close(ifd);
		entrypc = interpbase + ieh.entry;
	}
	/* the exec wiped the arena segment with the rest of the old
	 * image; the new one starts brk/mmap from scratch and needs
	 * it back */
	segat(Mapbase, Mapsize);
	stacktop = buildstack(countargs(gargv), gargv);
	return 0;
}

static long
dosyscall(Ureg *ur)
{
	ulong nr, a1, a2, a3, a4, a5;
	long r;

	nr = ur->ax;
	a1 = ur->bx;
	a2 = ur->cx;
	a3 = ur->dx;
	a4 = ur->si;
	a5 = ur->di;
	r = -Enosys;
	{
		static int flowc;
		int si, sk, a, b;

		if(++flowc >= 2000){
			flowc = 0;
			for(si = 0; si < NSOCK; si++){
				if(!sockmap[si][1])
					continue;
				a = b = 0;
				for(sk = 0; sk < 256; sk++){
					a += wopc[si][sk];
					b += ropc[si][sk];
				}
				if(a || b)
					fprint(2, "linuxrun: FLOW g%d wr=%d rd=%d\n",
						sockmap[si][0], a, b);
			}
		}
	}
	sysring[sysri%32][0] = nr;
	sysring[sysri%32][1] = a1;
	sysring[sysri%32][2] = a2;
	sysring[sysri%32][3] = a3;
	sysri++;
	switch(nr){
	case 1:		/* exit */
	case 252:	/* exit_group */
		dumpopc();
		fprint(2, "linuxrun: exit status=%lux\n", a1 & 0xff);
		snprint(exitstr, sizeof exitstr, "%lux", a1 & 0xff);
		exits(exitstr);
		return 0;
	case 3:		/* read */
		r = sockread((int)a1, (void*)a2, a3);
		if(r < 0 && r != -11)	/* keep EAGAIN */
			r = -Ebadf;
		break;
	case 4:		/* write */
		r = write(sockwritefd((int)a1), (void*)a2, a3);
		if(r > 0)
			sockopc(sockslot((int)a1), (void*)a2, r, 1);
		if(r < 0)
			r = -Ebadf;
		break;
	case 5:		/* open */
		r = sysopen(a1, a2, ur->si);
		break;
	case 295:	/* openat: only the AT_FDCWD form */
		if(a1 != 0xffffff9cUL)
			r = -Ebadf;
		else
			r = sysopen(a2, a3, ur->si);
		break;
	case 300:	/* fstatat64: AT_EMPTY_PATH on an fd, or AT_FDCWD */
		if(a2 != 0 && ((char*)a2)[0] == 0 || a4 & 0x1000)
			r = fillstat64(a3, (int)a1);
		else if(a1 != 0xffffff9cUL)
			r = -Ebadf;
		else{
			int sfd;

			sfd = open((char*)a2, OREAD);
			if(sfd < 0)
				r = -Enoent;
			else{
				r = fillstat64(a3, sfd);
				close(sfd);
			}
		}
		break;
	case 6:		/* close */
		{
			int i;

			i = sockslot((int)a1);
			if(i >= 0){
				close(sockmap[i][2] >> 16);
				close(sockmap[i][2] & 0xffff);
				sockmap[i][1] = 0;
			}else if(a1 >= 0x10000UL){
				close((int)(a1 >> 16));
				close((int)(a1 & 0xffff));
			}else
				close((int)a1);
		}
		r = 0;
		break;
	case 8:		/* creat */
		r = sysopen(a1, LoCreat|LoTrunc|1, 0666);
		break;
	case 10:	/* unlink */
		r = remove((char*)a1) < 0 ? -Enoent : 0;
		break;
	case 13:	/* time */
		r = time(nil);
		if(a1 != 0)
			*(ulong*)a1 = r;
		break;
	case 19:	/* lseek */
		r = seek((int)a1, a2, a3);
		break;
	case 20:	/* getpid */
	case 224:	/* gettid */
		r = getpid();
		break;
	case 24:	/* getuid */
	case 47:	/* getgid */
	case 49:	/* geteuid */
	case 50:	/* getegid */
		r = 0;
		break;
	case 33:	/* access */
		r = access((char*)a1, AEXIST) < 0 ? -Enoent : 0;
		break;
	case 45:	/* brk */
		if(a1 >= Brkbase && a1 < Brkbase+Brksize)
			brkcur = a1;
		r = brkcur;
		break;
	case 54:	/* ioctl */
		if(a2 == 0x80045430){	/* TIOCGPTN: ptsname needs it */
			int pi;

			pi = ptyindex((int)a1);
			if(pi < 0 || a3 < 0x10000)
				r = -Enotty;
			else{
				*(int*)a3 = pi;
				r = 0;
			}
		}else if(a2 == 0x540d){		/* TIOCGETD: N_TTY */
			if(a3 < 0x10000)
				r = -Efault;
			else{
				*(int*)a3 = 0;
				r = 0;
			}
		}else if(a2 == 0x5401){		/* TCGETS: a sane cooked
			 * terminal satisfies xterm's spawn */
			if(a3 < 0x10000)
				r = -Efault;
			else{
				ulong *t;

				t = (ulong*)a3;
				t[0] = 0x500 | 0x400 | 0x100;	/* ICRNL|IXON... */
				t[1] = 0x1 | 0x4;		/* OPOST|ONLCR */
				t[2] = 0xbf;			/* B38400|CS8|CREAD */
				t[3] = 0x8b3;			/* ISIG|ICANON|ECHO */
				((uchar*)a3)[16] = 0;		/* c_line */
				memset((uchar*)a3+17, 0x7f, 19);
				((uchar*)a3)[17] = 3;		/* VINTR ^C */
				r = 0;
			}
		}else if(a2 == 0x5413){		/* TIOCGWINSZ */
			if(a3 < 0x10000)
				r = -Efault;
			else{
				((ushort*)a3)[0] = 24;	/* rows */
				((ushort*)a3)[1] = 80;	/* cols */
				((ushort*)a3)[2] = 0;
				((ushort*)a3)[3] = 0;
				r = 0;
			}
		}else if(a2 == 0x5410 || a2 == 0x80047476){ /* TIOCGPGRP,
					TIOCSPTLCK: report pgrp 0, no lock */
			if(a3 > 0x10000)
				*(int*)a3 = 0;
			r = 0;
		}else if(a2 == 0x541b){		/* FIONREAD: bytes queued */
			if(a3 < 0x10000)
				r = -Efault;
			else{
				int q;

				q = sockinready((int)a1);
				*(int*)a3 = q > 0 ? q : 0;
				r = 0;
			}
		}else if(a2 == 0x5402 || a2 == 0x5403 || a2 == 0x5404 ||
		    a2 == 0x540e || a2 == 0x5410 || a2 == 0x5414 ||
		    a2 == 0x541f || a2 == 0x5421 || a2 == 0x40045431)
			r = 0;			/* termios sets: accept */
		else
			r = -Enotty;
		break;
	case 90:	/* old_mmap: ebx points at the arg struct */
		{
			ulong *m;

			m = (ulong*)a1;
			r = sysmmap(m[0], m[1], m[2], m[3], m[4], m[5]);
		}
		break;
	case 91:	/* munmap */
		r = 0;
		break;
	case 140:	/* _llseek(fd, hi, lo, result*, whence) */
		{
			vlong off;
			ulong *resp;

			off = seek((int)a1, ((vlong)(long)a2<<32) | (a3 & 0xffffffffUL), (int)a5);
			resp = (ulong*)a4;
			if(resp == nil)
				r = -Einval;
			else if(off < 0)
				r = -Espipe;	/* pipes: what Linux returns */
			else{
				resp[0] = (ulong)off;
				resp[1] = (ulong)((uvlong)off >> 32);
				r = 0;
			}
		}
		break;
	case 199:	/* getuid32 */
	case 200:	/* getgid32 */
	case 201:	/* geteuid32 */
	case 202:	/* getegid32: shells' privilege-drop checks read these */
		r = 0;
		break;
	case 183:	/* getcwd: dash prints a warning without it */
		if(a1 == 0)
			r = -Efault;
		else{
			char wdb[1024];

			if(getwd(wdb, sizeof wdb) == nil)
				r = -Enoent;
			else{
				strcpy((char*)a1, wdb);
				r = a1;
			}
		}
		break;
	case 122:	/* uname */
		r = sysuname(a1);
		break;
	case 145:	/* readv */
		r = sysreadv(a1, a2, a3);
		break;
	case 146:	/* writev */
		r = syswritev(a1, a2, a3);
		break;
	case 76:	/* getrlimit */
	case 191:	/* ugetrlimit: fill in an infinite rlimit */
		if(a2 != 0){
			*(ulong*)a2 = 0x7fffffff;
			*(ulong*)(a2+4) = 0x7fffffff;
		}
		r = 0;
		break;
	case 78:	/* gettimeofday */
		{
			vlong t;

			t = nsec();
			if(a1 != 0){
				*(ulong*)a1 = t/1000000000;
				*(ulong*)(a1+4) = (t/1000)%1000000;
			}
		}
		r = 0;
		break;
	case 125:	/* mprotect */
	case 219:	/* madvise */
	case 172:	/* prctl */
	case 174:	/* rt_sigaction */
	case 175:	/* rt_sigprocmask */
	case 238:	/* sendfile: not yet */
		r = 0;
		break;
	case 163:	/* mremap: MAYMOVE semantics - copy to a new range */
		{
			ulong newsz;

			newsz = ((a3 + Pgsz-1) / Pgsz) * Pgsz;
			if(mapbump + newsz > Mapbase + Mapsize)
				r = -Enomem;
			else{
				ulong nva;

				nva = mapbump;
				mapbump += newsz;
				memmove((void*)nva, (void*)a1, a2 < a3 ? a2 : a3);
				r = nva;
			}
		}
		break;
	case 192:	/* mmap2 */
		r = sysmmap(a1, a2, a3, ur->si, ur->di, ur->bp * Pgsz);
		break;
	case 197:	/* fstat64: real mode/size so ld.so accepts the file */
		r = fillstat64(a2, (int)a1);
		break;
	case 180:	/* pread64: fd, buf, count, poslo, poshi */
		r = seek((int)a1, ((vlong)ur->di<<32) | ur->si, 0);
		if(r < 0)
			r = -Ebadf;
		else
			r = read((int)a1, (void*)a2, a3);
		break;
	case 258:	/* set_tid_address */
		r = getpid();
		break;
	case 265:	/* clock_gettime */
		{
			vlong t;

			t = nsec();
			if(a2 != 0){
				*(ulong*)a2 = t/1000000000;
				*(ulong*)(a2+4) = t%1000000000;
			}
		}
		r = 0;
		break;
	case 355:	/* getrandom */
		{
			ulong i, x;

			x = nsec();
			for(i = 0; i < a2; i++){
				x = x*1103515245 + 12345;
				((uchar*)a1)[i] = (x>>16) & 0xFF;
			}
		}
		r = a2;
		break;
	case 243:	/* set_thread_area */
		r = syssetthreadarea(a1);
		break;
	case 329:	/* epoll_create1: a pipe stands in for the object */
		{
			int p[2];
			if(pipe(p) < 0)
				r = -Enomem;
			else{
				close(p[1]);
				r = p[0];
			}
		}
		break;
	case 254:	/* epoll_create: a synthetic descriptor; real fd
			 * numbers would collide with the guest's own */
		r = 0x45000000 + (nepollsets++ & 0xffff);
		break;
	case 255:	/* epoll_ctl_old */
	case 324:	/* kept: some builds route ctl here */
	case 266:	/* observed epoll_ctl variant (a1=epfd a2=op a3=fd) */
	case 406:	/* observed epoll_ctl variant */
		{
			int i, slot;

			if(!epinit){
				epinit = 1;
				for(i = 0; i < Maxep; i++)
					eptab[i].epfd = -1;
			}
			if(a2 == 2){	/* EPOLL_CTL_DEL: a2 is the op code */
				for(i = 0; i < Maxep; i++)
					if(eptab[i].epfd == (int)a1 && eptab[i].fd == (int)a3)
						eptab[i].epfd = -1;
				r = 0;
				break;
			}
			slot = -1;
			for(i = 0; i < Maxep; i++){
				if(eptab[i].epfd == (int)a1 && eptab[i].fd == (int)a3){
					slot = i;
					break;
				}
				if(slot < 0 && eptab[i].epfd == -1)
					slot = i;
			}
			if(slot < 0){
				r = -Enomem;
				break;
			}
			eptab[slot].epfd = (int)a1;
			eptab[slot].fd = (int)a3;
			if(a4 != 0){
				/* the event struct: events(4) + data(8) */
				eptab[slot].events = *(ulong*)a4;
				eptab[slot].data = *(ulong*)((uchar*)a4+4);
			}
			r = 0;
		}
		break;
	case 256:	/* epoll_wait_old */
		{
			ulong *ev;
			int i, maxev, n;

			ev = (ulong*)a2;
			maxev = (int)a3;
			n = 0;
			for(i = 0; i < Maxep && n < maxev; i++){
				int evv, slot;

				if(eptab[i].epfd != (int)a1)
					continue;
				/* the listener is readable only while an
				 * unserved connection is queued; reporting
				 * it otherwise makes the server spin in
				 * failing accepts */
				if(islistener((int)eptab[i].fd) && !listenqueued())
					continue;
				/* connected bridge sockets: EPOLLIN only
				 * when the pipe holds data, else the
				 * server reads before the client has sent
				 * anything and both ends block */
				evv = eptab[i].events & 0xffffffff;
				slot = sockslot((int)eptab[i].fd);
				if(slot >= 0 && sockmap[slot][2] != 0 &&
				    (evv & 1) && sockinready((int)eptab[i].fd) <= 0)
					evv &= ~1;
				/* report readiness only: registration flags
				 * like EPOLLET must not come back, Xorg
				 * reads any extra bit as a socket error */
				evv &= 5;	/* EPOLLIN|EPOLLOUT */
				if(evv == 0)
					continue;
				if(ev != nil){
					ev[n*3] = evv;
					ev[n*3+1] = (ulong)eptab[i].data;
					ev[n*3+2] = 0;
				}
				n++;
			}
			if(n == 0){
				/* yield only after a burst of empty polls:
				 * the server's scheduler pokes
				 * epoll_wait(0) in a tight loop and a
				 * yield on every poll distorts its timing */
				static int nempty;

				if(++nempty >= 64){
					nempty = 0;
					if(a4 > 50)
						sleep(50);
					else
						sleep(a4 > 0 ? a4 : 1);
				}
			}
			r = n;
		}
		break;
	case 323:	/* eventfd: a pipe pair, counter semantics ignored */
		{
			int p[2];

			if(pipe(p) < 0)
				r = -Enomem;
			else{
				close(p[1]);
				r = p[0];
			}
		}
		break;
	case 2:		/* fork */
	case 190:	/* vfork: a real copy is a valid implementation */
	case 120:	/* clone: threads share the guest "shared" segments */
		{
			int pid, mfd;
			void (*starter)(void);

			if(pipe(forkready) < 0){
				fprint(2, "linuxrun: fork pipe: %r\n");
				forkready[0] = forkready[1] = -1;
			}
			/* RFFDG copies the fd table (Linux fork semantics):
			 * RFCFDG would empty it, leaving the child without
			 * fd 2 and without the forkready pipe */
			pid = rfork(RFPROC|RFFDG|RFNOTEG);
			if(pid < 0){
				fprint(2, "linuxrun: rfork: %r\n");
				r = -Enomem;
			}
			else if(pid == 0){
				if(ldtfd >= 0){
					close(ldtfd);
					ldtfd = -1;
				}
				initedtls = 0;
				listenerfd = -1;
				if(forkready[0] >= 0)
					close(forkready[0]);
				/* the note-stack segment came along COW: just
				 * re-register it (a fresh attach would need
				 * a segment slot the child does not have) */
				registernotestack();
				/* rfork left us without note state and without
				 * the foreign mark: re-register both */
				atnotify(traphandler, 1);
				mfd = open("/dev/mark", OWRITE);
				if(mfd >= 0){
					write(mfd, "1", 1);
					close(mfd);
				}
				forksnap = !(nr == 120 && (a1 & LcVmnul));
				if(!forksnap && a2 != 0)
					ur->sp = a2;	/* thread switch stack */
				if(nr == 120 && a4 != 0)
					syssetthreadarea(a4);	/* CLONE_SETTLS:
					 * without a TLS of its own the
					 * thread's first canary access
					 * trips the stack protector */
				/* Returning through the handler would call
				 * noted() without a pending note and kill us.
				 * Bounce off our own ud2 starter instead:
				 * that note arrives on our private note
				 * stack, where the handler takes the private
				 * snapshot and installs the registers saved
				 * below.  Nothing here may use more stack
				 * than a handful of frames: we are still on
				 * the parent's shared note stack until the
				 * starter note fires. */
				ur->ax = 0;
				forkregs = *ur;
				forkregs.pc = ur->pc + 2;
				forkpending = 1;
				starter = (void(*)(void))trapinsn;
				starter();
				exits("fork child");	/* not reached */
			}else{
				/* hold the shared note stack until the child
				 * has finished on its own */
				if(forkready[0] >= 0){
					char b[1];

					close(forkready[1]);
					read(forkready[0], b, 1);
					close(forkready[0]);
				}
				r = pid;
				if(nr == 120 && (a1 & LcVmnul) && a5 != 0)
					*(ulong*)a5 = pid;
			}
		}
		break;
	case 11:	/* execve */
		{
			static char *gargv[256];
			static char argstr[8192];
			static char pathbuf[256];
			ulong *ap;
			int na, j, k;

			/* copy the strings now: sysexecve detaches the old
			 * image (including the guest stack they live on)
			 * before buildstack reads them back */
			j = 0;
			k = 0;
			ap = (ulong*)a2;
			for(na = 0; na < 255 && ap != nil && ap[na] != 0; na++){
				int m;

				m = strlen((char*)ap[na]);
				if(k + m + 1 >= sizeof argstr)
					break;
				strcpy(argstr + k, (char*)ap[na]);
				gargv[na] = argstr + k;
				k += m + 1;
			}
			gargv[na] = nil;
			if(a1 != 0){
				strncpy(pathbuf, (char*)a1, sizeof pathbuf - 1);
				pathbuf[sizeof pathbuf - 1] = 0;
			}else
				pathbuf[0] = 0;
			r = sysexecve(pathbuf, gargv);
			if(r == 0){
				/* the handler adds 2 for the syscall insn we
				 * replaced: pre-compensate so the guest
				 * enters at the true entry, not entry+2 */
				ur->pc = entrypc - 2;
				ur->sp = stacktop;
				ur->ax = 0;
				ur->bx = 0;
				ur->cx = 0;
				ur->dx = 0;
				ur->si = 0;
				ur->di = 0;
				ur->bp = 0;
				return 0;
			}
		}
		break;
	case 102:	/* socketcall */
		r = dosocketcall(a1, a2);
		break;
	case 359:	/* socket (direct): AF_UNIX only */
		if(a1 != 1){
			r = -Eacces;
			break;
		}
		r = socknewslot(0);
		if(r < 0)
			r = -Enomem;
		break;
	case 361:	/* bind (direct) */
		listenfd = a1;
		if(nlis < 8)
			lisfds[nlis++] = a1;
		r = sysbindlisten(a2 + 2);
		break;
	case 362:	/* connect (direct) */
		r = sysconnect(a2 + 2);
		if(r >= 0){
			int i;

			i = sockslot(a1);
			if(i >= 0)
				sockmap[i][2] = r;
		}
		break;
	case 363:	/* listen (direct) */
	case 366:	/* setsockopt (direct) */
	case 373:	/* shutdown (direct) */
		r = 0;
		break;
	case 367:	/* getsockname (direct) */
	case 368:	/* getpeername (direct) */
		r = syssockname(a2, a3);
		break;
	case 365:	/* getsockopt (direct): args are fd, level, optname,
			 * optval, optlen - optval/a4 is the only pointer
			 * worth writing, optlen/a5 gets its size */
		if(a4 > 0x10000)
			*(int*)a4 = 0;
		if(a5 > 0x10000)
			*(int*)a5 = 4;
		r = 0;
		break;
	case 364:	/* accept4 (direct): glibc's accept() on i386 */
		r = sysaccept();
		if(r >= 0x10000){
			int ns;

			ns = socknewslot(r);
			if(ns < 0)
				r = -Enomem;
			else
				r = ns;
		}
		break;
	case 369:	/* sendto (direct) */
		r = write(sockwritefd((int)a1), (void*)a2, a3);
		if(r > 0)
			sockopc(sockslot((int)a1), (void*)a2, r, 1);
		if(r < 0)
			r = -Ebadf;
		break;
	case 370:	/* sendmsg (direct): glibc routes xcb's sendmsg here */
		r = syssendmsg(a1, a2);
		break;
	case 371:	/* recvfrom (direct) */
		r = sockread((int)a1, (void*)a2, a3);
		if(r < 0 && r != -11)	/* keep EAGAIN */
			r = -Ebadf;
		break;
	case 372:	/* recvmsg (direct) */
		r = sysrecvmsg(a1, a2);
		break;
	case 220:	/* getdents64 */
		r = sysgetdents64((int)a1, a2, a3);
		break;
	case 168:	/* poll: bridge sockets report POLLIN only when the
		 * pipe actually holds data - a lying POLLIN makes xcb
		 * read before it has written its setup request and
		 * deadlock both ends of the connection; a blocking poll
		 * must also not return zero ready fds or xcb's read
		 * loop declares the connection dead */
		if(a1 != 0 && a2 != 0){
			/* struct pollfd: fd(4), events(2), revents(2) */
			struct Lpollfd { int fd; ushort events, revents; } *pf;
			long tleft, k;

			pf = (struct Lpollfd*)a1;
			tleft = (long)a3;	/* ms; negative blocks */
			for(;;){
				r = 0;
				for(k = 0; k < (long)a2; k++){
					int ev, re;

					ev = pf[k].events;
					re = 0;
					if(sockslot(pf[k].fd) >= 0){
						if(sockmap[sockslot(pf[k].fd)][2] != 0){
							if(ev & 0x4)		/* POLLOUT */
								re |= 0x4;
							if((ev & 0x1) && sockinready(pf[k].fd) > 0)
								re |= 0x1;	/* POLLIN */
						}else if((ev & 0x1) && listenqueued())
							re |= 0x1;	/* the listener */
					}else
						re = ev;	/* passthrough fds stay ready */
					pf[k].revents = re;
					if(re != 0)
						r++;
				}
				if(r > 0 || tleft == 0)
					break;
				if(tleft > 0 && tleft <= 20){
					sleep(tleft);
					tleft = 0;
					continue;
				}
				sleep(20);
				if(tleft > 0)
					tleft -= 20;
			}
		}else{
			if(a3 == 0)
				sleep(1);
			else
				sleep(50);
			r = 0;
		}
		break;
	case 308:	/* pselect6: same shape as select; the
		 * signal-mask argument is ignored */
	case 142:	/* select: honest readiness - a lying read-ready set
		 * makes the server read before the client has sent
		 * anything (and block), while clearing requested bits
		 * hides clients from the dispatcher entirely */
		{
			ulong rin[32], win[32];
			ulong *rd, *wr, *ex;
			int maxfd, k, nready;

			maxfd = (int)a1;
			if(maxfd > 1024)
				maxfd = 1024;
			memset(rin, 0, sizeof rin);
			memset(win, 0, sizeof win);
			if(a2 != 0)
				memmove(rin, (void*)a2, (maxfd+7)/8);
			if(a3 != 0)
				memmove(win, (void*)a3, (maxfd+7)/8);
			/* zero-timeout selects poll without sleeping;
			 * everything else breathes between iterations */
			if(a5 == 0 || (((ulong*)a5)[0] | ((ulong*)a5)[1]) != 0)
				sleep(50);
			rd = (ulong*)a2;
			wr = (ulong*)a3;
			ex = (ulong*)a4;
			if(rd != nil) memset(rd, 0, 128);
			if(wr != nil) memset(wr, 0, 128);
			if(ex != nil) memset(ex, 0, 128);
			nready = 0;
			for(k = 0; k < maxfd; k++){
				int slot;

				if((rin[k/32]>>(k%32)) & 1){
					int re;

					slot = sockslot(k);
					if(slot >= 0){
						if(sockmap[slot][2] != 0)
							re = sockinready(k) > 0;
						else
							re = listenqueued();
					}else
						re = 1;
					if(re){
						rd[k/32] |= 1UL<<(k%32);
						nready++;
					}
				}
				if((win[k/32]>>(k%32)) & 1){
					wr[k/32] |= 1UL<<(k%32);
					nready++;
				}
			}
			r = nready;
		}
		break;
	case 42:	/* pipe */
	case 331:	/* pipe2 */
		if(pipe((int*)a1) < 0)
			r = -Enomem;
		else
			r = 0;
		break;
	case 85:	/* readlink */
		r = -Enoent;
		break;
	case 195:	/* stat64 */
	case 196:	/* lstat64 */
		{
			int sfd;

			sfd = open((char*)a1, OREAD);
			if(sfd < 0)
				r = -Enoent;
			else{
				r = fillstat64(a2, sfd);
				close(sfd);
			}
		}
		break;
	case 9:		/* link: exclusive-create a copy (lock files) */
		{
			int ifd, ofd;
			char buf[4096];
			long n;

			if(access((char*)a2, AEXIST) >= 0){
				r = -17;	/* -EEXIST */
				break;
			}
			ifd = open((char*)a1, OREAD);
			if(ifd < 0){
				r = -Enoent;
				break;
			}
			ofd = create((char*)a2, OWRITE|OEXCL, 0644);
			if(ofd < 0){
				close(ifd);
				r = -Eacces;
				break;
			}
			while((n = read(ifd, buf, sizeof buf)) > 0)
				write(ofd, buf, n);
			close(ifd);
			close(ofd);
			r = 0;
		}
		break;
	case 87:	/* unlink */
		r = remove((char*)a1) < 0 ? -Enoent : 0;
		break;
	case 41:	/* dup */
		r = dup((int)a1, -1);
		if(r < 0)
			r = -Ebadf;
		else{
			int i;

			i = sockslot((int)a1);
			if(i >= 0)
				sockmapfd(r, sockmap[i][2]);
		}
		break;
	case 63:	/* dup2 */
		if(a2 == a1){
			r = a1;
			break;
		}
		close((int)a2);
		r = dup((int)a1, (int)a2);
		if(r < 0)
			r = -Ebadf;
		else{
			int i;

			i = sockslot((int)a1);
			if(i >= 0)
				sockmapfd(r, sockmap[i][2]);
		}
		break;
	case 330:	/* dup3: flags ignored */
		close((int)a2);
		r = dup((int)a1, (int)a2);
		if(r < 0)
			r = -Ebadf;
		else{
			int i;

			i = sockslot((int)a1);
			if(i >= 0)
				sockmapfd(r, sockmap[i][2]);
		}
		break;
	case 39:	/* mkdir */
		if(create((char*)a1, OREAD, DMDIR|0777) < 0)
			r = -Eacces;
		else
			r = 0;
		break;
	case 240:	/* futex */
		r = dofutex(a1, a2, a3, a4);
		break;
	case 162:	/* nanosleep */
		{
			ulong ms;

			ms = 1;
			if(a1 != 0)
				ms = ((ulong*)a1)[0]*1000 + (((ulong*)a1)[1])/1000000;
			sleep(ms ? ms : 1);
		}
		r = 0;
		break;
	case 55:	/* fcntl */
	case 221:	/* fcntl64 */
		switch(a2){
		case 0:	/* F_DUPFD */
			r = dup((int)a1, -1);
			if(r < 0)
				r = -Ebadf;
			else{
				int i;

				i = sockslot((int)a1);
				if(i >= 0)
					sockmapfd(r, sockmap[i][2]);
			}
			break;
		case 2:	/* SETFD: remember FD_CLOEXEC for exec */
			if(a1 < 1024){
				if(a3 & 1)
					cloexecfd[a1 >> 5] |= 1 << (a1 & 31);
				else
					cloexecfd[a1 >> 5] &= ~(1 << (a1 & 31));
			}
			r = 0;
			break;
		case 3:	/* GETFL */
			r = 2;
			{
				int i;

				i = sockslot((int)a1);
				if(i >= 0 && sockmap[i][3])
					r = 0x802;	/* O_RDWR|O_NONBLOCK */
			}
			break;
		case 4:	/* SETFL: remember O_NONBLOCK on bridge sockets -
			 * XCB and Xorg both read sockets expecting EAGAIN
			 * when empty, and a blocking read deadlocks the
			 * whole connection */
			{
				int i;

				i = sockslot((int)a1);
				if(i >= 0)
					sockmap[i][3] = (a3 & 0x800) != 0;
			}
			r = 0;
			break;
		default:
			r = 0;
		}
		break;
	case 66:	/* setsid */
		r = 0;
		break;
	case 64:	/* getppid (61 is ustat) */
		r = 1;
		break;
	case 37:	/* kill: cross-process notes are not wired; the
			 * senders only probe, so report success */
	case 57:	/* setpgid */
	case 27:	/* alarm */
	case 311:	/* set_robust_list */
	case 386:	/* rseq */
	case 65:	/* getgroups: root, no supplementary groups */
	case 241:	/* sched_setaffinity */
	case 208:	/* setresuid32: xterm's seteuid before spawning */
	case 210:	/* setresgid32: xterm's setegid ditto */
		r = 0;
		break;
	case 242:	/* sched_getaffinity: one cpu, mask filled in */
		if(a3 > 0 && a4 > 0x10000)
			memset((void*)a4, 0xff, a3 < 128 ? a3 : 128);
		r = a3 < 128 ? a3 : 128;
		break;
	case 116:	/* sysinfo: a struct of counters, zeroed apart
			 * from the memory totals GLib likes to see */
		if(a1 < 0x10000)
			r = -Efault;
		else{
			memset((void*)a1, 0, 64);
			((ulong*)a1)[1] = 512*1024*1024;	/* totalram */
			((ulong*)a1)[2] = 256*1024*1024;	/* freeram */
			((ulong*)a1)[13] = 512;			/* mem_unit */
			r = 0;
		}
		break;
	case 328:	/* eventfd2: a pipe stands in; the counter
			 * semantics are approximated by the stream */
	case 290:	/* mlock */
		{
			int p[2];

			if(pipe(p) < 0)
				r = -Enomem;
			else{
				close(p[1]);
				r = p[0];
			}
		}
		break;
	case 403:	/* clock_gettime64 */
	case 408:	/* clock_gettime64 alias used by some stubs */
		if(a2 < 0x10000)
			r = -Efault;
		else{
			vlong t;

			t = nsec();
			((vlong*)a2)[0] = t/1000000000;
			((vlong*)a2)[1] = t%1000000000;
			r = 0;
		}
		break;
	case 422:	/* futex_time64 */
		r = dofutex(a1, a2, a3, a4);
		break;
	case 99:	/* statfs: buf is the second argument, 84 bytes */
		if(a2 > 0x10000)
			memset((void*)a2, 0, 84);
		r = 0;
		break;
	case 268:	/* statfs64: buf is the third argument and is
		 * exactly 84 bytes on i386 - bigger smashes callers */
		if(a3 > 0x10000)
			memset((void*)a3, 0, 84);
		r = 0;
		break;
	case 209:	/* getresuid32 */
	case 211:	/* getresgid32 */
		if(a1 > 0x10000)
			*(ulong*)a1 = 0;
		if(a2 > 0x10000)
			*(ulong*)a2 = 0;
		if(a3 > 0x10000)
			*(ulong*)a3 = 0;
		r = 0;
		break;
	case 40:	/* rmdir */
		r = remove((char*)a1) < 0 ? -Eacces : 0;
		break;
	case 114:	/* wait4(-1, status*, 0, nil): reap one child */
		{
			Waitmsg *w;

			w = wait();
			if(w == nil)
				r = -10;	/* -ECHILD */
			else{
				if(a2 != 0)
					*(int*)a2 = strtol(w->msg, nil, 16) << 8;
				r = w->pid;
				free(w);
			}
		}
		break;
	case 60:	/* umask */
	case 30:	/* utime */
	case 15:	/* chmod */
	case 16:	/* lchown */
	case 212:	/* chown32 */
	case 213:	/* setuid32: Popen children _exit(127) if it fails */
	case 214:	/* setgid32 */
	case 23:	/* setuid */
	case 46:	/* setgid */
	case 94:	/* setgroups */
	case 291:	/* inotify_init: no events are ever reported, so a
			 * quiet placeholder fd satisfies GLib monitors */
	case 332:	/* inotify_init1 */
		{
			int p[2];

			if(pipe(p) < 0)
				r = -Enomem;
			else{
				close(p[1]);
				r = p[0];
			}
		}
		break;
	case 292:	/* inotify_add_watch */
		r = ++inotifywd;
		break;
	case 293:	/* inotify_rm_watch */
		r = 0;
		break;
	default:
		/* name the gap once: an unimplemented call that a
		 * program depends on shows up here before it dies */
		{
			static uchar seen[NRMAX/8+1];

			if(nr < NRMAX && !(seen[nr/8] & (1<<(nr%8)))){
				seen[nr/8] |= 1<<(nr%8);
				fprint(2, "linuxrun: unimplemented syscall %lud\n", nr);
			}
		}
		break;
	}
	if(verbose)
		fprint(2, "linuxrun: sys %lux -> %ld\n", nr, r);
	return r;
}



/*
 * The first ud2 trap starts the program (install entry registers);
 * later ud2 traps are syscalls to emulate and resume.  Anything else
 * goes to the default disposition.
 */
static int
traphandler(void *v, char *msg)
{
	Ureg *ur;
	uchar *pc;

	if(v == nil)
		return 0;
	ur = v;
	if(msg != nil && strstr(msg, "write on closed pipe") != nil){
		/* Linux programs expect write() to fail with EPIPE
		 * (every toolkit ignores SIGPIPE); letting this note
		 * take its default disposition kills whoever touches
		 * a dead client's socket - the whole desktop follows */
		fprint(2, "linuxrun: pipenote handled\n");
		return 1;
	}
	if(msg != nil && strstr(msg, "pipe") != nil){
		int q;

		fprint(2, "linuxrun: pipenote unmatched:");
		for(q = 0; q < 40 && msg[q]; q++)
			fprint(2, " %2.2ux", (uchar)msg[q]);
		fprint(2, "\n");
	}
	if(msg != nil && strcmp(msg, "linux sys") != 0 &&
	    strncmp(msg, "sys: trap: invalid opcode", 25) != 0){
		static int z;

		if(z++ < 20)
			fprint(2, "linuxrun: note: %s\n", msg);
	}
	if(msg != nil && strcmp(msg, "linux sys") == 0){
		/* the kernel gates guest int $0x80 here (devldt procs) */
		if(!started){
			started = 1;
			ur->pc = entrypc;
			ur->sp = stacktop;
			ur->ax = 0;
			ur->bx = 0;
			ur->cx = 0;
			ur->dx = 0;
			ur->si = 0;
			ur->di = 0;
			ur->bp = 0;
			return 1;
		}
		if(forkpending){
			/* a fork/clone child bouncing off the starter */
			forkpending = 0;
			*ur = forkregs;
			return 1;
		}
		ur->ax = dosyscall(ur);
		ur->pc += 2;
		return 1;
	}
	if(ur->trap != TrapUD){
		/* dump guest faults so the caller of a bad call is visible */
		if(started){
			ulong *stk;
			int i;

			fprint(2, "linuxrun: guest fault trap=%lux pc=%lux sp=%lux bx=%lux bxptr=%lux\n",
				ur->trap, ur->pc, ur->sp, ur->bx,
				ur->bx > 0x10000 && ur->bx < 0x70000000 ? *(ulong*)ur->bx : 0);
			fprint(2, "linuxrun: ax=%lux bx=%lux cx=%lux dx=%lux si=%lux di=%lux bp=%lux\n",
				ur->ax, ur->bx, ur->cx, ur->dx, ur->si, ur->di, ur->bp);
			for(i = 0; i < 32; i++){
				int ri;

				ri = (sysri+i) % 32;
				if(sysring[ri][0] != 0)
					fprint(2, "linuxrun:  sys-%d nr=%lux a1=%lux a2=%lux a3=%lux\n",
						i, sysring[ri][0], sysring[ri][1], sysring[ri][2], sysring[ri][3]);
			}
			dumpsegments();
			stk = (ulong*)ur->sp;
			for(i = 0; i < 12; i++)
				fprint(2, "linuxrun:  sp+%d = %lux\n", i*4, stk[i]);
						if(ur->bp > ur->sp && ur->bp < ur->sp + 0x800)
				for(i = -2; i < 10; i++)
					fprint(2, "linuxrun:  bp%+d = %lux\n", i*4,
						((ulong*)ur->bp)[i]);
		}
		return 0;
	}
	pc = (uchar*)ur->pc;
	if(pc[0] != 0x0f || pc[1] != 0x0b)
		return 0;
	if(!started){
		started = 1;
		ur->pc = entrypc;
		ur->sp = stacktop;
		ur->ax = 0;
		ur->bx = 0;
		ur->cx = 0;
		ur->dx = 0;
		ur->si = 0;
		ur->di = 0;
		ur->bp = 0;
		return 1;
	}
	if(forkpending){
		/* a fork/clone child bouncing off the starter: we are on
		 * our own note stack now, so it is safe to privatize the
		 * guest image (the parent is parked on the forkready
		 * pipe and cannot touch the shared segments) */
		forkpending = 0;
		*ur = forkregs;
		if(forksnap){
			int s, tfd;
			ulong va, len, ulen, a, fastlen;
			char snapname[64];
			static uchar pg[Pgsz];

			forksnap = 0;
			fastlen = 0;
			/* Copy the image through a scratch file, one page
			 * at a time, skipping untouched (all-zero) pages:
			 * fresh "memory" attachments come back zeroed, and
			 * neither a big malloc nor an extra segment is
			 * available inside this note handler. */
			snprint(snapname, sizeof snapname,
				"/tmp/lrsnap.%d", getpid());
			tfd = -1;
			/* RAM scratch when the touched pages fit: the
			 * ufs-backed /tmp costs tens of seconds per
			 * 70MB GTK fork and stalls the whole session;
			 * pages beyond the scratch spill to the file */
			{
				uchar *cur;
				ulong fasttot;

				cur = nil;
				fasttot = 0;
				if(forkscratch != nil){
					cur = (uchar*)forkscratch;
				}
				for(s = 0; s < nguestsegs; s++){
					va = guestsegs[s][0];
					len = ulen = guestsegs[s][1];
					if(va == Mapbase)
						ulen = mapbump - Mapbase;
					for(a = 0; a < ulen; a += Pgsz){
						ulong hdr[2];
						uchar *pp;
						int k, nz;

						pp = (uchar*)va + a;
						nz = 0;
						for(k = 0; k < Pgsz; k += sizeof(ulong))
							if(*(ulong*)(pp+k) != 0){
								nz = 1;
								break;
							}
						if(!nz)
							continue;
						if(cur != nil && fasttot + 8 + Pgsz <= Scratchsize){
							((ulong*)cur)[0] = (ulong)pp;
							((ulong*)cur)[1] = Pgsz;
							memmove(cur+8, pp, Pgsz);
							cur += 8 + Pgsz;
							fasttot += 8 + Pgsz;
							continue;
						}
						/* overflowed RAM: spill the rest to the file */
						hdr[0] = (ulong)pp;
						hdr[1] = Pgsz;
						if(tfd < 0){
							tfd = create(snapname, ORDWR|OTRUNC, 0600);
							if(tfd < 0){
								fprint(2, "linuxrun: fork snap create: %r\n");
								exits("fork snapshot");
							}
						}
						if(write(tfd, hdr, 8) != 8 ||
						    write(tfd, pp, Pgsz) != Pgsz){
							fprint(2, "linuxrun: fork snap write: %r\n");
							exits("fork snapshot");
						}
					}
				}
				fastlen = fasttot;
			}
			for(s = 0; s < nguestsegs; s++){
				va = guestsegs[s][0];
				segdetach((void*)va);
				if(segattach(0, "memory", (void*)va, guestsegs[s][1]) == (void*)-1){
					fprint(2, "linuxrun: fork attach %#lux %#lux: %r\n",
						va, guestsegs[s][1]);
					exits("fork attach");
				}
			}
			{
				uchar *cur;

				cur = (uchar*)forkscratch;
				while(cur && cur < (uchar*)forkscratch + fastlen){
					memmove((void*)((ulong*)cur)[0], cur+8, Pgsz);
					cur += 8 + Pgsz;
				}
			}
			if(tfd >= 0){
				seek(tfd, 0, 0);
				for(;;){
					ulong hdr[2];
					long n;

					n = readn(tfd, hdr, 8);
					if(n < 8)
						break;
					if(readn(tfd, pg, Pgsz) != Pgsz){
						fprint(2, "linuxrun: fork snap read: %r\n");
						exits("fork snapshot");
					}
					memmove((void*)hdr[0], pg, Pgsz);
				}
				close(tfd);
				remove(snapname);
			}
		}
		if(forkready[1] >= 0){
			write(forkready[1], "x", 1);
			close(forkready[1]);
			forkready[1] = -1;
		}
		return 1;
	}
	ur->ax = dosyscall(ur);
	ur->pc += 2;
	if(tlsfsokay)
		ur->fs = tlsselector;
	return 1;
}

static void
runguest(void)
{
	void (*f)(void);
	int mfd;

	/* tell the kernel this process runs foreign binaries (their
	 * int $0x80 becomes a note instead of a plan9 syscall) */
mfd = open("/dev/mark", OWRITE);
	if(mfd < 0){
		if(bind("#z", "/dev", MAFTER) >= 0)
			mfd = open("/dev/mark", OWRITE);
	}
	if(mfd >= 0){
		write(mfd, "1", 1);
		close(mfd);
	}
	if(mfd >= 0)
		attachnotestack();
	else
		fprint(2, "linuxrun: foreign mark failed: %r\n");

	atnotify(traphandler, 1);
	f = (void(*)(void))trapinsn;
	f();
	fatal("returned from the guest");	/* not reached */
}

static void
usage(void)
{
	fprint(2, "usage: linuxrun [-nv] prog\n");
	exits("usage");
}

void
main(int argc, char *argv[])
{
	Ehdr eh;
	uchar hdr[64];
	int fd, i;

	ARGBEGIN{
	case 'n':
		analyzeonly = 1;
		break;
	case 'v':
		verbose = 1;
		break;
	case 'p':
		phdrmode = 1;
		break;
	case 'P':
		phdrmode = 2;
		break;
	default:
		usage();
	}ARGEND
	if(argc < 1)
		usage();

	fd = open(argv[0], OREAD);
	if(fd < 0)
		fatal("open %s: %r", argv[0]);
	if(readat(fd, hdr, sizeof hdr, 0) < 0)
		fatal("read %s: %r", argv[0]);

	memset(&eh, 0, sizeof eh);
	memmove(eh.ident, hdr, Elfident);
	if(eh.ident[EiClass] != Elfclass32 || eh.ident[EiData] != Elfdata2lsb)
		fatal("%s: not a 32-bit little-endian ELF", argv[0]);
	eh.type = le16(hdr+16);
	eh.machine = le16(hdr+18);
	eh.entry = le32(hdr+24);
	eh.phoff = le32(hdr+28);
	eh.phentsize = le16(hdr+42);
	eh.phnum = le16(hdr+44);
	if(eh.machine != Em386)
		fatal("%s: not an i386 binary", argv[0]);
	if(eh.type != EtExec && eh.type != EtDyn)
		fatal("%s: not an executable (type %d)", argv[0], eh.type);
	if(eh.phnum > Maxph)
		fatal("%s: too many program headers", argv[0]);

	if(analyzeonly){
		print("linuxrun: %s: static 386 Linux ELF entry %#lux\n",
			argv[0], eh.entry);
		exits(nil);
	}

	nphhdrs = eh.phnum;
	entrypc = loadelf(fd, &eh, eh.type == EtDyn ? Piebase : 0);
	mainentry = entrypc;
	close(fd);
	/* the phdrs live in the first PT_LOAD; translate the file offset */
	phdrva = 0;
	if(phdrmode == 1){
		for(i = 0; i < nph; i++){
			if(ph[i].type == PtLoad && ph[i].memsz > 0){
				phdrva = ph[i].vaddr & ~(Pgsz-1);
				break;
			}
		}
		if(verbose)
			fprint(2, "linuxrun: phdr(mode ehdr) %#lux\n", phdrva);
	}
	for(i = 0; i < nph; i++){
		if(ph[i].type == PtLoad && ph[i].memsz > 0){
			if(eh.phoff >= ph[i].offset &&
			    eh.phoff < ph[i].offset + ph[i].filesz)
				phdrva = ph[i].vaddr + (eh.phoff - ph[i].offset);
			break;
		}
	}
	if(verbose)
		fprint(2, "linuxrun: phdr %#lux\n", phdrva);

	/* dynamic: load the interpreter and hand control to it */
	if(dynamic){
		int ifd;
		Ehdr ieh;
		uchar ihdr[64];

		if(interppath[0] == 0)
			fatal("no interpreter path");
		ifd = open(interppath, OREAD);
		if(ifd < 0)
			fatal("open %s: %r", interppath);
		if(readat(ifd, ihdr, sizeof ihdr, 0) < 0)
			fatal("read %s: %r", interppath);
		memset(&ieh, 0, sizeof ieh);
		memmove(ieh.ident, ihdr, Elfident);
		ieh.type = le16(ihdr+16);
		ieh.machine = le16(ihdr+18);
		ieh.entry = le32(ihdr+24);
		ieh.phoff = le32(ihdr+28);
		ieh.phentsize = le16(ihdr+42);
		ieh.phnum = le16(ihdr+44);
		if(ieh.machine != Em386 || ieh.type != EtDyn)
			fatal("%s: not an i386 shared interpreter", interppath);
		interpbase = Interpbase;
		loadelf(ifd, &ieh, interpbase);
		close(ifd);
		/* ld.so enters; AT_ENTRY/AT_PHDR still describe the main */
		entrypc = interpbase + ieh.entry;
		if(verbose)
			fprint(2, "linuxrun: interp %s base %#lux entry %#lux\n",
				interppath, interpbase, entrypc);
	}

	brkcur = Brkbase;
	segat(Mapbase, Mapsize);
	/* snapshots use the file path; the session mounts a
	 * ramfs on /tmp so that path is RAM-fast */
	forkscratch = nil;

	stacktop = buildstack(argc, argv);
	if(verbose)
		fprint(2, "linuxrun: entry %#lux stack %#lux\n", entrypc, stacktop);

	runguest();
	exits(nil);
}
