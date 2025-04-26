all:
	gcc main.c -o exfs2

reference:
	gcc reference.c -o reference

reference2:
	gcc reference2.c -o reference2

clean:
	rm -f exfs2 dataseg0 dataseg1 dataseg2 dataseg3 dataseg4 dataseg5 dataseg6 dataseg7 dataseg8 dataseg9 inodeseg0 inodeseg1 inodeseg2 inodeseg3 inodeseg4 inodeseg5 inodeseg6 inodeseg7 inodeseg8 inodeseg9 
