#ifndef FILESYS_INODE_H
#define FILESYS_INODE_H

#include <stdbool.h>
#include "filesys/off_t.h"
#include "devices/block.h"

struct bitmap;

void inode_init(void);
bool inode_create(block_sector_t, off_t, bool);
struct inode* inode_open(block_sector_t);
struct inode* inode_reopen(struct inode*);
block_sector_t inode_get_inumber(const struct inode*);
void inode_close(struct inode*);
void inode_remove(struct inode*);
off_t inode_read_at(struct inode*, void*, off_t size, off_t offset);
off_t inode_write_at(struct inode*, const void*, off_t size, off_t offset);
void inode_deny_write(struct inode*);
void inode_allow_write(struct inode*);
off_t inode_length(const struct inode*);

bool isRemoved(struct inode*); // needed so other files can access inode's removed attribute
bool isDirectory(struct inode*); // needed so other files can access inode's data's isDir attribute
void setInodeParent(block_sector_t, block_sector_t); // needed so other files can set inode parents for ".."
block_sector_t getInodeParent(struct inode*); // needed so other files can get inode parents for ".."

#endif /* filesys/inode.h */
