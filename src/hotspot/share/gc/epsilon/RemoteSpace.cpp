//
// Created by adam on 4/11/22.
//

#include "RemoteSpace.hpp"
#include "rpcMessages.hpp"
#include <arpa/inet.h>
#include <stdlib.h>
#include "gc/epsilon/roots.hpp"
#include <string.h>
#include <csignal>
#include "oops/objArrayKlass.hpp"
#include "oops/instanceMirrorKlass.hpp"
#include "oops/arrayOop.inline.hpp"
#include "classfile/javaClasses.hpp"
#include "oops/oop.hpp"
#include "oops/oop.inline.hpp"
#include "gc/serial/markSweep.hpp"
#include "gc/serial/markSweep.inline.hpp"
#include "memory/iterator.inline.hpp"
#include "gc/shared/referenceProcessor.inline.hpp"
#include "gc/epsilon/gc_helper.hpp"
#include "classfile/javaClasses.hpp"
#include <fcntl.h>
#include <immintrin.h>
#include <time.h>
#include <pthread.h>  
#include <aio.h>
#include <unistd.h>
#include <thread>
#include <chrono>
#include <sys/mman.h>

int sockfd_remote = -1;

std::mutex lock_remote;
std::mutex lock_collect;
//std::mutex lock_collect2;
std::mutex lock_allocbuffer;
std::mutex lock_gc_helper;
std::mutex lock_alloc_print;
std::atomic<bool> collecting;
std::atomic<size_t> softmax;
//AllocationBuffer* alloc_buffer;
std::atomic<uint64_t> free_space;
struct ticket_lock ticket;

void* start_addr;
uint64_t cap;
std::atomic<uint64_t> used_;
int fd_for_heap;
int shm_fd;
void* epsilon_sh_mem;
SharedMem* RemoteSpace::shm;
SharedMem* SharedMem::shm;
void* fsync_constantly(void* arg);
//std::atomic<int> test_collect;  //TODO remove this



RemoteSpace::RemoteSpace() : ContiguousSpace() {
	if(sockfd_remote < 0){
	    sockfd_remote = socket(AF_INET, SOCK_STREAM, 0);
	    if (sockfd_remote < 0)
	    {
	        perror ("socket");
	        exit (EXIT_FAILURE);
	    }
	    server.sin_family = AF_INET;
	    server.sin_addr.s_addr = inet_addr("127.0.0.1");
	    server.sin_port = htons( RSPACE_PORT );
	    if (connect(sockfd_remote, (struct sockaddr *)&server , sizeof(server)) < 0)
	    {
	        puts("connect error 1");
	    }
	}
	int one = 1;
	setsockopt(sockfd_remote, SOL_TCP, TCP_NODELAY, &one, sizeof(one));

    int newsize = 131072;
	setsockopt(sockfd_remote, SOL_SOCKET, SO_SNDBUF, &newsize, sizeof(newsize));
	setsockopt(sockfd_remote, SOL_SOCKET, SO_RCVBUF, &newsize, sizeof(newsize));
    signal(SIGUSR1, start_collect_sig);
    signal(SIGUSR2, end_collect_sig);

#if GCHELPER	
	gchelper.initialize();
#endif

}

RemoteSpace::~RemoteSpace(){
    //close(sockfd_remote);
}

int alloc_fd;

int64_t start_time;

