enum {
	Maxargs = 256,
	Maxenv = 256,
	Envbytes = 32768,
};

extern char *genv[Maxenv];
void initenv(void);
int setguestenv(char*);
int copyguestenv(char**, char**, char*, int);
