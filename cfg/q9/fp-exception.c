#include <u.h>
#include <libc.h>

static void
fpnote(void*, char *note)
{
	if(strncmp(note, "sys: fp:", 8) == 0)
		exits(nil);
	noted(NDFLT);
}

void
main(void)
{
	int pid;
	Waitmsg *w;
	volatile double zero, result;

	pid = fork();
	if(pid < 0)
		sysfatal("fork: %r");
	if(pid == 0){
		notify(fpnote);
		zero = 0.0;
		result = zero / zero;
		print("unexpected floating point result: %g\n", result);
		exits("exception not delivered");
	}
	w = wait();
	if(w == nil || w->pid != pid || w->msg[0] != 0)
		sysfatal("floating point exception did not reach child note handler");
	free(w);
	zero = 3.5;
	result = zero * 2.0;
	if(result != 7.0)
		sysfatal("parent floating point state changed");
	print("taiji-fp-exception-ok\n");
	exits(nil);
}
