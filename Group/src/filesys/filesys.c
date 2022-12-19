#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "threads/thread.h"
#include "userprog/process.h"

/* Max cache size is 64 sectors */
#define MAX_CACHE_SIZE 65

struct cache_entry {
  struct list_elem elem;
  bool dirty;
  block_sector_t sector;
  uint8_t buffer[BLOCK_SECTOR_SIZE];
};

static struct lock cache_lock;
static struct list cache;

/* Partition that contains the file system. */
struct block* fs_device;

static void do_format(void);

/* Initializes the buffer cache */
static void buffer_cache_init(void) {
  lock_init(&cache_lock);
  list_init(&cache);
  struct cache_entry* dummy = malloc(sizeof(struct cache_entry));
  dummy->sector = -1;
  dummy->dirty = false;
  list_push_front(&cache, &dummy->elem);
}

static int get_cache_size(void) {
  int size = 0;
  struct list_elem* e;
  for (e = list_begin(&cache); e != list_end(&cache); e = list_next(e)) {
    size += 1;
  }
  return size;
}

/* Evict the least recently used entry from the cache */
static void cache_evict(void) {
  struct list_elem* dummy = list_end(&cache);
  struct list_elem* e = dummy->prev;
  struct cache_entry* entry = list_entry(e, struct cache_entry, elem);
  if (entry->dirty) {
    block_write(fs_device, entry->sector, entry->buffer);
  }
  int size = get_cache_size();
  list_remove(e);
  free(entry);
}

/* Check if an entry containing sector is already in the cache. If so, return entry. Else return NULL */
static struct cache_entry* check_cache(block_sector_t sector) {
  struct list_elem* e;
  for (e = list_begin(&cache); e != list_end(&cache); e = list_next(e)) {
    struct cache_entry* entry = list_entry(e, struct cache_entry, elem);
    if (entry->sector == sector) {
      return entry;
    }
  }
  return NULL;
}

/* Read SIZE bytes from SECTOR into BUFFER using cache, starting at SECTOR_OFS */
void buffer_cache_read(block_sector_t sector, uint8_t* buffer, off_t sector_ofs, off_t size) {
  lock_acquire(&cache_lock);

  /* Check if entry containing SECTOR is already in the cache */
  struct cache_entry* entry = NULL;
  entry = check_cache(sector);
  if (entry == NULL) {
    /* If it's not in cache, we need to make a new entry, read from disk into entry, and add entry to cache. */
    entry = malloc(sizeof(struct cache_entry));
    entry->dirty = false;
    entry->sector = sector;
    block_read(fs_device, entry->sector, entry->buffer);
    /* Evict if we're at capacity */
    if (get_cache_size() == MAX_CACHE_SIZE) {
      cache_evict();
    }
  } else {
    /* If it is in the cache, then we need remove it so we can move it to the front */
    list_remove(&entry->elem);
  }
  /* Add entry */
  list_push_front(&cache, &entry->elem);
  /* Copy size bytes of the cache buffer, starting at sector_ofs, into the buffer arg */
  memcpy(buffer, entry->buffer + sector_ofs, size);
  lock_release(&cache_lock);
}

