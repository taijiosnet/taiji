#include <u.h>
#include <libc.h>
#include "environment.h"

char *genv[Maxenv];
static char envstore[Envbytes];
static int nenv, used;

/* Keep the Linux PATH independent of rc's list-valued path.  Session
 * settings come from /env; callers can supply further values with -e. */
void
initenv(void)
{
	static char *names[] = {
		"HOME", "USER", "LOGNAME", "DISPLAY", "XAUTHORITY", "LANG",
		"LC_ALL", "TERM", "SHELL", "TMPDIR", "DBUS_SESSION_BUS_ADDRESS",
		"GDK_GL", "NO_AT_BRIDGE", "LD_LIBRARY_PATH", nil,
	};
	char *s, *entry;
	int i;

	setguestenv("PATH=/bin:/usr/bin:/sbin:/usr/sbin");
	setguestenv("HOME=/root");
	setguestenv("LD_BIND_NOW=1");
	for(i = 0; names[i] != nil; i++){
		s = getenv(names[i]);
		if(s == nil)
			continue;
		entry = smprint("%s=%s", names[i], s);
		if(entry == nil || setguestenv(entry) < 0)
			sysfatal("Linux environment too large");
		free(entry);
		free(s);
	}
}

int
setguestenv(char *s)
{
	char *eq;
	int i, n, len;

	eq = strchr(s, '=');
	if(eq == nil || eq == s)
		return -1;
	n = eq-s+1;
	len = strlen(s)+1;
	for(i = 0; i < nenv; i++)
		if(strncmp(genv[i], s, n) == 0)
			break;
	if(i >= Maxenv-1 || used+len > sizeof envstore)
		return -1;
	genv[i] = envstore+used;
	memmove(genv[i], s, len);
	used += len;
	if(i == nenv)
		genv[++nenv] = nil;
	return 0;
}

/* execve's environment belongs to the old guest image.  Copy it before
 * that image is detached, and reject overflow instead of dropping entries. */
int
copyguestenv(char **src, char **dst, char *buf, int size)
{
	int i, len, off;

	off = 0;
	for(i = 0; src != nil && src[i] != nil; i++){
		if(i >= Maxenv-1)
			return -1;
		len = strlen(src[i])+1;
		if(len > size-off)
			return -1;
		dst[i] = buf+off;
		memmove(dst[i], src[i], len);
		off += len;
	}
	dst[i] = nil;
	return 0;
}
