#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vm_dbg.h"

#define NOPS (16)

#define OPC(i) ((i) >> 12)
#define DR(i) (((i) >> 9) & 0x7)
#define SR1(i) (((i) >> 6) & 0x7)
#define SR2(i) ((i) & 0x7)
#define FIMM(i) ((i >> 5) & 01)
#define IMM(i) ((i) & 0x1F)
#define SEXTIMM(i) sext(IMM(i), 5)
#define FCND(i) (((i) >> 9) & 0x7)
#define POFF(i) sext((i) & 0x3F, 6)
#define POFF9(i) sext((i) & 0x1FF, 9)
#define POFF11(i) sext((i) & 0x7FF, 11)
#define FL(i) (((i) >> 11) & 1)
#define BR(i) (((i) >> 6) & 0x7)
#define TRP(i) ((i) & 0xFF)

/* New OS declarations */

// OS bookkeeping constants
#define PAGE_SIZE       (2048)  // Page size in words (of 2 bytes)
#define OS_MEM_SIZE     (2)     // OS Region size. Also the start of the page tables' page
#define Cur_Proc_ID     (0)     // id of the current process
#define Proc_Count      (1)     // total number of processes, including ones that finished executing.
#define OS_STATUS       (2)     // Bit 0 shows whether the PCB list is full or not
#define OS_FREE_BITMAP  (3)     // Bitmap for free pages

// Process list and PCB related constants
#define PCB_SIZE  (3)  // Number of fields in a PCB
#define PID_PCB   (0)  // Holds the pid for a process
#define PC_PCB    (1)  // Value of the program counter for the process
#define PTBR_PCB  (2)  // Page table base register for the process

#define CODE_SIZE       (2)  // Number of pages for the code segment
#define HEAP_INIT_SIZE  (2)  // Number of pages for the heap segment initially

bool running = true;

typedef void (*op_ex_f)(uint16_t i);
typedef void (*trp_ex_f)();

enum { trp_offset = 0x20 };
enum regist { R0 = 0, R1, R2, R3, R4, R5, R6, R7, RPC, RCND, PTBR, RCNT };
enum flags { FP = 1 << 0, FZ = 1 << 1, FN = 1 << 2 };

uint16_t mem[UINT16_MAX + 1] = {0};
uint16_t reg[RCNT] = {0};
uint16_t PC_START = 0x3000;

void initOS();
int createProc(char *fname, char *hname);
void loadProc(uint16_t pid);
uint16_t allocMem(uint16_t ptbr, uint16_t vpn, uint16_t read, uint16_t write);  // Can use 'bool' instead
int freeMem(uint16_t ptr, uint16_t ptbr);
static inline uint16_t mr(uint16_t address);
static inline void mw(uint16_t address, uint16_t val);
static inline void tbrk();
static inline void thalt();
static inline void tyld();
static inline void trap(uint16_t i);

static inline uint16_t sext(uint16_t n, int b) { return ((n >> (b - 1)) & 1) ? (n | (0xFFFF << b)) : n; }
static inline void uf(enum regist r) {
    if (reg[r] == 0)
        reg[RCND] = FZ;
    else if (reg[r] >> 15)
        reg[RCND] = FN;
    else
        reg[RCND] = FP;
}
static inline void add(uint16_t i)  { reg[DR(i)] = reg[SR1(i)] + (FIMM(i) ? SEXTIMM(i) : reg[SR2(i)]); uf(DR(i)); }
static inline void and(uint16_t i)  { reg[DR(i)] = reg[SR1(i)] & (FIMM(i) ? SEXTIMM(i) : reg[SR2(i)]); uf(DR(i)); }
static inline void ldi(uint16_t i)  { reg[DR(i)] = mr(mr(reg[RPC]+POFF9(i))); uf(DR(i)); }
static inline void not(uint16_t i)  { reg[DR(i)]=~reg[SR1(i)]; uf(DR(i)); }
static inline void br(uint16_t i)   { if (reg[RCND] & FCND(i)) { reg[RPC] += POFF9(i); } }
static inline void jsr(uint16_t i)  { reg[R7] = reg[RPC]; reg[RPC] = (FL(i)) ? reg[RPC] + POFF11(i) : reg[BR(i)]; }
static inline void jmp(uint16_t i)  { reg[RPC] = reg[BR(i)]; }
static inline void ld(uint16_t i)   { reg[DR(i)] = mr(reg[RPC] + POFF9(i)); uf(DR(i)); }
static inline void ldr(uint16_t i)  { reg[DR(i)] = mr(reg[SR1(i)] + POFF(i)); uf(DR(i)); }
static inline void lea(uint16_t i)  { reg[DR(i)] =reg[RPC] + POFF9(i); uf(DR(i)); }
static inline void st(uint16_t i)   { mw(reg[RPC] + POFF9(i), reg[DR(i)]); }
static inline void sti(uint16_t i)  { mw(mr(reg[RPC] + POFF9(i)), reg[DR(i)]); }
static inline void str(uint16_t i)  { mw(reg[SR1(i)] + POFF(i), reg[DR(i)]); }
static inline void rti(uint16_t i)  {} // unused
static inline void res(uint16_t i)  {} // unused
static inline void tgetc()        { reg[R0] = getchar(); }
static inline void tout()         { fprintf(stdout, "%c", (char)reg[R0]); }
static inline void tputs() {
  uint16_t *p = mem + reg[R0];
  while(*p) {
    fprintf(stdout, "%c", (char) *p);
    p++;
  }
}
static inline void tin()      { reg[R0] = getchar(); fprintf(stdout, "%c", reg[R0]); }
static inline void tputsp()   { /* Not Implemented */ }
static inline void tinu16()   { fscanf(stdin, "%hu", &reg[R0]); }
static inline void toutu16()  { fprintf(stdout, "%hu\n", reg[R0]); }