/* Write SIZE bytes from BUFFER to SECTOR using cache, starting at SECTOR_OFS */
void buffer_cache_write(block_sector_t sector, uint8_t* buffer, off_t sector_ofs, off_t size) {
  lock_acquire(&cache_lock);
  /* Check if entry containing SECTOR is already in the cache */
  struct cache_entry* entry = NULL;
  entry = check_cache(sector);
  if (entry == NULL) {
    /* If it's not in cache, we need to make a new entry, read from disk into entry, and add entry to cache. */
    entry = malloc(sizeof(struct cache_entry));
    entry->dirty = false;
    entry->sector = sector;
    block_read(fs_device, entry->sector, entry->buffer);
    /* Evict if we're at capacity */
    if (get_cache_size() == MAX_CACHE_SIZE) {
      cache_evict();
    }
  } else {
    /* If it is in the cache, then we need remove it so we can move it to the front */
    list_remove(&entry->elem);
  }
  list_push_front(&cache, &entry->elem);
  memcpy(entry->buffer + sector_ofs, buffer, size);
  entry->dirty = true;
  lock_release(&cache_lock);
}

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void filesys_init(bool format) {
  fs_device = block_get_role(BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC("No file system device found, can't initialize file system.");

  buffer_cache_init();
  inode_init();
  free_map_init();

  if (format)
    do_format();

  free_map_open();

  // Set current process's cwd to root
  thread_current()->pcb->cwd = dir_open_root();
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void filesys_done(void) {
  struct list_elem* e;
  for (e = list_begin(&cache); e != list_end(&cache); e = list_next(e)) {
    struct cache_entry* entry = list_entry(e, struct cache_entry, elem);
    if (entry->dirty) {
      block_write(fs_device, entry->sector, entry->buffer);
    }
  }
  free_map_close();
}

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
  // Argument isDir is true if we want to create a directory
bool filesys_create(const char* name, off_t initial_size, bool isDir) {
  block_sector_t inode_sector = 0;

  // char* dirName;
  // char* fileName;
  // bool success = getDirAndFile(name, &dirName, &fileName); // storing the directory name in dirName and the file name in fileName
  // if (!success) {
  //   free(dirName);
  //   free(fileName);
  //   return false;
  // }

  // struct dir* dir = getDirectory(dirName); // getting the parent directory
  // if (dir == NULL) {
  //   free(dirName);
  //   free(fileName);
  //   return false;
  // }
  
  // struct inode* inode;
  // dir_lookup(dir, fileName, &inode);
  // if (inode != NULL) { // inode already exists, so file with NAME already exists
  //   inode_close(inode);
  //   dir_close(dir);

  //   free(dirName);
  //   free(fileName);

  //   return false;
  // }
  
  char fileName[NAME_MAX + 1];
  struct dir* dir;
  struct inode* inode;
  bool success = getDirAndInode(name, &fileName, &dir, &inode);
  if (!success) {
    return false;
  }

  success = (dir != NULL && block_allocate(&inode_sector) &&
                  inode_create(inode_sector, initial_size, isDir) && dir_add(dir, fileName, inode_sector));
  if (!success && inode_sector != 0) {
    free_map_release(inode_sector, 1);
  }
  if (success && isDir) { // set the parent for the dir we just created
    setInodeParent(inode_sector, inode_get_inumber(getDirInode(dir)));
  }
  dir_close(dir);

  // free(dirName);
  // free(fileName);

  return success;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file* filesys_open(const char* name) {
  // // Split full file/directory name into directory and file name
  // char* dirName;
  // char* fileName;
  // bool success = getDirAndFile(name, &dirName, &fileName);
  // if (!success) {
  //   return NULL;
  // }

  // // Get the parent file/directory
  // struct dir* dir = getDirectory(dirName);
  // if (dir == NULL) {
  //   free(dirName);
  //   free(fileName);
  //   return NULL;
  // }

  // struct inode* inode;
  // if (fileName[0] == '\0') { // want to open current dir
  //   inode = getDirInode(dir);
  // } else {
  //   // Get the file's inode
  //   dir_lookup(dir, fileName, &inode);
  // }

  char fileName[NAME_MAX + 1];
  struct dir* dir;
  struct inode* inode;
  bool success = getDirAndInode(name, fileName, &dir, &inode);
  if (!success) {
    return NULL;
  }

  dir_close(dir);

  // Open the file using that inode
  if (inode == NULL) {
    // free(dirName);
    // free(fileName);
    return NULL;
  }

  // free(dirName);
  // free(fileName);

  return file_open(inode);
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool filesys_remove(const char* name) {
  // // Split full file/directory name into directory and file name
  // char* dirName;
  // char* fileName;
  // bool success = getDirAndFile(name, &dirName, &fileName);
  // if (!success) {
  //   return NULL;
  // }

  // // Get the parent file/directory
  // struct dir* dir = getDirectory(dirName);
  // if (dir == NULL) {
  //   free(dirName);
  //   free(fileName);
  //   return NULL;
  // }

  // // Get the current file/directory's inode
  // struct inode* inode;
  // dir_lookup(dir, fileName, &inode);

  char fileName[NAME_MAX + 1];
  struct dir* dir;
  struct inode* inode;
  bool success = getDirAndInode(name, fileName, &dir, &inode);
  if (!success) {
    return NULL;
  }

  // Remove the file/directory
  // For files, we can remove whenever
  // For directories, we can only remove if the directory is empty
  if (inode == NULL) {
    success = false;
  } else if (!isDirectory(inode)) { // remove file
    success = dir_remove(dir, fileName);
  } else { // remove directory IF AND ONLY IF it is empty
    struct dir* targetDir = dir_open(inode);
    bool isEmpty = dirIsEmpty(targetDir);
    dir_close(targetDir);
    if (isEmpty) {
      success = dir_remove(dir, fileName);
    } else {
      success = false;
    }
  }

  // closing open inode and dir
  inode_close(inode);
  dir_close(dir);

  // free(dirName);
  // free(fileName);

  return success;
}

/* Formats the file system. */
static void do_format(void) {
  printf("Formatting file system...");
  free_map_create();
  if (!dir_create(ROOT_DIR_SECTOR, 16))
    PANIC("root directory creation failed");
  free_map_close();
  printf("done.\n");
}
