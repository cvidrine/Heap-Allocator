/*
 * File: allocator.c
 * Author: Charles Vidrine and Charlie Furrer
 * ----------------------
 * A trivial allocator. Very simple code, heinously inefficient.
 * An allocation request is serviced by incrementing the heap segment
 * to place new block on its own page(s). The block has a pre-node header
 * containing size information. Free is a no-op: blocks are never coalesced
 * or reused. Realloc is implemented using malloc/memcpy/free.
 * Using page-per-node means low utilization. Because it grows the heap segment
 * (using expensive OS call) for each node, it also has low throughput
 * and terrible cache performance.  The code is not robust in terms of
 * handling unusual cases either.
 *
 * In short, this implementation has not much going for it other than being
 * the smallest amount of code that "works" for ordinary cases.  But
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include "allocator.h"
#include "segment.h"




/*Global declarations and macros*/

/*---------------------------------------------------------------*/

#define ALIGNMENT 8 //required to be allocated in multiples of eight
#define WSIZE 4 //header/footer size (bytes)
#define NUM_BUCKETS 52 //Number of buckets in the free list
#define EXPONENTIAL_INDEX_START 30 //start of exponential buckets (sz 512)
#define EXPONENTIAL_INDEX_CONSTANT 23 //9 is the smallest exponential bucket, and 30 is largest 
#define REALLOC_BUFFER 1.2 //Multiplier for the buffer size in realloc
#define INITIAL_PAGES 1 //Number of pages given to the program initially
#define MIN_SPLIT_SIZE 176
#define MAX_EXTRA_PAGES 1

typedef struct Memblock Memblock;

//Structure for the header, holds the size of the previous contiguous block and the size of the current block.
typedef struct {
    unsigned int prevsz;
    unsigned int size;
} Header;

//Each block holds a header at all times and a pointer to the next and previous element in the linked list for when it is freed.
typedef struct Memblock {
    Header header;
    Memblock* next; //pointer to the next member in the linked list 
    Memblock* prev;
} Memblock;


//New memory in the heap segment that has never been allocated before
static Memblock* wilderness;

//Freed list
static void* memblock_array[NUM_BUCKETS];

//Index of the highest bucket that isn't null.
static int largest_block_index;

//How many extra pages the program gets when it needs more. Dynamically grows.
static int d_extra_pages = 1; 

//Pointer to the start of the heap.
static void* heap_start; 

//Typedef for iteration functions -- allows usage of iterate_through to remove_elemnent when any type of conditions met
typedef bool (*iteration_cmp_fn)(Memblock* b, unsigned int adj_sz);
/*---------------------------------------------------------------*/




/*Functions for interfacing with the blocks*/
/*---------------------------------------------------------------*/
//rounds up to the nearest mult of 8 (as long as it's a power of two)
static inline size_t roundup(size_t sz, size_t mult){
    return (sz + mult-1) & ~(mult-1);
}

//returns the two fields as a header 
static inline unsigned int pack(unsigned int size, bool alloc_status){
   return (size | alloc_status); 
}

//Returns the size of the previous contiguous block.
static inline unsigned int get_prev_size(void* b) {
    return (((Memblock*)b)->header.prevsz & ~0x7);
}

//get the size of the current block from the header.
static inline unsigned int get_size(void* b){
    return (((Memblock*)b)->header.size & ~0x7);
}
//Uses a header to find if a block is allocated.
static inline bool is_allocd(void* b){
    return (((Memblock*)b)->header.size & 0x1);
}

//Checks if an address is within the heap location.
static inline bool in_bounds(void* location){ //TODO: write to be less specific
    return (location >= heap_start && (char*)location <= (char*)heap_start + heap_segment_size());
}

//if prev block is allocd or is the first block, return NULL. Otherwise returns previous contiguous block.
static inline Memblock* prev_contiguous(void* b){
    Memblock* prev_block = (Memblock*)((char*)b - get_prev_size(b)); 
    if((void *)prev_block <= heap_start) return NULL;
    return prev_block;
}

//Returns next contiguous block in memory.
static inline Memblock* next_contiguous(void* b){
    Memblock *next =(Memblock *)((char *)b + get_size(b));
    return next;
}

