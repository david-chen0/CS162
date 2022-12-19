#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "threads/synch.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk {
  // block_sector_t start; /* First data sector. */
  off_t length;         /* File size in bytes. */
  unsigned magic;       /* Magic number. */
  block_sector_t double_indirect; // block_sector_t[128]
  bool isDir; // set to true if the inode is for a directory
  block_sector_t parent; // need this for .. in subdirectory parsing
  
  uint32_t unused[123]; /* Not used. */ // NOTE THAT THIS WAS 125 AT FIRST BUT I DECREMENTED IT SO WE CAN PUT isDir AND parent
};

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t bytes_to_sectors(off_t size) { return DIV_ROUND_UP(size, BLOCK_SECTOR_SIZE); }

/* In-memory inode. */
struct inode {
  struct list_elem elem;  /* Element in inode list. */
  block_sector_t sector;  /* Sector number of disk location. */
  int open_cnt;           /* Number of openers. */
  bool removed;           /* True if deleted, false otherwise. */
  int deny_write_cnt;     /* 0: writes ok, >0: deny writes. */
  // struct inode_disk data; /* Inode content. */
  struct lock inode_lock; /* Lock for synchronization */
  off_t length;           /* File size in bytes */
};

/* Get inode_disk from sector specified in inode */
static struct inode_disk* get_inode_disk(struct inode* inode) {
  if (inode == NULL || inode->sector < 0) {
    return NULL;
  }
  struct inode_disk* disk_inode = calloc(1, sizeof(struct inode_disk));
  ASSERT(sizeof *disk_inode == BLOCK_SECTOR_SIZE);
  buffer_cache_read(inode->sector, disk_inode, 0, BLOCK_SECTOR_SIZE);
  return disk_inode;
}

// Helper function so other files can look into inode's removed attribute
bool isRemoved(struct inode* inode) {
  return inode->removed;
}

// Helper function so other files can look into inode's data's isDir attribute
bool isDirectory(struct inode* inode) {
  struct inode_disk* buf = get_inode_disk(inode);
  ASSERT(buf != NULL);
  bool result = buf->isDir;
  free(buf);
  return result;
}

// Helper function so other files can set the current inode's parent
void setInodeParent(block_sector_t sector, block_sector_t parent) {
  struct inode_disk* disk_inode = calloc(1, sizeof(*disk_inode));
  ASSERT(sizeof *disk_inode == BLOCK_SECTOR_SIZE);
  buffer_cache_read(sector, disk_inode, 0, BLOCK_SECTOR_SIZE);
  
  disk_inode->parent = parent;
  buffer_cache_write(sector, disk_inode, 0, BLOCK_SECTOR_SIZE);
  free(disk_inode);
}

