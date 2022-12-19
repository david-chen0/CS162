/*
 * mm_alloc.c
 */

#include "mm_alloc.h"

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

#include <stdbool.h>

typedef struct block {
  size_t size; // current size of the block
  bool isFree; // true if the block is free
  struct block* prev; // previous block in the LL
  struct block* next; // next block in the LL
  char content[]; // content of the block
} block;

struct block* head;

// Adds block addBlock right after curBlock and returns if operation was successful
bool addBlock(struct block* curBlock, struct block* addBlock) {
  if (curBlock == NULL) {
    return false;
  }

  struct block* next = curBlock->next;
  addBlock->prev = curBlock;
  addBlock->next = next;
  curBlock->next = addBlock;

  if (next != NULL) {
    next->prev = addBlock;
  }

  return true;
}

// Creates a block with content size = size and returns pointer to the block(NOT THE CONTENT)
void* createBlock(size_t size) {
  void* heap = sbrk(size + sizeof(block));

  if (heap == (void*) -1) {
    return NULL;
  }

  struct block* curBlock = (struct block*) heap;
  curBlock->size = size;
  curBlock->isFree = false;
  curBlock->prev = NULL;
  curBlock->next = NULL;
  
  memset(&curBlock->content, 0, size);
  return curBlock;
}

// Splits the block into two and returns if the operation was successful.
bool splitBlock(struct block* curBlock, size_t size) {
  if (curBlock == NULL) {
    return false;
  }

  // size_t firstBlockSize = curBlock->size - size - sizeof(struct block);

  // struct block* secondBlock = &curBlock->content + firstBlockSize;
  // secondBlock->size = size;
  // secondBlock->isFree = false;
  // if (!addBlock(curBlock, secondBlock)) {
  //   return false;
  // }

  // curBlock->size = firstBlockSize;
  // return true;
  memset(&curBlock->content, 0, curBlock->size);

  size_t curSizeCopy = curBlock->size;
  curBlock->isFree = false;
  curBlock->size = size;

  struct block* nextBlock = (char*) &curBlock->content + size; // need to cast to char so that size only increments by 1 byte per value
  // struct block* nextBlock = &curBlock->content;
  // nextBlock += size;
  nextBlock->size = curSizeCopy - size - sizeof(struct block);
  nextBlock->isFree = true;
  
  if (!addBlock(curBlock, nextBlock)) {
    return false;
  }
  return true;
}

// Merges secondBlock into firstBlock and then zeros firstBlock's content. Assumes both the blocks are unused.
void mergeBlocks(struct block* firstBlock, struct block* secondBlock) {
  firstBlock->size = (size_t) &secondBlock->content - (size_t) &firstBlock->content + secondBlock->size;
  
  firstBlock->next = secondBlock->next;
  if (secondBlock->next) {
    secondBlock->next->prev = firstBlock;
  }

  memset(&firstBlock->content, 0, firstBlock->size);
}

void* mm_malloc(size_t size) {
  //TODO: Implement malloc
  if (size == 0) {
    return NULL;
  }

  if (head == NULL) { // LL is empty
    head = createBlock(size);
    if (head == NULL) {
      return NULL;
    }
    return &head->content; // need to return pointer to content, not to metadata
  }

  struct block* cur = head;
  struct block* prev = NULL;
  while (cur != NULL) {
    if (cur->isFree && cur->size >= size) { // since this block is free and has size >= what is required, we use this block
      if (cur->size >= size * 2 + sizeof(struct block)) { // block is too large so we need to split it
        splitBlock(cur, size);
        // cur->isFree = true;
        // cur = cur->next; // the block after our current one is what we allocated
        // cur->isFree = false;
      } else {
        memset(&cur->content, 0, cur->size);
        cur->size = size;
        cur->isFree = false;
      }
      return &cur->content;
    }
    prev = cur;
    cur = cur->next;
  }

  // need to make a new block for it
  struct block* newBlock = createBlock(size);
  if (newBlock == NULL) {
    return NULL;
  }
  addBlock(prev, newBlock);
  return &newBlock->content;
}

void* mm_realloc(void* ptr, size_t size) {
  //TODO: Implement realloc
  if (ptr == NULL) {
    return mm_malloc(size);
  } else if (size == 0) {
    mm_free(ptr);
    return NULL;
  }

  struct block* cur = ptr - sizeof(struct block);
  if (cur->size >= size) {
    cur->size = size;
    return ptr;
  }

  void* newBlockContent = mm_malloc(size);
  if (newBlockContent == NULL) {
    return NULL;
  }
  struct block* newBlock = newBlockContent - sizeof(struct block); // might not be necessary, but why not
  memset(&newBlock->content, 0, size);
  if (size > cur->size) {
    memcpy(&newBlock->content, &cur->content, cur->size);
  } else {
    memcpy(&newBlock->content, &cur->content, size);
  }
  mm_free(ptr);

  return &newBlock->content;
}

void mm_free(void* ptr) {
  //TODO: Implement free
  if (ptr == NULL) {
    return;
  }

  struct block* cur = ptr - sizeof(struct block);
  struct block* prev = cur->prev;
  struct block* next = cur->next;

  cur->isFree = true;
  memset(ptr, 0, cur->size);

  while (prev != NULL && prev->isFree) {
    mergeBlocks(prev, cur);
    cur = prev;
    prev = prev->prev;
  }
  
  while (next != NULL && next->isFree) {
    mergeBlocks(cur, next);
    next = cur->next;
  }
}