//Jumps over the header to get the payload to return to the user.
static inline void* get_payload(void* allocd_block){
    return (void*)((char*)allocd_block + sizeof(Header)); 
}
/*---------------------------------------------------------------*/




/*functions for adjusting the wilderness*/
/*---------------------------------------------------------------*/

//requests pages based on the size request 
static inline void request_pages(unsigned int adjusted_sz){
    int num_pages = (adjusted_sz/PAGE_SIZE) + 1;
    if (d_extra_pages > MAX_EXTRA_PAGES) d_extra_pages = MAX_EXTRA_PAGES;
    num_pages += d_extra_pages;
    d_extra_pages ++;
    wilderness->header.size += num_pages * PAGE_SIZE;
    extend_heap_segment(num_pages); //if its not big enough
}

//adjusts the wilderness fields 
static inline void increment_wilderness(unsigned int adjusted_sz){
    unsigned int wilderness_size = get_size(wilderness);
    wilderness = (Memblock*)((char *)wilderness + adjusted_sz);
    wilderness->header.size = wilderness_size - adjusted_sz; 
}

/*---------------------------------------------------------------*/





/*set block fields*/
/*---------------------------------------------------------------*/
//sets the header and footer of an allocated block
static inline Memblock * create_allocd_block(Memblock* b,unsigned int adjusted_sz){
    b->header.size = pack(adjusted_sz,true); 
    next_contiguous(b)->header.prevsz = b->header.size;
    return b;
}

//sets header, footer and freed status... next field set to null
static inline Memblock * create_freed_block(Memblock* b, unsigned int adjusted_sz){
    b->header.size = pack(adjusted_sz,false);
    next_contiguous(b)->header.prevsz = b->header.size;
    b->next = NULL;
    b->prev = NULL;
    return b;
}
/*---------------------------------------------------------------*/



/*Iteration_cmp_fn*/
/*---------------------------------------------------------------*/
//Returns true if a given block is the exact size that we are looking for.
static inline bool perfect_match(Memblock* current, unsigned int target_size){
    return get_size(current) == target_size;
}

//Returns true if a given block is larger than the size we are looking for.
static inline bool best_fit(Memblock* current, unsigned int target_size){
    return get_size(current) >= target_size; 
}
/*---------------------------------------------------------------*/





/*functions for interfacing with the array.*/
/*---------------------------------------------------------------*/
//based on the size, do the necessary calculations to map to bucket
static inline int get_index(unsigned int sz){
    if(sz < 256) return ((sz-24)/8);
    else return (24 - (__builtin_clz(sz) + 1)) + EXPONENTIAL_INDEX_CONSTANT; //equal to 23, just to help with array
}

//Removes an element from the freed array and adjusts the pointers accordingly.
static inline void remove_element(void* curr, int index){
    Memblock* currblock = (Memblock *)curr;
    Memblock* prev_block = currblock->prev;
    Memblock* next_block = currblock->next;
    if (prev_block != NULL)
        prev_block->next = next_block;
    if (prev_block == NULL)
        memblock_array[index] = next_block;
    if (next_block != NULL)
        next_block->prev = prev_block;
    currblock->prev = NULL;
    currblock->next = NULL;
}

//stores a new freed block in the array
static void add_element(Memblock* new){
    int index = get_index(get_size(new)); //get index
    if(index > largest_block_index) largest_block_index = index;
    Memblock* old_first = memblock_array[index];
    new->next = NULL;
    if(old_first != NULL){
        old_first->prev = new; //point old first the new first  
        new->next  = old_first;  //point new first to old first 
    }
    new->prev = NULL;
    memblock_array[index] = new;
}

//iterates through the array bucket and removes a match (a match can be anything the iteration_cmp_fn returns true for)
static inline Memblock* iterate_through_bucket(int arr_index, unsigned int adjusted_size,iteration_cmp_fn compare){ 
    Memblock* curr = (Memblock*)memblock_array[arr_index]; 
    if(curr == NULL) return NULL;
    while(true){ 
        if(compare(curr,adjusted_size)){
            remove_element(curr,arr_index);
            return curr;
        }
        if(curr->next == NULL) break;    
        curr = (Memblock*)curr->next;  
    }
    return NULL;
}

