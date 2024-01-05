/*
Originally created by Dyorgio Nascimento on 2020-12-10.
Modified for possum by anacrolix starting 2023-01-05, to determine block cloning in values file snapshots.
*/
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <string.h>

// declare methods
void printUsage(char* executable);
int compare_blocks(int block_size, char *filenameA, char *filenameB, int fdA, int fdB);
int compare_boundary_blocks(char *filenameA, char *filenameB, int fdA, int fdB);
void check_disk_fs(char *filename, bool is_forced_mode);
struct stat check_file(char *filename, bool is_forced_mode);

// entrypoint
int main(int args_count, char **args) {

  bool is_forced_mode = false;
  bool is_quick_mode = false;
  int opt;
  while ( (opt = getopt(args_count, args, "fqv?h")) != -1) {
       switch ( opt ) {
         case 'f': is_forced_mode = true; break;
         case 'q': is_quick_mode = true; break;
         case 'v': fprintf(stderr, "APFS Clone Checker - Version: 1.0.0.0\n"); exit(EXIT_SUCCESS); break;
         case '?':
         case 'h': printUsage(args[0]);
         default:
            printUsage(args[0]);
       }
  }
  if ( args_count - optind < 2 ) {
      printUsage(args[0]);
  }

  char* filenameA = args[optind];
  char* filenameB = args[optind + 1];

  check_disk_fs(filenameA, is_forced_mode);
  check_disk_fs(filenameB, is_forced_mode);

  struct stat statA = check_file(filenameA, is_forced_mode);
  struct stat statB = check_file(filenameB, is_forced_mode);

  if (statA.st_dev != statB.st_dev || statA.st_size != statB.st_size || statA.st_size < 1
      || statA.st_ino == statB.st_ino) {
    // clones only are supported on same device and have same size em blocks count, a file cannot be a clone of itself
    fprintf(stdout,"0\n");
    exit(EXIT_SUCCESS);
  }

  int fdA = open(filenameA, O_RDONLY);
  if (fdA < 0 ) {
    fprintf(stderr,"%s: Cannot open. %s\n", filenameA, strerror(errno));
    if ( is_forced_mode ) {
      fprintf(stdout,"0\n");
      exit(EXIT_SUCCESS);
    } else {
      exit(EXIT_FAILURE);
    }
  }

  int fdB = open(filenameB, O_RDONLY);
  if ( fdB < 0 ) {
    fprintf(stderr,"%s: Cannot open. %s\n", filenameB, strerror(errno));
    close(fdA);
    if ( is_forced_mode ) {
      fprintf(stdout,"0\n");
      exit(EXIT_SUCCESS);
    } else {
      exit(EXIT_FAILURE);
    }
  }

  int result;
  if ( is_quick_mode ) {
    result = compare_boundary_blocks(filenameA, filenameB, fdA, fdB);
  } else {
    result = compare_blocks(statA.st_blksize, filenameA, filenameB, fdA, fdB);
  }

  close(fdA);
  close(fdB);

  if ( result != -1 ) {
    fprintf(stdout,"%i\n", result);
    exit(EXIT_SUCCESS);
  } else {
    if ( is_forced_mode ) {
      fprintf(stdout,"0\n");
      exit(EXIT_SUCCESS);
    } else {
      exit(EXIT_FAILURE);
    }
  }
}

void printUsage(char* executable){
    fprintf(stderr, "Usage: %s [-fqv] fileA fileB\n", executable);
    exit(EXIT_FAILURE);
}

