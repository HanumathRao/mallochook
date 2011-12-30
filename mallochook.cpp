#include <iostream>
#include <fstream>
#include <sstream>
#include <execinfo.h>
#include <signal.h>
#include <sys/time.h>
#include <boost/unordered_map.hpp>
#include <pthread.h>
using namespace std;

#define DEPTH 20
struct memoryblock{
	void* stack[DEPTH];
	size_t stackdepth;
	size_t bytes;
	bool reallocated;
	uint64_t alloc_time;

	memoryblock(): bytes(0), reallocated(false) {}
	memoryblock(size_t bytes): bytes(bytes), reallocated(false) {}
};

typedef boost::unordered_map<void*, memoryblock> boostmap;

static void* dumpmemoryinfo(void*);

static __thread bool inhook=false;
static pthread_mutex_t mutex;
static pthread_cond_t doyourjob;
static void* dumpmemoryinfo(void*);
static uint64_t start;

static void wakedumper(int){
	pthread_cond_signal(&doyourjob);
}

static uint64_t gettimemillis(){
	timeval t;
	gettimeofday(&t, 0);
	return uint64_t(t.tv_sec)*1000L+(t.tv_usec/1000L);
}

static int init(){
	pthread_mutex_init(&mutex, 0);
	pthread_cond_init(&doyourjob, 0);
	pthread_t id;
	pthread_create(&id, 0, dumpmemoryinfo, 0);
	signal(SIGUSR1, wakedumper);
	start=gettimemillis();
	cout<<"libmallokhook loaded"<<endl;
	return 0;
}

static boostmap& getmap(){
	static boostmap memoryinfo;
	static volatile int inithook=init();
	return memoryinfo;
}

extern "C" void* __libc_malloc(size_t);
extern "C" void* malloc(size_t size){
	void* result=__libc_malloc(size);
	if(inhook) return result;
	inhook=true;
	boostmap& memoryinfo=getmap();

	memoryblock info(size);
	info.alloc_time=gettimemillis();
	info.stackdepth=backtrace(info.stack, DEPTH);
	pthread_mutex_lock(&mutex);
	memoryinfo[result]=info;
	pthread_mutex_unlock(&mutex);

	inhook=false;
	return result;
}

extern "C" void* __libc_realloc(void*, size_t);
extern "C" void* realloc(void* ptr, size_t size){
	void* result=__libc_realloc(ptr, size);
	if(inhook) return result;
	inhook=true;
	boostmap& memoryinfo=getmap();

	pthread_mutex_lock(&mutex);
	boostmap::iterator it=memoryinfo.find(ptr);
	if(it!=memoryinfo.end()){
		(memoryinfo[result]=it->second).reallocated=true;
		memoryinfo.erase(it);
	}
	pthread_mutex_unlock(&mutex);

	inhook=false;
	return result;
}

extern "C" void __libc_free(void*);
extern "C" void free(void* ptr){
	__libc_free(ptr);
	if(inhook) return;
	inhook=true;
	boostmap& memoryinfo=getmap();

	pthread_mutex_lock(&mutex);
	boostmap::iterator it=memoryinfo.find(ptr);
	if(it!=memoryinfo.end()) memoryinfo.erase(it);
	pthread_mutex_unlock(&mutex);

	inhook=false;
}

static void* dumpmemoryinfo(void*){
	while(true){
		pthread_mutex_lock(&mutex);
		pthread_cond_wait(&doyourjob, &mutex);
		inhook=true;
		boostmap& memoryinfo=getmap();

		stringstream filename;
		static int uniquifier=0;
		static uint64_t lastclock=0;
		uint64_t nowclock=gettimemillis();
		if(!lastclock || lastclock!=nowclock){
			uniquifier=0;
			lastclock=nowclock;
		}
		filename<<"memoryinfo_"<<nowclock-start<<'_'<<uniquifier++;
		ofstream out(filename.str().c_str());
		out<<"memoryinforaw={"<<endl;
		for(boostmap::iterator it=memoryinfo.begin(); it!=memoryinfo.end(); it++){
			if(it!=memoryinfo.begin()) out<<","<<endl;
			memoryblock& bl=it->second;
			out<<"{"<<(bl.reallocated?"True":"False")<<", "<<bl.bytes<<", {";
			for(size_t i=0; i<bl.stackdepth; i++) out<<(i?", ":"")<<(unsigned long int)(bl.stack[i]);
			out<<"}, "<<it->second.alloc_time-start<<"}";
		}
		out<<endl<<"};";
		out.close();
		pthread_mutex_unlock(&mutex);

		inhook=false;
	}
	return 0;
}
