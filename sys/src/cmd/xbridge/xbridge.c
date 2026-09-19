#include <u.h>
#include <libc.h>
#include <draw.h>
#include <thread.h>
#include <mouse.h>
#include <keyboard.h>
#include "x11.h"

enum { Cliplimit = 1024*1024, Snarfchunk = 4096, Titleheight = 28 };
int mainstacksize = 64*1024;
static Xconn x;
static Image *frame;
static Image *titlecolor;
static char *title;
static ulong clipwin, clipboard, utf8, targets, property, incratom;
static char *cliptext;
static int graphical, buttons, pending;
static ulong requested;
static int which;
static vlong lastclip;
static vlong clipdeadline;
/* INCR selection transfer in progress: the owner streams the value onto
 * our property window in chunks we consume through PropertyNotify. */
static uchar *incrb;
static int incrn, incrcap;
static char *snarffile = "/dev/snarf";
static char apppath[64];
static Mousectl *inputmouse;
static Keyboardctl *inputkeyboard;
static int timerpid;

/* Stop the I/O workers before tearing down their shared display and file
 * descriptors.  Killing through proc control also interrupts blocked reads. */
static void
finish(char *status)
{
	char path[64];
	int pids[3], i, fd;

	pids[0] = inputmouse == nil ? 0 : inputmouse->pid;
	pids[1] = inputkeyboard == nil ? 0 : inputkeyboard->pid;
	pids[2] = timerpid;
	for(i = 0; i < nelem(pids); i++)
		if(pids[i] > 0){
			snprint(path, sizeof path, "/proc/%d/ctl", pids[i]);
			fd = open(path, OWRITE);
			if(fd >= 0){
				write(fd, "kill", 4);
				close(fd);
			}
		}
	threadexitsall(status);
}

static int
timeout(void*, char *note)
{
	if(strcmp(note, "alarm") != 0)
		return 0;
	fprint(2, "xbridge: X server timed out\n");
	finish("timeout");
	return 1;
}

static void
closeconnection(void)
{
	xclose(&x);
}

static char*
readsnarf(void)
{
	char *s;
	int fd, n, k;

	fd = open(snarffile, OREAD);
	if(fd < 0)
		return nil;
	s = malloc(Cliplimit+1);
	if(s == nil)
		sysfatal("out of memory");
	n = 0;
	while(n < Cliplimit && (k = read(fd, s+n, Cliplimit-n)) > 0)
		n += k;
	close(fd);
	if(n == Cliplimit){
		free(s);
		return nil;
	}
	s[n] = 0;
	return s;
}

static void
writesnarf(char *s)
{
	int fd, i, n, k, total;

	fd = open(snarffile, OWRITE|OTRUNC);
	if(fd < 0)
		return;
	n = strlen(s);
	total = 0;
	/* The 9P mount behind /dev/snarf carries bounded writes; the
	 * window server accumulates them until the file is closed. */
	for(i = 0; i < n; i += Snarfchunk){
		k = n-i < Snarfchunk ? n-i : Snarfchunk;
		k = write(fd, s+i, k);
		if(k <= 0)
			break;
		total += k;
	}
	close(fd);
	if(total == n){
		free(cliptext);
		cliptext = strdup(s);
	}
}

static void
incrreset(void)
{
	free(incrb);
	incrb = nil;
	incrn = incrcap = 0;
}

static void
clipinit(void)
{
	uchar req[32];

	clipboard = xatom(&x, "CLIPBOARD");
	utf8 = xatom(&x, "UTF8_STRING");
	targets = xatom(&x, "TARGETS");
	property = xatom(&x, "CLIPBOARD_DATA");
	incratom = xatom(&x, "INCR");
	clipwin = x.base | (x.mask & -x.mask);
	memset(req, 0, sizeof req);
	req[0] = 1; /* CreateWindow: unmapped InputOnly selection owner */
	p32(req+4, clipwin);
	p32(req+8, x.root);
	p16(req+16, 1);
	p16(req+18, 1);
	p16(req+22, 2);
	xsend(&x, req, sizeof req);
	/* PropertyNotify drives INCR chunk arrival on this window. */
	memset(req, 0, sizeof req);
	req[0] = 2; /* ChangeWindowAttributes */
	p32(req+4, clipwin);
	p32(req+8, 1<<11); /* CWEventMask */
	p32(req+12, 1<<22); /* PropertyChangeMask */
	xsend(&x, req, 16);
}