int compare_blocks(int block_size, char *filenameA, char *filenameB, int fdA, int fdB) {

  long sts = 0;
  struct log2phys physA;
  struct log2phys physB;

  for ( off_t offset = 0; sts >= 0; offset += block_size ) {
    physA.l2p_devoffset = offset;
    // get current blocks physical location
    sts = fcntl(fdA, F_LOG2PHYS_EXT, &physA);
    if ( sts < 0 && errno == ERANGE ) {
      physB.l2p_devoffset = offset;
      sts = fcntl(fdB, F_LOG2PHYS_EXT, &physB);
      if ( sts < 0 && errno == ERANGE ) {
        // both files seeked to the end with same offsets
        return true;
      } else if ( sts < 0 ) {
        fprintf(stderr,"%s: Cannot convert logical to physical offset. %i %s\n", filenameB, errno, strerror(errno));
        return -1;
      }
      break;
    } else if ( sts < 0 ) {
      fprintf(stderr,"%s: Cannot convert logical to physical offset. %i %s\n", filenameA, errno, strerror(errno));
      return -1;
    }

    physB.l2p_devoffset = offset;
    sts = fcntl(fdB, F_LOG2PHYS_EXT, &physB);
    if ( sts < 0 && errno == ERANGE ) {
      // insanity check, size of files already verified before
      break;
    } else if ( sts < 0 ) {
      fprintf(stderr,"%s: Cannot convert logical to physical offset. %i %s\n", filenameB, errno, strerror(errno));
      return -1;
    }

    fprintf(stderr, "%lld %lld\n", physA.l2p_devoffset, physB.l2p_devoffset);
    if ( physA.l2p_devoffset != physB.l2p_devoffset ) {
      // found a diff block
      break;
    }
  }

  // not a clone (check loop breaked)
  return false;
}

int compare_boundary_blocks(char *filenameA, char *filenameB, int fdA, int fdB) {

  long sts = 0;
  struct log2phys physA;
  struct log2phys physB;
  // get initial blocks physical location
  sts = fcntl(fdA, F_LOG2PHYS, &physA);
  if ( sts < 0 ) {
    fprintf(stderr,"%s: Cannot convert logical to physical offset. %i %s\n", filenameA, errno, strerror(errno));
    return -1;
  }

  sts = fcntl(fdB, F_LOG2PHYS, &physB);
  if ( sts < 0 ) {
    fprintf(stderr,"%s: Cannot convert logical to physical offset. %i %s\n", filenameB, errno, strerror(errno));
    return -1;
  }

  if ( physA.l2p_devoffset == physB.l2p_devoffset ) {
    // Move to end of files
     sts = lseek(fdA, -1, SEEK_END);
     if ( sts < 0 ) {
       fprintf(stderr,"%s: Cannot seek. %ld %s\n", filenameA, sts, strerror(errno));
       return -1;
     }
     sts = lseek(fdB, -1, SEEK_END);
     if ( sts < 0 ) {
       fprintf(stderr,"%s: Cannot seek. %ld %s\n", filenameB, sts, strerror(errno));
       return -1;
     }

     // get last blocks physical location
     sts = fcntl(fdA, F_LOG2PHYS, &physA);
     if ( sts < 0 ) {
       fprintf(stderr,"%s: Cannot convert logical to physical offset. %i %s\n", filenameA, errno, strerror(errno));
       return -1;
     }

     sts = fcntl(fdB, F_LOG2PHYS, &physB);
     if ( sts < 0 ) {
       fprintf(stderr,"%s: Cannot convert logical to physical offset. %i %s\n", filenameB, errno, strerror(errno));
       return -1;
     }

     return physA.l2p_devoffset == physB.l2p_devoffset;
  }

  return false;
}

void check_disk_fs(char *filename, bool is_forced_mode) {
  struct statfs fs;
  if( statfs(filename, &fs) == 0 ) {
    if( strcmp(fs.f_fstypename, "apfs") != 0) {
      fprintf(stderr, "%s: Only APFS is supported: %s\n", filename, fs.f_fstypename);
      if ( is_forced_mode ) {
        fprintf(stdout,"0\n");
        exit(EXIT_SUCCESS);
      } else {
        exit(EXIT_FAILURE);
      }
    }
  }
}

struct stat check_file(char *filename, bool is_forced_mode) {
  struct stat st;
  if ( stat(filename, &st) < 0 ) {
    fprintf(stderr, "%s: No such file\n", filename);
    if ( is_forced_mode ) {
      fprintf(stdout,"0\n");
      exit(EXIT_SUCCESS);
    } else {
      exit(EXIT_FAILURE);
    }
  }

  if ( (st.st_mode & S_IFMT) != S_IFREG ) {
    fprintf(stderr, "%s: Not a regular file\n", filename);
    if ( is_forced_mode ) {
      fprintf(stdout,"0\n");
      exit(EXIT_SUCCESS);
    } else {
      exit(EXIT_FAILURE);
    }
  }
  return st;
}
