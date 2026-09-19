#include <u.h>
#include <libc.h>
#include <keyboard.h>
#include "x11.h"

static int connectionseq;
static int allowbadmap;
/* Errors carrying this request sequence are skipped instead of fatal; used
 * for optional RandR requests whose failure means "keep the old size". */
static int tolerateseq;
static int tolerateerr;

uint
x16(uchar *p)
{
	return p[0] | p[1]<<8;
}

ulong
x32(uchar *p)
{
	return (ulong)p[0] | (ulong)p[1]<<8 | (ulong)p[2]<<16 | (ulong)p[3]<<24;
}

void
p16(uchar *p, uint v)
{
	p[0] = v;
	p[1] = v>>8;
}

void
p32(uchar *p, ulong v)
{
	p16(p, v);
	p16(p+2, v>>16);
}

static void
sendall(int fd, uchar *p, int n)
{
	int k;

	while(n > 0){
		k = write(fd, p, n);
		if(k <= 0)
			sysfatal("X connection write: %r");
		p += k;
		n -= k;
	}
}

void
xsend(Xconn *x, uchar *p, int n)
{
	alarm(15000);
	if(n < 4 || n%4 != 0 || n > 65535*4)
		sysfatal("invalid X request size");
	p16(p+2, n/4);
	sendall(x->wr, p, n);
	x->seq++;
}

uchar*
xreply(Xconn *x, uchar *hdr, int *size)
{
	uchar *data;
	ulong n;
	int type;

	for(;;){
		if(readn(x->rd, hdr, 32) != 32)
			sysfatal("X connection closed: %r");
		type = hdr[0]&127;
		if(type == 0 && allowbadmap && hdr[1] == 11 && hdr[10] == 101 && x16(hdr+2) == x->seq){
			*size = 0;
			alarm(0);
			return nil;
		}
		if(type == 0 && tolerateseq != 0 && x16(hdr+2) == tolerateseq){
			tolerateseq = 0;
			tolerateerr = hdr[1];
			*size = 0;
			alarm(0);
			return nil;
		}
		if(type == 0)
			sysfatal("X error %d, request %d.%d, sequence %ud",
				hdr[1], hdr[10], hdr[11], x16(hdr+2));
		if(type == 1){
			tolerateseq = 0;
			break;
		}
		if(type == 35)
			sysfatal("unexpected extended X event");
		if(x->nevents == nelem(x->events))
			sysfatal("X event queue full");
		memmove(x->events[x->nevents++], hdr, 32);
	}
	if(x16(hdr+2) != x->seq)
		sysfatal("unexpected X reply sequence");
	n = x32(hdr+4);
	if(n > 4*1024*1024)
		sysfatal("X reply too large");
	*size = n*4;
	data = malloc(*size+1);
	if(data == nil)
		sysfatal("out of memory");
	if(readn(x->rd, data, *size) != *size)
		sysfatal("short X reply: %r");
	data[*size] = 0;
	alarm(0);
	return data;
}

static void
post(char *path, int fd)
{
	int p;

	p = create(path, OWRITE, 0600);
	if(p < 0 || fprint(p, "%d", fd) < 0)
		sysfatal("post X transport %s: %r", path);
	close(p);
}

/* Xvfb can start with an empty core map when its external keyboard
 * compiler is unavailable.  Our private display needs a usable default.
 * Install a real US map through X11; clients see the same map as XTEST. */