static void
selectionrequest(uchar *ev)
{
	uchar notify[44], atoms[12], *data;
	ulong requestor, sel, target, prop;
	int n, i, k;
	Rune r;

	requestor = x32(ev+12);
	sel = x32(ev+16);
	target = x32(ev+20);
	prop = x32(ev+24);
	if(prop == 0)
		prop = target; /* ICCCM's obsolete clients */
	if((sel != clipboard && sel != 1) || cliptext == nil)
		prop = 0;
	else if(target == targets){
		p32(atoms, targets);
		p32(atoms+4, utf8);
		p32(atoms+8, 31); /* STRING */
		xproperty(&x, requestor, prop, 4, 32, atoms, sizeof atoms);
	}else if(target == utf8)
		xproperty(&x, requestor, prop, utf8, 8, (uchar*)cliptext, strlen(cliptext));
	else if(target == 31){
		n = strlen(cliptext);
		data = malloc(n+1);
		if(data == nil)
			sysfatal("out of memory");
		for(i = k = 0; i < n; k++){
			i += chartorune(&r, cliptext+i);
			data[k] = r <= 255 ? r : '?';
		}
		xproperty(&x, requestor, prop, 31, 8, data, k);
		free(data);
	}else
		prop = 0;
	memset(notify, 0, sizeof notify);
	notify[0] = 25; /* SendEvent */
	p32(notify+4, requestor);
	notify[12] = 31; /* SelectionNotify */
	p32(notify+16, x32(ev+4));
	p32(notify+20, requestor);
	p32(notify+24, sel);
	p32(notify+28, target);
	p32(notify+32, prop);
	xsend(&x, notify, sizeof notify);
}

static void
selectionnotify(uchar *ev)
{
	ulong type, prop;
	uchar *data;
	char *s;
	int format, n, i, k;
	Rune r;

	if(!pending || x32(ev+8) != clipwin || x32(ev+12) != requested)
		return;
	prop = x32(ev+20);
	if(prop == 0 && x32(ev+16) == utf8){
		xconvert(&x, clipwin, requested, 31, property);
		return;
	}
	pending = 0;
	if(prop != property)
		return;
	data = xgetproperty(&x, clipwin, prop, &type, &format, &n);
	if(data == nil)
		return;
	if(format == 32 && type == incratom && n >= 4){
		/* The owner streams the real value in PropertyNotify chunks. */
		incrreset();
		incrcap = 64*1024;
		incrb = malloc(incrcap);
		if(incrb == nil)
			sysfatal("out of memory");
		incrn = 0;
		pending = 2;
		clipdeadline = nsec()+60000000000LL;
		free(data);
		return;
	}
	/* Binary selections other than text are declined; never replace the
	 * desktop clipboard with protocol metadata. */
	if(format == 8 && type == utf8 && n < Cliplimit)
		writesnarf((char*)data);
	else if(format == 8 && type == 31 && n < Cliplimit/UTFmax){
		s = malloc(n*UTFmax+1);
		if(s == nil)
			sysfatal("out of memory");
		for(i = k = 0; i < n; i++){
			r = data[i];
			k += runetochar(s+k, &r);
		}
		s[k] = 0;
		writesnarf(s);
		free(s);
	}
	free(data);
}

