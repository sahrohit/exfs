#include<stdio.h>

// Your program should allow ﬁles to be added to your ﬁle system, read, and removed from it, and for the contents of the ﬁle system to be listed. It might be easiest to describe the intended functionality by providing examples of each. For example, the following command line invocation should list the contents of your ﬁle system starting with the root directory:
// ./exfs2 -l

void listFiles(const char *path) {
    // This function would list files in the given path
    // Implementation is not provided here
}

// In this example, the ﬁle a/b/c/example should be removed from your ﬁle system if it exists. Leave the directories even if they’re empty. If the end of the provided path is a directory and not a regular ﬁle, then remove the directory and all ﬁles contained within it.
// The following invocation would read the contents of the speciﬁed ﬁle to stdout, then redirect that output to ﬁle foobar.
// ./exfs2 -e /a/b/c/example > foobar

void readFile(const char *filename) {
    // This function would read the contents of the given file
    // Implementation is not provided here
}

// The output might be a ﬁle system tree, showing the names of each directory and ﬁle contained within your ﬁle system (think about recursing through it starting with the root, adding a number of leading tabs to each line of output equivalent to the depth of recursion). The following invocation would add the speciﬁed ﬁle to your ﬁle system (as well as each directory in the path):
// ./exfs2 -a /a/b/c -f /local/path/to/example

void addFile(const char *filename, const char *content) {
    // This function would write content to the given file
    // Implementation is not provided here
}

// In this example, /a/b/c is the path at which you intend the input ﬁle to reside in your ﬁle system. The intended result is that you will create exfs2 directories a, a/b, and a/b/c, then create a ﬁle named example in directory c and copy the contents of the input ﬁle at /local/path/to/example into exfs2. Each directory in the path should be created if it does not already exist, resulting in an inode being assigned for each directory that is created, and a directory entry added to the directory’s data block (if it’s a new directory, you’ll also need a new data block for its inode to point to).
// The following invocation would remove the speciﬁed ﬁle from your ﬁle system (as well as any directory that becomes empty after you remove the speciﬁed ﬁle):
// ./exfs2 -r /a/b/c/example

void removeFile(const char *filename) {
    // This function would remove the given file
    // Implementation is not provided here
}

// Think of adding a debugging option that would print each inode in the path and the data they point to. For example, in case of directories, print the name of each ﬁle and the associated inode’s number, in case of ﬁles, just print that it is a regular ﬁle.
// ./exfs2 -D /a/b/c/example

void debugPrint(const char *message) {
    // This function would print debug messages
    // Implementation is not provided here
}


int main() {
    printf("Hello, World!\n");
    return 0;
}