static void
defaultkeyboard(Xconn *x)
{
	static char *normal[] = {"1234567890-=", "qwertyuiop[]", "asdfghjkl;'`", "\\zxcvbnm,./"};
	static char *shifted[] = {"!@#$%^&*()_+", "QWERTYUIOP{}", "ASDFGHJKL:\"~", "|ZXCVBNM<>?"};
	static int first[] = {10, 24, 38, 51};
	static ulong special[][2] = {
		{9, 0xff1b}, {22, 0xff08}, {23, 0xff09}, {36, 0xff0d},
		{37, 0xffe3}, {50, 0xffe1}, {62, 0xffe2}, {64, 0xffe9},
		{65, 0x20}, {66, 0xffe5}, {110, 0xff50}, {111, 0xff52},
		{112, 0xff55}, {113, 0xff51}, {114, 0xff53}, {115, 0xff57},
		{116, 0xff54}, {117, 0xff56}, {118, 0xff63}, {119, 0xffff},
	};
	uchar *req, hdr[32];
	int i, j, count, size, n;

	memset(x->keys, 0, sizeof x->keys);
	for(i = 0; i < nelem(first); i++)
		for(j = 0; normal[i][j]; j++){
			x->keys[first[i]+j][0] = normal[i][j];
			x->keys[first[i]+j][1] = shifted[i][j];
		}
	for(i = 0; i < nelem(special); i++){
		x->keys[special[i][0]][0] = special[i][1];
		x->keys[special[i][0]][1] = special[i][1];
	}
	for(i = 0; i < 10; i++)
		x->keys[67+i][0] = x->keys[67+i][1] = 0xffbe+i;
	x->keys[95][0] = x->keys[95][1] = 0xffc8;
	x->keys[96][0] = x->keys[96][1] = 0xffc9;
	count = x->maxkey-x->minkey+1;
	size = 8+count*8;
	req = mallocz(size, 1);
	if(req == nil) sysfatal("out of memory");
	req[0] = 100; /* ChangeKeyboardMapping */
	req[1] = count;
	req[4] = x->minkey;
	req[5] = 2;
	for(i = 0; i < count; i++)
		for(j = 0; j < 2; j++)
			p32(req+8+(i*2+j)*4, x->keys[x->minkey+i][j]);
	xsend(x, req, size);
	memset(req, 0, 20);
	req[0] = 118; /* SetModifierMapping: two slots per modifier */
	req[1] = 2;
	req[4] = 50; req[5] = 62;
	req[6] = 66;
	req[8] = 37;
	req[10] = 64;
	xsend(x, req, 20);
	free(xreply(x, hdr, &n));
	free(req);
	if(hdr[1] != 0)
		sysfatal("cannot initialize X keyboard modifiers");
	x->nsyms = 2;
}