static void
propertynotify(uchar *ev)
{
	uchar *data;
	ulong type, win, atom;
	int format, n;

	if(pending != 2)
		return;
	win = x32(ev+4);
	atom = x32(ev+8);
	if(win != clipwin || atom != property || ev[1] != 0) /* NewValue only */
		return;
	data = xgetproperty(&x, clipwin, property, &type, &format, &n);
	if(data == nil)
		return;
	if(n == 0){
		/* Zero-length chunk closes the INCR transfer. */
		free(data);
		if(incrn > 0 && incrn < Cliplimit && format == 8){
			incrb[incrn] = 0;
			writesnarf((char*)incrb);
		}
		incrreset();
		pending = 0;
		return;
	}
	if(format == 8 && type == utf8){
		if(incrn+n > incrcap){
			while(incrn+n > incrcap)
				incrcap *= 2;
			incrb = realloc(incrb, incrcap);
			if(incrb == nil)
				sysfatal("out of memory");
		}
		if(incrn+n < Cliplimit){
			memmove(incrb+incrn, data, n);
			incrn += n;
		}
	}
	free(data);
	clipdeadline = nsec()+60000000000LL;
}

static void
clipevents(void)
{
	uchar ev[32];
	int budget;

	for(budget = 0; x.nevents > 0 && budget < 64; budget++){
		memmove(ev, x.events[0], 32);
		memmove(x.events[0], x.events[1], --x.nevents*32);
		switch(ev[0]&127){
		case 28: propertynotify(ev); break;
		case 30: selectionrequest(ev); break;
		case 31: selectionnotify(ev); break;
		}
	}
}

static void
clipboardpoll(void)
{
	char *s;
	ulong owner, sel;

	s = readsnarf();
	if(s != nil){
		if(cliptext == nil || strcmp(cliptext, s) != 0){
			free(cliptext);
			cliptext = s;
			incrreset();
			xown(&x, clipwin, clipboard);
			xown(&x, clipwin, 1);
			pending = 0;
		}else
			free(s);
	}
	if(nsec()-lastclip < 500000000LL)
		return;
	lastclip = nsec();
	if(pending && nsec() < clipdeadline)
		return;
	if(pending == 2)
		incrreset(); /* the owner stalled; drop the partial transfer */
	pending = 0;
	which ^= 1;
	sel = which ? clipboard : 1;
	owner = xowner(&x, sel);
	if(owner != 0 && owner != clipwin){
		requested = sel;
		pending = 1;
		clipdeadline = nsec()+3000000000LL;
		xconvert(&x, clipwin, sel, utf8, property);
	}
}

static void
hidewindow(void)
{
	int fd;

	fd = open("/dev/wctl", OWRITE);
	if(fd >= 0){
		write(fd, "hide", 4);
		close(fd);
	}
}

static void
paint(void)
{
	Point origin;
	Rectangle bar;

	if(!graphical || frame == nil)
		return;
	draw(screen, screen->r, display->black, nil, ZP);
	bar = screen->r;
	bar.max.y = bar.min.y+Titleheight;
	draw(screen, bar, titlecolor, nil, ZP);
	string(screen, addpt(bar.min, Pt(8, 6)), display->white, ZP, font, title);
	string(screen, Pt(bar.max.x-20, bar.min.y+6), display->white, ZP, font, "x");
	string(screen, Pt(bar.max.x-44, bar.min.y+6), display->white, ZP, font, "_");
	origin = addpt(screen->r.min, Pt(0, Titleheight));
	draw(screen, rectaddpt(frame->r, origin), frame, nil, ZP);
	flushimage(display, 1);
}

/* Follow the native window's size: resize the X screen through RandR and
 * rebuild the frame image.  Small differences are ignored to keep the
 * letterbox stable; failures keep the previous size. */
static void refresh(void);

static void
fitdisplay(void)
{
	int w, h;

	if(frame == nil)
		return;
	w = Dx(screen->r);
	h = Dy(screen->r)-Titleheight;
	if(w < 160)
		w = 160;
	if(h < 120)
		h = 120;
	if(abs(w-x.width) < 8 && abs(h-x.height) < 8)
		return;
	if(!xresize(&x, w, h))
		return;
	freeimage(frame);
	frame = allocimage(display, Rect(0, 0, x.width, x.height), XRGB32, 0, DBlack);
	if(frame == nil)
		sysfatal("allocate application image: %r");
	refresh();
}

