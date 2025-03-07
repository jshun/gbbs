// This code is part of the Problem Based Benchmark Suite (PBBS)
// Copyright (c) 2016 Guy Blelloch, Daniel Ferizovic, and the PBBS team
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights (to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject to
// the following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
// LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
// OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
// WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

// A concurrent allocator for any fixed type T
// Keeps a local pool per processor
// Grabs list_size elements from a global pool if empty, and
// Returns list_size elements to the global pool when local pool=2*list_size
// Keeps track of number of allocated elements.
// Probably more efficient than a general purpose allocator

#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <atomic>
#include "concurrent_stack.h"
#include "utilities.h"
#include "memory_size.h"

struct block_allocator {
 private:

  static const size_t default_alloc_size = 1000000;
  static const size_t default_list_size = 1 << 16;
  static const size_t pad_size = 256;

  struct block {
    block* next;
  };
    
  using block_p = block*;

  struct alignas(64) thread_list {
    size_t sz;
    block_p head;  
    block_p mid;
    char cache_line[pad_size];
    thread_list() : sz(0), head(NULL) {};
  };

    
  block_p initialize_list(block_p);
  block_p get_list();
  concurrent_stack<uchar*> pool_roots;
  concurrent_stack<block_p> global_stack;
  thread_list* local_lists;

  size_t list_length;
  size_t max_blocks;
  size_t block_size_;
  std::atomic<size_t> blocks_allocated;
  uchar* allocate_blocks(size_t num_blocks);

 public:
  static int thread_count;
  void* alloc();
  void free(void*);
  void reserve(size_t n = default_alloc_size);
  size_t block_size () {return block_size_;}
  size_t num_allocated_blocks() {return blocks_allocated;}
  size_t num_used_blocks();

  ~block_allocator();
  block_allocator(size_t block_size,
		 size_t blocks_count = default_alloc_size,
		 size_t list_length_ = default_list_size,
		 size_t max_blocks_ = 0);

};

int block_allocator::thread_count = num_workers();

// Allocate a new list of list_length elements

auto block_allocator::initialize_list(block_p start) -> block_p {
  block_p p = start;

  for (size_t i = 0; i < list_length - 1; i++) {
    block_p nxt = (block_p) (((uchar*) p) + block_size_);
    p->next = nxt;
    p = nxt;
  }
  p->next = NULL;
  return start;
}

size_t block_allocator::num_used_blocks() {
  size_t free_blocks = global_stack.size()*list_length;
  for (int i = 0; i < thread_count; ++i) 
    free_blocks += local_lists[i].sz;
  return blocks_allocated - free_blocks;
}

auto block_allocator::allocate_blocks(size_t num_blocks) -> uchar* { 
  uchar* start = (uchar*) aligned_alloc(pad_size,
					num_blocks * block_size_+ pad_size);
  if (start == NULL) {
    fprintf(stderr, "Cannot allocate space in block_allocator");
    exit(1); }

  blocks_allocated += num_blocks; // atomic
  if (blocks_allocated > max_blocks) {
    fprintf(stderr, "Too many blocks in block_allocator, change max_blocks");
    exit(1);  }

  pool_roots.push(start); // keep track so can free later
  return start;
}

// Either grab a list from the global pool, or if there is none
// then allocate a new list
auto block_allocator::get_list() -> block_p {
    maybe<block_p> rem = global_stack.pop();
    if (rem) return *rem;
    block_p start = (block_p) allocate_blocks(list_length);
    return initialize_list(start);
}

// Allocate n elements across however many lists are needed (rounded up)
void block_allocator::reserve(size_t n) {
  size_t num_lists = thread_count + ceil(n / (double)list_length);
  uchar* start = allocate_blocks(list_length*num_lists);
  parallel_for(0, num_lists, [&] (size_t i) {
    block_p offset = (block_p) (start + i * list_length * block_size_);
    global_stack.push(initialize_list(offset));
  });
}

block_allocator::block_allocator(size_t block_size,
			       size_t blocks_count,
			       size_t list_length_,
			       size_t max_blocks_) {
  blocks_allocated = 0;
  block_size_ = block_size;
  list_length = list_length_;
  max_blocks = max_blocks_;
  if (max_blocks_ == 0) 
    max_blocks = (3*getMemorySize()/block_size)/4;

  reserve(blocks_count);

  // all local lists start out empty
  local_lists = new thread_list[thread_count];
}

block_allocator::~block_allocator() {
    delete[] local_lists;

    maybe<uchar*> x;
    while ((x = pool_roots.pop())) std::free(*x);
    pool_roots.clear();
    global_stack.clear();

    blocks_allocated = 0;
}

void block_allocator::free(void* ptr) {
  block_p new_node = (block_p) ptr;
  int id = worker_id();

  if (local_lists[id].sz == list_length+1) {
      local_lists[id].mid = local_lists[id].head;
  } else if (local_lists[id].sz == 2*list_length) {
    global_stack.push(local_lists[id].mid->next);
    local_lists[id].mid->next = NULL;
    local_lists[id].sz = list_length;
  }
  new_node->next = local_lists[id].head;
  local_lists[id].head = new_node;
  local_lists[id].sz++;
}

inline void* block_allocator::alloc() {
    int id = worker_id(); 

    if (!local_lists[id].sz)  {
      local_lists[id].head = get_list();
      local_lists[id].sz = list_length;
    }

    local_lists[id].sz--;
    block_p p = local_lists[id].head;
    local_lists[id].head = local_lists[id].head->next;

    return (void*) p;
}

