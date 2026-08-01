#define _GNU_SOURCE
#define ITERS 100000ULL
#define PAGE_SIZE 4096
#define SAMPLES 100000
#include <stdbool.h>
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include <sched.h>
#include <pthread.h>
#include <x86intrin.h>
#include <stdint.h>
#include <pthread.h>

//simple compare func used for later
int compare(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

//fills cacheline with variable pong + padding (struct ensures nothing gets between) 
static struct {
  volatile uint64_t pong;
  char pad[64 - sizeof(uint64_t)];
} pong_line __attribute__((aligned(64)));

//stop signal so the worker thread can quit
static struct {
  volatile int stop;
  char pad[60];
} ctl __attribute__((aligned(64)));

// part of the ping pong code that responds to incrementing the pong variable
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
  for (uint64_t i = 0; i < ITERS; i++){
     while (pong_line.pong != 2*i + 1) if (ctl.stop) return NULL;
     pong_line.pong = 2 * i + 2;
  }
  return NULL;
}

int main(){
//setting local, remote, mix bools, + a table for latency mapping and ncpu is the #cpus. i and j are for loops
	
  bool all_local = false, all_remote = false, mix_LR = false;  
  long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
  uint64_t latency_mapping[ncpu][ncpu];
  int i, j;

//from each vcpu to every other vcpu (no duplicate vcpu pairings or same vcpu <-> same vcpu)              First setting all of latency_mapping to 0

  for (i = 0; i < ncpu; i++){
    for (j = 0; j < ncpu; j++){
      latency_mapping[i][j] = 0;
    }
  }

//this is where each cache line transfer latency happens. create a thread and pin it to each vCPU   
  for (i = 0; i < ncpu; i++){
    for (j = i+1; j < ncpu; j++){
      int cpu0 = i;
      int cpu1 = j;
      if (cpu0 >= ncpu || cpu1 >= ncpu) {
        fprintf(stderr, "CPU index out of range (ncpu=%ld)\n", ncpu);
        return 1;
      }
      pthread_t t1;
      pong_line.pong = 0;
      ctl.stop = 0;
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
      unsigned aux0, aux1;

      //check tsc ticks before pong access. lfence stops the CPU from running later instructions early.
      uint64_t preaccess = __rdtscp(&aux0);
      _mm_lfence();
      fprintf(stderr, "main: entering ping loop\n");
      
      //deadline is the time to give up, after ~4ish seconds, something is broken. timed out is essentially a boolean that forces it to send a pong incrementation back
      uint64_t deadline = __rdtsc() + (3ULL << 32);
      int timed_out = 0;

      for (uint64_t k = 0; k < ITERS; k++){
        while (pong_line.pong != 2 * k) {
          if (__rdtsc() > deadline) { timed_out = 1; break; }
        }
        if (timed_out) break;
        pong_line.pong = (2*k) + 1;
      }

      while (!timed_out && pong_line.pong != 2*ITERS) {
        if (__rdtsc() > deadline) timed_out = 1;
      }
      
      //check tsc ticks after pong access
      uint64_t postaccess = __rdtscp(&aux1);
      _mm_lfence();

      //error check
      if (timed_out || (aux0 & 0xfff) != (aux1 & 0xfff)) {
        ctl.stop = 1;
        pthread_join (t1, NULL);
        fprintf(stderr, "pair (%d,%d): migrated %u -> %u, discarding\n", i, j, aux0 & 0xfff, aux1 & 0xfff);
        continue;
      }

      // get diff here and plug it into mapping
      fprintf(stderr, "main: finished ping loop\n");
      pthread_join(t1, NULL);
      uint64_t diff = postaccess - preaccess;
      latency_mapping[i][j] = diff / (2ULL * ITERS);
     }
   }

   // print cache latency of each vcpu entry (purely for viewing) 
   for (i = 0; i < ncpu; i++){
     for (j = i+1; j < ncpu; j++){
       printf("vCPU %d and vCPU %d have a cache latency of %lu\n", i, j, latency_mapping[i][j]);
     }
   }
   
   // vals holds each latency mapping in a 1D array to be sorted
   uint64_t vals[ncpu * ncpu];
   int nv = 0;

   for (i = 0; i < ncpu; i++){
     for (j = i + 1; j < ncpu; j++){
        if (latency_mapping[i][j] > 0)      
          vals[nv++] = latency_mapping[i][j];
     }
   }  

   //sort plain list
   qsort(vals, nv, sizeof(uint64_t), compare);
   
   // best is the biggest jump in ratios when dividing one by another. T determines where the line is drawn. r is one jump. T is sqrt in order to find geometric mean (equal magnitude) 
   double best = 1.0, T = 0.0;
   
   for (int k = 0; k + 1 < nv; k++) {
     double r = (double)vals[k+1] / (double)vals[k];
     if (r > best) { best = r; T = sqrt((double)vals[k] * (double)vals[k+1]); }
   }
    // if best > or < 2.0, that's enough to determine a local / remote split. 
    //
    
    if (best < 1.80){
      printf("largest gap %.2fx, confirming if it is local or remote:\n", best);
    }

    else { //Assign vCPU to different nodes. nr_rooms counts the number of nodes. 
      
      mix_LR = true; 
      printf("threshold %.0f (gap %.2fx)\n", T, best);

      int node_id[ncpu];
      int nr_nodes = 0;
      for (i = 0; i < ncpu; i++) node_id[i] = -1;

      for (i = 0; i < ncpu; i++){
        if (node_id[i] != -1) continue;      
        node_id[i] = nr_nodes;               
        for (j = i + 1; j < ncpu; j++){
          if (node_id[j] != -1) continue;
          uint64_t pair = latency_mapping[i][j];
          if (pair > 0 && (double)pair < T) node_id[j] = nr_nodes;
        }
        nr_nodes++;
      }
//print Node and it's corresponding vCPU 
      for (int n = 0; n < nr_nodes; n++){
        printf("Node %d: vCPU", n);
        for (i = 0; i < ncpu; i++)
          if (node_id[i] == n) printf(" %d", i);
        printf("\n");
      }
      printf("These are %d nodes\n", nr_nodes);

    }   
    
  //Only get here if all ratios are similar, indicating it's either all vcpus are local to each other or all remote
    
    if (mix_LR == false){

    //anchor page logic

      char *anchor[ncpu];
      uint64_t page_matrix[ncpu][ncpu];
      int owner, reader;
      for (owner = 0; owner < ncpu; owner++){
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(owner, &set);
        if (sched_setaffinity(0, sizeof(set), &set) != 0){
          perror("sched_setaffinity");
          return 1;
        }
        sched_yield();

        // asks for blank memory, stores it in anchor pointer
	
        anchor[owner] = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
                             MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        if (anchor[owner] == MAP_FAILED){
          perror("mmap");
          return 1;
        }

	//Advises OS to not convert into a huge page
        madvise(anchor[owner], PAGE_SIZE, MADV_NOHUGEPAGE);
        anchor[owner][0] = 1;   // first touch: fault the page in from THIS vCPU (It is understood this is not guaranteed) 
      }
        
      //How long each vcpu takes to read each page 

      for (reader = 0; reader < ncpu; reader++){
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(reader, &set);
        if (sched_setaffinity(0, sizeof(set), &set) != 0){
          perror("sched_setaffinity");
          return 1;
        }
        sched_yield();
 
        for (owner = 0; owner < ncpu; owner++){
          uint64_t acc_table[SAMPLES];
          unsigned aux;
          volatile char access;
 
          for (int acc = 0; acc < SAMPLES; acc++){
            _mm_clflush(anchor[owner]);
            _mm_mfence();
            _mm_lfence();
            uint64_t preaccess = __rdtscp(&aux);
            _mm_lfence();
            access = anchor[owner][0];
            uint64_t postaccess = __rdtscp(&aux);
            _mm_lfence();
            acc_table[acc] = postaccess - preaccess;
          }
          (void)access;
          qsort(acc_table, SAMPLES, sizeof(uint64_t), compare);
          page_matrix[reader][owner] = acc_table[0];   // min, not median
        }
      }
 // print grid 
      for (reader = 0; reader < ncpu; reader++){
        printf("vCPU %d page latencies:", reader);
        for (owner = 0; owner < ncpu; owner++){
          printf(" %6lu", page_matrix[reader][owner]);
        }
        printf("\n");
      }
 // find fastest page read (minimum) 
      uint64_t page_min = page_matrix[0][0];
      for (reader = 0; reader < ncpu; reader++){
        for (owner = 0; owner < ncpu; owner++){
          if (page_matrix[reader][owner] < page_min)
            page_min = page_matrix[reader][owner];
        }
      }
 // finds fastest cache in latency_mapping grid (minimum) 
      uint64_t cache_min = 0;
      for (i = 0; i < ncpu; i++){
        for (j = i + 1; j < ncpu; j++){
          if (latency_mapping[i][j] == 0) continue;
          if (cache_min == 0 || latency_mapping[i][j] < cache_min)
            cache_min = latency_mapping[i][j];
        }
      }
 
 // no usable samples if cache_min is 0. Else, get the ratio of cache / page latencies. if this is < 2 all are local. Else all are remote. 
      if (cache_min == 0){
        printf("no usable cache samples, cannot classify\n");
      } else {
        double lr_ratio = (double)cache_min / (double)page_min;
        printf("cache_min %lu, page_min %lu, ratio %.2fx\n",
               cache_min, page_min, lr_ratio);
 
        if (lr_ratio < 2.0){
          all_local = true;
          printf("all vCPUs local to each other (1 node)\n");
        } else {
          all_remote = true;
          printf("all vCPUs remote from each other (%ld nodes)\n", ncpu);
        }
      }
 // unmap pages
      for (owner = 0; owner < ncpu; owner++)
        munmap(anchor[owner], PAGE_SIZE);

    } 
  return 0;
}