// Helper function so other files can get the current inode's parent
block_sector_t getInodeParent(struct inode* inode) {
  struct inode_disk* buf = get_inode_disk(inode);
  block_sector_t parent = buf->parent;
  free(buf);
  return parent;
}

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t byte_to_sector(const struct inode* inode, off_t pos) {
  ASSERT(inode != NULL);
  if (pos < 0) {
    return -1;
  }
  if (pos < inode->length) {
    /* Get inode_disk from the sector specified in inode */
    struct inode_disk* disk_inode = get_inode_disk(inode);

    /* Each indirect pointer stores BLOCK_SECTOR_SIZE * 128 bytes */
    int indirect_index = pos / (BLOCK_SECTOR_SIZE * 128);
    /* Now we're only looking at a section of BLOCK_SECTOR_SIZE * 128 bytes */
    int new_pos = pos % (BLOCK_SECTOR_SIZE * 128);
    /* Each direct pointer stores BLOCK_SECTOR_SIZE bytes */
    int direct_index = new_pos / (BLOCK_SECTOR_SIZE);

    /* Fetch array of indirect pointers */
    block_sector_t indirect_ptrs[128];
    for (int k = 0; k < 128; k++) {
      indirect_ptrs[k] = 0;
    }
    buffer_cache_read(disk_inode->double_indirect, indirect_ptrs, 0, BLOCK_SECTOR_SIZE);

    /* Fetch array of data blocks */
    block_sector_t indirect = indirect_ptrs[indirect_index];
    block_sector_t data_blocks[128];
    for (int k = 0; k < 128; k++) {
      data_blocks[k] = 0;
    }
    buffer_cache_read(indirect, data_blocks, 0, BLOCK_SECTOR_SIZE);

    block_sector_t data_block = data_blocks[direct_index];
    free(disk_inode);
    return data_block;
  }
    
  else
    return -1;
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;
static struct lock open_inodes_lock;

/* Initializes the inode module. */
void inode_init(void) { 
  list_init(&open_inodes);
  lock_init(&open_inodes_lock);
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool inode_create(block_sector_t sector, off_t length, bool isDir) {
  struct inode_disk* disk_inode = NULL;
  bool success = false;

  ASSERT(length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT(sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc(1, sizeof *disk_inode);
  if (disk_inode != NULL) {
    size_t sectors = bytes_to_sectors(length);
    disk_inode->length = length;
    disk_inode->magic = INODE_MAGIC;
    disk_inode->isDir = isDir;

    /* Failed to allocate the double indirect */
    if (!block_allocate(&disk_inode->double_indirect)) {
      free(disk_inode);
      return false;
    }
    if (disk_inode->double_indirect == 0) {
      free(disk_inode);
      return false;
    }

    /* Writes disk_inode to disk at sector */
    buffer_cache_write(sector, disk_inode, 0, BLOCK_SECTOR_SIZE);

    if (sectors > 0) {
      /* Each indirect pointer holds 128 sectors */
      int num_indirects_needed = (sectors + 128 - 1) / 128; // Rounding up
      /* Too big of a file */
      if (num_indirects_needed > 128) {
        /* TODO: Rollback */
        free(disk_inode);
        return false;
      }
      /* Allocating the indirect blocks we need */
      block_sector_t indirects[128];
      for (int k = 0; k < 128; k++) {
        indirects[k] = 0;
      }
      for (int i = 0; i < num_indirects_needed; i++) {
        if (!block_allocate(&indirects[i])) {
          /* TODO: Rollback */
          free(disk_inode);
          return false;
        }
        if (indirects[i] == 0) {
          /* TODO: Rollback */
          free(disk_inode);
          return false;
        }
        /* In all iterations but the last one, we use the full 128 data blocks */
        int num_data_blocks_needed = 128;
        /* In the last iteration, we need sectors mod 128 blocks allocated */
        if (i == num_indirects_needed - 1) {
          num_data_blocks_needed = sectors % 128;
        }
        block_sector_t data_blocks[128];
        for (int k = 0; k < 128; k++) {
          data_blocks[k] = 0;
        }
        /* Allocating the data blocks we need*/
        for (int j = 0; j < num_data_blocks_needed; j++) {
          if (!block_allocate(&data_blocks[j])) {
            /* TODO: Rollback */
            free(disk_inode);
            return false;
          }
          if (data_blocks[j] == 0) {
            /* TODO: Rollback */
            free(disk_inode);
            return false;
          }
        }
        /* Write data blocks to disk, at sector indirects[i] */
        buffer_cache_write(indirects[i], data_blocks, 0, BLOCK_SECTOR_SIZE);
      }
      /* Write indirect blocks to disk, at sector double_indirect */
      buffer_cache_write(disk_inode->double_indirect, indirects, 0, BLOCK_SECTOR_SIZE);

      success = true;
    }
    free(disk_inode);
  }
  return true;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode* inode_open(block_sector_t sector) {
  struct list_elem* e;
  struct inode* inode;
  lock_acquire(&open_inodes_lock);
  /* Check whether this inode is already open. */
  for (e = list_begin(&open_inodes); e != list_end(&open_inodes); e = list_next(e)) {
    inode = list_entry(e, struct inode, elem);
    if (inode->sector == sector) {
      inode_reopen(inode);
      lock_release(&open_inodes_lock);
      return inode;
    }
  }

  /* Allocate memory. */
  inode = malloc(sizeof *inode);
  if (inode == NULL) {
    lock_release(&open_inodes_lock);
    return NULL;
  }

  /* Initialize. */
  list_push_front(&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;

  struct inode_disk* disk_inode = get_inode_disk(inode);
  disk_inode->length += 0;
  inode->length = disk_inode->length;

  lock_init(&inode->inode_lock);
  // block_read(fs_device, inode->sector, &inode->data); No longer needed since inodes keep track of disk_inode with just sector
  lock_release(&open_inodes_lock);
  free(disk_inode);
  return inode;
}

/* Reopens and returns INODE. */
struct inode* inode_reopen(struct inode* inode) {
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t inode_get_inumber(const struct inode* inode) { return inode->sector; }

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void inode_close(struct inode* inode) {
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0) {
    /* Remove from inode list and release lock. */
    list_remove(&inode->elem);

    /* Deallocate blocks if removed. */
    if (inode->removed) {
      free_map_release(inode->sector, 1);
      // free_map_release(inode->data.start, bytes_to_sectors(inode->data.length));
    }

    free(inode);
  }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void inode_remove(struct inode* inode) {
  ASSERT(inode != NULL);
  inode->removed = true;
}

static bool inode_grow(struct inode* inode, off_t size) {
  ASSERT(size > inode->length);
  /* Get inode_disk from inode */
  struct inode_disk* disk_inode = get_inode_disk(inode);
  if (disk_inode == NULL) {
    return false;
  }

  /* Read in our 128 indirect pointers */
  block_sector_t indirects[128];
  for (int k = 0; k < 128; k++) {
    indirects[k] = 0;
  }
  buffer_cache_read(disk_inode->double_indirect, indirects, 0, BLOCK_SECTOR_SIZE);

  /* First, we'll allocate any indirects we need. Data blocks will be done later */
  for (int i = 0; i < 128; i++) {
    /* Each indirect has BLOCK_SECTOR_SIZE * 128 bytes 
    Say we only have 1 indirect allocated = maximum of 512*128 = 65536 bytes. If size = 70000 bytes,
    then we need another indirect block. On the i = 0 iteration, indirects[i] != 0 so we already allocated.
    On the i = 1 iteration, size > 512*128*1 and indirects[i] = 0 so we allocate the additional indirect we need.
    On the i >= 2 iterations, size < 512*128*i so we do not allocate any more.
    */
    if (size > (BLOCK_SECTOR_SIZE * 128 * i) && indirects[i] == 0) {
      if (!block_allocate(&indirects[i])) {
        /* TODO: Rollback */
        return false;
      };
      if (indirects[i] == 0) {
        /* TODO: Rollback */
        return false;
      }
    }
  }
  /* Write any updates to the double indirect back to disk */
  buffer_cache_write(disk_inode->double_indirect, indirects, 0, BLOCK_SECTOR_SIZE);

  /* Now, we need to allocate any additional data blocks we need */
  for (int i = 0; i < 128; i++) {
    /* Only check the indirects we've allocated */
    if (indirects[i] != 0) {
      /* Read in data blocks for current indirect pointer */
      block_sector_t data_blocks[128];
      for (int k = 0; k < 128; k++) {
        data_blocks[k] = 0;
      }
      buffer_cache_read(indirects[i], data_blocks, 0, BLOCK_SECTOR_SIZE);
      /* Iterate through all data blocks */
      for (int j = 0; j < 128; j++) {
        /* If size > (All bytes in indirects seen so far) + (All bytes in data_blocks seen so far for this indirect)
           AND this data block hasn't been allocated yet, then allocate it */
        if (size > (BLOCK_SECTOR_SIZE*128*i + BLOCK_SECTOR_SIZE*j) && data_blocks[j] == 0) {
          if (!block_allocate(&data_blocks[j])) {
            /* TODO: Rollback */
            return false;
          };
          if (data_blocks[j] == 0) {
            /* TODO: Rollback */
            return false;
          }
          static char zeros[BLOCK_SECTOR_SIZE];
          buffer_cache_write(data_blocks[j], zeros, 0, BLOCK_SECTOR_SIZE);
        }
      }
      /* Write any updates to the indirect pointer back to disk */
      buffer_cache_write(indirects[i], data_blocks, 0, BLOCK_SECTOR_SIZE);
    }
  }
  disk_inode->length = size;
  buffer_cache_write(inode->sector, disk_inode, 0, BLOCK_SECTOR_SIZE);
  inode->length = size;
  free(disk_inode);
  return true;
}

static bool inode_shrink(struct inode* inode, off_t size) {
  ASSERT(size < inode->length);
  /* Get inode_disk from inode */
  struct inode_disk* disk_inode = get_inode_disk(inode);
  if (disk_inode == NULL) {
    return false;
  }

  /* Read in our 128 indirect pointers */
  block_sector_t indirects[128];
  for (int k = 0; k < 128; k++) {
    indirects[k] = 0;
  }
  buffer_cache_read(disk_inode->double_indirect, indirects, 0, BLOCK_SECTOR_SIZE);

  /* Iterate backwards through indirects */
  for (int i = 127; i >= 0; i--) {
    /* If size can be captured by all "lower" indirects, then we don't need this entire indirect */
    if (size <= (BLOCK_SECTOR_SIZE*128*i) && indirects[i] != 0) {
      /* Read in data blocks */
      block_sector_t data_blocks[128];
      for (int k = 0; k < 128; k++) {
        data_blocks[k] = 0;
      }
      buffer_cache_read(indirects[i], data_blocks, 0, BLOCK_SECTOR_SIZE);
      /* Free all data blocks in this indirect */
      for (int j = 0; j < 128; j--) {
        if (data_blocks[j] != 0) {
          block_release(data_blocks[j]);
          data_blocks[j] = 0;
        } 
      }
      /* Write updated data blocks */
      buffer_cache_write(indirects[i], data_blocks, 0, BLOCK_SECTOR_SIZE);
      /* Release the indirect block */ 
      block_release(indirects[i]);
      indirects[i] = 0;
    }
    /* If size lies somewhere inside this indirect, then we need to free the extra data blocks in this indirect */
    if (size > (BLOCK_SECTOR_SIZE*128*i) && size <= (BLOCK_SECTOR_SIZE*128*(i+1)) && indirects[i] != 0) {
      /* Read in data blocks */
      block_sector_t data_blocks[128];
      for (int k = 0; k < 128; k++) {
        data_blocks[k] = 0;
      }
      buffer_cache_read(indirects[i], data_blocks, 0, BLOCK_SECTOR_SIZE);
      /* Iterate through data blocks */
      for (int j = 127; j >= 0; j--) {
        /* If size can be captured by all the "lower" indirects and "lower" data blocks in this indirect, free the current data block */
        if (size <= (BLOCK_SECTOR_SIZE*128*i + BLOCK_SECTOR_SIZE*j) && data_blocks[j] != 0) {
          block_release(data_blocks[j]);
          data_blocks[j] = 0;
        }
      }
      /* Write updated data_blocks */
      buffer_cache_write(indirects[i], data_blocks, 0, BLOCK_SECTOR_SIZE);
    }
  }
  /* Write updated indirects */
  buffer_cache_write(disk_inode->double_indirect, indirects, 0, BLOCK_SECTOR_SIZE);
  disk_inode->length = size;
  buffer_cache_write(inode->sector, disk_inode, 0, BLOCK_SECTOR_SIZE);
  inode->length = size;
  free(disk_inode);
  return true;


}

/* Grow or shrink an inode based on given size */
bool inode_resize(struct inode* inode, off_t size) {
  if (size > inode->length) {
    return inode_grow(inode, size);
  } else if (size < inode->length) {
    return inode_shrink(inode, size);
  }
  return true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t inode_read_at(struct inode* inode, void* buffer_, off_t size, off_t offset) {
  uint8_t* buffer = buffer_;
  off_t bytes_read = 0;
  uint8_t* bounce = NULL;

  lock_acquire(&inode->inode_lock);

  while (size > 0) {
    /* Disk sector to read, starting byte offset within sector. */
    block_sector_t sector_idx = byte_to_sector(inode, offset);
    int sector_ofs = offset % BLOCK_SECTOR_SIZE;

    /* Bytes left in inode, bytes left in sector, lesser of the two. */
    off_t inode_left = inode_length(inode) - offset;
    int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
    int min_left = inode_left < sector_left ? inode_left : sector_left;

    /* Number of bytes to actually copy out of this sector. */
    int chunk_size = size < min_left ? size : min_left;
    if (chunk_size <= 0)
      break;

    /* Read chunk_size of sector in buffer, with sector_ofs */
    buffer_cache_read(sector_idx, buffer + bytes_read, sector_ofs, chunk_size);

    /* Advance. */
    size -= chunk_size;
    offset += chunk_size;
    bytes_read += chunk_size;
  }
  lock_release(&inode->inode_lock);
  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t inode_write_at(struct inode* inode, const void* buffer_, off_t size, off_t offset) {
  const uint8_t* buffer = buffer_;
  off_t bytes_written = 0;
  uint8_t* bounce = NULL;

  if (inode->deny_write_cnt)
    return 0;

  lock_acquire(&inode->inode_lock);

  // This condition is wrong, consider when offset is less than length but offset + size > length
  if (offset + size > inode->length) {
    off_t new_size = offset + size;
    bool success = inode_resize(inode, new_size);
    if (!success) {
      printf("Resize failed\n");
      lock_release(&inode->inode_lock);
      return 0;
    }
  }

  while (size > 0) {
    /* Sector to write, starting byte offset within sector. */
    block_sector_t sector_idx = byte_to_sector(inode, offset);
    int sector_ofs = offset % BLOCK_SECTOR_SIZE;

    /* Bytes left in inode, bytes left in sector, lesser of the two. */
    off_t inode_left = inode_length(inode) - offset;
    int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
    int min_left = inode_left < sector_left ? inode_left : sector_left;

    /* Number of bytes to actually write into this sector. */
    int chunk_size = size < min_left ? size : min_left;
    if (chunk_size <= 0)
      break;

    /* Write */
    buffer_cache_write(sector_idx, buffer + bytes_written, sector_ofs, chunk_size);
    /* Advance. */
    size -= chunk_size;
    offset += chunk_size;
    bytes_written += chunk_size;
  }
  free(bounce);
  lock_release(&inode->inode_lock);
  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void inode_deny_write(struct inode* inode) {
  inode->deny_write_cnt++;
  ASSERT(inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void inode_allow_write(struct inode* inode) {
  ASSERT(inode->deny_write_cnt > 0);
  ASSERT(inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t inode_length(const struct inode* inode) { return inode->length; }