void
eresized(int new)
{
	if(new && getwindow(display, Refnone) < 0)
		sysfatal("resize: %r");
	fitdisplay();
	paint();
}

static void
refresh(void)
{
	uchar *pixels;
	int n;

	if(apppath[0] && access(apppath, AEXIST) < 0)
		finish(nil);
	pixels = ximage(&x, &n);
	if(loadimage(frame, frame->r, pixels, n) != n)
		sysfatal("load X image: %r");
	free(pixels);
	clipevents();
	clipboardpoll();
	paint();
}

static void
mouse(Mouse m)
{
	Point p;
	int i, changed;

	p = subpt(m.xy, screen->r.min);
	if((m.buttons&1) && !(buttons&1) && p.y >= 0 && p.y < Titleheight &&
	   p.x >= Dx(screen->r)-52 && p.x < Dx(screen->r)-28){
		/* Minimize: the taskbar restores the window on demand. */
		hidewindow();
		buttons = m.buttons;
		return;
	}
	if((m.buttons&1) && !(buttons&1) && p.y >= 0 && p.y < Titleheight &&
	   p.x >= Dx(screen->r)-28){
		if(!xrequestclose(&x))
			fprint(2, "xbridge: use the application's Quit command\n");
		buttons = m.buttons;
		return;
	}
	p.y -= Titleheight;
	if(p.x < 0) p.x = 0;
	if(p.y < 0) p.y = 0;
	if(p.x >= x.width) p.x = x.width-1;
	if(p.y >= x.height) p.y = x.height-1;
	xinput(&x, 6, 0, p.x, p.y);
	changed = (buttons ^ m.buttons)&7;
	for(i = 0; i < 3; i++)
		if(changed & (1<<i))
			xinput(&x, (m.buttons&(1<<i)) ? 4 : 5, i+1, 0, 0);
	if((m.buttons&8) && !(buttons&8)){
		xinput(&x, 4, 4, 0, 0);
		xinput(&x, 5, 4, 0, 0);
	}
	if((m.buttons&16) && !(buttons&16)){
		xinput(&x, 4, 5, 0, 0);
		xinput(&x, 5, 5, 0, 0);
	}
	buttons = m.buttons;
}

static void
snapshot(char *path)
{
	uchar *pixels, *rgb;
	int n, fd, i, j;

	pixels = ximage(&x, &n);
	rgb = malloc(x.width*x.height*3);
	if(rgb == nil)
		sysfatal("out of memory");
	for(i = j = 0; i < n; i += 4){
		rgb[j++] = pixels[i+2];
		rgb[j++] = pixels[i+1];
		rgb[j++] = pixels[i];
	}
	fd = create(path, OWRITE, 0666);
	if(fd < 0 || fprint(fd, "P6\n%d %d\n255\n", x.width, x.height) < 0 ||
	   write(fd, rgb, j) != j)
		sysfatal("write screenshot: %r");
	close(fd);
	free(rgb);
	free(pixels);
	fprint(2, "xbridge: captured %dx%d\n", x.width, x.height);
}

static void
usage(void)
{
	fprint(2, "usage: xbridge [-T] [-p app-pid] [-t title] [-s screenshot.ppm] socket-path\n");
	threadexitsall("usage");
}

/* Exercise the bridge against a real X server and an independent client.
 * The client owns a window, receives input and serves/requests selections.
 * Use a private snarf file so this check never changes the user's clipboard. */