trp_ex_f trp_ex[10] = {tgetc, tout, tputs, tin, tputsp, thalt, tinu16, toutu16, tyld, tbrk};
static inline void trap(uint16_t i) { trp_ex[TRP(i) - trp_offset](); }
op_ex_f op_ex[NOPS] = {/*0*/ br, add, ld, st, jsr, and, ldr, str, rti, not, ldi, sti, jmp, res, lea, trap};

/**
  * Load an image file into memory.
  * @param fname the name of the file to load
  * @param offsets the offsets into memory to load the file
  * @param size the size of the file to load
*/
void ld_img(char *fname, uint16_t *offsets, uint16_t size) {
    FILE *in = fopen(fname, "rb");
    if (NULL == in) {
        fprintf(stderr, "Cannot open file %s.\n", fname);
        exit(1);
    }

    for (uint16_t s = 0; s < size; s += PAGE_SIZE) {
        uint16_t *p = mem + offsets[s / PAGE_SIZE];
        uint16_t writeSize = (size - s) > PAGE_SIZE ? PAGE_SIZE : (size - s);
        fread(p, sizeof(uint16_t), (writeSize), in);
    }
    
    fclose(in);
}

void run(char *code, char *heap) {
  while (running) {
    uint16_t i = mr(reg[RPC]++);
    op_ex[OPC(i)](i);
  }
}

// YOUR CODE STARTS HERE

void initOS() {
//initialize the physical memory
mem[Cur_Proc_ID]=0xFFFF;
mem[Proc_Count] = 0;
mem[OS_STATUS] = 0;
//free page bitmap uses 3 bits as os reserved and frame 2 as page tables,set them not avaible while others are free
//1111 1111 1111 1000
//single memory bit is 16 bits,so it is seperated as 16 16
mem[OS_FREE_BITMAP] = 0x1FFF;
//set other 16 bits free
mem[OS_FREE_BITMAP + 1] = 0xFFFF;
  return;
}

// Process functions to implement
int createProc(char *fname, char *hname) {
//check if the OS segment is full
uint16_t pid = mem[Proc_Count];
    if ((mem[OS_STATUS] & 1) || pid >= 64) {
        printf("The OS memory region is full. Cannot create a new PCB.\n");
        //set 1 to indicate it is not available
        mem[OS_STATUS] |= 1; 
        return 0;
    }
//increment count
mem[Proc_Count]++;
//PCB set
uint16_t pcb_addr = 12 + (pid * 3);
uint16_t ptbr_val = 0x1000 + (pid * 32);
mem[pcb_addr + PID_PCB] = pid;
mem[pcb_addr + PC_PCB] = 0x3000;
mem[pcb_addr + PTBR_PCB] = ptbr_val;
    //set code segments
    if (!allocMem(ptbr_val, 6, UINT16_MAX, 0)) {
        printf("Cannot create code segment.\n");
        return 0;
    }
    // Allocate VPN 7
    if (!allocMem(ptbr_val, 7, UINT16_MAX, 0)) {
        printf("Cannot create code segment.\n");
        freeMem(6, ptbr_val); // Rollback
        return 0;}
        //load the code image,extract PFN
        uint16_t code_offsets[2];
    code_offsets[0] = (mem[ptbr_val + 6] >> 11) * PAGE_SIZE;
    code_offsets[1] = (mem[ptbr_val + 7] >> 11) * PAGE_SIZE;
    //write to tbe physical memory
    ld_img(fname, code_offsets, 2 * PAGE_SIZE);
    //allocate code segment
    if (!allocMem(ptbr_val, 8, UINT16_MAX, UINT16_MAX)) {
        printf("Cannot create heap segment.\n");
        freeMem(6, ptbr_val);
        freeMem(7, ptbr_val);
        return 0;
    }
    if (!allocMem(ptbr_val, 9, UINT16_MAX, UINT16_MAX)) {
        printf("Cannot create heap segment.\n");
        freeMem(6, ptbr_val);
        freeMem(7, ptbr_val);
        freeMem(8, ptbr_val);
        return 0;
    }
    //load heap image
    uint16_t heap_offsets[2];
    heap_offsets[0] = (mem[ptbr_val + 8] >> 11) * PAGE_SIZE;
    heap_offsets[1] = (mem[ptbr_val + 9] >> 11) * PAGE_SIZE;

    ld_img(hname, heap_offsets, 2 * PAGE_SIZE);
  return 1;
}

