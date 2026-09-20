#include <u.h>
#include <libc.h>
#include "environment.h"
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

/* Linux i386 signal state: Wine registers SIGSEGV handlers through
 * rt_sigaction and dispatches its i386 unix thunks (deliberate `hlt`
 * privileged faults) from them.  Without delivery, every thunk dies. */
static ulong sighandler[65];
static ulong sigrestorer[65];
static int sigreturning;
static ulong sigret_pc, sigret_sp, sigret_ax, sigret_bp, sigret_bx,
	sigret_cx, sigret_dx, sigret_si, sigret_di;


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

/* Deliver signo to the guest handler by building an i386 rt_sigframe:
 *   +0   pretcode (doubles as the handler's return address)
 *   +4   sig, +8 pinfo, +12 puc  (the handler's stack arguments)
 *   +16  siginfo (128 bytes)
 *   +144 ucontext: uc_flags/link/stack(12), sigcontext at +164,
 *        uc_sigmask after the sigcontext
 *   +264 retcode: popl %eax; movl $173,%eax; int $0x80
 * Returns 1 when the ureg now resumes in the handler. */
static int
deliversignal(int signo, Ureg *ur, ulong sicode, ulong siaddr)
{
	uchar *f;
	ulong sp;
	int i;

	if(!started || signo < 1 || signo > 64)
		return 0;
	if(sighandler[signo] == 0 || sighandler[signo] == 1)
		return 0;
	if(ur->sp < 0x10000 || ur->sp > 0x7f000000)
		return 0;
	sp = (ur->sp - 8 - 288) & ~7;
	f = (uchar*)sp;
	for(i = 0; i < nguestsegs; i++)
		if(guestsegs[i][0] <= sp && sp+288 <= guestsegs[i][0]+guestsegs[i][1])
			break;
	if(i >= nguestsegs)
		return 0;
	memset(f, 0, 288);
	*(ulong*)(f+0) = sp + 264;		/* pretcode */
	*(ulong*)(f+4) = signo;
	*(ulong*)(f+8) = sp + 16;		/* pinfo */
	*(ulong*)(f+12) = sp + 144;		/* puc */
	*(ulong*)(f+16) = signo;		/* si_signo */
	*(ulong*)(f+20) = 0;			/* si_errno */
	*(ulong*)(f+24) = sicode;		/* si_code */
	*(ulong*)(f+28) = siaddr;		/* si_addr */
	/* sigcontext (uc_mcontext) at +164 */
	*(ulong*)(f+164+16) = ur->di;
	*(ulong*)(f+164+20) = ur->si;
	*(ulong*)(f+164+24) = ur->bp;
	*(ulong*)(f+164+28) = ur->sp;
	*(ulong*)(f+164+32) = ur->bx;
	*(ulong*)(f+164+36) = ur->dx;
	*(ulong*)(f+164+40) = ur->cx;
	*(ulong*)(f+164+44) = ur->ax;
	*(ulong*)(f+164+48) = 13;		/* trapno: #GP */
	*(ulong*)(f+164+52) = 0;		/* err */
	*(ulong*)(f+164+56) = ur->pc;		/* ip */
	*(ulong*)(f+164+64) = 0x200;		/* eflags: IF */
	*(ulong*)(f+164+68) = ur->sp;		/* sp at signal */
	f[264] = 0x58;				/* popl %eax */
	f[265] = 0xb8;				/* movl $173,%eax */
	*(ulong*)(f+266) = 173;
	f[270] = 0xcd; f[271] = 0x80;		/* int $0x80 */
	ur->pc = sighandler[signo];
	ur->sp = sp;
	ur->ax = 0;
	return 1;
}

/* The guest-visible process id.  Every clone thread is a separate host
 * process with its own host pid, but threads share the guest image and
 * must all see one getpid(): glvnd's libGLX fork check (and glib) store
 * the pid in shared memory and reset everything when it changes between
 * calls - host pids per thread made xfwm4 spin in __glDispatchReset
 * forever.  CLONE_VM children inherit the parent's value through the
 * rfork copy; real fork children re-assign their own new pid. */
ulong guestprocid;

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
static long dofutex(ulong, ulong, ulong, ulong, ulong, ulong);

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

/* is this guest range actually mapped in this process?  native calls
 * that take guest pointers must check, or a stale pointer kills the
 * whole translation process with an unattributable fault note */
static int
guestok(ulong addr, ulong len)
{
	int i;

	for(i = 0; i < nguestsegs; i++)
		if(guestsegs[i][0] <= addr && addr+len <= guestsegs[i][0]+guestsegs[i][1])
			return 1;
	return 0;
}

static void
guestprobe(char *site, ulong addr, ulong len)
{
	static int zg;

	if(!guestok(addr, len) && zg++ < 20)
		fprint(2, "linuxrun: GUESTPTR p%d %s %#lux +%lux UNMAPPED\n",
			getpid(), site, addr, len);
}


/* back a clone thread's stack on demand: Wine creates threads with
 * stacks anywhere in its reserved area (seen at 0xdfff0000), outside
 * every segment; without backing, the child's first stack access
 * kills the process.  Called from the post-rfork context, which can
 * attach. */
static void
ensurestack(ulong sp)
{
	ulong base;
	int i;

	if(sp < 0x10000)
		return;
	for(i = 0; i < nguestsegs; i++)
		if(guestsegs[i][0] <= sp && sp < guestsegs[i][0]+guestsegs[i][1])
			return;
	base = (sp - 256*1024) & ~(Pgsz-1);
	segat(base, 512*1024);
	if(verbose)
		fprint(2, "linuxrun: thread stack %#lux backed at %#lux\n", sp, base);
}