static void
selftest(char *socket)
{
	Xconn peer;
	uchar req[44], *data, *ev, *big;
	char path[128], *s;
	ulong win, type, prop, requestorwin;
	int fd, n, format, i, key, button, request, bign, off, len;
	char *outgoing = "native clipboard: caf\xc3\xa9";
	char *incoming = "X11 clipboard: ni\xc3\xb1o";

	snprint(path, sizeof path, "/tmp/xbridge-snarf.%d", getpid());
	snarffile = path;
	fd = create(path, OWRITE, 0600);
	if(fd < 0 || write(fd, outgoing, strlen(outgoing)) != strlen(outgoing))
		sysfatal("test snarf: %r");
	close(fd);
	clipinit();
	clipboardpoll();
	xconnect(&peer, socket);
	win = peer.base | (peer.mask & -peer.mask);
	memset(req, 0, sizeof req);
	req[0] = 1;
	p32(req+4, win);
	p32(req+8, peer.root);
	p16(req+16, 24);
	p16(req+18, 24);
	p16(req+22, 1);
	p32(req+28, (1<<1)|(1<<11));
	p32(req+32, 0x55aabb);
	p32(req+36, 0x4f);
	xsend(&peer, req, 40);
	memset(req, 0, sizeof req);
	req[0] = 8;
	p32(req+4, win);
	xsend(&peer, req, 8);
	memset(req, 0, sizeof req);
	req[0] = 42;
	p32(req+4, win);
	xsend(&peer, req, 12);
	xowner(&peer, clipboard); /* ensure the window is mapped */
	xinput(&x, 6, 0, 8, 8);
	xinput(&x, 4, 1, 0, 0);
	xinput(&x, 5, 1, 0, 0);
	xkey(&x, 'A');
	xowner(&x, clipboard);
	xowner(&peer, clipboard);
	key = button = 0;
	for(i = 0; i < peer.nevents; i++){
		if(peer.events[i][0] == 2) key++;
		if(peer.events[i][0] == 4) button++;
	}
	if(key == 0 || button != 1)
		sysfatal("X input test: key=%d button=%d", key, button);
	peer.nevents = 0;
	fprint(2, "xbridge: keyboard and pointer test passed\n");

	xconvert(&peer, win, clipboard, utf8, property);
	xowner(&peer, clipboard);
	xowner(&x, clipboard);
	clipevents();
	xowner(&x, clipboard);
	xowner(&peer, clipboard);
	data = xgetproperty(&peer, win, property, &type, &format, &n);
	if(data == nil || type != utf8 || format != 8 || strcmp((char*)data, outgoing) != 0)
		sysfatal("native-to-X clipboard test failed");
	free(data);
	peer.nevents = 0;
	fprint(2, "xbridge: native-to-X clipboard test passed\n");

	xown(&peer, win, clipboard);
	xowner(&peer, clipboard);
	lastclip = 0;
	which = 0;
	clipboardpoll();
	xowner(&x, clipboard);
	xowner(&peer, clipboard);
	request = 0;
	for(i = 0; i < peer.nevents; i++){
		ev = peer.events[i];
		if((ev[0]&127) != 30) continue;
		prop = x32(ev+24);
		xproperty(&peer, x32(ev+12), prop, utf8, 8, (uchar*)incoming, strlen(incoming));
		memset(req, 0, sizeof req);
		req[0] = 25;
		p32(req+4, x32(ev+12));
		req[12] = 31;
		p32(req+16, x32(ev+4));
		memmove(req+20, ev+12, 16);
		xsend(&peer, req, 44);
		request++;
	}
	xowner(&peer, clipboard);
	xowner(&x, clipboard);
	clipevents();
	s = readsnarf();
	if(request != 1 || s == nil || strcmp(s, incoming) != 0)
		sysfatal("X-to-native clipboard test failed");
	free(s);
	peer.nevents = 0;
	fprint(2, "xbridge: X-to-native clipboard test passed\n");

	/* Large transfers cross the ChangeProperty request cap when serving
	 * and the bounded 9P writes when taking the native clipboard. */
	bign = 300*1024;
	big = malloc(bign+1);
	if(big == nil)
		sysfatal("out of memory");
	for(i = 0; i < bign; i++)
		big[i] = 'a'+(i%26);
	big[bign] = 0;
	fd = create(path, OWRITE, 0600);
	if(fd < 0 || write(fd, big, bign) != bign)
		sysfatal("test snarf: %r");
	close(fd);
	lastclip = 0;
	which = 0;
	clipboardpoll();
	xconvert(&peer, win, clipboard, utf8, property);
	xowner(&peer, clipboard);
	xowner(&x, clipboard);
	clipevents();
	xowner(&x, clipboard);
	xowner(&peer, clipboard);
	data = xgetproperty(&peer, win, property, &type, &format, &n);
	if(data == nil || type != utf8 || format != 8 || n != bign ||
	   memcmp(data, big, bign) != 0)
		sysfatal("large native-to-X clipboard test failed");
	free(data);
	peer.nevents = 0;
	fprint(2, "xbridge: large native-to-X clipboard test passed\n");

	xown(&peer, win, clipboard);
	xowner(&peer, clipboard);
	lastclip = 0;
	which = 0;
	clipboardpoll();
	xowner(&x, clipboard);
	xowner(&peer, clipboard);
	request = 0;
	for(i = 0; i < peer.nevents; i++){
		ev = peer.events[i];
		if((ev[0]&127) != 30) continue;
		prop = x32(ev+24);
		xproperty(&peer, x32(ev+12), prop, utf8, 8, (uchar*)big, bign);
		memset(req, 0, sizeof req);
		req[0] = 25;
		p32(req+4, x32(ev+12));
		req[12] = 31;
		p32(req+16, x32(ev+4));
		memmove(req+20, ev+12, 16);
		xsend(&peer, req, 44);
		request++;
	}
	xowner(&peer, clipboard);
	xowner(&x, clipboard);
	clipevents();
	s = readsnarf();
	if(request != 1 || s == nil || strlen(s) != bign || memcmp(s, big, bign) != 0)
		sysfatal("large X-to-native clipboard test failed");
	free(s);
	peer.nevents = 0;
	fprint(2, "xbridge: large X-to-native clipboard test passed\n");

	/* INCR: the owner streams the value in PropertyNotify chunks instead
	 * of one property write. */
	xown(&peer, win, clipboard);
	xowner(&peer, clipboard);
	lastclip = 0;
	which = 0;
	clipboardpoll();
	xowner(&x, clipboard);
	xowner(&peer, clipboard);
	request = requestorwin = 0;
	prop = 0;
	for(i = 0; i < peer.nevents; i++){
		ev = peer.events[i];
		if((ev[0]&127) != 30) continue;
		requestorwin = x32(ev+12);
		prop = x32(ev+24);
		memset(req, 0, sizeof req);
		p32(req, bign);
		xproperty(&peer, requestorwin, prop, incratom, 32, req, 4);
		memset(req, 0, sizeof req);
		req[0] = 25;
		p32(req+4, requestorwin);
		req[12] = 31;
		p32(req+16, x32(ev+4));
		p32(req+20, requestorwin);
		p32(req+24, incratom);
		p32(req+28, prop);
		xsend(&peer, req, 44);
		request++;
	}
	if(request != 1 || requestorwin == 0)
		sysfatal("INCR test did not receive a selection request");
	for(off = 0; off < bign; off += 65536){
		len = bign-off < 65536 ? bign-off : 65536;
		xproperty(&peer, requestorwin, prop, utf8, 8, (uchar*)big+off, len);
		xowner(&x, clipboard); /* drain events toward the bridge */
		clipevents();
	}
	xproperty(&peer, requestorwin, prop, utf8, 8, (uchar*)"", 0);
	xowner(&x, clipboard);
	clipevents();
	s = readsnarf();
	if(s == nil || strlen(s) != bign || memcmp(s, big, bign) != 0)
		sysfatal("INCR clipboard test failed");
	free(s);
	free(big);
	peer.nevents = 0;
	fprint(2, "xbridge: INCR clipboard test passed\n");
	type = xatom(&peer, "WM_PROTOCOLS");
	prop = xatom(&peer, "WM_DELETE_WINDOW");
	p32(req, prop);
	xproperty(&peer, win, type, 4, 32, req, 4);
	xowner(&peer, clipboard);
	if(xrequestclose(&x) < 1)
		sysfatal("close request not sent");
	xowner(&x, clipboard);
	xowner(&peer, clipboard);
	request = 0;
	for(i = 0; i < peer.nevents; i++){
		ev = peer.events[i];
		if((ev[0]&127) == 33 && ev[1] == 32 && x32(ev+4) == win &&
		   x32(ev+8) == type && x32(ev+12) == prop)
			request++;
	}
	if(request != 1)
		sysfatal("application close protocol test failed");
	xclose(&peer);
	remove(path);
	fprint(2, "xbridge: X-to-native clipboard test passed\n");
	fprint(2, "xbridge: application close protocol test passed\n");

	/* RandR screen resizing: the bridge follows its native window.  The
	 * X server caps the range at its start size and may refuse mode
	 * changes entirely; both are acceptable, the bridge letterboxes. */
	if(!xresize(&x, 400, 300))
		fprint(2, "xbridge: RandR resize not supported by this X server\n");
	else if(x.width != 400 || x.height != 300)
		sysfatal("RandR resize reported %dx%d", x.width, x.height);
	else{
		if(!xresize(&x, 640, 480))
			sysfatal("RandR resize back to 640x480 failed");
		if(x.width != 640 || x.height != 480)
			sysfatal("RandR restore reported %dx%d", x.width, x.height);
		fprint(2, "xbridge: RandR resize test passed\n");
	}
}