void
xconnect(Xconn *x, char *socket)
{
	int a[2], b[2], fd, n, off, i, j, min, max, seq;
	char path[512];
	uchar hello[12], hdr[32], req[16], *data, *root;

	memset(x, 0, sizeof *x);
	x->rd = x->wr = -1;
	alarm(15000);
	seq = connectionseq++;
	snprint(path, sizeof path, "%s.req", socket);
	if(access(path, AEXIST) < 0)
		sysfatal("X server is not ready at %s", socket);
	if(pipe(a) < 0 || pipe(b) < 0)
		sysfatal("X pipes: %r");
	x->rd = b[0];
	x->wr = a[1];
	/* Match linuxrun's local AF_UNIX transport.  Publish the marker
	 * and reply endpoint before the request endpoint (the ready flag). */
	snprint(x->ticket, sizeof x->ticket, "%s.t.%d.%d", socket, getpid(), seq);
	fd = create(x->ticket, OWRITE, 0600);
	if(fd < 0 || fprint(fd, "%d", b[1]) < 0)
		sysfatal("X connection marker: %r");
	close(fd);
	snprint(x->postwrite, sizeof x->postwrite, "/srv/x.c.%d.%d.b", getpid(), seq);
	post(x->postwrite, b[1]);
	snprint(x->postread, sizeof x->postread, "/srv/x.c.%d.%d.a", getpid(), seq);
	post(x->postread, a[0]);
	close(a[0]);
	close(b[1]);
	memset(hello, 0, sizeof hello);
	hello[0] = 'l';
	p16(hello+2, 11);
	sendall(x->wr, hello, sizeof hello);
	if(readn(x->rd, hdr, 8) != 8)
		sysfatal("X handshake: %r");
	n = x16(hdr+6)*4;
	if(n < 32 || n > 262140)
		sysfatal("invalid X setup size");
	data = malloc(n);
	if(data == nil || readn(x->rd, data, n) != n)
		sysfatal("short X setup");
	if(hdr[0] != 1)
		sysfatal("X server refused connection: %.*s", hdr[1], (char*)data);
	x->base = x32(data+4);
	x->mask = x32(data+8);
	x->order = data[22];
	x->minkey = data[26];
	x->maxkey = data[27];
	off = 32+((x16(data+16)+3)&~3);
	if(data[20] == 0 || off+data[21]*8+40 > n)
		sysfatal("invalid X screen setup");
	root = data+off+data[21]*8;
	x->root = x32(root);
	x->width = x16(root+20);
	x->height = x16(root+22);
	x->depth = root[38];
	for(i = 0; i < data[21]; i++)
		if(data[off+i*8] == x->depth)
			x->bpp = data[off+i*8+1];
	if(x->depth != 24 || x->bpp != 32 || x->order != 0 ||
	   x->width < 1 || x->height < 1 || x->width*x->height > 4*1024*1024)
		sysfatal("xbridge requires a little-endian 24-bit Xvfb screen");
	free(data);
	memset(req, 0, sizeof req);
	req[0] = 98; /* QueryExtension */
	p16(req+4, 5);
	memmove(req+8, "XTEST", 5);
	xsend(x, req, 16);
	data = xreply(x, hdr, &n);
	free(data);
	if(!hdr[8])
		sysfatal("X server does not support XTEST input");
	x->xtest = hdr[9];
	memset(req, 0, sizeof req);
	req[0] = x->xtest;
	req[4] = 2;
	p16(req+6, 2);
	xsend(x, req, 8);
	free(xreply(x, hdr, &n));
	min = x->minkey;
	max = x->maxkey;
	fprint(2, "xbridge: X screen %dx%d keys %d..%d\n", x->width, x->height, min, max);
	for(off = min; off <= max; off += 16){
		int count;

		count = max-off+1;
		if(count > 16) count = 16;
		memset(req, 0, sizeof req);
		req[0] = 101; /* GetKeyboardMapping */
		req[4] = off;
		req[5] = count;
		xsend(x, req, 8);
		allowbadmap = off == min;
		data = xreply(x, hdr, &n);
		allowbadmap = 0;
		if(data == nil){
			defaultkeyboard(x);
			break;
		}
		x->nsyms = hdr[1];
		if(x->nsyms < 1 || x->nsyms > 8 || n != count*x->nsyms*4)
			sysfatal("unsupported X keyboard mapping");
		for(i = 0; i < count; i++)
			for(j = 0; j < x->nsyms; j++)
				x->keys[off+i][j] = x32(data+(i*x->nsyms+j)*4);
		free(data);
	}
}

void
xclose(Xconn *x)
{
	if(x->rd >= 0) close(x->rd);
	if(x->wr >= 0) close(x->wr);
	if(x->ticket[0]) remove(x->ticket);
	if(x->postread[0]) remove(x->postread);
	if(x->postwrite[0]) remove(x->postwrite);
	x->rd = x->wr = -1;
}

ulong
xatom(Xconn *x, char *name)
{
	uchar req[256], hdr[32];
	int n, len;

	len = strlen(name);
	if(len > sizeof req-8)
		sysfatal("X atom too long");
	memset(req, 0, sizeof req);
	req[0] = 16;
	p16(req+4, len);
	memmove(req+8, name, len);
	xsend(x, req, (len+11)&~3);
	free(xreply(x, hdr, &n));
	return x32(hdr+8);
}

