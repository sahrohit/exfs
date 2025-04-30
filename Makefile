all:
	gcc main.c -o exfs2

reset:
	rm -f dataseg{0..100} inodeseg{0..100}

clean:
	rm -f exfs2 dataseg{0..100} inodeseg{0..100}