void RemoteSpace::initialize(MemRegion mr, bool clear_space, bool mangle_space) {
	start_time = (int64_t)(((int64_t)os::javaTimeNanos()) / 1e9 );
#if GCHELPER
	_mr = mr;
#endif
    struct msg_initialize msg;
    HeapWord* mr_start = mr.start();
	start_addr = (void*) mr_start;
	printf("Start: %p\n",  mr_start);
	if(UseCompressedClassPointers)
		printf("Used compressed class pointers");

    size_t  mr_word_size = mr.word_size();
	msg.msg_type.type = 'i';
    msg.mr_start =  mr_start;
    msg.mr_word_size =  mr_word_size;
    msg.obj_array_base = (uint32_t) arrayOopDesc::base_offset_in_bytes(T_OBJECT);
	//ioctl(fd_for_heap, 0, 0);
	msg.obj_array_length = arrayOopDesc::length_offset_in_bytes();
	msg.mirror_static_offset = InstanceMirrorKlass::offset_of_static_fields();
	msg.mirror_static_count_offset = java_lang_Class::_static_oop_field_count_offset;
	msg.referent_offset = java_lang_ref_Reference::referent_offset;
	msg.discovered_offset = java_lang_ref_Reference::discovered_offset;
    msg.clear_space = clear_space;
    msg.mangle_space = mangle_space;
	msg.pid = getpid();
    write(sockfd_remote, &msg, sizeof(struct msg_initialize));
	//printf("\nPID: %d\n", getpid());
	//sleep(5);
	//
	
	RemoteSpace::poule = nullptr;
	cap = mr_word_size*8;
	free_space.store(cap);
	//softmax.store((cap*SOFTMAX_PER)/100);
	softmax.store((GB)/100);
	used_.store(0);
	collecting.store(false);
	
	//alloc_buffer = (AllocationBuffer*) malloc(sizeof(AllocationBuffer));
	//alloc_buffer->initialize(sockfd_remote);

	alloc_fd = creat("alloc_data", 0);


	epsilon_sh_mem = setup_shm();
	shm = (SharedMem*) malloc(sizeof(SharedMem));
	shm->initialize(epsilon_sh_mem, this);
	SharedMem::shm = shm;
	
	int ret = madvise(start_addr, cap,  MADV_NOHUGEPAGE );
	madvise(start_addr, cap,  MADV_RANDOM ); //TODO C'est la ligne la plus importante de ma thèse

	printf("madvise ret: %d\n",ret);
	
	SharedMem::init_ticket_lock(&ticket);
	pthread_t fsync_thread;
	pthread_create(&fsync_thread, NULL,fsync_constantly, NULL);
	
	
}

void* fsync_constantly(void* arg){
	while(true){
		msync(start_addr, cap, MS_SYNC);
		usleep(100000);
		//sleep();
	}	
}

void RemoteSpace::post_initialize(){
#if GCHELPER
	_span_based_discoverer.set_span(_mr);
	rp = new ReferenceProcessor(&_span_based_discoverer);
	gchelper.set_rp(rp);
#endif
}

HeapWord *RemoteSpace::par_allocate(size_t word_size) {
    counter++;
    struct msg_par_allocate * msg = (struct msg_par_allocate*) calloc(1,sizeof(struct msg_par_allocate));

	msg->msg_type.type = 'a';
    msg->word_size =  word_size;

    SharedMem::spin_lock(&ticket);
    write(sockfd_remote, msg, sizeof(struct msg_par_allocate));

    HeapWord** result = (HeapWord**) malloc(sizeof(HeapWord*));
    read(sockfd_remote, result, sizeof(HeapWord*));
    SharedMem::spin_unlock(&ticket);
    std::free(msg);
    HeapWord * allocated = *result;
    //if(allocated == nullptr){
    //    collect();
    //}
    std::free(result);
    return allocated;
}


