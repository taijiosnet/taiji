#include <u.h>
#include <libc.h>
#include <draw.h>
#include <thread.h>
#include <cursor.h>
#include <mouse.h>
#include <keyboard.h>
#include <frame.h>
#include <fcall.h>
#include "dat.h"

/* Desktop panels and popups share the desktop's image and input stream.
 * Only their declared rectangles sit above application windows. */
enum { Maxoverlay = 16 };
static Image *layers[Maxoverlay];
static int nlayers;
static Window *owner;

void
woverlayraise(void)
{
	int i;

	for(i=0; i<nlayers; i++)
		topwindow(layers[i]);
}

Window*
woverlaypoint(Point pt)
{
	int i;

	for(i=nlayers-1; i>=0; i--)
		if(ptinrect(pt, layers[i]->r))
			return owner;
	return nil;
}

void
woverlayclear(Window *w)
{
	int i;

	if(w != owner)
		return;
	for(i=0; i<nlayers; i++)
		freeimage(layers[i]);
	nlayers = 0;
	owner = nil;
}

int
woverlay(Window *w, char *s, char *err)
{
	char *words[4*Maxoverlay+1], *end;
	int xy[4], i, j, n, count;
	long coordinate;
	Rectangle rects[Maxoverlay], r;
	Image *next[Maxoverlay];

	if(!w->desktop || w->deleted || w->i == nil){
		strcpy(err, "only the desktop can publish overlays");
		return -1;
	}
	n = tokenize(s, words, nelem(words));
	if(n%4 != 0 || n > 4*Maxoverlay){
		strcpy(err, "overlay needs rectangles");
		return -1;
	}
	count = 0;
	for(i=0; i<n; i+=4){
		for(j=0; j<4; j++){
			coordinate = strtol(words[i+j], &end, 10);
			if(*end || end == words[i+j] || coordinate < -32768 || coordinate > 32767){
				strcpy(err, "bad overlay coordinate");
				return -1;
			}
			xy[j] = coordinate;
		}
		r = rectaddpt(Rect(xy[0], xy[1], xy[2], xy[3]), w->i->r.min);
		if(rectclip(&r, w->i->r) && rectclip(&r, screen->r))
			rects[count++] = r;
	}
	/* Allocate before replacing any visible layers. */
	for(i=0; i<count; i++){
		if(i<nlayers && eqrect(layers[i]->r, rects[i]))
			next[i] = layers[i];
		else{
			next[i] = allocwindow(wscreen, rects[i], Refbackup, DWhite);
			if(next[i] == nil){
				for(j=0; j<i; j++)
					if(j>=nlayers || next[j] != layers[j])
						freeimage(next[j]);
				strcpy(err, "cannot allocate desktop overlay");
				return -1;
			}
		}
	}
	for(i=0; i<nlayers; i++)
		if(i>=count || layers[i] != next[i])
			freeimage(layers[i]);
	nlayers = count;
	owner = count > 0 ? w : nil;
	for(i=0; i<count; i++){
		layers[i] = next[i];
		draw(layers[i], rects[i], w->i, nil, rects[i].min);
	}
	woverlayraise();
	flushimage(display, 1);
	return 1;
}