void loadProc(uint16_t pid) {
//update current ID
mem[Cur_Proc_ID] = pid;

//get the pcb adr
uint16_t pcb_addr = 12 + (pid * 3);
//load cpu registers
    reg[RPC] = mem[pcb_addr + PC_PCB];
    reg[PTBR] = mem[pcb_addr + PTBR_PCB];
}

uint16_t allocMem(uint16_t ptbr, uint16_t vpn, uint16_t read, uint16_t write) {
//first check if the page is not free
//calculate the PTE location
uint16_t pte_addr = ptbr + vpn;
//check the bit 0
if (mem[pte_addr] & 0x1) {
    return 0;
  }
  int pfn=-1;
  //scan all frames
for(int i=0;i<32;i++){
    uint16_t mask = 1 << (15 - (i % 16));
    //use 16 modulo to corresponding i
    int bitmap_addr;
    if(i<16){
      //lower 16 bits
      bitmap_addr = OS_FREE_BITMAP;
    }
    else{
      //upper 16 bits
      bitmap_addr = OS_FREE_BITMAP + 1;
    }
    if(mem[bitmap_addr] & mask){
      pfn=i;
      mem[bitmap_addr] &= ~mask;
      break;
    }
  }
  //if free frame is not found
if(pfn==-1){
    return 0;
  }
  //initialize the PTE according to rules
uint16_t new_pte = (pfn << 11);
if (write){
new_pte |= (1 << 2);
  } 
  if (read){
 new_pte |= (1 << 1);
  } 
  new_pte |= 1; // Set Valid bit
  mem[pte_addr] = new_pte;
  return 1;
}

int freeMem(uint16_t vpn, uint16_t ptbr) {
uint16_t pte_addr = ptbr + vpn;
uint16_t pte = mem[pte_addr];
  //check if valid bit is 0
if ((pte & 0x1) == 0) { 
    return 0; 
  }
  //get the PFE
uint16_t pfn = pte >> 11;
  //determine upper or lower 16 bits
uint16_t mask = 1 << (15 - (pfn % 16));
int bitmap_addr;
if (pfn < 16) {
      bitmap_addr = OS_FREE_BITMAP;
  } else {
      bitmap_addr = OS_FREE_BITMAP + 1;
  }
  //mark bit as free
  mem[bitmap_addr] |= mask;
  //already clear
  mem[pte_addr] &= ~1;

  return 1;
}

static inline void tbrk() {
uint16_t r0 = reg[R0];
uint16_t vpn = r0 >> 11;
//seperate three bits
int16_t protection_alloc = r0 & 0x7;
//extract W/R/A/F
int is_alloc = protection_alloc & 1;   // Bit 0
int read = (protection_alloc >> 1) & 1; // Bit 1
int write = (protection_alloc >> 2) & 1; // Bit 2
uint16_t cur_pid = mem[Cur_Proc_ID];
//check reserved segment
if (vpn < 3) {
        printf("Cannot allocate/free memory for the reserved segment.\n");
        thalt();
        return;
    }
    if (is_alloc) {
        printf("Heap increase requested by process %d.\n", cur_pid);
        //check if already alloc
        uint16_t pte_addr = reg[PTBR] + vpn;
        if (mem[pte_addr] & 1) {
            printf("Cannot allocate memory for page %d of pid %d since it is already allocated.\n", vpn, cur_pid);
            return;
        }
       //check mem is full
        if (mem[OS_FREE_BITMAP] == 0 && mem[OS_FREE_BITMAP + 1] == 0) {
            printf("Cannot allocate more space for pid %d since there is no free page frames.\n", cur_pid);
            return;
        }
       //attempt to allocate
        allocMem(reg[PTBR], vpn, read, write);

    } else {
        // freeing request
        printf("Heap decrease requested by process %d.\n", cur_pid);

        //check if not allocated
        uint16_t pte_addr = reg[PTBR] + vpn;
        if ((mem[pte_addr] & 1) == 0) {
            printf("Cannot free memory of page %d of pid %d since it is not allocated.\n", vpn, cur_pid);
            return;
        }

        // Perform free
        freeMem(vpn, reg[PTBR]);
    }
}