//Adjusts the index of the largest allocated block.
static void adjust_largest(){
    for(int i=largest_block_index; i>=0; i--) {
        if(memblock_array[i] != NULL) {
            largest_block_index=i;
            return;
        }
    }
}

//Splits a block and puts the remainder back into the Free array.
static inline Memblock * split_block(Memblock *largeChunk, int index, unsigned int adjustedsz){//Block you wanna cut, index of that block, size you wanna steal.
    unsigned int original_size = get_size(largeChunk);
    Memblock* remainder = (Memblock*)((char *)largeChunk + adjustedsz);
    remainder = create_freed_block(remainder, original_size-adjustedsz); 
    add_element(remainder); 
    if(index == largest_block_index) adjust_largest();
    return largeChunk;
}

//Searches the array buckets for a block that can service the current request. If the block is found, it is split if necessary.
static inline Memblock* find_closest_fit(int arr_index, unsigned int adjustedSize){ 
    if(arr_index > largest_block_index) return NULL;
    for(int i = arr_index; i <=largest_block_index; i++){ 
        Memblock* return_segment = iterate_through_bucket(i,adjustedSize,best_fit); 
        if(return_segment == NULL) continue;
        unsigned int returned_size = get_size(return_segment); //looking for 48, no perfect fit found... returns 56, cannot split
        if(returned_size < adjustedSize + MIN_SPLIT_SIZE) return create_allocd_block(return_segment,returned_size);
        return_segment = split_block(return_segment, arr_index, adjustedSize);//does this mean we're splitting no matter what??
        return create_allocd_block(return_segment, adjustedSize);  
    }
    return NULL;
}

//Returns a block to service the current request, or NULL if none are in the freed list
static inline Memblock *find_fit(unsigned int adjustedSize){
    int index = get_index(adjustedSize);
    Memblock* perfect_fit = NULL; 
    if(memblock_array[index] != NULL) perfect_fit = iterate_through_bucket(index, adjustedSize,perfect_match);
    if(perfect_fit != NULL) return perfect_fit; 
    return find_closest_fit(index,adjustedSize);
}
/*---------------------------------------------------------------*/





/*other useful functions*/
/*---------------------------------------------------------------*/
//Rounds the size of a block to the next multiple of 8 and adds room for the header.
static inline unsigned int adjust_size(size_t usersz){
    unsigned int result = (unsigned int)usersz;
    if(result < 16) result = 16;
    return roundup(result, ALIGNMENT) + sizeof(Header);
}
//backs up from the payload and returns the Memblock
static inline Memblock* block_from_payload(void* pointer){
   return (Memblock*)((char*)pointer - sizeof(Header)); 
}
/*---------------------------------------------------------------*/






/*coalescing function*/
/*---------------------------------------------------------------*/
//Coalesce a given block with its neighbors.
static inline Memblock *coalesce_block(Memblock* middle_block){
    Header *header = &middle_block->header;
    Header *next = &next_contiguous(middle_block)->header;
    Memblock *prev_block = prev_contiguous(middle_block);
    Header *prev = NULL;
    if(prev_block != NULL)  prev = &prev_block->header;
    if(!is_allocd(next) && (Memblock *)next != wilderness){
        remove_element(next, get_index(get_size(next)));
        header->size = pack(get_size(header) + get_size(next), false);    
    }
    if(prev != NULL && !is_allocd(prev)){
        remove_element(prev, get_index(get_size(prev)));
        prev->size = pack(get_size(header) + get_size(prev), false);
        header = prev;
    }
    next_contiguous(header)->header.prevsz = header->size;
    return (Memblock *)header;
}

//stores payload in a buffer and then coalesces blocks around the original payload ptr
static inline void* merge_and_realloc(Memblock* block,Memblock* previous_block, void* ptr, unsigned int size){
    char buffer[size]; //Store old data in a buffer (could be overwritten).
    memcpy(buffer,ptr,(size_t)get_size(block) - sizeof(Header));
    block = coalesce_block(block);
    block = create_allocd_block(block,get_size(block));
    void* new_location = get_payload(block);
    if(previous_block != NULL)
        memcpy(new_location, buffer, size - sizeof(Header));
    return new_location;
}
/*---------------------------------------------------------------*/






