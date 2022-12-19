#include "filesys/directory.h"
#include <stdio.h>
#include <string.h>
#include <list.h>
#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "userprog/process.h"

/* A directory. */
struct dir {
  struct inode* inode; /* Backing store. */
  off_t pos;           /* Current position. */
};

/* A single directory entry. */
struct dir_entry {
  block_sector_t inode_sector; /* Sector number of header. */
  char name[NAME_MAX + 1];     /* Null terminated file name. */
  bool in_use;                 /* In use or free? */
};

// Helper method for other files to access the directory's inode
struct inode* getDirInode(struct dir* dir) {
  return dir->inode;
}

// Helper method for other files to check if a directory is empty(mainly for filesys_remove)
bool dirIsEmpty(struct dir* dir) {
  struct dir_entry entry;
  off_t offset = 0;
  for (; inode_read_at(dir->inode, &entry, sizeof entry, offset) == sizeof entry; offset += sizeof entry) {
    if (entry.in_use) {
      return false;
    }
  }
  return true;
}

/* Creates a directory with space for ENTRY_CNT entries in the
   given SECTOR.  Returns true if successful, false on failure. */
bool dir_create(block_sector_t sector, size_t entry_cnt) {
  return inode_create(sector, entry_cnt * sizeof(struct dir_entry), true); // the arg should be true, check if bug pops up
}

/* Opens and returns the directory for the given INODE, of which
   it takes ownership.  Returns a null pointer on failure. */
struct dir* dir_open(struct inode* inode) {
  // CHECK THIS LATER
  // for some reason, this should work but it was causing a bu on dir-mkdir
  if (!isDirectory(inode)) {
    return NULL;
  }

  struct dir* dir = calloc(1, sizeof *dir);
  if (inode != NULL && dir != NULL) {
    dir->inode = inode;
    dir->pos = 0;
    return dir;
  } else {
    inode_close(inode);
    free(dir);
    return NULL;
  }
}

/* Opens the root directory and returns a directory for it.
   Return true if successful, false on failure. */
struct dir* dir_open_root(void) {
  return dir_open(inode_open(ROOT_DIR_SECTOR)); // isDir check covered by dir_open, although its unnecessary
}

/* Opens and returns a new directory for the same inode as DIR.
   Returns a null pointer on failure. */
struct dir* dir_reopen(struct dir* dir) {
  return dir_open(inode_reopen(dir->inode)); // isDir check covered by dir_open
}

/* Destroys DIR and frees associated resources. */
void dir_close(struct dir* dir) {
  if (dir != NULL) {
    inode_close(dir->inode);
    free(dir);
  }
}

/* Returns the inode encapsulated by DIR. */
struct inode* dir_get_inode(struct dir* dir) {
  return dir->inode;
}

/* Searches DIR for a file with the given NAME.
   If successful, returns true, sets *EP to the directory entry
   if EP is non-null, and sets *OFSP to the byte offset of the
   directory entry if OFSP is non-null.
   otherwise, returns false and ignores EP and OFSP. */
static bool lookup(const struct dir* dir, const char* name, struct dir_entry* ep, off_t* ofsp) {
  struct dir_entry e;
  size_t ofs;

  ASSERT(dir != NULL);
  ASSERT(name != NULL);

  for (ofs = 0; inode_read_at(dir->inode, &e, sizeof e, ofs) == sizeof e; ofs += sizeof e)
    if (e.in_use && !strcmp(name, e.name)) {
      if (ep != NULL)
        *ep = e;
      if (ofsp != NULL)
        *ofsp = ofs;
      return true;
    }
  return false;
}

/* Searches DIR for a file with the given NAME
   and returns true if one exists, false otherwise.
   On success, sets *INODE to an inode for the file, otherwise to
   a null pointer.  The caller must close *INODE. */