static inline void tyld() {
//save current state to pcb
uint16_t cur_pid = mem[Cur_Proc_ID];
//find next process that can run
int start_pid = (cur_pid + 1) % mem[Proc_Count];
int next_pid = -1;
//scan frum current pid
for(int i=0;i<mem[Proc_Count];i++){
  int temp_pid = (start_pid + i) % mem[Proc_Count];
        uint16_t temp_pcb = 12 + (temp_pid * 3);     
         
        //check if alive
        if (mem[temp_pcb + PID_PCB] != 0xFFFF) {
            next_pid = temp_pid;
            break;
        }
}
//switch if new process found
if (next_pid != -1 && next_pid != cur_pid) {
  uint16_t pcb_addr = 12 + (cur_pid * 3);
        mem[pcb_addr + PC_PCB] = reg[RPC];
        mem[pcb_addr + PTBR_PCB] = reg[PTBR];
        printf("We are switching from process %d to %d.\n", cur_pid, next_pid);
        loadProc(next_pid);
    }
}

// Instructions to modify
static inline void thalt() {
  uint16_t cur_pid = mem[Cur_Proc_ID];
    uint16_t ptbr = reg[PTBR];
//free all pages
    for (int vpn = 0; vpn < 32; vpn++) {
        freeMem(vpn, ptbr);
    }
//terminated pcb
    uint16_t pcb_addr = 12 + (cur_pid * 3);
    mem[pcb_addr + PID_PCB] = 0xFFFF;

//find next process
int next_pid = -1;
int start_pid = (cur_pid + 1) % mem[Proc_Count];
for (int i = 0; i < mem[Proc_Count]; i++) {
        int temp_pid = (start_pid + i) % mem[Proc_Count];
        uint16_t temp_pcb = 12 + (temp_pid * 3);
        if (mem[temp_pcb + PID_PCB] != 0xFFFF) {
            next_pid = temp_pid;
            break;
        }
    }
    //switch or terminate
    if (next_pid != -1) {
        // Found another process, switch to it
        loadProc(next_pid);
    } else {
        //no process is left
        running = false;
    } 
}

static inline uint16_t mr(uint16_t address) {
//extract vpn offset
uint16_t vpn = address >> 11;
uint16_t offset = address & 0x7FF;
//check reserved region
if (vpn < 3) {
        printf("Segmentation fault.\n");
        exit(1);
    }
//get the PTE
uint16_t pte_addr = reg[PTBR] + vpn;
uint16_t pte = mem[pte_addr]; 
//check valid bit
if ((pte & 0x1) == 0) {
        printf("Segmentation fault inside free space.\n");
        exit(1);
    }
//check read protection bit
if ((pte & 0x2) == 0) {
        printf("Cannot read the page.\n");
        exit(1);
    } 
    //translate physical adress by getting upper 5 bits
    uint16_t pfn = pte >> 11;
    uint16_t phys_addr = (pfn << 11) | offset;
    return mem[phys_addr];
}

static inline void mw(uint16_t address, uint16_t val) {
  //extract vpn offset
    uint16_t vpn = address >> 11;
    uint16_t offset = address & 0x7FF;
   //reserved region
    if (vpn < 3) {
        printf("Segmentation fault.\n");
        exit(1);
    }
    //get PTE
    uint16_t pte_addr = reg[PTBR] + vpn;
    uint16_t pte = mem[pte_addr];
    //valid bit
    if ((pte & 0x1) == 0) {
        printf("Segmentation fault inside free space.\n");
        exit(1);
    }
    //protection bit
    if ((pte & 0x4) == 0) {
        printf("Cannot write to a read-only page.\n");
        exit(1);
    }
    //physical adress
    uint16_t pfn = pte >> 11;
    uint16_t phys_addr = (pfn << 11) | offset;
  mem[phys_addr] = val;
}

// YOUR CODE ENDS HERE