void
xinput(Xconn *x, int type, int detail, int px, int py)
{
	uchar req[36];

	memset(req, 0, sizeof req);
	req[0] = x->xtest;
	req[1] = 2;
	req[4] = type;
	req[5] = detail;
	p32(req+12, x->root);
	p16(req+24, px);
	p16(req+26, py);
	xsend(x, req, sizeof req);
}

static int
keycode(Xconn *x, ulong sym, int *shift)
{
	int i, j;

	for(j = 0; j < 2 && j < x->nsyms; j++)
		for(i = x->minkey; i <= x->maxkey; i++)
			if(x->keys[i][j] == sym){
				*shift = j;
				return i;
			}
	return 0;
}

void
xkey(Xconn *x, Rune r)
{
	ulong sym;
	int code, shift, ctrl, sc, cc, dummy;

	ctrl = 0;
	sym = r;
	switch(r){
	case '\n': sym = 0xff0d; break;
	case '\t': sym = 0xff09; break;
	case Kctab: sym = 0xff09; break; /* the chord is consumed by rio9 */
	case Kbs: sym = 0xff08; break;
	case Kdel: sym = 0xffff; break;
	case Kesc: sym = 0xff1b; break;
	case Khome: sym = 0xff50; break;
	case Kleft: sym = 0xff51; break;
	case Kup: sym = 0xff52; break;
	case Kright: sym = 0xff53; break;
	case Kdown: sym = 0xff54; break;
	case Kpgup: sym = 0xff55; break;
	case Kpgdown: sym = 0xff56; break;
	case Kend: sym = 0xff57; break;
	case Kins: sym = 0xff63; break;
	default:
		if(r > 0 && r < 27){
			ctrl = 1;
			sym = r+'a'-1;
		}else if(r > KF && r <= KF+12)
			sym = 0xffbd+r-KF;
		else if(r > 255)
			sym = 0x01000000|r;
		break;
	}
	shift = 0;
	code = keycode(x, sym, &shift);
	if(code == 0)
		return;
	sc = keycode(x, 0xffe1, &dummy);
	cc = keycode(x, 0xffe3, &dummy);
	if(shift && sc) xinput(x, 2, sc, 0, 0);
	if(ctrl && cc) xinput(x, 2, cc, 0, 0);
	xinput(x, 2, code, 0, 0);
	xinput(x, 3, code, 0, 0);
	if(ctrl && cc) xinput(x, 3, cc, 0, 0);
	if(shift && sc) xinput(x, 3, sc, 0, 0);
}

uchar*
ximage(Xconn *x, int *size)
{
	uchar req[20], hdr[32], *data;

	memset(req, 0, sizeof req);
	req[0] = 73;
	req[1] = 2; /* ZPixmap */
	p32(req+4, x->root);
	p16(req+12, x->width);
	p16(req+14, x->height);
	p32(req+16, ~0UL);
	xsend(x, req, sizeof req);
	data = xreply(x, hdr, size);
	if(*size != x->width*x->height*4 || hdr[1] != 24)
		sysfatal("unexpected X image layout");
	return data;
}

/* Ask clients to close through their normal UI, including unsaved-change
 * handling.  There is no X window manager on the private display. */
int
xrequestclose(Xconn *x)
{
	uchar req[44], hdr[32], *children, *protocols;
	ulong wmprotocols, wmdelete, win;
	int n, count, i, j, size, sent;

	wmprotocols = xatom(x, "WM_PROTOCOLS");
	wmdelete = xatom(x, "WM_DELETE_WINDOW");
	memset(req, 0, sizeof req);
	req[0] = 15; /* QueryTree */
	p32(req+4, x->root);
	xsend(x, req, 8);
	children = xreply(x, hdr, &n);
	count = x16(hdr+16);
	if(count*4 > n)
		sysfatal("invalid X window tree");
	sent = 0;
	for(i = 0; i < count; i++){
		win = x32(children+i*4);
		memset(req, 0, sizeof req);
		req[0] = 20; /* GetProperty, retain the client's property */
		p32(req+4, win);
		p32(req+8, wmprotocols);
		p32(req+12, 4); /* ATOM */
		p32(req+20, 1024);
		xsend(x, req, 24);
		protocols = xreply(x, hdr, &size);
		if(hdr[1] == 32 && x32(hdr+8) == 4)
			for(j = 0; j+4 <= size; j += 4)
				if(x32(protocols+j) == wmdelete){
					memset(req, 0, sizeof req);
					req[0] = 25; /* SendEvent */
					p32(req+4, win);
					req[12] = 33; /* ClientMessage */
					req[13] = 32;
					p32(req+16, win);
					p32(req+20, wmprotocols);
					p32(req+24, wmdelete);
					xsend(x, req, sizeof req);
					sent++;
					break;
				}
		free(protocols);
	}
	free(children);
	return sent;
}

