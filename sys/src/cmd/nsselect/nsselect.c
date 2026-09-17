#include <u.h>
#include <libc.h>
#include "kryon.h"

extern void eshutdown(void);

/* Namespace selector for the TaijiOS boot.  Two stages: pick a
 * namespace from /lib/namespaces, then a window manager from the
 * /lib/wms entries that declare support for it, and launch the
 * manager's command through a pre-forked rc launcher that owns the
 * namespace/session environment.  A click only selects; a second click
 * of the highlighted card confirms, so a stray click meant to focus
 * the emulator cannot start a session by itself.  Esc goes back a
 * stage, and from the first stage exits with a non-empty status so the
 * boot profile can drop to a shell. */

enum {
	Maxns = 16,
	Maxwm = 32,
	Fieldlen = 128,
	Cmdlen = 256,
	Buflen = 8192,
	Cardw = 300,
	Cardh = 118,
	Cardgap = 22
};

typedef struct Ns Ns;
typedef struct Wm Wm;
typedef struct Launch Launch;
struct Ns {
	char id[Fieldlen];
	char name[Fieldlen];
	char desc[Fieldlen];
};
struct Wm {
	char ns[Fieldlen];
	char name[Fieldlen];
	char desc[Fieldlen];
	char command[Cmdlen];
};
struct Launch {
	char id[Fieldlen];
	char command[Cmdlen];
};

enum {
	StageNs,
	StageWm
};

Ns nss[Maxns];
int nns;
Wm wms[Maxwm];
int nwm;
int stage = StageNs;
int nssel;
int wmsel;
int launchfd = -1;
int launchpid = -1;

char *nspath = "/lib/namespaces";
char *wmpath = "/lib/wms";