bool dir_lookup(const struct dir* dir, const char* name, struct inode** inode) {
  struct dir_entry e;

  ASSERT(dir != NULL);
  ASSERT(name != NULL);

  if (lookup(dir, name, &e, NULL))
    *inode = inode_open(e.inode_sector);
  else
    *inode = NULL;

  return *inode != NULL;
}

/* Adds a file named NAME to DIR, which must not already contain a
   file by that name.  The file's inode is in sector
   INODE_SECTOR.
   Returns true if successful, false on failure.
   Fails if NAME is invalid (i.e. too long) or a disk or memory
   error occurs. */
bool dir_add(struct dir* dir, const char* name, block_sector_t inode_sector) {
  struct dir_entry e;
  off_t ofs;
  bool success = false;

  ASSERT(dir != NULL);
  ASSERT(name != NULL);

  /* Check NAME for validity. */
  if (*name == '\0' || strlen(name) > NAME_MAX)
    return false;

  /* Check that NAME is not in use. */
  if (lookup(dir, name, NULL, NULL))
    goto done;

  /* Set OFS to offset of free slot.
     If there are no free slots, then it will be set to the
     current end-of-file.

     inode_read_at() will only return a short read at end of file.
     Otherwise, we'd need to verify that we didn't get a short
     read due to something intermittent such as low memory. */
  for (ofs = 0; inode_read_at(dir->inode, &e, sizeof e, ofs) == sizeof e; ofs += sizeof e)
    if (!e.in_use)
      break;

  /* Write slot. */
  e.in_use = true;
  strlcpy(e.name, name, sizeof e.name);
  e.inode_sector = inode_sector;
  success = inode_write_at(dir->inode, &e, sizeof e, ofs) == sizeof e;

done:
  return success;
}

/* Removes any entry for NAME in DIR.
   Returns true if successful, false on failure,
   which occurs only if there is no file with the given NAME. */
bool dir_remove(struct dir* dir, const char* name) {
  struct dir_entry e;
  struct inode* inode = NULL;
  bool success = false;
  off_t ofs;

  ASSERT(dir != NULL);
  ASSERT(name != NULL);

  /* Find directory entry. */
  if (!lookup(dir, name, &e, &ofs))
    goto done;

  /* Open inode. */
  inode = inode_open(e.inode_sector);
  if (inode == NULL)
    goto done;

  /* Erase directory entry. */
  e.in_use = false;
  if (inode_write_at(dir->inode, &e, sizeof e, ofs) != sizeof e)
    goto done;

  /* Remove inode. */
  inode_remove(inode);
  success = true;

done:
  inode_close(inode);
  return success;
}

/* Reads the next directory entry in DIR and stores the name in
   NAME.  Returns true if successful, false if the directory
   contains no more entries. */
bool dir_readdir(struct dir* dir, char name[NAME_MAX + 1]) {
  struct dir_entry e;

  while (inode_read_at(dir->inode, &e, sizeof e, dir->pos) == sizeof e) {
    dir->pos += sizeof e;
    if (e.in_use) {
      strlcpy(name, e.name, NAME_MAX + 1);
      return true;
    }
  }
  return false;
}


/* Extracts a file name part from *SRCP into PART, and updates *SRCP so that the
   next call will return the next file name part. Returns 1 if successful, 0 at
   end of string, -1 for a too-long file name part. */
static int get_next_part(char part[NAME_MAX + 1], const char** srcp) {
  const char* src = *srcp;
  char* dst = part;

  /* Skip leading slashes.  If it's all slashes, we're done. */
  while (*src == '/')
    src++;
  if (*src == '\0')
    return 0;

  /* Copy up to NAME_MAX character from SRC to DST.  Add null terminator. */
  while (*src != '/' && *src != '\0') {
    if (dst < part + NAME_MAX)
      *dst++ = *src;
    else
      return -1;
    src++;
  }
  *dst = '\0';

  /* Advance source pointer. */
  *srcp = src;
  return 1;
}


