#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <sched.h>
#include <pthread.h>
#include <x86intrin.h>
#include <stdint.h>

#define ITERS 10000


//Goal: write a program that uses cache line transfer latency to gauge how many physical NUMA nodes are present in the VM and how each vCPU maps to such nodes. 


static volatile uint64_t pong __attribute__((aligned(64)));
static char pad[64];   

void *worker(void *arg){
  int cpu1 = *(int *)arg;
  cpu_set_t set1;
  CPU_ZERO(&set1);
  CPU_SET(cpu1, &set1);
  if (sched_setaffinity(0, sizeof(set1), &set1) != 0){
    perror("sched_setaffinity");
    return NULL;
  }
  
  fprintf(stderr, "Worker: pinned to CPU %d (actually on %d), entering loop\n", cpu1, sched_getcpu());

  for (int i = 0; i < ITERS; i++){
     while (pong != 2*i + 1) ;
     pong = 2 * i + 2;
  }
  return NULL; 
}
int main(){
  
  long ncpu = sysconf(_SC_NPROCESSORS_ONLN);

  //Pin 2 threads to 2 seperate vCPUs
    int cpu0 = 0;
    int cpu1 = 1;

    if (cpu0 >= ncpu || cpu1 >= ncpu) { 
      fprintf(stderr, "CPU index out of range (ncpu=%ld)\n", ncpu);
      return 1;
    }

    pthread_t t1;
    pthread_create(&t1, NULL, worker, &cpu1); 
      
  cpu_set_t set0;
  CPU_ZERO(&set0);
  CPU_SET(cpu0, &set0);
  if (sched_setaffinity(0, sizeof(set0), &set0) != 0){
    perror("sched_setaffinity");
    return 1;
  }

  fprintf(stderr, "main: pinned to cpu %d (actually on %d)\n", cpu0, sched_getcpu()); 

  sched_yield(); 
     
   unsigned aux;
   
   _mm_lfence();
   uint64_t preaccess = __rdtscp(&aux);
   fprintf(stderr, "main: entering ping loop\n");

   for (int i = 0; i < ITERS; i++){
     while (pong != 2 * i) ; 
       pong = (2*i) + 1;
   }  
   
   while (pong != 2*ITERS) ;

   uint64_t postaccess = __rdtscp(&aux);
   _mm_lfence();
   fprintf(stderr, "main: finished ping loop\n");
   pthread_join(t1, NULL);

   uint64_t diff = postaccess - preaccess;

   printf("%d round trips took %lu tsc ticks (%lu ticks / one way transfer).\n", ITERS, (unsigned long)diff, (unsigned long) (diff / (2 * ITERS)));
		  
  
   return 0;
}