HeapWord *RemoteSpace::par_allocate_klass(size_t word_size, Klass* klass) {
	HeapWord* allocated;
	//printf("Test alloc wait lock");
	//printf("Tentative d'alloc\n");

	//lock_collect.lock();
	SharedMem::spin_lock(&ticket);
	struct msg_par_allocate_klass msg ;
	msg.msg_type.type = 'k';
    msg.word_size = static_cast<uint32_t>(word_size);
    msg.klass = static_cast<uint32_t>((uint64_t) klass);
	write(sockfd_remote, &msg, sizeof(struct msg_par_allocate_klass));
    struct msg_alloc_response result ;
    read(sockfd_remote, &result, sizeof(msg_alloc_response));
	SharedMem::spin_unlock(&ticket);

	allocated = result.ptr;


    if(allocated != nullptr){
//#if GCHELPER
//		if(klass->is_instance_klass() &&( ((InstanceKlass*)klass)->is_reference_instance_klass() || ((InstanceKlass*)klass)->is_mirror_instance_klass() || ((InstanceKlass*)klass)->is_class_loader_instance_klass() )){
//		//if(klass->is_instance_klass() &&( ((InstanceKlass*)klass)->is_reference_instance_klass() || ((InstanceKlass*)klass)->is_mirror_instance_klass() )){
//		//if(klass->is_instance_klass() && ( ((InstanceKlass*)klass)->is_reference_instance_klass() )){
//			gchelper.add_root(allocated);
//		}
//		//else if(klass->is_objArray_klass() && (strstr(klass->external_name(), "reflect.Method") != NULL || strstr(klass->external_name(), "reflect.Constructor") != NULL) ){
//	//	////else if(klass->is_objArray_klass() && strstr(klass->external_name(), "reflect") != NULL ){
//		//	lock_gc_helper.lock();
//		//	gchelper.add_root(allocated);
//		//	lock_gc_helper.unlock(); 
//		//}
//#endif

		//lock_alloc_print.lock();
		//dprintf(alloc_fd,"%p, %s, collection: %d\n", allocated, klass->external_name(), test_collect.load());
		//lock_alloc_print.unlock();


		used_.fetch_add(word_size*8);// + HEADER_OFFSET);
        //uint64_t iptr = (uint64_t)allocated;
		//uint32_t short_klass = Klass::encode_klass_not_null(klass);

	//	//printf("Klass: %u, name: %s\n", short_klass, allocated,klass->external_name());
		////printf("Klass: %u, addr: %p, size: %lu, name: %s\n", short_klass,allocated, word_size,klass->external_name());

		//if(klass->is_objArray_klass()){
		//	*((uint32_t*)(iptr - KLASS_OFFSET)) = short_klass + 1;
		//}else if(klass->is_typeArray_klass()){
		//	*((uint32_t*)(iptr - KLASS_OFFSET)) = 42;
		//}else if(klass->is_instance_klass()){
		//	*((uint32_t*)(iptr - KLASS_OFFSET)) = short_klass;
		//}
        //*((uint32_t*)(iptr - SIZE_OFFSET)) = (uint32_t) word_size ;// +1;

		//return allocated;
    }

	//SharedMem::spin_unlock(&ticket);
	//lock_collect.unlock();

	//printf("Alloc: %p\n", allocated);
    return allocated;
}

void RemoteSpace::concurrent_post_allocate(HeapWord*allocated, size_t word_size, Klass* klass){

	//lock_collect.lock();
//#if GCHELPER

//	if(klass->is_instance_klass() &&( ((InstanceKlass*)klass)->is_reference_instance_klass() || ((InstanceKlass*)klass)->is_mirror_instance_klass() || ((InstanceKlass*)klass)->is_class_loader_instance_klass() )){
//	//if(klass->is_instance_klass() &&( ((InstanceKlass*)klass)->is_reference_instance_klass() || ((InstanceKlass*)klass)->is_mirror_instance_klass() )){
//	//if(klass->is_instance_klass() &&( ((InstanceKlass*)klass)->is_reference_instance_klass() )){
//	//	//lock_collect.lock();
//		gchelper.add_root(allocated);
//	////	//lock_collect.unlock();
//	}
//	//else if(klass->is_objArray_klass() && strstr(klass->external_name(), "reflect") != NULL){
//	////////else if(klass->is_objArray_klass() && (strstr(klass->external_name(), "reflect.Method") != NULL || strstr(klass->external_name(), "reflect.Constructor") != NULL) ){
//	//	lock_gc_helper.lock();
//	//	gchelper.add_root(allocated);
//	//	lock_gc_helper.unlock(); 
//	//}
//
//	//}
//#endif

	//lock_alloc_print.lock();
	//printf("%p, %s, collection: %d\n", allocated, klass->external_name(), test_collect.load());
	//lock_alloc_print.unlock();


	//used_.fetch_add(word_size*8);
    //uint64_t iptr = (uint64_t)allocated;
	//uint32_t short_klass = static_cast<uint32_t>((uint64_t)klass);

	//if(klass->is_instance_klass()){
	//	*((uint32_t*)(iptr - KLASS_OFFSET)) = short_klass;
	//}else if(klass->is_objArray_klass()){
	//	*((uint32_t*)(iptr - KLASS_OFFSET)) = short_klass + 1;
	//}else if(klass->is_typeArray_klass()){
	//	*((uint32_t*)(iptr - KLASS_OFFSET)) = 42;
	//}

//	*((uint32_t*)((uint64_t)allocated - KLASS_OFFSET)) = (klass->is_typeArray_klass()) ? 42 : ( Klass::encode_klass_not_null(klass) + klass->is_objArray_klass() ) ;

	//else{printf("Caca Boudin\n");}
    //*((uint32_t*)((uint64_t)allocated - SIZE_OFFSET)) = (uint32_t) word_size;
    *(uint32_t*)((uint64_t)allocated) = (uint32_t) word_size;
}