// Gets the filename, dir, and inode
// Need this for some reason because accessing the filename in another method is slow?????
// Filename will be the last part: ex: /a/b will give a filename of b
// Dir will be the directory of /a
// Inode will be the dir's inode
// Basically like running getDirAndFile and getDirectory
// Returns whether the operation was successful
bool getDirAndInode(char* dirPath, char fileName[NAME_MAX + 1], struct dir** dir, struct inode** inode) {
  if (dirPath[0] == '\0') { // cant have empty path, ex: userprog/open-empty
    return false;
  }

  struct process* curProcess = thread_current()->pcb;

  struct dir* curDir;
  if (dir[0] == '/' || curProcess->cwd == NULL) {
    // Absolute path, so use root
    curDir = dir_open_root();
  } else {
    // relative path, so use cwd
    curDir = dir_reopen(curProcess->cwd);
  }

  // *fileName = malloc(sizeof(char) * (NAME_MAX + 1));
  // *fileName[0] = '\0';

  struct inode* curInode = inode_reopen(curDir->inode);

  // // Make sure the current inode is still active
  // if (isRemoved(curInode)) {
  //   return NULL;
  // }

  while (get_next_part(fileName, &dirPath) == 1) {
    if (strcmp(fileName, ".") == 0) { // current directory
      continue;
    }

    if (curDir == NULL || curInode == NULL || !isDirectory(curInode)) {
      dir_close(curDir);
      inode_close(curInode);
      return false;
    }

    if (strcmp(fileName, "..") == 0) { // parent directory
      struct inode* parentInode = getInodeParent(curInode); // get parent inode
      inode_close(curInode);
      curInode = parentInode;
      continue;
    }

    struct dir* nextDir = dir_open(curInode);
    dir_close(curDir);
    curDir = nextDir;
    dir_lookup(curDir, fileName, &curInode);
  }

  // // Return directory if its inode is still active
  // if (!isRemoved(curInode)) {
  //   return false;
  // }

  *dir = curDir;
  *inode = curInode;
  return true;
}

// Open and return a pointer to the directory corresponding to the input dir name. Return NULL if there is no such directory.
struct dir* getDirectory(char* dir) {
  struct process* curProcess = thread_current()->pcb;

  struct dir* curDir;
  if (dir[0] == '/' || curProcess->cwd == NULL) {
    // Absolute path, so use root
    curDir = dir_open_root();
  } else {
    // relative path, so use cwd
    curDir = dir_reopen(curProcess->cwd);
  }

  // Make sure the current inode is still active
  if (isRemoved(curDir->inode)) {
    return NULL;
  }

  char parts[NAME_MAX + 1]; // only need this to be able to call get_next_part
  while (get_next_part(parts, &dir) == 1) {
    if (strcmp(parts, ".") == 0) { // current directory
      continue;
    }

    if (strcmp(parts, "..") == 0) { // parent directory
      struct inode* parentInode = getInodeParent(curDir->inode); // get parent inode
      struct dir* nextDir = dir_open(parentInode); // get parent dir from parent inode
      dir_close(curDir); // close current dir
      if (nextDir == NULL) { // shouldnt ever trigger
        return NULL;
      }
      curDir = nextDir; // move to next dir
      continue;
    }

    // Get the next inode by looking through the current directory
    struct inode* nextInode;
    bool success = dir_lookup(curDir, parts, &nextInode);
    if (!success) {
      dir_close(curDir);
      return NULL;
    }

    // Get the next directory using the next inode
    struct dir* nextDir = dir_open(nextInode);
    // inode_close(nextInode);

    // Close current directory and move to next directory
    dir_close(curDir);
    if (nextDir == NULL) {
      return NULL;
    }
    curDir = nextDir;
  }

  // Return directory if its inode is still active
  if (!isRemoved(curDir->inode)) {
    return curDir;
  }