ulong
xowner(Xconn *x, ulong selection)
{
	uchar req[8], hdr[32];
	int n;

	memset(req, 0, sizeof req);
	req[0] = 23;
	p32(req+4, selection);
	xsend(x, req, sizeof req);
	free(xreply(x, hdr, &n));
	return x32(hdr+8);
}

void
xown(Xconn *x, ulong window, ulong selection)
{
	uchar req[16];

	memset(req, 0, sizeof req);
	req[0] = 22;
	p32(req+4, window);
	p32(req+8, selection);
	xsend(x, req, sizeof req);
}

void
xconvert(Xconn *x, ulong win, ulong sel, ulong target, ulong prop)
{
	uchar req[24];

	memset(req, 0, sizeof req);
	req[0] = 24;
	p32(req+4, win);
	p32(req+8, sel);
	p32(req+12, target);
	p32(req+16, prop);
	xsend(x, req, sizeof req);
}

void
xproperty(Xconn *x, ulong win, ulong prop, ulong type, int format, uchar *data, int n)
{
	uchar *req;
	int size, unit, chunk, len, mode;

	/* ChangeProperty requests are capped by the 16-bit request length.
	 * Assemble larger values with Replace followed by Append chunks;
	 * the property is complete once the last request lands. */
	unit = format/8;
	if(unit <= 0 || n < 0 || n%unit != 0)
		sysfatal("invalid X property length");
	if(n == 0){
		/* A zero-length value is a distinct event: INCR transfers
		 * close with one. */
		req = mallocz(24, 1);
		if(req == nil)
			sysfatal("out of memory");
		req[0] = 18;
		p32(req+4, win);
		p32(req+8, prop);
		p32(req+12, type);
		req[16] = format;
		xsend(x, req, 24);
		free(req);
		return;
	}
	chunk = 240*1024;
	mode = 0;
	while(n > 0){
		len = n < chunk ? n : chunk;
		size = (24+len+3)&~3;
		req = mallocz(size, 1);
		if(req == nil)
			sysfatal("out of memory");
		req[0] = 18;
		req[1] = mode;
		p32(req+4, win);
		p32(req+8, prop);
		p32(req+12, type);
		req[16] = format;
		p32(req+20, len/unit);
		memmove(req+24, data, len);
		xsend(x, req, size);
		free(req);
		mode = 2; /* Append */
		data += len;
		n -= len;
	}
}

uchar*
xgetproperty(Xconn *x, ulong win, ulong prop, ulong *type, int *format, int *size)
{
	uchar req[24], hdr[32], *data;
	int n;

	memset(req, 0, sizeof req);
	req[0] = 20;
	req[1] = 1;
	p32(req+4, win);
	p32(req+8, prop);
	p32(req+20, 256*1024); /* long-length: one 1 MiB 8-bit property fits */
	xsend(x, req, sizeof req);
	data = xreply(x, hdr, &n);
	*type = x32(hdr+8);
	*format = hdr[1];
	*size = x32(hdr+16)*(*format/8);
	if(x32(hdr+12) != 0 || *size > n){
		free(data);
		return nil;
	}
	data[*size] = 0;
	return data;
}