/*Main Functions*/
/*---------------------------------------------------------------*/
//initializes the heap segment for each program/script that wants to use the allocator.
bool myinit(){
    heap_start = wilderness = init_heap_segment(INITIAL_PAGES); // reset heap segment to empty, no pages allocated
    wilderness->header.size = PAGE_SIZE * INITIAL_PAGES;
    d_extra_pages = 1;
    for(int i = 0; i < NUM_BUCKETS; i ++) memblock_array[i] = NULL; //initialize array
    largest_block_index = -1;
    return true;
}

//Searches the free Array for a block to service the request, and adjust the wilderness or request more pages if free Array can't
//service the request. Returns a pointer to the payload.
void *mymalloc(size_t requestedsz){
    if(requestedsz == 0) return NULL;
    unsigned int adjusted_size = adjust_size(requestedsz);   
    Memblock *block = find_fit(adjusted_size);
    if(block == NULL){
        if(adjusted_size >= get_size(wilderness)) request_pages(adjusted_size);
        block = wilderness;
        increment_wilderness(adjusted_size); 
        block = create_allocd_block(block,adjusted_size);
    } else{
        unsigned int block_size = get_size(block);
        block = create_allocd_block(block,block_size); 
    }
    return get_payload(block);
}

//Frees the memory associated with a given payload. Coalesced freed block with surruonding and adds to array.
void myfree(void *ptr){
    Memblock* newly_freed = block_from_payload(ptr); 
    if(!in_bounds(ptr)) return;
    newly_freed = create_freed_block(newly_freed, get_size(newly_freed));
    newly_freed = coalesce_block(newly_freed);
    if (next_contiguous(newly_freed) != wilderness) add_element(newly_freed);
    else { 
        newly_freed->header.size = pack(get_size(newly_freed) + get_size(wilderness), false); 
        wilderness = newly_freed;
    }
}


//Reallocates the given block of memory. First checks if there is enough buffer given so that
//the same block can be returned. Then, checks if coalescing will give enough memory. If so, realloc will
//coalesce the blocks and give the client the new payload size.
void *myrealloc(void *oldptr, size_t newsz){
    if(newsz <= 0) return NULL;
    if(oldptr == NULL) return mymalloc(newsz);

    unsigned int adjustedsz = adjust_size(newsz);//returns padded block
    Memblock *old_block = block_from_payload(oldptr);
    unsigned int old_size = get_size(old_block);
    unsigned int coalesce_size = 0;

    //If current block has enough space for request.
    if (adjustedsz <= old_size) return oldptr;
 
    Memblock *prev_block = prev_contiguous(old_block);
    Memblock *next_block = next_contiguous(old_block);

    //Check if the next and previous block are free. If so, adds to coalescing size.
    if(next_block != wilderness && !is_allocd(next_block)) coalesce_size = get_size(next_block);
    if(prev_block != NULL && !is_allocd(prev_block)) coalesce_size += get_size(prev_block);
    
    //If coalesce_size is big enough to service the request, blocks are coalesced.
    if(adjustedsz <= old_size + coalesce_size) 
        return merge_and_realloc(old_block,prev_block,oldptr,old_size);

    //If all else fails, malloc, memcpy, and free.
    void *newptr = mymalloc(newsz * REALLOC_BUFFER);
    memcpy(newptr, oldptr, (size_t)get_size(old_block) - sizeof(Header));
    myfree(oldptr);
    return newptr;
}





//iterates through the array bucket and removes a match (a match can be anything the iteration_cmp_fn returns true for)
static inline bool all_elements_free(int arr_index){ 
    Memblock* curr = (Memblock*)memblock_array[arr_index]; 
    if(curr == NULL) return true;
    while(true){ 
        if(is_allocd(curr)) return false;
        if(curr->next == NULL) break;    
        curr = (Memblock*)curr->next;  
    }
    return true;
}

// validate_heap is your debugging routine to detect/report
// on problems/inconsistency within your heap data structures
bool validate_heap(){
    for(int i=0; i<NUM_BUCKETS; i++){
        if(!all_elements_free(i)) return false;
    }
    return true;
}
/*---------------------------------------------------------------*/


