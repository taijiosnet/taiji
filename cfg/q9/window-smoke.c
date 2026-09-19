#include <u.h>
#include <libc.h>
#include <draw.h>
#include "rill_platform.h"

static const RillPlatformServices *platform;
static RillTask tasks[8];
static int ids[2];

static void
check(int ok, char *message)
{
	if(!ok){
		fprint(2, "taiji-window-failed: %s: %r\n", message);
		exits("failed");
	}
}

static int
windowfile(int id, char *name, int mode)
{
	char path[128];

	snprint(path, sizeof path, "/dev/wsys/%d/%s", id, name);
	return open(path, mode);
}

static void
control(int id, char *command)
{
	int fd;

	fd = windowfile(id, "wctl", OWRITE);
	check(fd >= 0, "open control");
	check(write(fd, command, strlen(command)) == strlen(command), command);
	close(fd);
}

static int
state(int id, char *wanted)
{
	char buf[128];
	int attempt, fd, n;

	for(attempt=0; attempt<50; attempt++){
		fd = windowfile(id, "winfo", OREAD);
		if(fd >= 0){
			n = read(fd, buf, sizeof(buf)-1);
			close(fd);
			if(n > 0){
				buf[n] = 0;
				if(strstr(buf, wanted) != nil)
					return 1;
			}
		}
		sleep(20);
	}
	return 0;
}

void
main(void)
{
	int fd, held, mousefd, i, n, pixels;
	char buf[128];
	uchar black[4], covered[4], sample[4];

	check(initdraw(nil, nil, "Desktop test") >= 0, "open desktop image");
	mousefd = open("/dev/mouse", OREAD);
	check(mousefd >= 0, "mark desktop as a graphical client");
	draw(screen, screen->r, display->black, nil, ZP);
	flushimage(display, 1);
	platform = RillPlatformCurrent();
	check(platform->list_tasks(tasks, 8) == 0, "exclude desktop");
	fd = open("/dev/wctl", OWRITE);
	check(fd >= 0, "desktop control");
	check(fprint(fd, "new -r 40 60 440 360 sleep 120") > 0, "first window");
	check(fprint(fd, "new -r 200 100 600 400 sleep 120") > 0, "second window");
	close(fd);
	for(i=0; i<50 && platform->list_tasks(tasks, 8)!=2; i++)
		sleep(20);
	check(platform->list_tasks(tasks, 8) == 2, "list both windows");
	ids[0] = tasks[0].id;
	ids[1] = tasks[1].id;
	/* Refnone clients repaint after other windows change visibility. */
	draw(screen, screen->r, display->black, nil, ZP);
	flushimage(display, 1);
	pixels = unloadimage(display->image, Rect(5, 5, 6, 6), black, sizeof black);
	check(pixels > 0, "sample desktop");
	check(unloadimage(display->image, Rect(50, 100, 51, 101), covered, sizeof covered) == pixels,
		"sample application");
	check(memcmp(black, covered, pixels) != 0, "application covers desktop");
	fd = open("/dev/wctl", OWRITE);
	check(fd >= 0, "overlay control");
	check(fprint(fd, "overlay 0 0 100 150") > 0, "publish desktop popup");
	check(unloadimage(display->image, Rect(50, 100, 51, 101), sample, sizeof sample) == pixels &&
		memcmp(black, sample, pixels) == 0, "popup renders over application");
	check(fprint(fd, "overlay 0 bad 100 150") < 0, "reject malformed popup");
	check(platform->focus_task(ids[0]), "activate below popup");
	check(unloadimage(display->image, Rect(50, 100, 51, 101), sample, sizeof sample) == pixels &&
		memcmp(black, sample, pixels) == 0, "popup stays above focused application");
	check(fprint(fd, "overlay") > 0, "dismiss popup");
	check(unloadimage(display->image, Rect(50, 100, 51, 101), sample, sizeof sample) == pixels &&
		memcmp(covered, sample, pixels) == 0, "dismissal reveals application");
	close(fd);
	fd = windowfile(ids[0], "wctl", OWRITE);
	check(fd >= 0 && fprint(fd, "overlay 0 0 100 100") < 0, "reject application overlay");
	close(fd);
	fd = windowfile(ids[0], "label", OWRITE);
	check(fd >= 0, "open label");
	check(write(fd, "Shared editor", 13) == 13, "set title");
	close(fd);
	/* An application owns its wctl reader throughout taskbar polling. */
	held = windowfile(ids[0], "wctl", OREAD);
	check(held >= 0 && read(held, buf, sizeof buf) > 0, "own resize stream");
	check(platform->focus_task(ids[0]), "activate visible window");
	check(state(ids[0], " current visible window"), "visible window focused");
	n = platform->list_tasks(tasks, 8);
	check(n == 2, "poll while resize reader held");
	for(i=0; i<n && tasks[i].id!=ids[0]; i++)
		;
	check(i<n && tasks[i].focused && strcmp(tasks[i].title, "Shared editor")==0,
		"title and focus snapshot");
	fd = windowfile(ids[0], "wctl", OREAD);
	check(fd < 0, "original resize reader still owns stream");
	close(held);
	control(ids[0], "hide");
	check(state(ids[0], " hidden window"), "hide window");
	check(platform->list_tasks(tasks, 8) == 2, "hidden task retained");
	check(platform->focus_task(ids[0]), "activate hidden window");
	check(state(ids[0], " current visible window"), "restore hidden window");
	check(platform->focus_task(ids[1]), "switch windows");
	check(state(ids[1], " current visible window"), "second window focused");
	check(state(ids[0], " notcurrent visible window"), "first window unfocused");
	check(!platform->focus_task(999999), "reject missing window");
	for(i=0; i<2; i++)
		control(ids[i], "delete");
	for(i=0; i<50 && platform->list_tasks(tasks, 8)!=0; i++)
		sleep(20);
	check(platform->list_tasks(tasks, 8) == 0, "remove closed tasks");
	close(mousefd);
	print("taiji-window-smoke-ok\n");
	exits(nil);
}