/* Resize the X screen through RandR.  RandR 1.2 servers take
 * RRSetScreenSize for arbitrary dimensions; RandR 1.0 servers (old Xvfb)
 * only switch between the sizes listed in RRGetScreenInfo, so pick a
 * matching sizeID.  Returns 1 when the size changed. */
int
xresize(Xconn *x, int width, int height)
{
	uchar req[32], hdr[32], *data;
	ulong ts, cts;
	int n, i, nsizes, nrateents, sizeid, vmin, best, bestd;

	if(width < 1 || height < 1 || width*height > 4*1024*1024)
		return 0;
	if(x->randr == 0){
		memset(req, 0, sizeof req);
		req[0] = 98; /* QueryExtension */
		p16(req+4, 5);
		memmove(req+8, "RANDR", 5);
		xsend(x, req, 16);
		data = xreply(x, hdr, &n);
		free(data);
		if(!hdr[8]){
			fprint(2, "xbridge: X server has no RANDR extension\n");
			return 0;
		}
		x->randr = hdr[9];
		memset(req, 0, sizeof req);
		req[0] = x->randr;
		req[1] = 0; /* X_RRQueryVersion */
		p32(req+4, 1);
		p32(req+8, 2);
		xsend(x, req, 12);
		free(xreply(x, hdr, &n));
		/* The reply carries CARD32 versions; the server speaks the
		 * lesser of ours and its own. */
		x->rrver = x32(hdr+12);
	}
	vmin = x->rrver;
	if(vmin >= 2){
		/* Learn the server's size limits once. */
		if(x->rrmaxw == 0){
			memset(req, 0, sizeof req);
			req[0] = x->randr;
			req[1] = 6; /* X_RRGetScreenSizeRange */
			p32(req+4, x->root);
			xsend(x, req, 8);
			data = xreply(x, hdr, &n);
			if(data != nil){
				free(data);
				x->rrminw = x16(hdr+8);
				x->rrminh = x16(hdr+10);
				x->rrmaxw = x16(hdr+12);
				x->rrmaxh = x16(hdr+14);
			}
			fprint(2, "xbridge: X screen size range %dx%d..%dx%d\n",
				x->rrminw, x->rrminh, x->rrmaxw, x->rrmaxh);
		}
		if(x->rrmaxw > 0){
			if(width > x->rrmaxw) width = x->rrmaxw;
			if(height > x->rrmaxh) height = x->rrmaxh;
			if(width < x->rrminw) width = x->rrminw;
			if(height < x->rrminh) height = x->rrminh;
		}
		/* A live CRTC pins the screen size (BadMatch otherwise).
		 * Detach every CRTC from its mode first: this is a software
		 * Xvfb, so nothing depends on a scanning CRTC to render. */
		memset(req, 0, sizeof req);
		req[0] = x->randr;
		req[1] = 8; /* X_RRGetScreenResources */
		p32(req+4, x->root);
		xsend(x, req, 8);
		data = xreply(x, hdr, &n);
		if(data != nil){
			ulong cts = x32(hdr+12);
			int ncrtc = x16(hdr+16);

			for(i = 0; i < ncrtc && (i+1)*4 <= n; i++){
				memset(req, 0, sizeof req);
				req[0] = x->randr;
				req[1] = 21; /* X_RRSetCrtcConfig */
				p32(req+4, x32(data+i*4));
				p32(req+8, 0); /* timestamp: CurrentTime */
				p32(req+12, cts);
				p16(req+24, 1); /* RR_Rotate_0 */
				/* mode stays None: the CRTC is detached */
				tolerateseq = x->seq+1;
				xsend(x, req, 32);
				free(xreply(x, hdr, &n));
				tolerateerr = 0;
			}
			free(data);
		}
		/* RRSetScreenSize: no reply; confirm through GetGeometry and
		 * treat the error, if any, as benign. */
		memset(req, 0, sizeof req);
		req[0] = x->randr;
		req[1] = 7; /* X_RRSetScreenSize */
		p32(req+4, x->root);
		p16(req+8, width);
		p16(req+10, height);
		p32(req+12, width*254/960); /* report a 96 dpi physical size */
		p32(req+16, height*254/960);
		tolerateseq = x->seq+1;
		xsend(x, req, 20);
	}else{
		/* Old servers disagree about the minor opcodes.  Try the
		 * known GetScreenInfo spellings until one answers with a
		 * reply carrying our root window. */
		data = nil;
		for(i = 0; i < 3 && data == nil; i++){
			int minor = i == 0 ? 1 : i == 1 ? 5 : 3;

			memset(req, 0, sizeof req);
			req[0] = x->randr;
			req[1] = minor;
			p32(req+4, x->root);
			tolerateseq = x->seq+1;
			xsend(x, req, 8);
			data = xreply(x, hdr, &n);
			if(data != nil && (x32(hdr+8) != x->root || x16(hdr+20) > 128)){
				free(data);
				data = nil;
			}
		}
		if(data == nil){
			fprint(2, "xbridge: cannot list X screen sizes\n");
			return 0;
		}
		ts = x32(hdr+12);
		cts = x32(hdr+16);
		nsizes = x16(hdr+20);
		nrateents = x16(hdr+28);
		sizeid = -1;
		best = -1;
		bestd = 0x7fffffff;
		for(i = 0; i < nsizes && nrateents*2+(i+1)*8 <= n; i++){
			uchar *sz = data+nrateents*2+i*8;
			int sw = x16(sz), sh = x16(sz+2), d;

			if(sw == width && sh == height){
				sizeid = i;
				break;
			}
			d = sw > width ? sw-width : width-sw;
			d += sh > height ? sh-height : height-sh;
			if(d < bestd){
				bestd = d;
				best = i;
			}
		}
		free(data);
		if(sizeid < 0){
			/* Only listed sizes exist here; snap to the nearest
			 * one and let the bridge letterbox the difference. */
			if(best < 0 || bestd > 400){
				fprint(2, "xbridge: no X screen size near %dx%d\n",
					width, height);
				return 0;
			}
			sizeid = best;
		}
		/* RRSetScreenConfig: rotation RR_Rotate_0.  Servers that
		 * predate RandR 1.1 reject the trailing rate field, so that
		 * request is one word shorter there. */
		memset(req, 0, sizeof req);
		req[0] = x->randr;
		req[1] = 2; /* X_RRSetScreenConfig */
		p32(req+4, x->root);
		p32(req+8, ts);
		p32(req+12, cts);
		p16(req+16, sizeid);
		p16(req+18, 1); /* RR_Rotate_0 */
		tolerateseq = x->seq+1;
		xsend(x, req, vmin >= 1 ? 24 : 20);
		data = xreply(x, hdr, &n);
		if(data == nil){
			fprint(2, "xbridge: RandR config to %dx%d was refused\n",
				width, height);
			return 0;
		}
		free(data);
		if(hdr[1] != 0){ /* RRSetConfigSuccess */
			fprint(2, "xbridge: RandR config to %dx%d failed: status %d\n",
				width, height, hdr[1]);
			return 0;
		}
	}
	memset(req, 0, sizeof req);
	req[0] = 14; /* GetGeometry */
	p32(req+4, x->root);
	xsend(x, req, 8);
	data = xreply(x, hdr, &n);
	if(data == nil){
		fprint(2, "xbridge: RandR resize to %dx%d was refused (X error %d)\n",
			width, height, tolerateerr);
		return 0;
	}
	free(data);
	width = x16(hdr+16);
	height = x16(hdr+18);
	if(width < 1 || height < 1 || width*height > 4*1024*1024)
		return 0;
	if(width == x->width && height == x->height)
		return 0;
	x->width = width;
	x->height = height;
	return 1;
}