void
trim(char *s)
{
	char *e;

	while(*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
		memmove(s, s+1, strlen(s));
	e = s+strlen(s);
	while(e > s && (e[-1] == ' ' || e[-1] == '\t' ||
	    e[-1] == '\r' || e[-1] == '\n'))
		*--e = 0;
}

void
copystr(char *dst, int ndst, char *src)
{
	int n;

	if(ndst <= 0)
		return;
	dst[0] = 0;
	if(src == nil)
		return;
	n = strlen(src);
	if(n >= ndst)
		n = ndst-1;
	memmove(dst, src, n);
	dst[n] = 0;
}

/* Split buf into | separated fields without modifying buf. */
int
splitfields(char *line, char *fields[], int nf)
{
	char *p, *e;
	int i, n;

	p = line;
	for(i = 0; i < nf; i++){
		e = strchr(p, '|');
		n = e != nil ? e - p : strlen(p);
		fields[i] = malloc(n+1);
		if(fields[i] == nil)
			return i;
		memmove(fields[i], p, n);
		fields[i][n] = 0;
		if(e == nil)
			return i+1;
		p = e+1;
	}
	return nf;
}

void
freefields(char *fields[], int nf)
{
	int i;

	for(i = 0; i < nf; i++)
		free(fields[i]);
}

int
loadnamespaces(char *path)
{
	char buf[Buflen+1], *lines[Maxns*2];
	char *f[8];
	int fd, n, nl, nf, i;

	fd = open(path, OREAD);
	if(fd < 0)
		return -1;
	n = read(fd, buf, Buflen);
	close(fd);
	if(n < 0)
		return -1;
	buf[n] = 0;
	nl = getfields(buf, lines, nelem(lines), 1, "\n");
	nns = 0;
	for(i = 0; i < nl && nns < Maxns; i++){
		trim(lines[i]);
		if(lines[i][0] == 0 || lines[i][0] == '#')
			continue;
		nf = splitfields(lines[i], f, 3);
		if(nf == 3){
			trim(f[0]);
			trim(f[1]);
			trim(f[2]);
			if(f[0][0] != 0 && f[1][0] != 0){
				copystr(nss[nns].id, sizeof nss[nns].id, f[0]);
				copystr(nss[nns].name, sizeof nss[nns].name, f[1]);
				copystr(nss[nns].desc, sizeof nss[nns].desc, f[2]);
				nns++;
			}
		}
		freefields(f, nf);
	}
	return nns;
}

int
loadwms(char *path)
{
	char buf[Buflen+1], *lines[Maxwm*2];
	char *f[8];
	int fd, n, nl, nf, i;

	fd = open(path, OREAD);
	if(fd < 0)
		return -1;
	n = read(fd, buf, Buflen);
	close(fd);
	if(n < 0)
		return -1;
	buf[n] = 0;
	nl = getfields(buf, lines, nelem(lines), 1, "\n");
	nwm = 0;
	for(i = 0; i < nl && nwm < Maxwm; i++){
		trim(lines[i]);
		if(lines[i][0] == 0 || lines[i][0] == '#')
			continue;
		nf = splitfields(lines[i], f, 4);
		if(nf == 4){
			trim(f[0]);
			trim(f[1]);
			trim(f[2]);
			trim(f[3]);
			if(f[0][0] != 0 && f[1][0] != 0 && f[3][0] != 0){
				copystr(wms[nwm].ns, sizeof wms[nwm].ns, f[0]);
				copystr(wms[nwm].name, sizeof wms[nwm].name, f[1]);
				copystr(wms[nwm].desc, sizeof wms[nwm].desc, f[2]);
				copystr(wms[nwm].command, sizeof wms[nwm].command, f[3]);
				nwm++;
			}
		}
		freefields(f, nf);
	}
	return nwm;
}

void
waitmousefree(void)
{
	int fd, i;

	for(i = 0; i < 50; i++){
		fd = open("/dev/mouse", ORDWR);
		if(fd >= 0){
			close(fd);
			return;
		}
		sleep(100);
	}
}

int
readfull(int fd, void *buf, int n)
{
	char *p;
	int r, total;

	p = buf;
	total = 0;
	while(total < n){
		r = read(fd, p + total, n - total);
		if(r <= 0)
			return total;
		total += r;
	}
	return total;
}

int
writefull(int fd, void *buf, int n)
{
	char *p;
	int r, total;

	p = buf;
	total = 0;
	while(total < n){
		r = write(fd, p + total, n - total);
		if(r <= 0)
			return total;
		total += r;
	}
	return total;
}

void
launcher(int fd)
{
	Launch l;

	memset(&l, 0, sizeof l);
	if(readfull(fd, &l, sizeof l) != sizeof l)
		exits(nil);
	close(fd);
	if(l.id[0] == 0 || l.command[0] == 0)
		exits("empty launch");
	putenv("namespace", l.id);
	putenv("session", l.id);
	waitmousefree();
	execl("/bin/rc", "rc", "-c", l.command, nil);
	sysfatal("exec %s failed: %r", l.command);
}

void
startlauncher(void)
{
	int fd[2], pid;

	if(pipe(fd) < 0)
		sysfatal("launcher pipe failed: %r");
	switch(pid = rfork(RFPROC|RFFDG|RFENVG|RFNOTEG)){
	case -1:
		sysfatal("launcher rfork failed: %r");
	case 0:
		close(fd[1]);
		launcher(fd[0]);
		exits(nil);
	default:
		close(fd[0]);
		launchfd = fd[1];
		launchpid = pid;
		break;
	}
}

void
waitsession(void)
{
	Waitmsg *w;

	while((w = wait()) != nil){
		if(w->pid == launchpid){
			if(w->msg[0] != 0)
				exits(w->msg);
			free(w);
			return;
		}
		free(w);
	}
}

Color
opaque_color(Color color)
{
	color.a = 255;
	return color;
}

int
wmlist(Wm *out, int cap)
{
	int i, n;

	n = 0;
	for(i = 0; i < nwm && n < cap; i++)
		if(strcmp(wms[i].ns, nss[nssel].id) == 0)
			out[n++] = wms[i];
	return n;
}

int
cards(int n)
{
	int cols;

	cols = n > 1 ? 2 : 1;
	return cols;
}

Rectangle
cardrect(int i, int n)
{
	int cols, rows, totalw, totalh;
	Rectangle r;

	cols = cards(n);
	rows = (n + cols - 1) / cols;
	totalw = cols*Cardw + (cols-1)*Cardgap;
	totalh = rows*Cardh + (rows-1)*Cardgap;
	r.x = (GetScreenWidth() - totalw)/2 + (i%cols)*(Cardw+Cardgap);
	r.y = (GetScreenHeight() - totalh)/2 + 24 + (i/cols)*(Cardh+Cardgap);
	r.width = Cardw;
	r.height = Cardh;
	return r;
}

void
drawcentered(char *s, int y, int size, Color color)
{
	int x;

	x = GetScreenWidth()/2 - MeasureText(s, size)/2;
	DrawText(s, x, y, size, color);
}

/* Draw a card.  Returns 1 when it is clicked this frame. */
int
drawcard(int i, int n, int selected, char *title, char *sub, char *tag)
{
	Rectangle r, stripe;
	Vector2 mouse;
	Color fill;
	int hover;

	r = cardrect(i, n);
	mouse = GetMousePosition();
	hover = CheckCollisionPointRec(mouse, r);
	if(i == selected)
		fill = opaque_color(GetThemeButton());
	else
		fill = opaque_color(GetThemeSurface());
	DrawRectangleRounded(r, 0.07f, 8, fill);
	if(hover && i != selected){
		DrawRectangleRounded(r, 0.07f, 8, Fade(GetThemeButtonHover(), 0.38f));
	}
	stripe = r;
	stripe.x += 14;
	stripe.y += 14;
	stripe.width = 44;
	stripe.height = 6;
	DrawRectangleRec(stripe, i == selected ?
	    opaque_color(GetThemeLink()) : Fade(GetThemeLink(), 0.55f));
	DrawText(title, (int)r.x + 16, (int)r.y + 34, 21,
	    opaque_color(GetThemeText()));
	DrawText(sub, (int)r.x + 16, (int)r.y + 64, 14,
	    Fade(GetThemeText(), 0.62f));
	DrawText(tag, (int)r.x + 16, (int)r.y + 86, 13,
	    Fade(GetThemeText(), 0.45f));
	return hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

void
startsession(Wm *wm)
{
	Launch l;

	memset(&l, 0, sizeof l);
	copystr(l.id, sizeof l.id, nss[nssel].id);
	copystr(l.command, sizeof l.command, wm->command);
	CloseWindow();
	if(launchfd < 0 || writefull(launchfd, &l, sizeof l) != sizeof l)
		sysfatal("launch %s failed: %r", wm->command);
	close(launchfd);
	launchfd = -1;
	/* release the mouse before the launched session starts: the
	 * launcher's waitmousefree() gives it 5s, then execs the WM
	 * anyway - einit would fail with 'already in use' */
	eshutdown();
	waitsession();
	exits(nil);
}

void
usage(void)
{
	fprint(2, "usage: nsselect [-n namespaces] [-w windowmanagers]\n");
	exits("usage");
}

void
main(int argc, char *argv[])
{
	Wm cur[Maxwm];
	int i, ncur, pick, blocktop, wasselected;

	ARGBEGIN{
	case 'n':
		nspath = EARGF(usage());
		break;
	case 'w':
		wmpath = EARGF(usage());
		break;
	default:
		usage();
	}ARGEND
	if(argc != 0)
		usage();

	if(loadnamespaces(nspath) <= 0)
		sysfatal("no namespaces in %s", nspath);
	if(loadwms(wmpath) <= 0)
		sysfatal("no window managers in %s", wmpath);
	startlauncher();

	SetSingleInstance(0);
	InitWindow(640, 480, "namespace");
	if(!IsWindowReady())
		sysfatal("kryon window failed: %r");
	EnableEventWaiting();
	SetTargetFPS(30);
	SetUIDefaultFontAutoLoad(1);
	RefreshSystemTheme();
	SetThemeSource(THEME_SOURCE_SYSTEM);
	SetThemeMode(THEME_MODE_SYSTEM);
	SetThemeStyle(THEME_STYLE_SYSTEM);
	SetCurrentTheme(GetDefaultThemeForThemeStyle(THEME_STYLE_SYSTEM),
	    SystemThemePrefersDark());
	ApplyCurrentUITheme();

	while(!WindowShouldClose()){
		ncur = wmlist(cur, nelem(cur));
		if(wmsel >= ncur)
			wmsel = 0;

		if(IsKeyPressed(KEY_LEFT) || IsKeyPressed(KEY_UP)){
			if(stage == StageNs && nssel > 0)
				nssel--;
			if(stage == StageWm && wmsel > 0)
				wmsel--;
		}
		if(IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_DOWN) ||
		    IsKeyPressed(KEY_TAB)){
			if(stage == StageNs && nssel < nns-1)
				nssel++;
			if(stage == StageWm && wmsel < ncur-1)
				wmsel++;
		}
		if(IsKeyPressed(KEY_ESCAPE)){
			if(stage == StageWm){
				stage = StageNs;
			}else{
				CloseWindow();
				exits("cancel");
			}
		}
		if(IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)){
			if(stage == StageNs)
				stage = StageWm;
			else
				startsession(&cur[wmsel]);
		}

		pick = -1;
		BeginDrawing();
		ClearBackground(opaque_color(GetThemeBackground()));
		BeginUIFrame(GetScreenWidth(), GetScreenHeight(), 1.0f);
		BeginUI(0x6e73656c);
		blocktop = GetScreenHeight()/2 - 24;
		if(stage == StageNs){
			drawcentered("Select namespace", blocktop - 96, 30,
			    opaque_color(GetThemeText()));
			drawcentered("Choose the TaijiOS namespace to start",
			    blocktop - 56, 15, Fade(GetThemeText(), 0.62f));
			wasselected = nssel;
			for(i = 0; i < nns; i++){
				if(drawcard(i, nns, i == nssel, nss[i].name,
				    nss[i].desc, nss[i].id)){
					if(i == wasselected)
						stage = StageWm;
					else
						nssel = i;
				}
			}
			drawcentered("Enter chooses  |  arrows move  |  Esc exits",
			    GetScreenHeight() - 46, 14, Fade(GetThemeText(), 0.55f));
		}else{
			drawcentered("Select window manager", blocktop - 96, 30,
			    opaque_color(GetThemeText()));
			drawcentered(nss[nssel].name, blocktop - 56, 15,
			    Fade(GetThemeText(), 0.62f));
			wasselected = wmsel;
			for(i = 0; i < ncur; i++){
				if(drawcard(i, ncur, i == wmsel, cur[i].name,
				    cur[i].desc, nss[nssel].id)){
					if(i == wasselected)
						pick = i;
					else
						wmsel = i;
				}
			}
			drawcentered("Enter or double-click starts  |  Esc goes back",
			    GetScreenHeight() - 46, 14, Fade(GetThemeText(), 0.55f));
		}
		EndUI();
		EndUIFrame();
		EndDrawing();
		if(pick >= 0)
			startsession(&cur[pick]);
	}
	CloseWindow();
	exits("cancel");
}