static void
tickproc(void *arg)
{
	Channel *ticks;

	ticks = arg;
	timerpid = getpid();
	for(;;){
		sleep(150);
		sendul(ticks, 1);
	}
}

void
threadmain(int argc, char **argv)
{
	Mousectl *mc;
	Keyboardctl *kc;
	Channel *ticks;
	Alt events[5];
	Rune key;
	char *shot;
	ulong tick;
	int test, apppid, resized;

	title = "Debian application";
	shot = nil;
	test = 0;
	ARGBEGIN{
	case 't': title = EARGF(usage()); break;
	case 'p':
		apppid = atoi(EARGF(usage()));
		if(apppid <= 0) usage();
		snprint(apppath, sizeof apppath, "/proc/%d", apppid);
		break;
	case 's': shot = EARGF(usage()); break;
	case 'T': test = 1; break;
	default: usage();
	}ARGEND
	if(argc != 1)
		usage();
	x.rd = x.wr = -1;
	atexit(closeconnection);
	threadnotify(timeout, 1);
	xconnect(&x, argv[0]);
	if(test){
		selftest(argv[0]);
		threadexitsall(nil);
	}
	if(shot != nil){
		snapshot(shot);
		threadexitsall(nil);
	}
	if(initdraw(nil, nil, title) < 0)
		sysfatal("open desktop window: %r");
	graphical = 1;
	titlecolor = allocimage(display, Rect(0, 0, 1, 1), RGB24, 1, 0x262a3aff);
	frame = allocimage(display, Rect(0, 0, x.width, x.height), XRGB32, 0, DBlack);
	if(frame == nil || titlecolor == nil)
		sysfatal("allocate application image: %r");
	clipinit();
	fitdisplay();
	mc = initmouse(nil, screen);
	kc = initkeyboard(nil);
	inputmouse = mc;
	inputkeyboard = kc;
	if(mc == nil || kc == nil)
		sysfatal("open application input: %r");
	ticks = chancreate(sizeof(ulong), 1);
	proccreate(tickproc, ticks, 8192);
	memset(events, 0, sizeof events);
	events[0].c = mc->c;
	events[0].v = &mc->Mouse;
	events[1].c = kc->c;
	events[1].v = &key;
	events[2].c = mc->resizec;
	events[2].v = &resized;
	events[3].c = ticks;
	events[3].v = &tick;
	for(test = 0; test < 4; test++)
		events[test].op = CHANRCV;
	events[4].op = CHANEND;
	refresh();
	fprint(2, "xbridge: window ready\n");
	for(;;){
		switch(alt(events)){
		case 0: mouse(mc->Mouse); break;
		case 1: xkey(&x, key); break;
		case 2: eresized(1); mc->image = screen; break;
		case 3: refresh(); break;
		}
	}
}