static int
readat(int fd, void *buf, long n, vlong off)
{
	if(seek(fd, off, 0) < 0)
		return -1;
	guestprobe("readat", (ulong)buf, n);
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
	{
		/* name the image and its LOAD layout: an exec whose
		 * segments land in unmapped guest memory (0xdfff seen)
		 * must be identifiable before it kills the process */
		static int zl;

		if(zl++ < 12){
			fprint(2, "linuxrun: LOADELF p%d entry=%lux phnum=%d base=%lux\n",
				getpid(), eh->entry, eh->phnum, base);
			for(i = 0; i < eh->phnum; i++){
				int t, fv, mz;

				t = le32(buf+i*eh->phentsize+0);
				fv = le32(buf+i*eh->phentsize+8);
				mz = le32(buf+i*eh->phentsize+20);
				if(t == 1 || fv+base > 0xd0000000)
					fprint(2, "linuxrun:  ph%d type=%d va=%lux+%lux\n",
						i, t, fv+base, mz);
			}
		}
	}
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
	ulong argv[Maxargs];
	ulong enva[Maxenv];
	int i, ne, naux, nvec;

	if(nargs < 1 || nargs >= Maxargs)
		fatal("too many arguments");
	st = segat(Stackbase, Stacksize);
	/* Reserve the top page for the syscall thunk. */
	strp = Stackbase + Stacksize - Pgsz;
	for(i = nargs-1; i >= 0; i--){
		int l;

		l = strlen(args[i]) + 1;
		if(l > strp-Stackbase-8192)
			fatal("argument list too large");
		strp -= l;
		strcpy((char*)st + (strp - Stackbase), args[i]);
		argv[i] = strp;
	}
	for(ne = 0; genv[ne] != nil; ne++){
		int l;

		l = strlen(genv[ne]) + 1;
		if(ne >= Maxenv-1 || l > strp-Stackbase-8192)
			fatal("environment too large");
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
static void sockpredrain(void);
/* plan9 notes (the handled closed-pipe one among them) interrupt a
 * blocked write; POSIX expects the syscall retried, but an error
 * return made libxtrans abandon half-written replies - the client
 * then waits forever for the rest of a big reply */
static void drainpeers(int);
static int peerqlen(int);

/* eventfd write: a wakeup is only meaningful when none is pending
 * (the counter semantics coalesce), so when the pipe already holds
 * data the new value is discarded instead of ever risking a blocking
 * write on the tiny pipe buffer */
static long
efdwr(int fd, void *buf, long n)
{
	static int ze;

	if(ze++ < 60)
		fprint(2, "linuxrun: EFDDBG p%d fd=%d n=%ld ql=%d\n",
			getpid(), fd, n, peerqlen(fd));
	if(peerqlen(fd) > 0)
		return n;
	return -2;	/* caller falls through to the normal write */
}

/* consume everything queued on a pipe without blocking */
static void
drainfd(int fd)
{
	char dbuf[512];
	Dir *d;
	int dn, tot;

	tot = 0;
	while((d = dirfstat(fd)) != nil){
		int ql;

		ql = d->length;
		free(d);
		if(ql <= 0)
			break;
		dn = read(fd, dbuf, ql < sizeof dbuf ? ql : sizeof dbuf);
		if(dn <= 0)
			break;
		tot += dn;
		if(tot > 1<<20)
			break;
	}
	if(tot)
		fprint(2, "linuxrun: DRAIN p%d fd=%d drained=%d\n", getpid(), fd, tot);
}

static long
sockwr(int fd, void *buf, long n)
{
	long w, done;
	char es[ERRMAX];
	int nint;

	{
		static int zpk;
		Dir *d;

		if(zpk++ < 60 && n == 1 && (d = dirfstat(fd)) != nil){
			fprint(2, "linuxrun: PRECHK p%d wfd=%d n=%ld q=%llux.%lux len=%llux\n",
				getpid(), fd, n,
				(vlong)d->qid.path, (ulong)d->qid.vers, (vlong)d->length);
			free(d);
		}
	}
	done = 0;
	nint = 0;
	while(done < n){
		/* never enter a blocking write on a full wakeup pipe: a
		 * swallowed alarm note makes plan9 RESUME the write, so
		 * the one alarm per arming leaves it stuck forever */
		{
			int ql;

			/* eventfd counter semantics coalesce wakeups, so a pipe holding
			 * more than one 8-byte wakeup is over-queued and safe
			 * to collapse (real eventfds never hold more than 8) */
			if(nint < 200 && (ql = peerqlen(fd)) > 16){
				nint++;
				drainpeers(fd);
				sleep(100);
				continue;
			}
		}
		alarm(3000);
		while((w = write(fd, (char*)buf+done, n-done)) < 0){
			errstr(es, sizeof es);
			if(strcmp(es, "interrupted") == 0 && nint < 40){
				/* a full wakeup-style pipe whose reader
				 * never drains it would freeze the writer
				 * forever: drain the peer end so the write
				 * can proceed (eventfd semantics tolerate
				 * counter loss).  Bounded retries with a
				 * pause - a raw drain/retry loop livelocked
				 * at full CPU when the drain freed nothing */
				nint++;
				alarm(0);
				drainpeers(fd);
				sleep(100);
				alarm(3000);
				continue;
			}
			alarm(0);
			if(nint >= 40)
				fprint(2, "linuxrun: WTIMEO p%d fd=%d n=%ld\n",
					getpid(), fd, n);
			return nint >= 40 ? -11 : -1;
		}
		alarm(0);
		if(w < n-done){
			/* plan9 pipes can take partial blocks when their
			 * queue fills: report it once (the fragment size
			 * names the limit) and keep pushing - returning
			 * the short count made Xorg flush one fragment
			 * per dispatch cycle, trickling 3MB replies at
			 * 32 bytes apiece */
			static int zs;

			if(zs++ < 10)
				fprint(2, "linuxrun: SHORTWR p%d wrote %ld of %ld errstr=%s\n",
					getpid(), w, n-done, es);
		}
		done += w;
	}
	return done;
}
static void sockopc(int, void*, long, int);
static void dumpopc(void);
static int sockslot(int);
static int socknewslot(ulong, int);
static int sockinready(int);
extern int sockmap[16][4];
static int postsrvfd(char*, int);

static ulong sockrawqlen(int);

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
	{
		static int zs;
		struct Liovec *sv;
		int sb;

		if(zs++ < 60 && m[2] > 0x10000){
			sv = (struct Liovec*)m[2];
			fprint(2, "linuxrun: SMSG p%d fd=%lux iov=%lux n=%lux:",
				getpid(), fd, m[2], m[3]);
			for(sb = 0; sb < 20 && sb < (int)sv[0].len; sb++)
				fprint(2, " %2.2ux", ((uchar*)sv[0].base)[sb]);
			fprint(2, "\n");
		}
	}
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
	long total;
	int slot;
	ulong i;

	v = (struct Liovec*)iov;
	slot = sockslot((int)fd);
	{
		static int zw;

		if(zw++ < 50 && cnt > 0 && v[0].len == 1 && iov > 0x10000){
			int zb;

			fprint(2, "linuxrun: WRV p%d fd=%lux slot=%d n=%lux b:",
				getpid(), fd, slot, v[0].len);
			for(zb = 0; zb < 12 && zb < (int)v[0].len; zb++)
				fprint(2, " %2.2ux", ((uchar*)v[0].base)[zb]);
			fprint(2, "\n");
		}
	}
	if(slot >= 0 && sockmap[slot][2] == 0)
		return -107;	/* -ENOTCONN: never write fd 0 for these */
	if(slot >= 0)
		sockpredrain();
	fd = sockwritefd((int)fd);
	if(cnt > 0 && v[0].len > 0)
		sockopc(slot, v[0].base, v[0].len, 1);
	total = 0;
	for(i = 0; i < cnt; i++){
		long n;

		if(v[i].len == 0)
			continue;
		n = sockwr((int)fd, v[i].base, v[i].len);
		{
			static int zwq;
			int ws;

			ws = sockslot((int)fd);
			if(v[i].len == 1 && zwq++ < 60 && ws >= 0 && sockmap[ws][2] != 0)
				fprint(2, "linuxrun: WCHK p%d gfd=%lux wrote=%ld qlen-read-end=%d\n",
					getpid(), fd, n, sockrawqlen(ws));
		}
		if(n < 0){
			if(total > 0)
				break;	/* report the partial write */
			return -Ebadf;
		}
		total += n;
		if(n < (long)v[i].len)
			break;	/* short write: report it, the guest
				 * buffers the rest - the old code
				 * returned the REQUESTED total and the
				 * unsent bytes were lost forever */
	}
	{
		static int zwv;
		ulong want;

		want = 0;
		for(i = 0; i < cnt; i++)
			want += v[i].len;
		if(zwv++ < 30 && total != (long)want)
			fprint(2, "linuxrun: PARTWRV p%d fd=%lux want=%lux got=%ld\n",
				getpid(), fd, want, total);
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
	total = 0;
	for(i = 0; i < cnt; i++){
		if(v[i].len == 0)
			continue;
		/* through sockread so buffered (pre-drained) data is
		 * served first */
		n = sockread((int)fd, v[i].base, v[i].len);
		if(n < 0)
			return n;
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
			ptytab[i].masterg = socknewslot((ptytab[i].s2m[0]<<16) | ptytab[i].m2s[1], 0);
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
	return socknewslot((ptytab[n].m2s[0]<<16) | ptytab[n].s2m[1], 0);
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
	{
		static int zo;

		if(zo++ < 500)
			fprint(2, "linuxrun: OPENCHK p%d %s\n", getpid(), p);
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
	{
		/* fd identity trace: the frozen dbus-daemon's last calls
		 * were read(fd=7)/read(fd=8) - name what each fd is so a
		 * stall scene identifies its file (ufs EOF hang vs own
		 * wakeup pipe) instantly */
		static int zo;

		if(zo++ < 200)
			fprint(2, "linuxrun: OPEN p%d fd=%d %s\n",
				getpid(), fd, p);
	}
	return fd;
}

static ulong mapbump = Mapbase + Brksize + Pgsz;

/* PROT_NONE reservations: recorded but not backed until mprotect makes
 * them accessible.  Wine's virtual_init reserves its whole views area
 * (about 2GB at 0x110000) this way before allocating anything real. */
static struct { ulong base, len; } resv[128];
static int nresv;

static void
resvadd(ulong base, ulong len)
{
	if(nresv < nelem(resv)){
		if(nresv > 0 &&
		   resv[nresv-1].base + resv[nresv-1].len == base){
			resv[nresv-1].len += len;
			return;
		}
		resv[nresv].base = base;
		resv[nresv].len = len;
		nresv++;
	}
}

static void
resvdrop(ulong addr, ulong len)
{
	int i;

	for(i = 0; i < nresv; i++){
		if(addr <= resv[i].base && resv[i].base + resv[i].len <= addr+len){
			memmove(&resv[i], &resv[i+1], (nresv-i-1)*sizeof resv[0]);
			nresv--;
			i--;
		}
	}
}

static void	zerorange(ulong, ulong);

/* Low guest memory, attached on demand: Wine's virtual_init builds its
 * views from small fixed mappings at low addresses that the main guest
 * segment does not cover.  Sized to what the VM can spare next to the
 * main segment. */
enum { Lowbase = 0x200000, Lowsize = 0x7e00000 };	/* the notestack's shared segment sits at 0x8000000 */
static int lowseg;

static void
ensurelow(void)
{
	if(!lowseg){
		/* Opt-in (LINUXRUN_LOW in the host environment): Wine needs
		 * low guest memory, but attaching it changes the note-frame
		 * behavior enough to destabilize ordinary guests (the
		 * unified smoke's Xvfb child dies with note-stack pointers
		 * leaking into syscalls).  The lazy note-context path below
		 * stays harmless for everyone else. */
		if(getenv("LINUXRUN_LOW") == nil)
			return;
		if(segattach(0, "memory", (void*)Lowbase, Lowsize) != (void*)-1){
			lowseg = 1;
			if(nguestsegs < 16){
				guestsegs[nguestsegs][0] = Lowbase;
				guestsegs[nguestsegs][1] = Lowsize;
				nguestsegs++;
			}
		}else if(verbose)
			fprint(2, "linuxrun: low segment attach failed: %r\n");
	}
}

/* back a reserved range on demand: only ranges that were actually
 * recorded as reservations are touched (zeroed or segmented); ordinary
 * mprotects over already-loaded memory must not be zeroed. */
static long
resvmaterialize(ulong addr, ulong len)
{
	int i, hit;

	hit = 0;
	for(i = 0; i < nresv; i++){
		if(addr <= resv[i].base && resv[i].base + resv[i].len <= addr+len){
			hit = 1;
			break;
		}
	}
	if(!hit)
		return 0;
	resvdrop(addr, len);
	if(addr >= Mapbase && addr+len <= Mapbase+Mapsize){
		zerorange(addr, len);
		return 0;
	}
	if((addr >= 0x68000000 && addr < 0x69000000) ||
	   (addr >= 0x7d000000 && addr < 0x7e000000))
		return 0;
	if(segat(addr, len) == (void*)-1){
		if(verbose)
			fprint(2, "linuxrun: reserve materialize %#lux +%#lux: %r (optimistic)\n",
				addr, len);
	}
	return 0;
}

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

	if(verbose)
		fprint(2, "linuxrun: mmap? addr=%#lux len=%#lux prot=%lux flags=%#lux fd=%d off=%#lux\n",
			addr, len, prot, flags, (int)fd, off);
	if(len == 0)
		return -Einval;
	len = ((len + Pgsz-1) / Pgsz) * Pgsz;
	if(prot == 0 && (flags & 0x20) && len >= 0x10000000){
		/* anonymous PROT_NONE of 256MB or more: a giant reservation,
		 * not memory (Wine's virtual_init reserves its whole views
		 * area, about 2GB, this way).  Record it - even MAP_FIXED,
		 * which must not unmap our runtime areas - and let mprotect
		 * materialize on demand.  Smaller PROT_NONE mappings keep
		 * the backed path: ld.so guards libraries with them and its
		 * version checks read through them. */
		if(flags & 0x10){
			if(addr == 0)
				return -Einval;
			resvdrop(addr, len);
			resvadd(addr, len);
			if(verbose)
				fprint(2, "linuxrun: reserve fixed %#lux +%#lux\n",
					addr, len);
			return addr;
		}
		if(addr != 0 && addr >= Mapbase && addr + len <= Mapbase + Mapsize)
			va = addr;
		else{
			if(mapbump + len > Mapbase + Mapsize)
				return -Enomem;
			va = mapbump;
			mapbump += len;
		}
		resvdrop(va, len);
		resvadd(va, len);
		if(verbose)
			fprint(2, "linuxrun: reserve %#lux +%#lux -> %#lux\n",
				addr, len, va);
		return va;
	}
	if(flags & 0x10){	/* MAP_FIXED: the caller chose the address */
		if(addr == 0 || addr + len > Mapbase + Mapsize)
			return -Enomem;
		if(addr < Mapbase){
			ensurelow();
			if(!lowseg || addr < Lowbase)
				return -Enomem;
		}
		va = addr;
		zerorange(va, len);
	}else{
		if(addr != 0 && addr >= Lowbase && addr + len <= Mapbase + Mapsize){
			if(addr < Mapbase){
				ensurelow();
				if(!lowseg)
					return -Enomem;
			}
			va = addr;	/* hint we can honour */
		}
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

/* fill an i386 struct statx so modern glibc and wine callers see
 * basic stats; they only need mask/mode/size/ino for existence probes */
static long
fillstatx(ulong addr, int fd)
{
	uchar *b;
	Dir *d;
	ulong mode;

	if(addr == 0)
		return 0;
	d = dirfstat(fd);
	if(d == nil)
		return -Ebadf;
	b = (uchar*)addr;
	memset(b, 0, 256);
	if(d->mode & DMDIR)
		mode = 0x41ed;			/* S_IFDIR|0755 */
	else
		mode = (d->mode & 0111) ? 0x81ed : 0x81a4;
	*(ulong*)(b+0) = 0x7ff;		/* stx_mask: STATX_BASIC_STATS */
	*(ulong*)(b+4) = 4096;		/* stx_blksize */
	*(ulong*)(b+16) = 1;		/* stx_nlink */
	*(ushort*)(b+28) = mode;	/* stx_mode */
	*(vlong*)(b+32) = d->qid.path;	/* stx_ino */
	*(vlong*)(b+40) = d->length;	/* stx_size */
	*(vlong*)(b+48) = (d->length+511)/512;	/* stx_blocks */
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
char isefd[NSOCK];	/* eventfd-style wakeup pipe: safe to drain */

/* queued bytes on the read end of an EVENTFD pipe whose write side is
 * fd, or -1 when fd is not an eventfd stand-in (socket pipes must never
 * be drained: their queue is live protocol data) */
static int
peerqlen(int fd)
{
	int i;
	Dir *d;
	int q;

	q = -1;
	for(i = 0; i < NSOCK; i++)
		if(sockmap[i][1] && (sockmap[i][2] & 0xffff) == fd){
			if(isefd[i] && (d = dirfstat(sockmap[i][2] >> 16)) != nil){
				q = d->length;
				free(d);
			}
			break;
		}
	return q;
}

/* a write to a full wakeup pipe must not freeze the writer: drain the
 * peer end of the bridge socket whose write side is fd */
static void
drainpeers(int fd)
{
	int i;

	for(i = 0; i < NSOCK; i++)
		if(sockmap[i][1] && (sockmap[i][2] & 0xffff) == fd){
			if(isefd[i])
				drainfd(sockmap[i][2] >> 16);
			break;
		}
}
/* Receive-side buffering: every guest write to a bridge socket first
 * drains the process's own readable pipes into these buffers, so a
 * peer blocked on a full pipe always completes - the two-pipe X
 * deadlock cannot form, and plan9 needs no nonblocking write. */
uchar *inq[NSOCK];
int soeof[NSOCK];	/* peer closed its end (read gave EOF) */
int gifseq[NSOCK];
int setupdone[NSOCK];	/* pending GetInputFocus (op 43) request seq, for the None->PointerRoot reply rewrite */
ulong inqn[NSOCK], inqcap[NSOCK];

static ulong
sockrawqlen(int i)
{
	Dir *d;
	ulong q;

	q = 0;
	if((d = dirfstat(sockmap[i][2] >> 16)) != nil){
		q = d->length;
		free(d);
	}
	return q;
}

static void
sockdrain(int i)
{
	ulong got, q, chunk;
	long n;

	got = 0;
	while(got < 1024*1024){
		q = sockrawqlen(i);
		if(q == 0)
			break;
		chunk = q > 16384 ? 16384 : q;
		if(inqn[i] + chunk > inqcap[i]){
			ulong nc;
			uchar *nq;

			nc = inqcap[i] ? inqcap[i] : 4096;
			while(nc < inqn[i] + chunk)
				nc *= 2;
			nq = malloc(nc);
			if(nq == nil)
				return;
			if(inqn[i])
				memmove(nq, inq[i], inqn[i]);
			free(inq[i]);
			inq[i] = nq;
			inqcap[i] = nc;
		}
		n = read(sockmap[i][2] >> 16, inq[i] + inqn[i], chunk);
		if(n <= 0)
			break;
		inqn[i] += n;
		got += n;
	}
}

/* drain every readable bridge pipe: called before a guest write can
 * block, so the peer's stuck write always finds room.
 *
 * DISARMED: plan9 pipes have no limit, so writes never block - but
 * this drain pulled up to 1MB of replies into the WRITING process'
 * inq, stealing bytes that belonged to another thread's pending read
 * on the same shared connection (CLONE_FILES threads are separate
 * host procs here).  xfwm4's threaded xfconf property sync hit this
 * constantly: one thread's ChangeProperty pre-drained the other
 * thread's GetProperty reply and everything stalled.  */
static void
sockpredrain(void)
{
}
/* X-protocol forensics: first byte of every write (request opcode) and
 * read (event/reply type) through a bridge socket, counted per slot */
int wopc[NSOCK][256];
int ropc[NSOCK][256];
int xcli[NSOCK];
long wroff[NSOCK];

static void
sockopc(int slot, void *buf, long n, int wr)
{
	static int npr, nseq, nxw;
	int op, seq;

	if(slot < 0 || n <= 0 || buf == nil)
		return;
	op = ((uchar*)buf)[0];
	seq = ((uchar*)buf)[2] | (((uchar*)buf)[3]<<8);
	/* sequence tracking: requests carry their seq at bytes 2-3,
	 * replies and errors too (events do not - skip those) */
	if(nseq < 900){
		if(wr){
			nseq++;
			fprint(2, "linuxrun: SEQ p%d g%d s%d wr op=%d seq=%d n=%ld",
				getpid(), sockmap[slot][0], slot, op, seq, n);
			/* the full request head for window-bearing ops: a
			 * corrupted window ID in a reply draws BadWindow
			 * downstream - the request names what it asked */
			if(op == 2 || op == 3 || op == 20 || op == 14 || op == 15 || op >= 128 || seq >= 60){
				int qb;

				fprint(2, " req:");
				for(qb = 0; qb < 16 && qb < n; qb++)
					fprint(2, " %2.2ux", ((uchar*)buf)[qb]);
			}
			fprint(2, "\n");
		}else if(op <= 1 || (wr == 0 && n <= 2600)){
			nseq++;
			fprint(2, "linuxrun: SEQ p%d g%d s%d rd op=%d seq=%d n=%ld",
				getpid(), sockmap[slot][0], slot, op, seq, n);
			if(op == 0 && n >= 8)
				/* error reply: byte1 = code, byte2-3 = seq,
				 * byte4-5 = minor, byte8 = major opcode */
				fprint(2, " XERR code=%d maj=%d",
					((uchar*)buf)[1], ((uchar*)buf)[8]);
			if(n <= 40 && (op <= 1 || seq >= 50)){
				/* full short reply bytes: the window-ID
				 * fields (owner at +8) are the suspects */
				int qb;

				fprint(2, " rep:");
				for(qb = 0; qb < n && qb < 32; qb++)
					fprint(2, " %2.2ux", ((uchar*)buf)[qb]);
			}
			fprint(2, "\n");
		}
	}
	if(wr){
		if(op == 43 && slot >= 0 && slot < NSOCK)
			gifseq[slot] = seq;
		/* first write per socket = the connection setup reply;
		 * dump RAW bytes 0-71 so vendor length, format count and
		 * the first screen's root window can be read directly off
		 * the wire (computed offsets disagreed with the bytes) */
		if(slot >= 0 && slot < NSOCK && setupdone[slot] < 10){
			int sb;

			setupdone[slot]++;
			fprint(2, "linuxrun: SETUP p%d g%d n=%ld bytes:",
				getpid(), sockmap[slot][0], n);
			for(sb = 0; sb < 80 && sb < n; sb++)
				fprint(2, " %2.2ux", ((uchar*)buf)[sb]);
			fprint(2, "\n");
		}
		wopc[slot][op]++;
		/* full write-stream dump for X client sockets (first write
		 * starts with byte-order char 'l'/'B'): the WM's requests
		 * 70/71 got BadWindow/BadDrawable with garbage bad-values,
		 * so we need every request header, not only each write's
		 * first request */
		{
			static int nrd;
			int i, m, lim;
			char hex[8200];

			if(!xcli[slot] && (((uchar*)buf)[0] == 'l' || ((uchar*)buf)[0] == 'B'))
				xcli[slot] = 1;
			lim = n < 4096 ? (int)n : 4096;
			if(xcli[slot] && nxw < 500){
				nxw++;
				for(i = 0; i < lim; i++)
					sprint(hex + 2*i, "%2.2ux", ((uchar*)buf)[i]);
				hex[2*lim] = 0;
				fprint(2, "linuxrun: XWR p%d g%d n=%ld off=%ld: %s\n",
					getpid(), sockmap[slot][0], n, wroff[slot], hex);
			}
			wroff[slot] += n;
		}
	}else{
		static int nrd;
		int i, m;
		char rh[8200];

		ropc[slot][op]++;
		if(xcli[slot] && nrd < 400 && n <= 4096){
			nrd++;
			m = (int)n;
			for(i = 0; i < m; i++)
				sprint(rh + 2*i, "%2.2ux", ((uchar*)buf)[i]);
			rh[2*m] = 0;
			fprint(2, "linuxrun: XRD p%d g%d n=%ld: %s\n",
				getpid(), sockmap[slot][0], n, rh);
		}
	}
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

/* CLONE_FILES threads share the guest fd table, but each thread is a
 * separate host process whose host-side sockmap is only a fork-time
 * copy (and the bounce used to wipe it outright).  A connection the
 * parent opened after the clone then resolves to "not a socket" and
 * the poll layer reports it always-ready - xfwm4's main loop spun
 * forever on such an fd.  Threads re-discover the parent's newer
 * connections from its live memory; the pipe ends are re-opened
 * locally through /proc/<ppid>/fd/N. */
int forkppid;

static void
socksync(void)
{
	char ppath[64], fpath[64];
	ulong nmap[NSOCK][4];
	int m, i;

	if(forkppid <= 0 || forkppid == getpid())
		return;
	snprint(ppath, sizeof ppath, "/proc/%d/mem", forkppid);
	m = open(ppath, OREAD);
	if(m < 0)
		return;
	if(pread(m, nmap, sizeof nmap, (vlong)(ulong)sockmap) == sizeof nmap){
		int ncop;

		ncop = 0;
		for(i = 0; i < NSOCK; i++){
			int rf, wf;

			if(!nmap[i][1] || sockmap[i][1])
				continue;
			rf = nmap[i][2] >> 16;
			wf = nmap[i][2] & 0xffff;
			snprint(fpath, sizeof fpath, "/proc/%d/fd/%d", forkppid, rf);
			rf = open(fpath, OREAD);
			snprint(fpath, sizeof fpath, "/proc/%d/fd/%d", forkppid, wf);
			wf = open(fpath, OWRITE);
			if(rf < 0 || wf < 0){
				if(rf >= 0)
					close(rf);
				if(wf >= 0)
					close(wf);
				continue;
			}
			sockmap[i][0] = nmap[i][0];
			sockmap[i][1] = 1;
			sockmap[i][2] = (rf<<16) | wf;
			sockmap[i][3] = nmap[i][3];
			soeof[i] = 0;
			inqn[i] = 0;
			ncop++;
		}
		if(ncop)
			fprint(2, "linuxrun: SOCKSYNC p%d from p%d +%d\n",
				getpid(), forkppid, ncop);
	}
	close(m);
}

static int
sockslot(int fd)
{
	int i;

	for(i = 0; i < NSOCK; i++)
		if(sockmap[i][1] && sockmap[i][0] == fd)
			break;
	if(i < NSOCK){
		/* STALE-HIT refresh: a thread may have re-registered this
		 * fd number with NEW pipes (e.g. the WM's second X socket
		 * landing on a number our copy still maps to the first
		 * connection - its writes then exited through the wrong
		 * pipe and the server never saw them).  The .f file
		 * carries the live packed value; re-adopt when it moves.
		 * Rate-limited like the miss path. */
		static vlong lasth;
		vlong now;
		char nb[64];
		int key, ff, s3;
		ulong live;

		now = nsec();
		if(now - lasth > 100LL*1000*1000){
			lasth = now;
			key = guestprocid ? (int)guestprocid : getpid();
			snprint(nb, sizeof nb, "/srv/x.m.%d.%d.f", key, fd);
			if((ff = open(nb, OREAD)) >= 0){
				char fb[64];
				int fn;
				ulong fl2;

				fn = readn(ff, fb, sizeof fb-1);
				close(ff);
				if(fn > 0){
					char *fp;

					fb[fn] = 0;
					fl2 = 0;
					live = 0;
					fp = fb;
					while(*fp == ' ')
						fp++;
					while(*fp >= '0' && *fp <= '9')
						fl2 = fl2*10 + *fp++ - '0';
					while(*fp == ' ')
						fp++;
					while(*fp >= '0' && *fp <= '9')
						live = live*10 + *fp++ - '0';
					if(live != 0 && live != sockmap[i][2] && (live >> 16) > 2 && (live & 0xffff) > 2){
						int rf2, wf2;

						snprint(nb, sizeof nb, "/srv/x.m.%d.%d.r", key, fd);
						rf2 = open(nb, OREAD);
						snprint(nb, sizeof nb, "/srv/x.m.%d.%d.w", key, fd);
						wf2 = open(nb, OWRITE);
						if(rf2 >= 0 && wf2 >= 0){
							fprint(2, "linuxrun: READOPT p%d fd=%d %lux -> %lux\n",
								getpid(), fd, sockmap[i][2], live);
							close(sockmap[i][2] >> 16);
							close(sockmap[i][2] & 0xffff);
							sockmap[i][2] = (rf2<<16) | wf2;
							sockmap[i][3] = fl2;
							soeof[i] = 0;
							inqn[i] = 0;
						}else{
							if(rf2 >= 0) close(rf2);
							if(wf2 >= 0) close(wf2);
						}
					}
				}
			}
		}
		return i;
	}
	/* Leader processes adopt too: a connection an eventfd-like
	 * stand-in or socket created by one of OUR threads is unknown
	 * here, and an unknown fd in epoll is reported always-ready -
	 * dbus-daemon's leader spun on its thread-made eventfds that
	 * way and froze in the blocking read.  (socksync consults the
	 * parent, no-op for leaders; the /srv adoption below is keyed
	 * by guestprocid, shared by the whole thread group.) */
	{
		static vlong last;
		vlong now;

		now = nsec();
		if(now - last > 100LL*1000*1000){
			last = now;
			if(forkppid > 0)
				socksync();
			for(i = 0; i < NSOCK; i++)
				if(sockmap[i][1] && sockmap[i][0] == fd)
					return i;
			/* a sibling thread may have opened this connection
			 * after our clone: bridge endpoints are published in
			 * /srv (x.m.<tgid>.<fd>.r/.w/.f) by whoever created
			 * them - opening the name shares the channel */
			{
				char nb[64];
				int rf, wf, ff, s, fl, key0;

				key0 = guestprocid ? (int)guestprocid : getpid();
				snprint(nb, sizeof nb, "/srv/x.m.%d.%d.r", key0, fd);
				rf = open(nb, OREAD);
				snprint(nb, sizeof nb, "/srv/x.m.%d.%d.w", key0, fd);
				wf = open(nb, OWRITE);
				if(rf < 0 || wf < 0){
					static int zaf;

					if(zaf++ < 40)
						fprint(2, "linuxrun: ADOPTFAIL p%d %s rf=%d wf=%d: %r\n",
							getpid(), nb, rf, wf);
				}
				if(rf >= 0 && wf >= 0){
					for(s = 0; s < NSOCK; s++)
						if(!sockmap[s][1])
							break;
					if(s < NSOCK){
						fl = 0;
						snprint(nb, sizeof nb, "/srv/x.m.%d.%d.f", key0, fd);
						ff = open(nb, OREAD);
						if(ff >= 0){
							char fb[8];
							int fn;

							fn = readn(ff, fb, sizeof fb-1);
							close(ff);
							if(fn > 0){
								fb[fn] = 0;
								fl = strtol(fb, nil, 10);
							}
						}
						sockmap[s][0] = fd;
						sockmap[s][1] = 1;
						sockmap[s][2] = (rf<<16) | wf;
						sockmap[s][3] = fl & 1;
						isefd[s] = (fl & 2) ? 1 : 0;
						soeof[s] = 0;
						inqn[s] = 0;
						fprint(2, "linuxrun: ADOPT p%d fd=%d from tgid=%d\n",
							getpid(), fd, forkppid);
						return s;
					}
				}
				if(rf >= 0)
					close(rf);
				if(wf >= 0)
					close(wf);
			}
		}
	}
	return -1;
}


/* Allocate a socket-map slot whose guest-visible descriptor is a real
 * plan9 fd number (a #c/pid placeholder): synthetic high ids leak into
 * guest pointer slots and crash later writes through them. */
static int
sockmapfd(int fd, ulong packed)
{
	int i, j;

	{
		static int zm;

		if(zm++ < 60)
			fprint(2, "linuxrun: SMAP p%d fd=%d pipes=%lux\n",
				getpid(), fd, packed);
	}
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
	/* publish the pipe ends so CLONE_FILES siblings can adopt this
	 * connection (keyed by thread-group id: forkppid for threads,
	 * own pid otherwise) */
	{
		char nb[64];
		int key, pf;

		key = guestprocid ? (int)guestprocid : getpid();
		snprint(nb, sizeof nb, "/srv/x.m.%d.%d.r", key, fd);
		/* unconnected slots (packed==0) publish NOTHING: the
		 * markers would carry fd numbers <= 2, and reading a
		 * marker dups that fd - the stale-hit refresh then
		 * read stdin forever inside sockslot and froze the
		 * writer mid-sendmsg (the WM died exactly there on
		 * every dbus \0) */
		if(packed != 0){
			postsrvfd(nb, packed >> 16);
			snprint(nb, sizeof nb, "/srv/x.m.%d.%d.w", key, fd);
			postsrvfd(nb, packed & 0xffff);
			snprint(nb, sizeof nb, "/srv/x.m.%d.%d.f", key, fd);
			pf = create(nb, OWRITE|OTRUNC, 0666);
			if(pf >= 0){
				fprint(pf, "0 %lux", packed);
				close(pf);
			}
		}
	}
	return j;
}

static int
socknewslot(ulong packed, int efd)
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
		sockmap[i][3] = 0;
		isefd[i] = efd;	/* BEFORE the publication below: the
				 * .f marker's flag bit must already
				 * reflect it or adopters lose the trait */
		/* same /srv publication as sockmapfd: accepted and
		 * socketpair-ish connections go through here */
		{
			char nb[64];
			int key, pf;

			if(packed != 0){
				key = guestprocid ? (int)guestprocid : getpid();
				snprint(nb, sizeof nb, "/srv/x.m.%d.%d.r", key, fd);
				postsrvfd(nb, packed >> 16);
				snprint(nb, sizeof nb, "/srv/x.m.%d.%d.w", key, fd);
				postsrvfd(nb, packed & 0xffff);
				snprint(nb, sizeof nb, "/srv/x.m.%d.%d.f", key, fd);
				pf = create(nb, OWRITE|OTRUNC, 0666);
				if(pf >= 0){
					int efd2, ei;

					efd2 = 0;
					for(ei = 0; ei < NSOCK; ei++)
						if(sockmap[ei][1] && sockmap[ei][0] == fd && isefd[ei])
							efd2 = 1;
					fprint(pf, "%d %lux", efd2 ? 2 : 0, packed);
					close(pf);
				}
			}
		}
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

/* guest file reads come 512 bytes at a time (freetype's buffer) and
 * each ufs-mediated read costs about a second - a 760KB font took half
 * an hour.  Serve plain-file reads from a small read-ahead cache. */
#define NRA 8
struct {
	int fd;
	int len;
	int off;
	uchar buf[65536];
} ratab[NRA];

static long
fileread(int fd, void *buf, long n)
{
	int i;

	for(i = 0; i < NRA; i++)
		if(ratab[i].fd == fd && ratab[i].off < ratab[i].len)
			break;
	if(i < NRA){
		long b;

		b = n < (long)(ratab[i].len - ratab[i].off) ? n : ratab[i].len - ratab[i].off;
		memmove(buf, ratab[i].buf + ratab[i].off, b);
		ratab[i].off += b;
		if(ratab[i].off >= ratab[i].len)
			ratab[i].fd = -1;
		return b;
	}
	/* miss: fill the emptiest slot with up to 64KB */
	for(i = 0; i < NRA; i++)
		if(ratab[i].fd == -1 || ratab[i].off >= ratab[i].len)
			break;
	if(i >= NRA)
		return read(fd, buf, n);
	ratab[i].fd = fd;
	ratab[i].off = 0;
	ratab[i].len = read(fd, ratab[i].buf, sizeof ratab[i].buf);
	if(ratab[i].len <= 0){
		ratab[i].fd = -1;
		return ratab[i].len;
	}
	{
		long b;

		b = n < (long)ratab[i].len ? n : (long)ratab[i].len;
		memmove(buf, ratab[i].buf, b);
		ratab[i].off = b;
		if(ratab[i].off >= ratab[i].len)
			ratab[i].fd = -1;
		return b;
	}
}

static int
fileqlen(int fd)
{
	Dir *d;
	int q;

	q = -1;
	if((d = dirfstat(fd)) != nil){
		q = d->length;
		free(d);
	}
	return q;
}

/* Linux: write/send on a socket that was never connected (or whose
 * connect was refused) fails with ENOTCONN immediately.  Our unconnected
 * slots carry packed==0, whose low half is host fd 0 - writing there
 * pushed gdbus retry data into stdin and could block on the console.
 * Callers must consult this before sockwr. */
static int
sockunconn(int fd)
{
	int i;

	i = sockslot(fd);
	return i >= 0 && sockmap[i][2] == 0;
}

/* the alarm note guards the 1-byte EOF probe below.  If the note
 * lands between alarm(1) and the read entering the kernel, the read
 * would block forever on a live writer - so re-arm while a probe is
 * in flight: a second note interrupts even that late-blocked read.
 * Re-arming unconditionally would storm: every re-arm fires again
 * one millisecond later, forever. */
static int probing;

static int
alarmnote(void *v, char *msg)
{
	Ureg *ur;

	if(msg == nil)
		return 0;
	if(strcmp(msg, "alarm") == 0){
		if(probing)
			alarm(1);
		return 1;
	}
	if(strcmp(msg, "sample") == 0 && started && !forkpending){
		/* the sampling profiler's poke: where is the guest now? */
		static int zs;

		ur = v;
		if(zs++ < 40)
			fprint(2, "linuxrun: SAMPLE p%d pc=%lux ax=%lux sp=%lux bx=%lux ret=%lux\n",
				getpid(), ur->pc, ur->ax, ur->sp, ur->bx,
				ur->sp > 0x10000 ? *(ulong*)ur->sp : 0);
		return 1;
	}
	return 0;
}

/* read through a bridge socket, honoring the guest's O_NONBLOCK:
 * an empty pipe must give EAGAIN immediately or XCB's nonblocking
 * recv wedges the connection forever - but an empty pipe whose
 * writer is gone must report EOF (0), or a dropped connection
 * looks like "no data yet" forever and the client spins on
 * recvmsg-EAGAIN.  A nonblocking stat cannot see the far end, so
 * an empty-and-closed pipe is probed with a 1-byte read under a
 * 1ms alarm: interrupted means a live writer (EAGAIN), 0 means EOF */
static long
sockread(int gfd, void *buf, ulong n)
{
	int i, slot;
	ulong b;

	i = sockslot(gfd);
	{
		/* who consumes bridge sockets: the xfwm4 spin has one
		 * thread polling a readable fd while the bytes vanish
		 * between check and drain - the reader's pid names the
		 * thread eating the connection */
		static int zr;

		if(i >= 0 && zr++ % 50 == 0 && zr < 3000)
			fprint(2, "linuxrun: RLOG p%d fd=%d inq=%lud qlen=%lud\n",
				getpid(), (int)gfd, inqn[i], sockrawqlen(i));
	}
	if(i >= 0 && inqn[i] > 0){
		ulong b;

		b = n < (long)inqn[i] ? n : inqn[i];
		memmove(buf, inq[i], b);
		memmove(inq[i], inq[i]+b, inqn[i]-b);
		inqn[i] -= b;
		sockopc(i, buf, b, 0);
		return b;
	}
	slot = i;
	if(i >= 0 && soeof[i])
		return 0;
	if(i >= 0 && sockmap[i][3] && sockinready(gfd) <= 0){
		/* Plain EAGAIN for an empty nonblocking read: the old
		 * alarm-probe variant interrupted its own blocked
		 * 1-byte read and the resume at the ld.so stub's ret
		 * trapped invalid-opcode, KILLING the caller (the WM
		 * died exactly here after an EAGAIN recvmsg - every
		 * retry ended this way).  EOF is still detected by the
		 * blocking path below and the write-end checks in
		 * poll/epoll. */
		return -11;
	}
	{
		static int zrc;

		/* only the reads that will actually block matter: an empty
		 * socket read with no EOF is the mutual-wait signature */
		if(zrc++ < 100 && i >= 0 && sockmap[i][2] != 0 && sockrawqlen(i) == 0 && !soeof[i])
			fprint(2, "linuxrun: RDBLK p%d gfd=%d slot=%d packed=%lux eof=%d\n",
				getpid(), (int)gfd, i, sockmap[i][2], soeof[i]);
		else if(zrc < 40 && i < 0)
			fprint(2, "linuxrun: RDFILE p%d fd=%d n=%lud qlen=%d\n",
				getpid(), (int)gfd, n, fileqlen(gfd));
	}
	i = read(sockreadfd(gfd), buf, n);
	if(i > 0)
		sockopc(sockslot(gfd), buf, i, 0);
	if(i > 0 && sockslot(gfd) >= 0 && sockslot(gfd) < NSOCK &&
	   setupdone[sockslot(gfd)] < 12){
		int rb;

		setupdone[sockslot(gfd)]++;
		fprint(2, "linuxrun: RD10 p%d g%d n=%d bytes:",
			getpid(), sockmap[sockslot(gfd)][0], (int)i);
		for(rb = 0; rb < 16 && rb < i; rb++)
			fprint(2, " %2.2ux", ((uchar*)buf)[rb]);
		fprint(2, "\n");
	}
	/* GetInputFocus reply: 01 00 seq2 revert-to(1) pad focus(4)@8.
	 * Xvfb answers focus=None when no WM has set focus yet; real
	 * servers commonly hand back PointerRoot here, and gdk's
	 * focus-tracking treats None badly downstream (xfwm4 queried
	 * attributes of window None and stalled).  Rewrite None ->
	 * PointerRoot (1), which gdk handles cleanly. */
	if(i == 32 && ((uchar*)buf)[0] == 1 && gifseq[slot] != 0 &&
	   (((uchar*)buf)[2] | (((uchar*)buf)[3]<<8)) == (gifseq[slot] & 0xffff) &&
	   ((ulong*)buf)[2] == 0 && ((ulong*)buf)[3] == 0){
		/* GetInputFocus reply (seq-matched): focus None ->
		 * PointerRoot - gdk's focus tracking treats None badly
		 * (xfwm4 queried attributes of window None and its
		 * startup stalled before the tree scan) */
		((uchar*)buf)[11] = 1;
		gifseq[slot] = 0;
	}
	if(i == 0 && sockslot(gfd) >= 0)
		soeof[sockslot(gfd)] = 1;	/* peer closed */
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
	{
		static int zp;
		Dir *d;

		d = dirfstat(fd);
		if(zp++ < 12)
			fprint(2, "linuxrun: POST p%d %s fd=%d q=%llux.%lux\n",
				getpid(), name, fd,
				d ? (vlong)d->qid.path : 0, d ? (ulong)d->qid.vers : 0);
		free(d);
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
		static int za;

		if(za++ < 20)
			fprint(2, "linuxrun: BINDABS p%d\n", getpid());
		return 0;
	}
	strncpy(boundpath, (char*)path, sizeof boundpath - 1);
	boundpath[sizeof boundpath - 1] = 0;
	{
		static int zb;

		if(zb++ < 20)
			fprint(2, "linuxrun: BIND p%d path=%s\n",
				getpid(), boundpath);
	}
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

/* per-client connection counter: a second XOpenDisplay from the same
 * pid (glvnd/mesa opens one, at-spi another) must queue a second
 * ticket, not overwrite the first */
static int connseq;
int forktrace;	/* >0: this fork-snapshot child traces its next syscalls */

static long
sysconnect(ulong path)
{
	char buf[512], line[128];
	int sfd, c2s[2], s2c[2];
	char target[128];

	{
		static int zsq;

		if(zsq++ < 400 && path != 0){
			int sb;

			fprint(2, "linuxrun: CONN p%d addr:", getpid());
			for(sb = 0; sb < 24; sb++)
				fprint(2, " %2.2ux", ((uchar*)path)[sb]);
			fprint(2, "\n");
		}
	}
	if(path == 0)
		return -Efault;
	if(((uchar*)path)[0] == 0)	/* abstract sockets: no pathname;
					 * ECONNREFUSED makes Xlib fall
					 * back to the pathname socket */
		return -111;
	/* the .req file existing is the server's bind mark */
	snprint(buf, sizeof buf, "%s.req", (char*)path);
	if(access(buf, AEXIST) < 0){
		static int zc;

		if(zc++ < 400)
			fprint(2, "linuxrun: CONNECT p%d refused %s (no .req)\n",
				getpid(), (char*)path);
		return -Enoent;
	}
	if(pipe(c2s) < 0 || pipe(s2c) < 0){
		static int zp1;

		if(zp1++ < 30)
			fprint(2, "linuxrun: CONNFPIP p%d %s: pipe: %r\n",
				getpid(), (char*)path);
		return -Enomem;
	}
	/* server reads what we write: publish c2s[0]; it writes back on
	 * s2c[1].  We keep c2s[1] (write) and s2c[0] (read). */
	snprint(target, sizeof target, "/srv/x.c.%d.%d.a", getpid(), connseq);
	if(postsrvfd(target, c2s[0]) < 0){
		static int zp2;

		if(zp2++ < 30)
			fprint(2, "linuxrun: CONNFPST p%d %s: postsrvfd a: %r\n",
				getpid(), target);
		return -Enomem;
	}
	snprint(target, sizeof target, "/srv/x.c.%d.%d.b", getpid(), connseq);
	if(postsrvfd(target, s2c[1]) < 0){
		static int zp3;

		if(zp3++ < 30)
			fprint(2, "linuxrun: CONNFPST p%d %s: postsrvfd b: %r\n",
				getpid(), target);
		return -Enomem;
	}
	/* the target socket marker: NOT in /srv - devsrv files are
	 * fd-sharing entries, readers get a dup of the writer's fd
	 * instead of the content.  The marker sits beside the server's
	 * .req file (a plain rootfs file); its content is the client's
	 * s2c[1] fd number, which the acceptor opens through
	 * /proc/<pid>/fd/N - the /srv OWRITE open provably does NOT
	 * share the pipe channel (qid mismatch, replies vanished) */
	{
		int tf;
		char mbuf[384];

		snprint(mbuf, sizeof mbuf, "%s.t.%d.%d", (char*)path, getpid(), connseq);
		tf = create(mbuf, OWRITE|OTRUNC, 0666);
		if(tf >= 0){
			fprint(tf, "%d", s2c[1]);
			close(tf);
		}
	}
	{
		static int zk;
		Dir *dc, *dd;

		/* pipe identity: qid of the posted ends (.a=c2s[0],
		 * .b=s2c[1]) - the acceptor prints its opened ends'
		 * qids; a mismatch proves the /srv open produced a
		 * copy instead of sharing the channel */
		dc = dirfstat(c2s[0]);
		dd = dirfstat(s2c[1]);
		if(zk++ < 400)
			fprint(2, "linuxrun: CONNECT p%d queued %s a=%llux.%lux b=%llux.%lux\n",
				getpid(), (char*)path,
				dc ? (vlong)dc->qid.path : 0, dc ? (ulong)dc->qid.vers : 0,
				dd ? (vlong)dd->qid.path : 0, dd ? (ulong)dd->qid.vers : 0);
		free(dc);
		free(dd);
	}
	connseq++;
	/* the two /srv posts ARE the queue: the (nonblocking)
	 * accept scans /srv for pending x.c.<pid>.<seq>.a entries */
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


static long
sysaccept(void)
{
	static char buf[524288];
	char target[64];
	int fd, n, cpid, cseq, rf, wf;
	char *p;

	if(boundpath[0] == 0){
		static int ze;

		if(ze++ < 4)
			fprint(2, "linuxrun: ACCENTRY p%d noboundpath\n", getpid());
		return -Ebadf;
	}
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
	{
		static int zd;

		if(zd++ < 4)
			fprint(2, "linuxrun: ACCLIST p%d mine=%s srv=%.*s\n",
				getpid(), boundpath,
				n < 200 ? n : 200, buf);
	}
	/* the marshaled directory stream carries names as plain
	 * strings: look for x.c.<pid>.<seq>.a with its .b twin */
	for(p = memfind(buf, n, "x.c."); p != nil; p = memfind(p+4, n-(int)(p+4-buf), "x.c.")){
		char *q;
		int s2;

		cpid = strtol(p+4, &q, 10);
		if(cpid <= 0 || q[0] != '.')
			continue;
		s2 = strtol(q+1, &q, 10);
		cseq = s2;
		if(s2 < 0 || q[0] != '.' || q[1] != 'a')
			continue;	/* only the .a twin queues */
		/* only tickets aimed at OUR socket: without this check any
		 * listener ate any pending connect (the dbus daemon stole
		 * the X server's clients).  The client drops a plain-file
		 * marker beside its target: <path>.t.<pid>.<seq> */
		{
			char mpath[384], mbuf[64];
			int ok, mf, mn, cwfd;

			snprint(mpath, sizeof mpath, "%s.t.%d.%d", boundpath, cpid, cseq);
			ok = 0;
			cwfd = -1;
			if((mf = open(mpath, OREAD)) >= 0){
				mn = readn(mf, mbuf, sizeof mbuf-1);
				close(mf);
				if(mn > 0){
					mbuf[mn] = 0;
					cwfd = strtol(mbuf, nil, 10);
					ok = cwfd > 2;
				}
			}
			if(!ok){
				static int zm;

				if(zm++ < 8)
					fprint(2, "linuxrun: TKSKIP p%d want=%s\n",
						getpid(), mpath);
				continue;
			}
			remove(mpath);	/* consume with the ticket */
		}
		snprint(target, sizeof target, "x.c.%d.%d.b", cpid, cseq);
		if(memfind(buf, n, target) == nil)
			continue;
		snprint(target, sizeof target, "/srv/x.c.%d.%d.a", cpid, cseq);
		rf = open(target, OREAD);
		snprint(target, sizeof target, "/srv/x.c.%d.%d.b", cpid, cseq);
		wf = open(target, OWRITE);
		if(rf < 0 || wf < 0){
			static int zaf;

			/* broken ticket: consume it, else the listener stays
			 * ready forever and the accept loop spins */
			if(zaf++ < 20)
				fprint(2, "linuxrun: TKCONSUME p%d client=%d seq=%d rf=%d wf=%d\n",
					getpid(), cpid, cseq, rf, wf);
			snprint(target, sizeof target, "/srv/x.c.%d.%d.a", cpid, cseq);
			remove(target);
			snprint(target, sizeof target, "/srv/x.c.%d.%d.b", cpid, cseq);
			remove(target);
			continue;
		}
		/* consume the ticket: entries are removed once served, so
		 * the same client can queue its next connection and stale
		 * posts never keep the listener falsely ready */
		snprint(target, sizeof target, "/srv/x.c.%d.%d.a", cpid, cseq);
		remove(target);
		snprint(target, sizeof target, "/srv/x.c.%d.%d.b", cpid, cseq);
		remove(target);
		{
			static int za;
			Dir *dr, *dw;

			/* the server<->client map: which guest proc is on
			 * which bridge pair, so reply writes can be traced
			 * to the client they belong to.  qids of the opened
			 * ends match the CONNECT print's - mismatch = the
			 * /srv open copied instead of shared */
			dr = dirfstat(rf);
			dw = dirfstat(wf);
			if(za++ < 40)
				fprint(2, "linuxrun: ACCEPT p%d client=%d seq=%d rf=%d wf=%d r=%llux.%lux w=%llux.%lux\n",
					getpid(), cpid, cseq, rf, wf,
					dr ? (vlong)dr->qid.path : 0, dr ? (ulong)dr->qid.vers : 0,
					dw ? (vlong)dw->qid.path : 0, dw ? (ulong)dw->qid.vers : 0);
			free(dr);
			free(dw);
		}
		return (rf<<16) | wf;
	}
	return -11;	/* -EAGAIN */
}

/* true when a client's pipe ends sit queued in /srv and no accept
 * has served them yet: accept removes the entries it serves, so a
 * pending ticket here means a genuinely waiting client */
static int
listenqueued(void)
{
	static char buf[524288];
	char *p, *q;
	int fd, n, cpid;

	if(nlis == 0)
		return 0;
	fd = open("/srv", OREAD);
	if(fd < 0)
		return 0;
	n = readn(fd, buf, sizeof buf-1);
	close(fd);
	if(n <= 0)
		return 0;
	/* garbage-collect stale connect tickets: unaccepted ones (server
	 * wedged or died) and the markers of dead generations pile up and
	 * pushed new tickets past the directory-read buffer, so listeners
	 * stopped seeing fresh connects (Xvfb never accepted xfwm4's
	 * third connection; its write then blocked on the full pipe) */
	{
		static vlong lastgc;

		if(nsec() - lastgc > 30LL*1000*1000*1000){
			char *g;
			char gpath[96];

			lastgc = nsec();
			for(g = memfind(buf, n, "x.c."); g != nil; g = memfind(g+4, n-(int)(g+4-buf), "x.c.")){
				char *q2;
				int cp2, sp2;
				Dir *gd;
				vlong age;

				cp2 = strtol(g+4, &q2, 10);
				if(cp2 <= 0 || q2[0] != '.')
					continue;
				sp2 = strtol(q2+1, &q2, 10);
				if(sp2 < 0 || q2[0] != '.')
					continue;
				/* still pending (both twins present)? keep it */
				{
					char twin[64];

					snprint(twin, sizeof twin, "x.c.%d.%d.b", cp2, sp2);
					if(memfind(buf, n, twin) != nil)
						continue;
				}
				snprint(gpath, sizeof gpath, "/srv/x.c.%d.%d.a", cp2, sp2);
				if((gd = dirstat(gpath)) != nil){
					age = nsec() - gd->mtime*1000000LL;
					free(gd);
					if(age > 300LL*1000*1000*1000){
						remove(gpath);
						snprint(gpath, sizeof gpath, "/srv/x.c.%d.%d.b", cp2, sp2);
						remove(gpath);
					}
				}
			}
		}
	}
	for(p = memfind(buf, n, "x.c."); p != nil; p = memfind(p+4, n-(int)(p+4-buf), "x.c.")){
		char tgt[64];
		int s2, tf, tn, ok;

		cpid = strtol(p+4, &q, 10);
		if(cpid <= 0 || q[0] != '.')
			continue;
		s2 = strtol(q+1, &q, 10);
		if(s2 < 0 || q[0] != '.' || q[1] != 'a')
			continue;
		snprint(tgt, sizeof tgt, "x.c.%d.%d.b", cpid, s2);
		if(memfind(buf, n, tgt) == nil)
			continue;
		/* only tickets aimed at this listener: the client's
		 * <path>.t.<pid>.<seq> marker beside our boundpath.
		 * A ticket from a DEAD client (crashed mid-connect) keeps
		 * the listener permanently ready and the server spins in
		 * failing accepts, never dispatching anything - dbus-daemon
		 * wedged exactly so, starving every main-loop client.  Reap
		 * the ticket and its marker instead of reporting it. */
		{
			char mpath[384];
			char ppath[64];

			snprint(ppath, sizeof ppath, "/proc/%d", cpid);
			if(access(ppath, AEXIST) < 0){
				char t2[64];

				snprint(t2, sizeof t2, "/srv/x.c.%d.%d.a", cpid, s2);
				remove(t2);
				snprint(t2, sizeof t2, "/srv/x.c.%d.%d.b", cpid, s2);
				remove(t2);
				snprint(mpath, sizeof mpath, "%s.t.%d.%d", boundpath, cpid, s2);
				remove(mpath);
				fprint(2, "linuxrun: TKREAP p%d dead client %d seq %d\n",
					getpid(), cpid, s2);
				continue;
			}
			snprint(mpath, sizeof mpath, "%s.t.%d.%d", boundpath, cpid, s2);
			if(access(mpath, AEXIST) >= 0){
				static int zlq;

				if(zlq++ < 60)
					fprint(2, "linuxrun: LQREADY p%d client=%d seq=%d\n",
						getpid(), cpid, s2);
				return 1;
			}
		}
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
	if(inqn[i] > 0)
		return 1;
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
		r = socknewslot(0, 0);
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
			if(i >= 0){
				static int zc3;

				if(zc3++ < 60)
					fprint(2, "linuxrun: CSTORE p%d gfd=%lux slot=%d packed=%lux\n",
						getpid(), a[0], i, r);
				isefd[i] = 0;
				/* sockmapfd updates the slot AND republishes
				 * the x.m markers with the connected pipes:
				 * the bare store left them at socket()-time
				 * packed=0 (or absent), starving adopters */
				sockmapfd(a[0], r);
			}
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
			 * whether a nonblocking connect finished.
			 * SO_PEERCRED (opt 17) needs real ucred {pid,uid,gid}:
			 * dbus's EXTERNAL SASL auth validates the peer uid
			 * through it and timed the connection out when the
			 * handshake never completed */
		if(a[2] == 17 && a[3] > 0x10000){
			static int zp;

			*(int*)a[3] = guestprocid ? (int)guestprocid : getpid();
			*(int*)(a[3]+4) = 0;
			*(int*)(a[3]+8) = 0;
			if(a[4] > 0x10000)
				*(int*)a[4] = 12;
			if(zp++ < 200)
				fprint(2, "linuxrun: PEERCRED p%d fd=%d -> pid=%d uid=0\n",
					getpid(), (int)a[0], *(int*)a[3]);
			r = 0;
			break;
		}
		if(a[3] > 0x10000)
			*(int*)a[3] = 0;
		if(a[4] > 0x10000)
			*(int*)a[4] = 4;
		r = 0;
		break;
	case 5:		/* accept */
		r = sysaccept();
		if(r >= 0){
			/* ALWAYS register: the packed pair is (rf<<16)|wf
			 * and rf can be 0 when the acceptor's stdin was
			 * closed - the old >= 0x10000 guard then skipped
			 * registration and the connection stayed forever
			 * unreadable (dbus-daemon never read xfwm4's
			 * connection: THE wedge) */
			int ns;

			ns = socknewslot(r, 0);
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
		{
			int sslot;

			sslot = sockslot((int)a[0]);
			if(sslot >= 0 && sockmap[sslot][2] == 0){
				static int zu2;

				if(zu2++ < 20)
					fprint(2, "linuxrun: WUNCONN p%d fd=%lux\n", getpid(), a[0]);
				r = -107;	/* -ENOTCONN */
				break;
			}
			if(sslot >= 0 && isefd[sslot]){
				r = efdwr(sockwritefd((int)a[0]), (void*)a[1], a[2]);
				if(r != -2)
					break;
			}
			if(sslot >= 0)
				sockpredrain();
			r = sockwr(sockwritefd((int)a[0]), (void*)a[1], a[2]);
		}
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
dofutex(ulong addr, ulong op, ulong val, ulong utime, ulong a5, ulong a6)
{
	int sub;
	{
		static int zo;
		static int zw;

		/* WAIT-side lifecycle only: the wakes are noise; whether a
		 * WAIT ever returns is the worker-thread question */
		if((op & 127) == 0 || (op & 127) == 9){
			if(zw++ < 200)
				fprint(2, "linuxrun: FTXW p%d op=%lux addr=%lux val=%lux to=%lux\n",
					getpid(), op, addr, val, utime);
		}else if(zo++ < 30)
			fprint(2, "linuxrun: FTX p%d op=%lux addr=%lux val=%lux\n",
				getpid(), op, addr, val);
	}

	sub = op & 127;
	switch(sub){
	case 0:		/* WAIT: poll the shared word; waiters re-check */
	case 9:		/* WAIT_BITSET: same, but the timespec is an
			 * ABSOLUTE CLOCK_MONOTONIC deadline */
		{
			vlong deadline, nowv;
			int i;

			/* utime is a guest timespec*; WAIT/WAIT_PRIVATE
			 * carry a RELATIVE timeout, BITSET an absolute
			 * one.  The old code returned ETIMEDOUT
			 * instantly for any timed wait, which turned
			 * glib's gdbus retries and glibc cond timeouts
			 * into zero-delay spin loops. */
			deadline = 0;
			if(utime != 0){
				vlong ts;

				ts = *(int*)utime;	/* nsec() scaled below */
				ts = ((vlong)*(ulong*)utime)*1000000000LL
					+ (vlong)*(ulong*)(utime+4);
				if(sub == 0)
					deadline = nsec() + ts;
				else{
					/* absolute: CLOCK_MONOTONIC */
					nowv = nsec();
					deadline = ts;
					if(deadline <= nowv){
						if(*(int*)addr == (int)val)
							return -110;
						return 0;
					}
				}
			}
			for(i = 0; i < 1000; i++){
				if(*(int*)addr != (int)val)
					return 0;
				if(utime != 0 && nsec() >= deadline)
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
			return 0;
		case 5:		/* WAKE_OP: the waker ALSO performs an atomic
			 * op on a second word - skipping it desyncs glibc's
			 * lowlevellock protocol, so do the op here */
		{
			int oparg, cmparg, newv, old;
			ulong addr2;

			USED(utime);
			addr2 = a5;	/* futex(uaddr1, WAKE_OP, val, val2, uaddr2) */
			oparg = (op >> 12) & 0xfff;
			cmparg = (op >> 24) & 0xfff;
			old = *(int*)addr2;
			switch((op >> 28) & 7){
			case 0: newv = old + oparg; break;
			case 1: newv = old - oparg; break;
			case 2: newv = old | oparg; break;
			case 3: newv = old & oparg; break;
			case 4: newv = old ^ oparg; break;
			case 5: newv = old | (1 << oparg); break;
			case 6: newv = old & ~(1 << oparg); break;
			default: newv = old; break;
			}
			*(int*)addr2 = newv;
			/* wake only when the comparator says so */
			switch(cmparg){
			case 0: return 1;
			case 1: return old == 0;
			case 2: return old != 0;
			case 3: return old < 0;
			case 4: return old <= 0;
			case 5: return old > 0;
			case 6: return old >= 0;
			}
			return 1;
		}
		}
		/* other flavors: report a spurious wakeup, which the
		 * futex protocol explicitly allows callers to tolerate
		 * by re-checking */
		return 0;
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

/* The guest-visible path of the running image, served by readlink on
 * /proc/self/exe; Wine derives its library directory from it. */
static char execpath[1024];

static void
rememberexec(char *path)
{
	if(path == nil || path[0] == 0)
		return;
	strncpy(execpath, path, sizeof execpath-1);
	execpath[sizeof execpath-1] = 0;
}

/* Rewrite /proc/self to the numeric directory: the native /proc has no
 * self entry, and realpath(/proc/self/exe) probes every component. */
static char*
fixproc(char *buf, int n, char *path)
{
	if(path == nil)
		return nil;
	if(strncmp(path, "/proc/self", 10) == 0 &&
	   (path[10] == 0 || path[10] == '/')){
		snprint(buf, n, "/proc/%d%s", getpid(), path+10);
		return buf;
	}
	return path;
}

/* readlink for the paths programs actually use; ordinary namespace files
 * keep the realpath-compatible EINVAL answer. */
static long
sysreadlink(char *path, char *buf, ulong bufsz)
{
	char fb[1024];
	char *t, *rest;
	int n;

	if(path == nil || buf == nil)
		return -Efault;
	t = nil;
	if(strcmp(path, "/proc/self/exe") == 0)
		t = execpath;
	else if(strncmp(path, "/proc/", 6) == 0){
		rest = path+6;
		while(*rest >= '0' && *rest <= '9')
			rest++;
		if(strcmp(rest, "/exe") == 0)
			t = execpath;
	}
	/* Only an absolute path is valid in the guest view; relative native
	 * invocations keep the old answer so multi-call binaries like
	 * busybox fall back to argv[0] as before. */
	if(t != nil && t[0] == '/'){
		n = strlen(t);
		if(n > bufsz)
			n = bufsz;
		memmove(buf, t, n);
		return n;
	}
	return access(fixproc(fb, sizeof fb, path), AEXIST) < 0 ? -Enoent : -Einval;
}

static long
sysexecve(char *path, char **gargv)
{
	int fd, i, j;
	Ehdr eh;
	uchar hdr[64];
	static int zx;

	if(zx++ < 60)
		fprint(2, "linuxrun: EXEC p%d %s (started=%d)\n",
			getpid(), path ? path : "?", started);
	if(path == nil || path[0] == 0)
		return -Efault;
	fd = open(path, OREAD);
	if(fd < 0){
		if(zx < 60)
			fprint(2, "linuxrun: EXECFAIL p%d %s: open: %r\n",
				getpid(), path);
		return -Enoent;
	}
	if(readat(fd, hdr, sizeof hdr, 0) < 0){
		close(fd);
		fprint(2, "linuxrun: EXECFAIL p%d %s: read: %r\n", getpid(), path);
		return -Enoent;
	}
	memset(&eh, 0, sizeof eh);
	memmove(eh.ident, hdr, Elfident);
	if(eh.ident[EiClass] != Elfclass32 || eh.ident[EiData] != Elfdata2lsb){
		close(fd);
		fprint(2, "linuxrun: EXECFAIL p%d %s: not i386 ELF\n", getpid(), path);
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
	rememberexec(path);
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
	/* AT_PHDR is a mapped address, including for non-PIE programs.
	 * Supplying the raw file offset made static libc fault at 0x34
	 * immediately after execve. */
	phdrva = 0;
	for(i = 0; i < nph; i++)
		if(ph[i].type == PtLoad && eh.phoff >= ph[i].offset &&
		   eh.phoff < ph[i].offset+ph[i].filesz){
			phdrva = ph[i].vaddr+eh.phoff-ph[i].offset;
			break;
		}
	close(fd);
	if(dynamic){
		int ifd;
		Ehdr ieh;
		uchar ihdr[64];

		if(interppath[0] == 0){
			fprint(2, "linuxrun: EXECFAIL p%d %s: no interp\n", getpid(), path);
			return -Enoexec;
		}
		ifd = open(interppath, OREAD);
		if(ifd < 0){
			fprint(2, "linuxrun: EXECFAIL p%d %s: interp %s: %r\n",
				getpid(), path, interppath);
			return -Enoent;
		}
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
	brkcur = Brkbase;
	segat(Mapbase, Mapsize);
	stacktop = buildstack(countargs(gargv), gargv);
	{
		static int zok;

		if(zok++ < 60)
			fprint(2, "linuxrun: EXECOK p%d %s entry=%lux\n",
				getpid(), path, entrypc);
	}
	return 0;
}

static long
dosyscall(Ureg *ur)
{
	ulong nr, a1, a2, a3, a4, a5, a6;
	long r;

	nr = ur->ax;
	a1 = ur->bx;
	a2 = ur->cx;
	a3 = ur->dx;
	a4 = ur->si;
	a5 = ur->di;
	a6 = ur->bp;
	r = -Enosys;
	{
		static int rainit;

		if(!rainit){
			int ri;

			rainit = 1;
			for(ri = 0; ri < NRA; ri++)
				ratab[ri].fd = -1;
		}
	}
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
				if(a || b){
					int q;
					int bsys;

					fprint(2, "linuxrun: FLOW p%d g%d wr=%d rd=%d lastsys=%lux inq=%lud pipe=%lud\n",
						getpid(), sockmap[si][0], a, b,
						sysri ? sysring[(sysri-1)%32][0] : 0,
						inqn[si], sockrawqlen(si));
					fprint(2, "linuxrun: TAIL p%d pc=%lux bp=%lux", getpid(), ur->pc, ur->bp);
					for(q = 16; q > 0; q--){
						if(sysri > q)
							bsys = sysring[(sysri-q)%32][0];
						else
							continue;
						fprint(2, " %lux", bsys);
					}
					fprint(2, "\n");
				}
			}
		}
	}
	{
		/* enter-trace: a process stuck in ONE blocking host call
		 * produces no further output of any kind - the last ENT
		 * line before the silence names the culprit syscall.
		 * Loader noise is excluded or the cap dies in
		 * library loading.  Fork-snapshot children trace EVERY
		 * syscall uncapped for their first moments (forktrace). */
		static int zent;

		if(forktrace > 0){
			forktrace--;
			fprint(2, "linuxrun: FT p%d nr=%lux a1=%lux a2=%lux a3=%lux\n",
				getpid(), nr, a1, a2, a3);
		}else if(zent++ < 3000 &&
		   (nr == 3 || nr == 4 || nr == 145 || nr == 146 ||
		    nr == 168 || nr == 240 || nr == 422 || nr == 102))
			fprint(2, "linuxrun: ENT p%d nr=%lux a1=%lux a2=%lux\n",
				getpid(), nr, a1, a2);
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
		if((a1 & 0xff) == 127 && started){
			/* posix_spawn children _exit(127) when their
			 * userspace pre-exec fails - dump the caller so
			 * the failing branch can be symbolized */
			ulong *stk;
			int i;

			fprint(2, "linuxrun: EXIT127 p%d pc=%lux sp=%lux bp=%lux\n",
				getpid(), ur->pc, ur->sp, ur->bp);
			fprint(2, "linuxrun: ax=%lux bx=%lux cx=%lux dx=%lux si=%lux di=%lux\n",
				ur->ax, ur->bx, ur->cx, ur->dx, ur->si, ur->di);
			for(i = 0; i < 32; i++){
				int ri;

				ri = (sysri+i) % 32;
				if(sysring[ri][0] != 0)
					fprint(2, "linuxrun:  sys-%d nr=%lux a1=%lux a2=%lux a3=%lux\n",
						i, sysring[ri][0], sysring[ri][1], sysring[ri][2], sysring[ri][3]);
			}
			if(ur->sp > 0x10000 && ur->sp < 0x70000000 &&
			   guestok(ur->sp, 64)){
				stk = (ulong*)ur->sp;
				for(i = 0; i < 16; i++)
					fprint(2, "linuxrun:  sp+%d = %lux\n", i*4, stk[i]);
			}
		}
		snprint(exitstr, sizeof exitstr, "%lux", a1 & 0xff);
		exits(exitstr);
		return 0;
	case 3:		/* read */
		if(sockslot((int)a1) < 0){
			r = fileread((int)a1, (void*)a2, a3);
			if(r < 0)
				r = -Ebadf;
		}else{
			r = sockread((int)a1, (void*)a2, a3);
			if(r < 0 && r != -11)	/* keep EAGAIN */
				r = -Ebadf;
		}
		break;
	/* NB: writes through bridge sockets can block the whole
	 * emulator when the pipe is full; with the X server mid-reply
	 * and a client mid-write this deadlocks both (traced: xfwm4
	 * freezes after its extension queries).  Three fixes were
	 * built, tested and reverted: qlen-capped partial writes (a
	 * writer cannot stat the opposite end's queue),
	 * alarm-interrupted writes (the alarm note derails the nested
	 * syscall-note handling), and per-socket output buffering
	 * flushed at the wait loops (the flush write itself blocks,
	 * and server and client mid-flush wedge each other exactly as
	 * before - plan9 pipes offer no nonblocking write at all).
	 * The remaining honest fix is to move the bridge transport
	 * off pipes entirely: a shared-memory ring that both endpoint
	 * processes can poll and drain without ever blocking. */
	case 4:		/* write */
		{
			int wslot;

			wslot = sockslot((int)a1);
			if(wslot < 0){
				static int zrw;
				char rb[64];
				int rkey;

				rkey = guestprocid ? (int)guestprocid : getpid();
				snprint(rb, sizeof rb, "/srv/x.m.%d.%d.f", rkey, (int)a1);
				if(access(rb, AEXIST) >= 0 && zrw++ < 40)
					fprint(2, "linuxrun: RAWW p%d fd=%lux (marker exists, not adopted)\n",
						getpid(), a1);
			}
			if(wslot >= 0 && sockmap[wslot][2] == 0){
				static int zu;

				if(zu++ < 20)
					fprint(2, "linuxrun: WUNCONN p%d fd=%lux\n", getpid(), a1);
				r = -107;	/* -ENOTCONN */
				break;
			}
			if(wslot >= 0 && isefd[wslot]){
				r = efdwr(sockwritefd((int)a1), (void*)a2, a3);
				if(r != -2)
					break;
			}
			if(wslot >= 0)
				sockpredrain();
			r = sockwr(sockwritefd((int)a1), (void*)a2, a3);
		}
		if(r > 0)
			sockopc(sockslot((int)a1), (void*)a2, r, 1);
		if(r < 0)
			r = -Ebadf;
		{
			/* the WM's last act before idling is a write to
			 * stderr whose content went unlogged - it names
			 * what stopped it before the tree scan */
			static int zs;

			if(zs++ < 60 && (a1 == 2 || a1 == 1 || a1 == 0xb) && a3 < 512){
				int sb;

				fprint(2, "linuxrun: ERRWR p%d fd=%lux:", getpid(), a1);
				for(sb = 0; sb < a3; sb++){
					uchar c = ((uchar*)a2)[sb];
					if(c >= 0x20 && c < 0x7f)
						fprint(2, "%c", c);
					else
						fprint(2, ".");
				}
				fprint(2, "\n");
			}
		}
		{
			/* partial-write forensics: a request the guest
			 * believes it sent but the pipe never delivered
			 * leaves the WM waiting forever for its reply */
			static int zw;

			if(zw++ < 30 && r != (long)a3)
				fprint(2, "linuxrun: PARTWR p%d fd=%lux want=%lux got=%ld\n",
					getpid(), a1, a3, r);
		}
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
			char fb[1024];
			int sfd;

			sfd = open(fixproc(fb, sizeof fb, (char*)a2), OREAD);
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
			int ci;
			static int zcl;

			for(ci = 0; ci < NRA; ci++)
				if(ratab[ci].fd == (int)a1)
					ratab[ci].fd = -1;
			if(zcl++ < 150 && (int)a1 >= 8)
				fprint(2, "linuxrun: CLOSE p%d fd=%lux\n", getpid(), a1);
		}
		/* do NOT remove the /srv x.m markers -
		 * they are keyed by the THREAD-GROUP id, so a sibling
		 * thread closing its copy of the fd (or a transient
		 * socket reusing the number) yanked the entries out
		 * from under processes that still needed them for
		 * adoption; their writes then fell through to raw host
		 * fds and froze (ADOPTFAIL 'file does not exist').
		 * Entries leak per session instead - bounded by the
		 * socket count, the 64K scans absorb that */
		{
			int i;

			i = sockslot((int)a1);
			if(i >= 0){
				close(sockmap[i][2] >> 16);
				close(sockmap[i][2] & 0xffff);
				sockmap[i][1] = 0;
				free(inq[i]);
				inq[i] = nil;
				inqn[i] = 0;
				inqcap[i] = 0;
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
		{
			int qi;

			for(qi = 0; qi < NRA; qi++)
				if(ratab[qi].fd == (int)a1)
					ratab[qi].fd = -1;
		}
		r = seek((int)a1, a2, a3);
		break;
	case 158:	/* sched_yield: glvnd's glX entry drain-wait yields in a
		 * loop; the caller ignores the result, but ENOSYS prints
		 * noise and looks like a missing feature */
		r = 0;
		break;
	case 20:	/* getpid: stable across clone threads (see guestprocid) */
		if(guestprocid == 0){
			guestprocid = getpid();
			fprint(2, "linuxrun: PIDINIT-LAZY p%d guest=%lux\n",
				getpid(), guestprocid);
		}
		if(guestprocid == 106){
			static int zw;

			if(zw++ < 10)
				fprint(2, "linuxrun: WHO106 p%d guest=%lux first=%d\n",
					getpid(), guestprocid, started);
		}
		r = guestprocid;
		break;
	case 224:	/* gettid: unique per thread == our host pid */
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
	case 341:	/* prlimit64(pid, resource, new_rlim, old_rlim): dbus-daemon
		 * reads its fd limit through this.  The rlimit struct on
		 * i386 holds TWO 64-bit fields (cur at +0, max at +8) -
		 * filling only the first 8 bytes left rlim_max garbage and
		 * dbus rejected its own limit ("Invalid argument") */
		if(a4 != 0 && a4 > 0x10000){
			ulong v;

			if(a2 == 3)
				v = 8*1024*1024;
			else if(a2 == 7)
				v = 1024;
			else
				v = 0x7fffffff;
			*(ulong*)a4 = v;
			*(ulong*)(a4+4) = 0;
			*(ulong*)(a4+8) = v;
			*(ulong*)(a4+12) = 0;
		}
		r = 0;
		break;
	case 76:	/* getrlimit */
	case 191:	/* ugetrlimit: fill in an infinite rlimit - except
			 * RLIMIT_STACK (resource 3): glibc derives the
			 * default pthread stack size from it, an infinite
			 * limit made pthread_create ask for a 2GB stack
			 * and abort pango's FcInit worker */
		if(a2 != 0){
			if(a1 == 3){
				*(ulong*)a2 = 8*1024*1024;
				*(ulong*)(a2+4) = 8*1024*1024;
			}else if(a1 == 7){
				/* RLIMIT_NOFILE: infinite made glibc's
				 * close-all-fds walk a million numbers
				 * (dbus-daemon burned CPU forever on
				 * --version before ever reaching main) */
				*(ulong*)a2 = 1024;
				*(ulong*)(a2+4) = 1024;
			}else{
				*(ulong*)a2 = 0x7fffffff;
				*(ulong*)(a2+4) = 0x7fffffff;
			}
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
	case 125:	/* mprotect: materialize reserved ranges on demand */
		{
			ulong mlen;

			mlen = ((a2 + Pgsz-1) / Pgsz) * Pgsz;
			if(a3 != 0)
				r = resvmaterialize(a1, mlen);
			else{
				resvdrop(a1, mlen);
				r = 0;
			}
		}
		break;
	case 219:	/* madvise */
	case 172:	/* prctl */
	case 175:	/* rt_sigprocmask */
	case 238:	/* sendfile: not yet */
		r = 0;
		break;
	case 174:	/* rt_sigaction: record the guest's handlers */
		{
			static int zsa;

			if(a1 == 0 || a1 > 64)
				r = -Einval;
			else{
				if(a2 != 0){
					sighandler[a1] = ((ulong*)a2)[0];
					sigrestorer[a1] = ((ulong*)a2)[2];
					if(zsa++ < 30)
						fprint(2, "linuxrun: SIGACTION p%d %lud handler=%lux\n",
							getpid(), a1, sighandler[a1]);
				}
				if(a3 != 0){
					((ulong*)a3)[0] = sighandler[a1];
					((ulong*)a3)[1] = 0;
					((ulong*)a3)[2] = sigrestorer[a1];
				}
				r = 0;
			}
		}
		break;
	case 173:	/* rt_sigreturn: restore the saved context */
		{
			uchar *f;

			f = (uchar*)(ur->sp - 4);
			if(f != nil && ur->sp > 0x10000 && ur->sp < 0x7f000000){
				ulong sc;

				sc = (ulong)f + 144 + 20;
				sigret_di = *(ulong*)(sc+16);
				sigret_si = *(ulong*)(sc+20);
				sigret_bp = *(ulong*)(sc+24);
				sigret_sp = *(ulong*)(sc+28);
				sigret_bx = *(ulong*)(sc+32);
				sigret_dx = *(ulong*)(sc+36);
				sigret_cx = *(ulong*)(sc+40);
				sigret_ax = *(ulong*)(sc+44);
				sigret_pc = *(ulong*)(sc+56);
				sigreturning = 1;
				r = 0;
			}else
				r = -Efault;
		}
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
	case 266:	/* clock_getres(clk_id, &res): 1ns resolution */
		if(a2 < 0x10000)
			r = -Efault;
		else{
			*(ulong*)a2 = 0;
			*(ulong*)(a2+4) = 1;
			r = 0;
		}
		break;
	case 265:	/* clock_gettime */
		{
			vlong t;

			/* guest clocks run faster than real time: glib/dbus
			 * timeouts (25-30s) expire before glacial emulated
			 * roundtrips complete - xfconfd died 'Timeout was
			 * reached' starting up.  All guests scale equally,
			 * so the universe stays consistent; timers just
			 * fire sooner in real time.  Scaled around a
			 * boot-time base: a bare multiply overflows the
			 * epoch-nanosecond magnitude and glib aborts on
			 * the negative 'non-monotonic' clock. */
			{
				static vlong clkbase = -1;

				if(clkbase < 0)
					clkbase = nsec();
				t = clkbase + (nsec()-clkbase)*1;
			}
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
		{
			int i, slot;

			if(!epinit){
				epinit = 1;
				for(i = 0; i < Maxep; i++)
					eptab[i].epfd = -1;
			}
			if(a2 == 2){	/* EPOLL_CTL_DEL: a2 is the op code.
					 * KEEP the entry: libdbus registers
					 * the SAME fd as separate read and
					 * write watches, so the write
					 * watch's DEL right after the read
					 * watch's ADD erased all interest -
					 * dbus-daemon then never serviced
					 * the fresh client and its auth
					 * timeout killed the connection.
					 * Reporting events for a deleted fd
					 * is a (legal) spurious wakeup; the
					 * dispatcher re-checks and moves
					 * on, while a lost read interest
					 * is unrecoverable */
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
			{
				static int z;

				if(z++ < 20)
					fprint(2, "linuxrun: ECTL p%d nr=%lux epfd=%d op=%lux fd=%d\n",
						getpid(), nr, (int)a1, a2, (int)a3);
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
			int i, maxev, n, zi;

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
				if(islistener((int)eptab[i].fd)){
					if(!listenqueued()){
						static int z;

						if(z++ < 30)
							fprint(2, "linuxrun: WSKIP p%d lfd=%d not queued\n",
								getpid(), (int)eptab[i].fd);
						continue;
					}
				}
				/* connected bridge sockets: EPOLLIN only
				 * when the pipe holds data, else the
				 * server reads before the client has sent
				 * anything and both ends block.  Readiness
				 * is reported DATA-DRIVEN regardless of
				 * the registered mask: libdbus splits the
				 * same fd into separate read/write watches
				 * and collapses to one entry here, so a
				 * write-only registration would otherwise
				 * hide readable data forever (the daemon
				 * then starved the client and its auth
				 * timeout killed the connection).  Extra
				 * IN events are legal spurious wakeups.
				 * EPOLLOUT is reported only when the
				 * registration asks for it: reporting it
				 * unconditionally wakes every poll of an
				 * IN-only watch and Xorg's main loop
				 * spins at full CPU. */
				evv = eptab[i].events & 0xffffffff;
				slot = sockslot((int)eptab[i].fd);
				if(slot >= 0 && sockmap[slot][2] != 0){
					evv &= 4;	/* EPOLLOUT only if registered */
					if(sockinready((int)eptab[i].fd) > 0)
						evv |= 1;
				}
				if(slot < 0 && (evv & 1)){
					/* a raw fd (pipe() stand-ins are
					 * unregistered): report EPOLLIN
					 * only when the queue really holds
					 * data - assuming always-ready
					 * made sd-event spin onto blocked
					 * reads of its empty wakeup pipes,
					 * freezing dbus-daemon at boot */
					Dir *d;

					if((d = dirfstat((int)eptab[i].fd)) != nil){
						if(d->length <= 0)
							evv &= ~1;
						free(d);
					}
				}
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
			if(n == 0 && a4 != 0){
				/* a blocking or timed wait expects to sleep
				 * until events appear: re-scan after each
				 * slice instead of returning a stale empty
				 * snapshot - data that arrives after the
				 * first scan was invisible to a caller that
				 * blocks in one epoll_wait (timeout-0
				 * callers still return immediately) */
				long tleft;

				tleft = a4;
				while(n == 0){
					sleep(20);
					if(tleft > 0){
						tleft -= 20;
						if(tleft <= 0)
							break;
					}
					/* re-scan */
					n = 0;
					for(zi = 0; zi < Maxep && n < maxev; zi++){
						int evv2, slot2;

						if(eptab[zi].epfd != (int)a1)
							continue;
						if(islistener((int)eptab[zi].fd) && !listenqueued())
							continue;
						evv2 = eptab[zi].events & 0xffffffff;
						slot2 = sockslot((int)eptab[zi].fd);
						if(slot2 >= 0 && sockmap[slot2][2] != 0 &&
						    (evv2 & 1) && sockinready((int)eptab[zi].fd) <= 0)
							evv2 &= ~1;
						if(slot2 < 0 && (evv2 & 1)){
							Dir *d;

							if((d = dirfstat((int)eptab[zi].fd)) != nil){
								if(d->length <= 0)
									evv2 &= ~1;
								free(d);
							}
						}
						evv2 &= 5;
						if(evv2 == 0)
							continue;
						if(ev != nil){
							ev[n*3] = evv2;
							ev[n*3+1] = (ulong)eptab[zi].data;
							ev[n*3+2] = 0;
						}
						n++;
					}
				}
			}
			{
				/* the spin forensics probe: what timeout the
				 * guest asked for, how many events came back,
				 * and what fds are registered on this epfd */
				static int zep;
				int zi, zn;

				if(zep++ % 20 == 0){
					zn = 0;
					fprint(2, "linuxrun: EPW p%d epfd=%d to=%ld n=%d fds:",
						getpid(), (int)a1, (long)a4, n);
					for(zi = 0; zi < Maxep && zn < 10; zi++){
						if(eptab[zi].epfd == (int)a1){
							int qs;

							qs = sockslot((int)eptab[zi].fd);
							fprint(2, " %d(q%lux)",
								(int)eptab[zi].fd,
								qs >= 0 ? sockrawqlen(qs) : 0xffff);
							zn++;
						}
					}
					fprint(2, "\n");
				}
			}
			r = n;
		}
		break;
	case 323:	/* eventfd: stand in as a bridge pipe - the counter
		 * semantics are approximated by the stream, and the
		 * socket map keeps both ends so guest writes reach the
		 * wakeup pipe and poll stays honest */
		{
			int p[2];

			if(pipe(p) < 0)
				r = -Enomem;
			else{
				r = socknewslot((p[0]<<16) | p[1], 1);
			}
		}
		break;
	case 2:		/* fork */
	case 190:	/* vfork: a real copy is a valid implementation */
	case 120:	/* clone: threads share the guest "shared" segments */
		{
			int pid, mfd;
			void (*starter)(void);
			static int zc;

			if(zc++ < 30)
				fprint(2, "linuxrun: CLONE p%d nr=%lux flags=%lux sp=%lux tls=%lux ctid=%lux\n",
					getpid(), nr, a1, a2, a4, a5);

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
				/* segments do not survive rfork: the main map
				 * is re-attached below, the low segment on
				 * demand */
				lowseg = 0;
				/* this post-rfork context can attach (it is
				 * where registernotestack succeeds), unlike
				 * note context; wine's clone children run
				 * low-address code immediately */
				ensurelow();
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
				if(forksnap){
					/* fresh guest process: adopt the new
					 * host pid (CLONE_VM threads keep
					 * the parent's guestprocid copy) */
					guestprocid = getpid();
					fprint(2, "linuxrun: FORKPID p%d guest=%lux\n",
						getpid(), guestprocid);
				}
				if(nr == 120 && a4 != 0){
					/* thread switch: back the requested
					 * stack before the child runs on it */
					ensurestack(a2);
					long rr;

					rr = syssetthreadarea(a4);	/* CLONE_SETTLS:
					 * without a TLS of its own the
					 * thread's first canary access
					 * trips the stack protector */
					fprint(2, "linuxrun: SETTLS p%d udesc=%lux -> %ld\n",
						getpid(), a4, rr);
					if(rr == 0){
						/* kernel CLONE_SETTLS also
						 * loads the new thread's
						 * segment register; glibc
						 * i386 TLS lives in %gs and
						 * nothing else loads it in
						 * the bounced child */
						ur->gs = tlsselector;
					}
				}
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
				fprint(2, "linuxrun: FCHILD p%d bouncing\n", getpid());
				starter = (void(*)(void))trapinsn;
				starter();
				exits("fork child");	/* not reached */
			}else{
				/* hold the shared note stack until the child
				 * has finished on its own.  The wait is a
				 * polled, BOUNDED one: a blocking read here
				 * cannot be interrupted (a swallowed alarm
				 * note makes plan9 resume it), so a child
				 * that dies before releasing would park the
				 * parent forever - the WM died exactly this
				 * way after claiming WM_S0 */
				if(forkready[0] >= 0){
					char b[1];
					int waited;
					static int z;

					close(forkready[1]);
					if(z++ < 10)
						fprint(2, "linuxrun: FPARK p%d waiting forkready\n", getpid());
					waited = 0;
					for(;;){
						int rr;
						Dir *d;

						if((d = dirfstat(forkready[0])) != nil){
							int ql;

							ql = d->length;
							free(d);
							if(ql > 0){
								rr = read(forkready[0], b, 1);
								if(rr == 1)
									break;
							}
						}
						if(++waited > 2400){
							fprint(2, "linuxrun: FPARKTIMEO p%d child=%d lost\n",
								getpid(), pid);
							break;
						}
						sleep(50);
					}
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
			static char *nextenv[Maxenv];
			static char envstr[2][Envbytes];
			static int envslot;
			static char *oldenv[Maxenv];
			ulong *ap;
			int na, j, k;

			/* copy the strings now: sysexecve detaches the old
			 * image (including the guest stack they live on)
			 * before buildstack reads them back.  Wine execs
			 * with argv/env on a 0xdfff-region stack that no
			 * segment covers - back it before reading. */
			j = 0;
			k = 0;
			ensurestack(a2);
			ensurestack(a3);
			ap = (ulong*)a2;
			for(na = 0; na < 255 && ap != nil && ap[na] != 0; na++){
				int m;

				ensurestack(ap[na]);
				m = strlen((char*)ap[na]);
				if(k + m + 1 >= sizeof argstr)
					break;
				strcpy(argstr + k, (char*)ap[na]);
				gargv[na] = argstr + k;
				k += m + 1;
			}
			gargv[na] = nil;
			if((ap != nil && ap[na] != 0) ||
			   copyguestenv((char**)a3, nextenv, envstr[envslot], Envbytes) < 0){
				r = -7; /* E2BIG */
				break;
			}
			if(a1 != 0){
				strncpy(pathbuf, (char*)a1, sizeof pathbuf - 1);
				pathbuf[sizeof pathbuf - 1] = 0;
			}else
				pathbuf[0] = 0;
			memmove(oldenv, genv, sizeof oldenv);
			memmove(genv, nextenv, sizeof nextenv);
			r = sysexecve(pathbuf, gargv);
			if(r != 0)
				memmove(genv, oldenv, sizeof oldenv);
			if(r == 0){
				/* Keep the live host copy intact if the next exec fails. */
				envslot ^= 1;
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
		r = socknewslot(0, 0);
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
			if(i >= 0){
				static int zc4;

				if(zc4++ < 60)
					fprint(2, "linuxrun: CSTORE p%d gfd=%lux slot=%d packed=%lux\n",
						getpid(), a1, i, r);
				isefd[i] = 0;
				sockmapfd(a1, r);
			}
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
			 * worth writing, optlen/a5 gets its size.
			 * SO_PEERCRED (optname a3==17) carries ucred for
			 * dbus's EXTERNAL auth - see the socketcall twin */
		if(a3 == 17 && a4 > 0x10000){
			static int zq;

			*(int*)a4 = guestprocid ? (int)guestprocid : getpid();
			*(int*)(a4+4) = 0;
			*(int*)(a4+8) = 0;
			if(a5 > 0x10000)
				*(int*)a5 = 12;
			if(zq++ < 200)
				fprint(2, "linuxrun: PEERCRED p%d fd=%d -> pid=%d uid=0\n",
					getpid(), (int)a1, *(int*)a4);
			r = 0;
			break;
		}
		if(a4 > 0x10000)
			*(int*)a4 = 0;
		if(a5 > 0x10000)
			*(int*)a5 = 4;
		r = 0;
		break;
	case 364:	/* accept4 (direct): glibc's accept() on i386 */
		r = sysaccept();
		if(r >= 0){
			int ns;

			ns = socknewslot(r, 0);
			if(ns < 0)
				r = -Enomem;
			else
				r = ns;
		}
		{
			static int z;

			if(z++ < 12)
				fprint(2, "linuxrun: ACC4 p%d -> %lux (err=%lux)\n",
					getpid(), (ulong)r, r < 0 ? -r : 0);
		}
		break;
	case 369:	/* sendto (direct) */
		if(sockslot((int)a1) >= 0)
			sockpredrain();
		r = sockwr(sockwritefd((int)a1), (void*)a2, a3);
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
						int q;

						q = sockslot(pf[k].fd);
						if(sockmap[q][2] != 0){
							if(ev & 0x4)		/* POLLOUT */
								re |= 0x4;
							if((ev & 0x1) && sockinready(pf[k].fd) > 0)
								re |= 0x1;	/* POLLIN */
							if(soeof[q])
								re |= 0x11;	/* POLLHUP|POLLIN:
										 * the peer closed -
										 * silence here hung
										 * xcb forever */
						}else if((ev & 0x1) && listenqueued())
							re |= 0x1;	/* the listener */
					}else{
						/* raw fd: POLLOUT stands (pipes
						 * barely block), POLLIN only
						 * when the queue holds data -
						 * always-ready woke empty
						 * wakeup pipes endlessly */
						Dir *d;

						re = ev & ~1;
						if(ev & 0x1){
							if((d = dirfstat(pf[k].fd)) != nil){
								if(d->length > 0)
									re |= 0x1;
								free(d);
							}
						}
					}
					pf[k].revents = re;
					if(re != 0)
						r++;
				}
				if(r > 0 || tleft == 0)
					break;
				{
					/* about to block with nothing ready: dump
					 * the whole poll set once in a while -
					 * a wakeup-pipe fd that never reports
					 * ready is the deadlock signature */
					static int zps;
					int pi;

					if(zps++ % 400 == 0 && zps < 1600){
						fprint(2, "linuxrun: PSET p%d n=%lux:",
							getpid(), a2);
						for(pi = 0; pi < (int)a2 && pi < 12; pi++)
							fprint(2, " %d/%ux/%ux",
								pf[pi].fd, pf[pi].events, pf[pi].revents);
						fprint(2, "\n");
					}
				}
				if(tleft > 0 && tleft <= 20){
					sleep(tleft);
					tleft = 0;
					continue;
				}
				{
					static int zpb;

					/* a poll still empty after its first
					 * slice: name the fds it waits on and
					 * their real state - the WM's final
					 * block lives here somewhere */
					if(zpb++ < 30){
						long k2;

						fprint(2, "linuxrun: PBLOCK p%d to=%ld:",
							getpid(), (long)a3);
						for(k2 = 0; k2 < (long)a2 && k2 < 6; k2++){
							Dir *d;
							ulong ql;
							int qs2;

							ql = 0;
							if((d = dirfstat(pf[k2].fd)) != nil){
								ql = d->length;
								free(d);
							}
							fprint(2, " %d/%ux/q%lux",
								pf[k2].fd, pf[k2].events, ql);
							qs2 = sockslot(pf[k2].fd);
							if(qs2 >= 0){
								Dir *d2;

								fprint(2, " map%lux",
									sockmap[qs2][2]);
								if((d2 = dirfstat(sockmap[qs2][2] >> 16)) != nil){
									fprint(2, "/r%lux", d2->length);
									free(d2);
								}
							}
						}
						fprint(2, "\n");
					}
				}
				sleep(20);
				if(tleft > 0)
					tleft -= 20;
				if(tleft < 0){
					/* infinite poll INCLUDING an eventfd
					 * stand-in: return a spurious 0
					 * after ~500ms.  glib's owner-check
					 * confusion (a thread that wrongly
					 * matches context->owner) means the
					 * wakeup eventfd never gets written;
					 * the parked main loop then never
					 * re-runs prepare and never sees the
					 * attached idle source.  A spurious
					 * return makes it iterate - prepare()
					 * dispatches the idle and the
					 * deadlock breaks.  Restricted to
					 * eventfd polls: a blanket cap broke
					 * the plain-socket SASL exchange. */
					static vlong lastspur;
					vlong now2;
					int efdq, qi;

					efdq = 0;
					for(qi = 0; qi < (long)a2; qi++){
						int qs2;

						qs2 = sockslot(pf[qi].fd);
						if(qs2 >= 0 && isefd[qs2]){
							efdq = 1;
							break;
						}
					}
					now2 = nsec();
					if(efdq && now2 - lastspur > 500LL*1000*1000){
						lastspur = now2;
						r = 0;
						break;
					}
				}
			}
			{
				/* spin forensics: what GTK-style callers are
				 * waiting on and what they got back - plus
				 * pc/bp/stack: a user-mode busy loop sampled
				 * at its poll sites names its glib source */
				static int zp;

				if(zp++ % 400 == 0 && zp < 4000){
					long k2;
					int qi;

					fprint(2, "linuxrun: PLOG p%d n=%ld to=%ld:",
						getpid(), r, (long)a3);
					for(k2 = 0; k2 < (long)a2 && k2 < 6; k2++)
						fprint(2, " %d/%ux/%ux",
							pf[k2].fd, pf[k2].events, pf[k2].revents);
					fprint(2, " pc=%lux bp=%lux stk:",
						ur->pc, ur->bp);
					for(k2 = 1; k2 < 8; k2++)
						fprint(2, " %lux", ((ulong*)ur->sp)[k2]);
					/* the unread-data theory: a reply nobody
					 * reads keeps the fd readable forever;
					 * report pending bytes WITHOUT draining
					 * (draining here steals them from the
					 * thread they belong to) */
					for(k2 = 0; k2 < (long)a2 && k2 < 6; k2++){
						qi = sockslot(pf[k2].fd);
						if(qi >= 0 && (pf[k2].revents & 1)){
							if(inqn[qi] > 0){
								int qb, qn;

								fprint(2, " peek:");
								qn = inqn[qi] < 16 ? inqn[qi] : 16;
								for(qb = 0; qb < qn; qb++)
									fprint(2, " %2.2ux", inq[qi][qb]);
							}
						}
					}
					fprint(2, "\n");
				}
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
			{
				static int z;
				int lm, ln;

				ln = 0;
				for(lm = 0; lm < 16; lm++)
					if(sockmap[lm][1] && sockmap[lm][2] == 0)
						ln++;
				if(z++ < 40)
					fprint(2, "linuxrun: SEL p%d nfds=%d lis=%d queued=%d\n",
						getpid(), maxfd, ln, listenqueued());
			}
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
		r = sysreadlink((char*)a1, (char*)a2, a3);
		break;
	case 305:	/* readlinkat: absolute paths or AT_FDCWD */
		if(a2 == 0)
			r = -Efault;
		else if(((char*)a2)[0] != '/' && a1 != 0xffffff9cUL)
			r = -Ebadf;
		else
			r = sysreadlink((char*)a2, (char*)a3, a4);
		break;
	case 307:	/* faccessat */
	case 439:	/* faccessat2 */
		{
			char fb[1024];
			int amode;

			if(a2 == 0)
				r = -Efault;
			else if(((char*)a2)[0] != '/' && a1 != 0xffffff9cUL)
				r = -Ebadf;
			else{
				amode = AEXIST;
				if(a3 & 4)
					amode = AREAD;
				else if(a3 & 2)
					amode = AWRITE;
				else if(a3 & 1)
					amode = AEXEC;
				r = access(fixproc(fb, sizeof fb, (char*)a2), amode) < 0 ?
					-Enoent : 0;
			}
		}
		break;
	case 195:	/* stat64 */
	case 196:	/* lstat64 */
		{
			char fb[1024];
			int sfd;

			sfd = open(fixproc(fb, sizeof fb, (char*)a1), OREAD);
			if(sfd < 0)
				r = -Enoent;
			else{
				r = fillstat64(a2, sfd);
				close(sfd);
			}
		}
		break;
	case 383:	/* statx: absolute, AT_FDCWD, dirfd-relative, or AT_EMPTY_PATH */
		{
			char full[1024], fb[1024];
			int sfd;

			full[0] = 0;
			r = -Ebadf;
			if(a2 == 0 || a5 == 0)
				r = -Efault;
			else if(((char*)a2)[0] == 0 && (a3 & 0x1000) && a1 < 1024)
				r = fillstatx(a5, (int)a1);
			else if(((char*)a2)[0] == '/' || a1 == 0xffffff9cUL){
				strncpy(full, fixproc(fb, sizeof fb, (char*)a2), sizeof full-1);
				full[sizeof full-1] = 0;
			}
			else if(a1 < 1024)
				snprint(full, sizeof full, "/proc/%d/fd/%lud/%s",
					getpid(), a1, (char*)a2);
			if(full[0]){
				sfd = open(full, OREAD);
				if(sfd < 0)
					r = -Enoent;
				else{
					r = fillstatx(a5, sfd);
					close(sfd);
				}
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
			{
				static int zd;

				if(zd++ < 40 && i >= 0)
					fprint(2, "linuxrun: DUP p%d %lux->%d pipes=%lux\n",
						getpid(), a1, r, sockmap[i][2]);
			}
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
			if(i >= 0){
				static int zd2;

				if(zd2++ < 60)
					fprint(2, "linuxrun: DUP2STORE p%d %lux->%lux packed=%lux\n",
						getpid(), a1, a2, sockmap[i][2]);
				sockmapfd(r, sockmap[i][2]);
			}
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
		r = dofutex(a1, a2, a3, a4, a5, a6);
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
				if(i >= 0){
					static int zf;

					if(zf++ < 40)
						fprint(2, "linuxrun: FDUP p%d fcntl %lux->%d pipes=%lux\n",
							getpid(), a1, r, sockmap[i][2]);
					sockmapfd(r, sockmap[i][2]);
				}
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
				if(i >= 0){
					sockmap[i][3] = (a3 & 0x800) != 0;
					/* keep the published flag current for
					 * threads that adopt this connection */
					{
						char nb[64];
						int key, pf;

						key = guestprocid ? (int)guestprocid : getpid();
						snprint(nb, sizeof nb,
							"/srv/x.m.%d.%d.f", key, (int)a1);
						pf = create(nb, OWRITE|OTRUNC, 0666);
						if(pf >= 0){
							fprint(pf, "%d", sockmap[i][3]);
							close(pf);
						}
					}
				}
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
	case 328:	/* eventfd2: stand in as a bridge pipe, like 323 -
			 * a closed write end would be EOF, i.e.
			 * permanently POLLIN-ready, and glib polls its
			 * wakeup eventfd: that fake readiness spun xfwm4's
			 * main loop forever */
	case 290:	/* mlock */
		{
			int p[2];

			if(pipe(p) < 0)
				r = -Enomem;
			else{
				r = socknewslot((p[0]<<16) | p[1], 1);
			}
		}
		break;
		case 406:	/* clock_nanosleep_time64(clk, flags, req64, rem64):
				 * glibc's nanosleep on modern i386 - ENOSYS here
				 * aborted pango's FcInit thread and took the
				 * window manager down with it; the request is the
				 * THIRD argument, and its fields are 64-bit
				 * ([0]/[1] as ulongs are sec.lo/sec.hi - reading
				 * [1] as nanoseconds collapsed every sleep to
				 * ~1ms and spun glib's timer loops flat out) */
		{
			ulong ms;
			vlong sec;
			vlong nsc;

			sec = *(vlong*)((uchar*)a3);
			nsc = *(vlong*)((uchar*)a3+8);
			ms = 1;
			if(a3 >= 0x10000 && a3 < 0x80000000 &&
			   sec >= 0 && sec < 100000 && nsc >= 0){
				ms = sec*1000 + nsc/1000000;
				if(ms > 1000)
					ms = 1000;
			}
			sleep(ms ? ms : 1);
		}
		r = 0;
		break;
		case 403:	/* clock_gettime64 */
		case 408:	/* clock_gettime64 alias used by some stubs */
			if(a2 < 0x10000)
				r = -Efault;
			else{
				vlong t;
				static int zck;

				{
					static vlong clkbase = -1;

					if(clkbase < 0)
						clkbase = nsec();
					t = clkbase + (nsec()-clkbase)*1;
				}	/* guest clock mult - see 265 */
				((vlong*)a2)[0] = t/1000000000;
				((vlong*)a2)[1] = t%1000000000;
				if(zck++ % 2000 == 0)
					fprint(2, "linuxrun: CK64 p%d clk=%ld sec=%lld nsec=%lld\n",
						getpid(), (long)a1,
						(vlong)(t/1000000000),
						(vlong)(t%1000000000));
				r = 0;
			}
			break;
	case 422:	/* futex_time64 */
		r = dofutex(a1, a2, a3, a4, a5, a6);
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
	case 214:	/* setresuid32 - Xorg's Popen child checks
		 * setuid(getuid()) and _exit(127)s on failure: this MUST
		 * succeed (it accidentally shared the splice body once and
		 * EINVAL'd every spawn) */
	case 23:	/* setuid */
	case 46:	/* setgid */
	case 94:	/* setgroups */
	case 291:	/* inotify_init: no events are ever reported, so a
			 * quiet placeholder fd satisfies GLib monitors */
		r = 0;
		break;
	case 340:	/* splice(fd_in, off_in, fd_out, off_out, len, flags):
			 * dbus's remaining ENOSYS.  A pipe-to-pipe move in
			 * one bounded chunk is enough for the callers here;
			 * blocking matches splice-on-pipe semantics */
		if(a1 < 0x10000UL && a4 < 0x10000UL && a5 > 0){
			static char sb[32*1024];	/* NOT on the note stack -
							 * a 32KB frame smashed
							 * it and killed guests */
			long got, want;

			want = a5 > sizeof sb ? sizeof sb : a5;
			got = read((int)a1, sb, want);
			if(got < 0)
				r = -Ebadf;
			else if(got == 0)
				r = 0;
			else{
				long put;

				put = 0;
				while(put < got){
					long w;

					w = write((int)a4, sb+put, got-put);
					if(w <= 0)
						break;
					put += w;
				}
				r = put;
			}
		}else
			r = -Einval;
		break;
	case 12:	/* chdir: report success WITHOUT moving the host cwd -
		 * a real chdir broke Xvfb's xkb lookups ("Failed to
		 * activate virtual core keyboard") because the guest's
		 * relative paths then resolved from the wrong root */
		r = 0;
		break;
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
	if(r == -11){
		static int z;

		if(z++ < 60)
			fprint(2, "linuxrun: E11 p%d nr=%lux a1=%lux a2=%lux a3=%lux\n",
				getpid(), nr, a1, a2, a3);
	}
	if((nr == 20 || nr == 24 || nr == 158) &&
	   ur->pc >= 0x68000000 && ur->pc < 0x69000000){
		/* getpid/sched_yield dispatched through ld.so's raw int80
		 * stub (%gs:0x10).  xfwm4's freeze loops here: glvnd's
		 * per-entry fork check (libGLX.so 0x3b50) calls getpid on
		 * every glX* entry, and its teardown drains in-flight
		 * entries with sched_yield.  [sp] = ret into libc getpid;
		 * sp+4 = ret into the caller.  glvnd's pid cache and entry
		 * refcount sit in libGLX's RW segment at fixed addresses
		 * (this session: cache 0x42bd10dc, cnt 0x42bd10e4). */
		static int zg;

		if(zg++ < 300){
			int q, ok;
			ulong bp, nexp;

			ok = 0;
			for(q = 0; q < nguestsegs; q++)
				if(guestsegs[q][0] <= 0x42bd10dc &&
				   0x42bd10e8 <= guestsegs[q][0]+guestsegs[q][1]){
					ok = 1;
					break;
				}
			fprint(2, "linuxrun: SPIN p%d nr=%lux r=%ld pc=%lux ret=%lux",
				getpid(), nr, r, ur->pc, *(ulong*)ur->sp);
			if(ok)
				fprint(2, " pidcache=%lux cnt=%lux",
					*(ulong*)0x42bd10dc, *(ulong*)0x42bd10e4);
			fprint(2, " stk:");
			for(q = 4; q < 68; q += 4)
				fprint(2, " %lux", ((ulong*)ur->sp)[q/4]);
			fprint(2, "\n");
			/* ebp-chain walk: [bp] = saved ebp, [bp+4] = ret */
			nexp = 0;
			for(bp = ur->bp; nexp < 10; nexp++){
				int gok;

				if(bp < 0x40000000 || bp > 0x70000000 || bp & 3)
					break;
				gok = 0;
				for(q = 0; q < nguestsegs; q++)
					if(guestsegs[q][0] <= bp &&
					   bp+8 <= guestsegs[q][0]+guestsegs[q][1]){
						gok = 1;
						break;
					}
				if(!gok)
					break;
				fprint(2, "linuxrun: BPWALK p%d bp=%lux ret=%lux\n",
					getpid(), bp, *(ulong*)(bp+4));
				bp = *(ulong*)bp;
				if(bp == 0 || bp == (ulong)-1)
					break;
			}
		}
	}
	if(nr == 192 || nr == 90){
		static int zm;

		if(zm++ < 500)
			fprint(2, "linuxrun: MAP p%d nr=%lux addr=%lux len=%lux -> %ld (bump=%lux)\n",
				getpid(), nr, a1, a2, r, mapbump);
	}
		if(verbose)
			fprint(2, "linuxrun: sys %lux -> %ld\n", nr, r);
	{
		/* set*id family results: Xorg's Popen child _exit(127)s
		 * if setuid(getuid()) fails - the number labels in the
		 * case table have been shifted before, so log every
		 * call and return of the whole family */
		static int zs;

		if(zs++ < 20 &&
		   (nr == 23 || nr == 46 || nr == 49 || nr == 50 ||
		    nr == 24 || nr == 47 || nr == 164 || nr == 165 ||
		    (nr >= 199 && nr <= 214) || nr == 2 || nr == 190))
			fprint(2, "linuxrun: SETID p%d nr=%lux(%ld) a1=%lux -> %ld\n",
				getpid(), nr, nr, a1, r);
	}
	{
		/* any failing syscall: one line each (capped) - hidden
		 * EINVALs from stubs otherwise masquerade as app errors
		 * ('Failed to get fd limit: Invalid argument') */
		static int ze;

		if(r < 0 && ze++ < 60)
			fprint(2, "linuxrun: SYSERR p%d nr=%lux a1=%lux a2=%lux a3=%lux a4=%lux -> %ld\n",
				getpid(), nr, a1, a2, a3, a4, r);
	}
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
	{
		/* unconditional, tightly capped: every note with its pc.  A
		 * forked foreign child can die on a trap note that never
		 * reaches this handler (note-stack remnant); this print is
		 * what proves that dispatch happened or not */
		static int zn;

		if(zn++ < 12)
			fprint(2, "linuxrun: NOTE p%d pc=%lux %s\n",
				getpid(), ur->pc, msg ? msg : "?");
	}
	if(msg != nil && strcmp(msg, "alarm") == 0){
		/* the dispatcher re-arms a 2s alarm on every syscall: each
		 * note samples the guest wherever it is - userspace loops
		 * (glib's check/dispatch between the wakeup read and the
		 * re-poll) included, which no syscall-level probe sees */
		static int za;

		if(za++ < 300)
			fprint(2, "linuxrun: SAMPLE p%d pc=%lux sp=%lux bp=%lux ax=%lux bx=%lux\n",
				getpid(), ur->pc, ur->sp, ur->bp, ur->ax, ur->bx);
		alarm(2000);
		return 1;
	}
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
		/* Trap faults go to the guest's SIGSEGV handler when it has
		 * one: Wine's i386 unix thunks are deliberate `hlt` faults
		 * dispatched exactly this way on Linux. */
		if(started && strstr(msg, "trap:") != nil &&
		   deliversignal(11, ur, 0x80, ur->pc)){
			static int zs;

			if(zs++ < 20)
				fprint(2, "linuxrun: SIGSEGV delivered pc=%lux handler=%lux\n",
					ur->pc, sighandler[11]);
			return 1;
		}
		/* Crashes inside the guest image need their instruction
		 * named: dump the bytes around the faulting pc before the
		 * default disposition takes the process. */
		if(started && strstr(msg, "trap:") != nil &&
		   ur->pc > 0x10000 && ur->pc < 0x7f000000){
			int q, ok;

			ok = 0;
			for(q = 0; q < nguestsegs; q++)
				if(guestsegs[q][0] <= ur->pc-16 &&
				   ur->pc+48 <= guestsegs[q][0]+guestsegs[q][1]){
					ok = 1;
					break;
				}
			if(ok){
				fprint(2, "linuxrun: FAULTCODE p%d pc=%lux bytes:",
					getpid(), ur->pc);
				for(q = -16; q < 48; q++)
					fprint(2, " %2.2ux", ((uchar*)ur->pc)[q]);
				fprint(2, "\n");
			}
		}
	}
	if(msg != nil && strcmp(msg, "linux sys") == 0){
		/* the kernel gates guest int $0x80 here (devldt procs) */
		if(!started){
			started = 1;
			guestprocid = getpid();
			fprint(2, "linuxrun: PIDINIT-SYS p%d guest=%lux\n",
				getpid(), guestprocid);
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
		alarm(2000);
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
	if(pc[0] != 0x0f || pc[1] != 0x0b){
		/* an invalid opcode that is not our ud2 syscall gate: the
		 * guest jumped to garbage (dbus-daemon died this way at
		 * pc=0xe70a with nothing but the suicide note).  Dump the
		 * scene before the default disposition takes the process */
		if(started){
			ulong *stk;
			int i;

			fprint(2, "linuxrun: guest UD pc=%lux sp=%lux trap=%lux\n",
				ur->pc, ur->sp, ur->trap);
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
			if(ur->sp > 0x10000 && ur->sp < 0x70000000 &&
			   guestok(ur->sp, 64)){
				stk = (ulong*)ur->sp;
				for(i = 0; i < 16; i++)
					fprint(2, "linuxrun:  sp+%d = %lux\n", i*4, stk[i]);
			}
		}
		return 0;
	}
	if(!started){
		started = 1;
		guestprocid = getpid();
		fprint(2, "linuxrun: PIDINIT-UD2 p%d guest=%lux\n",
			getpid(), guestprocid);
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
		int wassnap;

		forkpending = 0;
		fprint(2, "linuxrun: FBOUNCE p%d\n", getpid());
		*ur = forkregs;
		/* which kind of child we are: a snapshot fork rebuilds
		 * private memory and drops the bridge; a CLONE_FILES
		 * thread keeps its inherited socket map and re-syncs
		 * newer connections from the parent */
		wassnap = forksnap;
		forksnap = 0;
		if(wassnap){
			int s, pmfd;
			ulong va, len, ulen, a;
			char ppidbuf[16];
			int ppid;

			/* Rebuild this child's image from the PARENT's
			 * memory through /proc/<ppid>/mem - RAM speed,
			 * no ufs file.  The old page-file snapshot
			 * trickled a 200MB image at ~500KB/s and
			 * parked the X server behind its xkbcomp fork
			 * for the whole session.  The parent keeps its
			 * shared segments, so its content stays
			 * readable while we copy. */
			pmfd = open("#c/ppid", OREAD);
			if(pmfd >= 0){
				int n;

				n = readn(pmfd, ppidbuf, sizeof ppidbuf-1);
				close(pmfd);
				if(n <= 0)
					exits("fork ppid");
				ppidbuf[n] = 0;
				ppid = strtol(ppidbuf, nil, 10);
				snprint(ppidbuf, sizeof ppidbuf,
					"/proc/%d/mem", ppid);
				pmfd = open(ppidbuf, OREAD);
			}else
				pmfd = -1;
			fprint(2, "linuxrun: FMEM p%d parent=%d open=%d\n", getpid(), ppid, pmfd);
			if(pmfd < 0){
				fprint(2, "linuxrun: fork mem open: %r\n");
				if(forkready[1] >= 0){
					write(forkready[1], "x", 1);
					close(forkready[1]);
					forkready[1] = -1;
				}
				exits("fork snapshot");
			}
			/* fresh private segments, then pull the
			 * parent's pages in */
			for(s = 0; s < nguestsegs; s++){
				va = guestsegs[s][0];
				segdetach((void*)va);
				if(segattach(0, "shared", (void*)va,
				    guestsegs[s][1]) == (void*)-1){
					fprint(2, "linuxrun: fork attach %#lux %#lux: %r\n",
						va, guestsegs[s][1]);
					close(pmfd);
					if(forkready[1] >= 0){
						write(forkready[1], "x", 1);
						close(forkready[1]);
						forkready[1] = -1;
					}
					exits("fork attach");
				}
			}
			for(s = 0; s < nguestsegs; s++){
				va = guestsegs[s][0];
				len = guestsegs[s][1];
				ulen = len;
				if(va == Mapbase)
					ulen = mapbump - Mapbase;
				for(a = 0; a < ulen; a += Pgsz){
					long n;

					n = pread(pmfd, (void*)(va+a), Pgsz, va+a);
					if(n < 0){
						fprint(2, "linuxrun: fork mem read %lux: %r\n", va+a);
						close(pmfd);
						if(forkready[1] >= 0){
							write(forkready[1], "x", 1);
							close(forkready[1]);
							forkready[1] = -1;
						}
						exits("fork snapshot");
					}
				}
			}
			close(pmfd);
			forktrace = 80;	/* uncapped syscall trace for this
					 * child's first moments: the
					 * xkbcomp spawn died silently */
			fprint(2, "linuxrun: FMEM p%d copy done\n", getpid());
		}else{
			/* CLONE_FILES thread: adopt the thread-group parent
			 * for socket lookups and pull in the connections it
			 * opened since the clone (the inherited map is a
			 * fork-time copy) */
			char pb[16];
			int pm, n;

			pm = open("#c/ppid", OREAD);
			if(pm >= 0){
				n = readn(pm, pb, sizeof pb-1);
				close(pm);
				if(n > 0){
					pb[n] = 0;
					forkppid = strtol(pb, nil, 10);
				}
			}
			socksync();
			fprint(2, "linuxrun: THREADSYNC p%d parent=%d\n",
				getpid(), forkppid);
		}
		/* the parent stays parked until the copy completes: the
		 * child runs guest code (dash) on this image before
		 * exec, and a torn copy null-derefs - but /proc/mem
		 * copies at RAM speed, so the park lasts seconds */
		if(forkready[1] >= 0){
			write(forkready[1], "x", 1);
			close(forkready[1]);
			forkready[1] = -1;
		}
		/* every fork in this workload execs right away (xkbcomp,
		 * dash, g_spawn helpers, xterm's shell); a child that
		 * kept the parent's bridge descriptors raced reads on
		 * the same pipes - stealing the parent's replies - and
		 * its exit closed the pipes under the parent.  Close
		 * the bridge in the child; CLOEXEC handling on exec
		 * covers anything else.  A CLONE_FILES thread is NOT an
		 * exec candidate: it shares the table - wiping its map
		 * is what made a shared fd look "not a socket" and
		 * spin the main loop on a fake-ready poll. */
		if(wassnap){
			int q;

			for(q = 0; q < NSOCK; q++){
				if(!sockmap[q][1])
					continue;
				close(sockmap[q][2] >> 16);
				close(sockmap[q][2] & 0xffff);
				close(sockmap[q][0]);
				sockmap[q][1] = 0;
				sockmap[q][2] = 0;
				sockmap[q][3] = 0;
			}
		}
		return 1;
	}
	ur->ax = dosyscall(ur);
	ur->pc += 2;
	if(tlsfsokay)
		ur->fs = tlsselector;
	if(sigreturning){
		/* rt_sigreturn: the frame's context wins, including a
		 * redirected pc when the handler dispatched a thunk */
		sigreturning = 0;
		ur->pc = sigret_pc;
		ur->sp = sigret_sp;
		ur->ax = sigret_ax;
		ur->bx = sigret_bx;
		ur->cx = sigret_cx;
		ur->dx = sigret_dx;
		ur->si = sigret_si;
		ur->di = sigret_di;
		ur->bp = sigret_bp;
	}
	alarm(2000);
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
	atnotify(alarmnote, 1);	/* alarm + sample notes, always armed */
	f = (void(*)(void))trapinsn;
	f();
	fatal("returned from the guest");	/* not reached */
}

static void
usage(void)
{
	fprint(2, "usage: linuxrun [-nv] [-e name=value] prog [args ...]\n");
	exits("usage");
}

void
main(int argc, char *argv[])
{
	Ehdr eh;
	uchar hdr[64];
	int fd, i;

	initenv();
	ARGBEGIN{
	case 'e':
		if(setguestenv(EARGF(usage())) < 0)
			fatal("invalid or oversized environment entry");
		break;
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

	rememberexec(argv[0]);

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
	/* see the exec path: low memory must attach outside note context */
	ensurelow();

	forkscratch = nil;	/* kernel COW replaced the snapshot;
				 * the scratch starved the note stack */
	stacktop = buildstack(argc, argv);
	if(verbose)
		fprint(2, "linuxrun: entry %#lux stack %#lux\n", entrypc, stacktop);

	runguest();
	exits(nil);
}
