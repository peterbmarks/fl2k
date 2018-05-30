// dump a binary data file with signed bytes
// stop after kMaxBytesToDump
// gcc dumpFile.c -o dumpFile
// ./dumpFile /media/marksp/DeveloperDisk/Audio/am_out.dat >plotme.txt
//
// gnuplot
// gnuplot> plot "plotme.txt"

#include <stdio.h>
#include <stdlib.h>

const long kMaxBytesToDump = 100000;

int main(int argc, const char * argv[]) {
	if(argc < 2) {
		printf("Utility to dump signed byte binary files\n");
		printf("Usage %s <fileName>\n", argv[0]);
		exit(0);
	}
	const char *inFileName = argv[1];
	//printf("dumping: %s\n", inFileName);
	FILE *infp = fopen(inFileName, "rb");
	if(infp) {
		// get the length of the file
		fseek(infp, 0L, SEEK_END);
		long length = ftell(infp);
		// seek back to the start
		fseek(infp, 0L, SEEK_SET);
		if(length > kMaxBytesToDump) {
			length = kMaxBytesToDump;
		}
		char c;
		for(long position = 0; position < length; position++) {
			c = getc(infp);
			printf("%d\n", (int)c);
		}
		fclose(infp);
	}
	return 0;
}
