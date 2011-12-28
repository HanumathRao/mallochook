#include <iostream>
#include <fstream>
#include <sstream>
#include <execinfo.h>
#include <signal.h>
#include <ctime>
#include <boost/unordered_map.hpp>
#include <pthread.h>
using namespace std;

#define DEPTH 20
struct memoryblock{
	void* stack[DEPTH];
	size_t stackdepth;
	size_t bytes;
	bool reallocated;

	memoryblock(): bytes(0), reallocated(false) {}
	memoryblock(size_t bytes): bytes(bytes), reallocated(false) {}
};

typedef boost::unordered_map<void*, memoryblock> boostmap;

static boostmap* getmemoryinfo();
static void* dumpmemoryinfo(void*);

static __thread bool inhook=false;
static pthread_mutex_t mutex;
static pthread_cond_t doyourjob;
static boostmap* volatile memoryinfo=getmemoryinfo();
static void* dumpmemoryinfo(void*);

static void wakedumper(int){
	pthread_cond_signal(&doyourjob);
}

//ensure that memoryinfo and other variables are set up on first call, it seems that it can happen even before module initialization
static boostmap* getmemoryinfo(){
	if(memoryinfo) return memoryinfo;
	pthread_mutex_init(&mutex, 0);
	pthread_cond_init(&doyourjob, 0);
	memoryinfo=new boostmap();
	pthread_t id;
	pthread_create(&id, 0, dumpmemoryinfo, 0);
	signal(SIGUSR1, wakedumper);
	return memoryinfo;
}

extern "C" void* __libc_malloc(size_t);
extern "C" void* malloc(size_t size){
	void* result=__libc_malloc(size);
	if(inhook) return result;
	inhook=true;
	boostmap& memoryinfo=*getmemoryinfo();

	memoryblock info(size);
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
	boostmap& memoryinfo=*getmemoryinfo();

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
	boostmap& memoryinfo=*getmemoryinfo();

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
		boostmap& memoryinfo=*getmemoryinfo();

		stringstream filename;
		static int uniquifier=0;
		static int lastclock=-1;
		clock_t nowclock=clock();
		if(lastclock<0 || lastclock!=nowclock){
			uniquifier=0;
			lastclock=nowclock;
		}
		filename<<"memoryinfo_"<<nowclock<<'_'<<uniquifier++;
		ofstream out(filename.str().c_str());
		out<<"memoryinforaw={"<<endl;
		for(boostmap::iterator it=memoryinfo.begin(); it!=memoryinfo.end(); it++){
			if(it!=memoryinfo.begin()) out<<","<<endl;
			memoryblock& bl=it->second;
			out<<"{"<<(bl.reallocated?"True":"False")<<", "<<bl.bytes<<", {";
			for(size_t i=0; i<bl.stackdepth; i++) out<<(i?", ":"")<<(unsigned long int)(bl.stack[i]);
			out<<"}}";
		}
		out<<endl<<"};";
		out.close();
		pthread_mutex_unlock(&mutex);

		inhook=false;
	}
	return 0;
}
