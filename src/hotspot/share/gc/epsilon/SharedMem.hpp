//
// Created by adam on 8/31/23.
//

#ifndef SPACESERVER_SHARED_MEM_HPP
#define SPACESERVER_SHARED_MEM_HPP

#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <vector>
#include <atomic>
#include <semaphore.h>
#include "gc/epsilon/rpcMessages.hpp"
#include "runtime/thread.hpp"

enum entry_state {
	UNUSED = 0,
	USED = 1
};


typedef struct batch{
    uint32_t bump;
	uint32_t end;
	uint32_t size;
    uint32_t thread_no;
	uint64_t next1;
	uint64_t next2;
    uint64_t prev2; 
	uint64_t array[BUFFER_SIZE];
} batch_t;

struct entry{
	std::atomic<uint64_t> batch;
	std::atomic<uint64_t> count;
	char padding[64 - sizeof(char*)*2];
} __attribute__((aligned (64)));

struct batch_queue{
	batch_t* head;
	batch_t* tail;
};

struct batch_stack{
	std::atomic<uint64_t> head;
	char padding[64 - sizeof(head)];
};

struct ticket_lock{                                                                                                                                                                         
    std::atomic<int> ticket;                                                                                                                                                                
    std::atomic<int> screen;                                                                                                                                                                
};                                                                                                                                                                                          


#define PRE_FREE_SIZE sizeof(struct batch_stack)
#define SPIN_SIZE sizeof(struct ticket_lock)
#define SEM_SIZE sizeof(sem_t)
#define NBR_OF_LINEAR_ENTRIES 8
#define NBR_OF_EXP_ENTRIES 10 //up to 8192 in size 
#define OVERFLOW_EXP 64 // to remove a jump in tlab assembly code
#define LINEAR_ENTRIES_WIDTH 4
#define START_OF_EXP (LINEAR_ENTRIES_WIDTH-1)*NBR_OF_LINEAR_ENTRIES
#define NBR_OF_ENTRIES (NBR_OF_LINEAR_ENTRIES*LINEAR_ENTRIES_WIDTH + NBR_OF_EXP_ENTRIES)
#define ENTRIES_SIZE (sizeof(struct entry)*NBR_OF_ENTRIES)
//#define BATCH_SPACE_SIZE (SHM_SIZE - ENTRIES_SIZE) 
#define BATCH_SPACE_SIZE (5ULL*1024*1024*1024UL)
#define NBR_OF_BATCHES (BATCH_SPACE_SIZE/sizeof(batch_t))


class PseudoTLAB; 
class RemoteSpace; 


class SharedMem {
public:
	void* start_addr;
	struct batch_stack* prefree_list;
	struct entry* entry_tab;
	batch_t* start_of_batches;
	//std::atomic<int>* share_lock;
	struct ticket_lock* share_lock;
	sem_t* mutex;

	std::atomic<int> thread_counter;
	RemoteSpace* rs;
	PseudoTLAB *tlab_list;
	struct ticket_lock tlab_list_lock;
	
	static SharedMem* shm;

//	struct batch_queue* free_list;
//	struct batch_queue* in_use_list;
	
public:
	void initialize(void* addr_, RemoteSpace* rs_);
	
	batch_t* abs_addr(uint64_t offset){
		return (batch_t*)(offset + (uint64_t)start_addr);
	}

	void stack_push(batch_stack* list, uint64_t batch);
	uint64_t stack_pop(batch_stack*);

	batch_t* get_new_batch(int index, int thread_offset, PseudoTLAB* tlab); 
	batch_t* get_new_batch_exp(int index, PseudoTLAB* tlab); 
	
	int nbr_threads();
	uint64_t get_while_time();
	void reset_used();
	void add_tlab(PseudoTLAB* t);
	void remove_tlab(PseudoTLAB* t);

    int size_of_buffer(int size){ 
		if(size <= 32)
			return 507;
		if(size <= 64)
    	    return 128;
    	if(size <= 128)
    	    return 64;
    	if(size <= 256) 
    	    return 32;
    	if(size <= 512)
    	    return 4;
    	if(size <= 1024)
    	    return 2;
		return 1;
	}

    static void init_ticket_lock(struct ticket_lock* t){ 
        t->ticket.store(0);                       
        t->screen.store(0);                       
    }                                             
                                                  
    static void spin_lock(struct ticket_lock* lock){     
        //ecrire un ticket lock                   
        int my = lock->ticket.fetch_add(1) ;      
        while(lock->screen.load() < my){          
            asm volatile("pause");                
        }                                         
    }                                             
                                                  
    static void spin_unlock(struct ticket_lock* lock){   
        lock->screen.fetch_add(1);                
    }


	int size_from_index(int index);

};


class PseudoTLAB {
private:
	batch_t* batch_tab[NBR_OF_LINEAR_ENTRIES + OVERFLOW_EXP];
	SharedMem* shm;
	int tid;
	int thread_offset ;

public:
	Thread* thread;
	uint64_t cumulative_time;
	PseudoTLAB* next;
	int nb_get_batch;
	int nb_loops;

	void init_fake_batch_entries(batch_t** b);
	void initialize(SharedMem* shm_, Thread* thread_);
	HeapWord* allocate(size_t word_size);
	//HeapWord* allocate(size_t word_size);
	void free();
	int index_from_size(int size); 
	size_t batch_index_from_size(size_t size); 
	size_t realsize_from_size(size_t size); 
	size_t batch_index_from_size_slow(size_t size); 

};


#endif //SPACESERVER_SHARED_MEM_HPP