  // Otherwise close the dir first
  dir_close(curDir);
  return NULL;
}

// Gets the directory name and file name from the input dir and returns if operation was successful
// If fullDir is a relative path, then resultDir will be a relative path
// Similarly, if fullDir is an absolute path, then resultDir will be an absolute path
bool getDirAndFile(char* fullDir, char** resultDir, char** resultFile) {
  char* tempResultDir = malloc(sizeof(char) * (strlen(fullDir) + 1));
  char* tempResultFile = malloc(sizeof(char) * (NAME_MAX + 1));

  int dirLen = 0;

  if (fullDir[0] == '/') { // absolute path
    tempResultDir[0] = '/';
    dirLen++;
  }

  int success;
  char prevPart[NAME_MAX + 1];
  prevPart[0] = '\0'; // need to do this for copying over
  char curPart[NAME_MAX + 1];
  curPart[0] = '\0'; // need to do this for copying over
  while ((success = get_next_part(curPart, &fullDir)) == 1) {
    int prevPartLength = strlen(prevPart);
    if (prevPartLength > 0) { // should be true for ever iteration EXCEPT for the first one
      memcpy(&tempResultDir[dirLen], prevPart, sizeof(char) * prevPartLength);
      tempResultDir[dirLen + prevPartLength] = '/';
      dirLen += prevPartLength + 1;
    }

    // switching cur to prev
    int curPartLength = strlen(curPart);
    memcpy(prevPart, curPart, sizeof(char) * curPartLength);
    prevPart[curPartLength] = '\0';
  }

  // get_next_part failed
  if (success == -1) {
    return false;
  }

  tempResultDir[dirLen] = '\0';
  memcpy(tempResultFile, curPart, strlen(curPart) + 1);
  tempResultFile[strlen(curPart)] = '\0';

  // Setting the resultDir and resultFile
  *resultDir = tempResultDir;
  *resultFile = tempResultFile;
  return true;
}


// THE ONE BELOW DOESNT TAKE INTO ACCOUNT WHEN YOU PASS IN AN ABSOLUTE DIRECTORY. NEED TO CONVERT TO A DIFFERENT APPROACH
// // Gets the directory name and file name from the input dir and returns if operation was successful
// // Used for the mkdir syscall
// bool getDirAndFile(char* fullDir, char** resultDir, char** resultFile) {
//   // Holders for resultDir and resultFile
//   char* tempResultDir = malloc(sizeof(char) * (strlen(fullDir) + 1));
//   tempResultDir[0] = '/';
//   tempResultDir[1] = '\0';
//   char* tempResultFile = malloc(sizeof(char) * (NAME_MAX + 1));

//   int success;
//   int dirLen = 1;
//   int latestPartLen = 0;
//   char parts[NAME_MAX + 1]; // use to call get_next_part
//   while ((success = get_next_part(parts, &fullDir)) == 1) {
//     latestPartLen = strlen(parts); // set this to the current part that we are processing
//     memcpy(&tempResultDir[dirLen], parts, sizeof(char) * latestPartLen); // copy the current part into the resultDir
//     dirLen += latestPartLen; // increment our count for the size of resultDir
//     tempResultDir[dirLen] = '/'; // for separating directories
//     dirLen += 1; // increment our count for '/'
//   }

//   // get_next_part failed
//   if (success == -1) {
//     return false;
//   }

//   // remove the latest part we added
//   dirLen -= latestPartLen + 1; // decrement our resultDir size(the +1 is for the '/')
//   memcpy(tempResultFile, &tempResultDir[dirLen], sizeof(char) * latestPartLen); // copy the latest part into resultFile
//   tempResultDir[dirLen] = '\0'; // null terminate the resultDir
//   tempResultFile[latestPartLen] = '\0';

//   // Setting the resultDir and resultFile
//   *resultDir = tempResultDir;
//   *resultFile = tempResultFile;
//   return true;
// }