void RemoteSpace::slow_path_post_alloc(uint64_t used_local){

	used_.fetch_add(used_local*8);
	if(used_glob() >= (softmax.load()*COLLECTION_THRESHOLD)/100  ){
		bool collected = false;
		if(collecting.compare_exchange_strong(collected, true)){
			collected = false;
			start_collect_sig(0);
		}
	}

	//lock_collect.unlock();
	//

}

void RemoteSpace::update_size(uint64_t used_local){
	//used_.fetch_add(used_local*8);


	//lock_collect.unlock();
	//

}

void RemoteSpace::set_end(HeapWord* value){
    struct msg_set_end msg;
	msg.msg_type.type = 'e';
    msg.value = value;
    write(sockfd_remote, &msg, sizeof(struct msg_set_end));
}

size_t RemoteSpace::used() const{
	return cap - free_space.load() + used_.load();
}

size_t used_glob(){
	return cap - free_space.load() + used_.load();
}

size_t RemoteSpace::capacity() const{
    return cap;
}


void getandsend_roots(){
    RootMark rm;
    rm.do_it();
    uint64_t array_length = (uint64_t) rm.getArraySize();
    uint64_t* root_array = rm.rootArray();
    write(sockfd_remote, &array_length, sizeof(uint64_t));
	//printf("Send roots, size: %lu\n", array_length);
    int res = write(sockfd_remote, root_array, sizeof(uint64_t)*array_length );
}

#if GCHELPER
void RemoteSpace::getandsend_helper(){
	//lock_gc_helper.lock();
    //gchelper.do_it();
    uint64_t array_length = (uint64_t) gchelper.getArraySize();
    uint64_t* helper_array = gchelper.helperArray();
	//printf("Send helper roots, size: %lu\n", array_length);
    write(sockfd_remote, &array_length, sizeof(uint64_t));
    int res = write(sockfd_remote, helper_array, sizeof(uint64_t)*array_length );
	//lock_gc_helper.unlock();
}
#endif


void RemoteSpace::safe_object_iterate(ObjectClosure* blk){return;};

void RemoteSpace::print_on(outputStream* st) const{return;}

void RemoteSpace::stw_pre_collect() {

	//printf("Total time in while (ns): %lu, nbr threads: %d\n",RemoteSpace::shm->get_while_time(), RemoteSpace::shm->nbr_threads());
	//exit(0);

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
	//lock_collect.lock();
	//test_collect.fetch_add(1);
	printf("[téléGC] Start of collection: Used: %luM (%lu%%), timestamp: %lds\n",  used_glob()/(1024*1024), (used_glob()*100)/cap, (int64_t)(((int64_t) os::javaTimeNanos())/1e9) - start_time );
	if(!rp_init){
		rp_init = true;
		post_initialize();
	}

	size_t start_used  = used();
	//printf("cap: %lu, used: %lu\n", cap, used_.load());

	//struct timeval start, end;
	//gettimeofday(&start, NULL);

	struct msg_collect msg;
	msg.msg_type.type = 'c';
	msg.base =(uint64_t) Universe::narrow_oop_base();
	msg.shift = Universe::narrow_oop_shift();
	msg.static_base = (uint32_t) InstanceMirrorKlass::offset_of_static_fields();
	msg.static_length = java_lang_Class::static_oop_field_count_offset();

	SharedMem::spin_lock(&ticket);

	write(sockfd_remote, &msg, sizeof(struct msg_collect));  // on envoie la base et le shift pour la conversion narrowoop en oop
	getandsend_roots();
#if GCHELPER
	getandsend_helper();
#endif


//	_mm_mfence();
	//int ret = fdatasync(fd_for_heap);
	msync(start_addr, cap, MS_SYNC);
	//printf("Ret: %d\n", ret);
	ioctl(fd_for_heap, 0, 1); //VERY IMPORTANT THAT THE IOCTL IS AFTER THE FSYNC
	int done = 1;
	write(sockfd_remote, &done, sizeof(int));  

	read(sockfd_remote, &done, sizeof(int));
	//struct deadbeef_req_t msg2;
	//read(sockfd_remote, &msg2, sizeof(deadbeef_req_t));
	//while(msg2.msg_type == 23){
	//	//printf("Collected addr: %p\n", (void*)msg2.addr);
	//	*((uint32_t*)((uint64_t)msg2.addr - KLASS_OFFSET)) = 0xffffffff;
	//	for(uint32_t i =0; i< msg2.size-1; i++){
	//		*(((uint64_t*)msg2.addr)+i) = 0xdeadbeefdeadbeef;
	//	}
	//	read(sockfd_remote, &msg2, sizeof(deadbeef_req_t));
	//}	
	SharedMem::spin_unlock(&ticket);

	clock_gettime(CLOCK_MONOTONIC, &end);
	printf("Collection setup took: %lums\n", (uint64_t)( (end.tv_sec - start.tv_sec)*1e3 + (end.tv_nsec - start.tv_nsec)/1e6));

}


