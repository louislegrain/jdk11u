//
// Created by adam on 4/11/22.
//

#ifndef JDK11U_REMOTESPACE_HPP
#define JDK11U_REMOTESPACE_HPP

#include "gc/shared/space.hpp"
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <mutex>
#include "gc/epsilon/epsilonHeap.hpp"
#include "gc/epsilon/rpcMessages.hpp"
#include "utilities/globalDefinitions.hpp"
#include "gc/epsilon/gc_helper.hpp"
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "gc/epsilon/AllocationBuffer.hpp"
#include "gc/epsilon/SharedMem.hpp"

#define minou(a,b) ((a)>(b)?(b):(a))
#define maxou(a,b) ((a)>(b)?(a):(b))

struct range_t {
	char* base;
	size_t size;
};

struct hw_list{
	oop hw;
	struct hw_list* next;
};

void getandsend_roots();
void start_collect_sig(int sig);
void end_collect_sig(int sig);
void* setup_shm(); 
size_t used_glob();

extern std::mutex lock_remote;
extern std::mutex lock_collect;
//extern std::mutex lock_collect2;
extern std::mutex lock_allocbuffer;
extern std::mutex lock_gc_helper;
extern std::mutex lock_alloc_print;
extern std::atomic<bool> collecting;
extern int sockfd_remote;
//extern AllocationBuffer* alloc_buffer;
extern std::atomic<uint64_t> free_space;
extern void* start_addr;
extern uint64_t cap;
extern std::atomic<size_t> softmax;
extern std::atomic<uint64_t> used_;
extern int shm_fd;
extern int fd_for_heap;
extern void* epsilon_sh_mem;
extern struct ticket_lock ticket;



//extern std::atomic<int> test_collect; //TODO remove this


class RemoteSpace : public ContiguousSpace{


public:

	static SharedMem* shm;

	static struct hw_list* poule;
	
	static void add_poule(oop hw){
		struct hw_list* new_poule = (struct hw_list*) malloc(sizeof(struct hw_list));
		new_poule->hw = hw;
		new_poule->next = poule;
		poule = new_poule;
	}

private:
    int counter;
    bool roots = true;
    struct sockaddr_in server;
	bool collected = false;
	struct range_t* heap_range;
	bool rp_init = false;
	float col_threshold = COLLECTION_THRESHOLD/100;
#if GCHELPER
	GCHelper gchelper;
	SpanSubjectToDiscoveryClosure _span_based_discoverer;
	ReferenceProcessor* rp;
	MemRegion _mr;
#endif

	int alloc_fd;


public:
    RemoteSpace();
    ~RemoteSpace();

    void initialize(MemRegion mr, bool clear_space, bool mangle_space);
    void post_initialize();
    HeapWord* par_allocate(size_t word_size);
    HeapWord* par_allocate_klass(size_t word_size, Klass* klass);
    void concurrent_post_allocate(HeapWord* allocated, size_t word_size, Klass* klass);
	void slow_path_post_alloc(uint64_t used_local);
	void update_size(uint64_t used_local);
    void set_end(HeapWord* value);
    size_t used() const;
    size_t capacity() const;
    void safe_object_iterate(ObjectClosure* blk);
    void print_on(outputStream* st) const;

    void stw_pre_collect();
    //void collect();

    void set_fd(int fd){ fd_for_heap = fd;}

	void deadbeef_area(uint64_t addr, int size, int type);

    void set_range(char* base, size_t size){
		heap_range = (struct range_t*) malloc(sizeof(struct range_t));
		heap_range->base = base; 
		heap_range->size = size; 
	};

#if GCHELPER
	void getandsend_helper();
#endif

};


#endif //JDK11U_REMOTESPACE_HPP
