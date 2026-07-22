#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <sched.h>
#include <x86intrin.h>
#include <sys/mman.h>
#include <unistd.h>
#include <numa.h>
#include <numaif.h>
#define PAGE_SIZE 4096

// GOAL: Write a program where every vCPU accesses an "anchor" page, measuring the latency and grouping together these vCPUs into the same NUMA node. Also measuring the number of NUMA nodes with this.  

int compare(const void *a, const void *b) { 
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}
int main()
{
   uint64_t acc_table[10000];
// Create an anchor page

   char *page = numa_alloc_onnode(PAGE_SIZE, 0);
   if (!page) {
     fprintf(stderr, "numa_alloc_onnode failed\n"); return 1;
   }
   page[0] = 100;
   int actual = -1;
   get_mempolicy(&actual, NULL, 0, page, MPOL_F_NODE | MPOL_F_ADDR);
   //printf("requested node 0, page actually on node %d\n", actual); test code 

//Pin thread to each vCPU 
  int cpu;
  long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
  for (cpu = 0; cpu < ncpu; cpu++) { 
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set); 
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
       perror("sched_setaffinity");
       continue;
    }
    sched_yield();
//  printf("This thread is on vCPU %d\n", sched_getcpu()); test code
    
    //Measure access latency
    unsigned aux;
    volatile char access;
    
    for (int acc = 0; acc < 10000; acc++) {
      _mm_clflush(page);
      _mm_mfence();
      _mm_lfence();    
      uint64_t preaccess = __rdtscp(&aux);
      _mm_lfence();
      access = page[0];
      uint64_t postaccess = __rdtscp(&aux);
      _mm_lfence();
      uint64_t latency = postaccess - preaccess;
      acc_table[acc] = latency;
    } 
    qsort(acc_table, 10000, sizeof(uint64_t), compare);
    //for (uint64_t element = 0; element < 10000; element++) { printf("%lu ", acc_table[element]);} test code
    printf("vCPU %d -> node %d: %lu median cycles\n", sched_getcpu(), actual, acc_table[5000]);

  }
  
  numa_free(page, PAGE_SIZE);
 	  
  return 0;
}