void start_collect_sig(int sig) {
	EpsilonHeap::heap()->vm_collect_impl();
}

void* flush_wb(void* arg){
	ioctl(fd_for_heap, 0, 2);
	collecting.store(false);
	return NULL;
}


void end_collect_sig(int sig) {
	pthread_t flush_thread;
	//lock_collect2.lock();
	free_space.store(0); //TODO uncomment this
	used_.store(0); //TODO uncomment this
	//RemoteSpace::shm->reset_used();

	//read(sockfd_collect, &free_space, sizeof(uint64_t));
	//lock_allocbuffer.lock();
	//alloc_buffer->free_all();
	//lock_allocbuffer.unlock();
	//ioctl(fd_for_heap, 0, 2);
	pthread_create(&flush_thread, NULL, flush_wb, NULL);
	opcode msg;
	msg.type = 'f';
	SharedMem::spin_lock(&ticket);
	write(sockfd_remote, &msg, sizeof(uint64_t));   //TODO uncomment this
	read(sockfd_remote, &free_space, sizeof(uint64_t)); //TODO uncomment this
	//printf("Swept: %lu\n", free_space);
	SharedMem::spin_unlock(&ticket);

	CodeCache::gc_epilogue();
	JvmtiExport::gc_epilogue();

	//gettimeofday(&end, NULL);
	//printf("[/téléGC] Collection:  Time: %lds;  Used before: %lu; Used after: %lu\n", end.tv_sec - start.tv_sec, start_used, used() );
	//printf("\n");
	printf("[téléGC] Collection: Used after: %luM (%lu%%), timestamp:%lds\n",  used_glob()/(1024*1024), (used_glob()*100)/cap, (int64_t)(((int64_t) os::javaTimeNanos())/1e9) - start_time );

	softmax.store(minou(used_glob()*3, cap));
	//softmax.store(maxou(minou(used_glob()*3, cap), (cap*SOFTMAX_PER)/100 ));

	//collecting.store(false);
	//lock_collect2.unlock();
	//lock_collect.unlock();
}




#if DEADBEEF
void RemoteSpace::deadbeef_area(uint64_t addr, int size, int type){
	uint64_t lorem;
	if(type == 42){
		lorem = 0xdeadbeefdeadbeef;
	}else{
		lorem = 0xcafebabecafebabe;
	}
	for(int i=0; i<size; i+=8){
		*((uint64_t*) (addr + i)) = lorem;
	}
}
#endif




void* setup_shm(){
	void* ret_addr;
	shm_fd = shm_open(SHM_NAME, O_RDWR, 0600);

 	if (shm_fd == -1)
 	    exit(-1);
 	/* Map the object into the caller's address space. */

 	ret_addr = mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, shm_fd, 0);
 	if (ret_addr == MAP_FAILED)
		exit(-1);

	return ret_addr;
}
