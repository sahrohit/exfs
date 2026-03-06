# EXFS

## Team Members
- Rohit Kumar Sah <rohsah@siue.edu>
- Prajwal D C <pdc@siue.edu>

EXFS2 is a file system implementation based on inodes and datasegments. Instrctions for usage of this file system is explained in the steps below.

## Project Structure

```
/
│
├── main.c              # Main application script
├── Makefile            # Experimental notebook-style script
└── sample.txt          # Project dependencies
└── README              # Readme of the project
```

## Installation

### Prerequisites

- C Compiler

### Setup

1. Clone the repository:
   ```bash
   git clone https://github.com/sahrohit/exfs.git
   cd exfs
   ```

2. Compile the source code using `make`:
   ```bash
   make
   ```

3. You can try adding `sample.txt` in the exfs2 file system :
   ```bash
      ./exfs2 -a /dir1/dir2/dir3 -f sample.txt
   ```

## Usage

### Listing all the files an directories

```bash
./exfs2 -l
```

### Adding file (text or binary) to the file system

```bash
./exfs2 -a <path in exfs> -f <path in local fs>
```

### Extract the content of the file

```bash
./exfs2 -e <path in exfs>
```

### Remove the file/directory

```bash
./exfs2 -r <path in exfs>
```

### Debug Path

```bash
./exfs2 -D <path in exfs>
```
