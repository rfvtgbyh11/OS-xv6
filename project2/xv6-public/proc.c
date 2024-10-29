#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

int nextpid = 1;
uint ret[NTHRD];
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

// need to acquire lock before call this function.
static struct thread*
allocthread(struct proc *p, thread_t tid){
  struct thread *t;
  char *sp;

  for(t = p->t; t < &p->t[NTHRD]; t++){
    if(t->state != UNUSED && t->tid == tid){
      cprintf("thread allocation error: pid %d tid %d duplicate\n", p->pid, t->tid);
      return 0;
    }
  }

  for(t = p->t; t < &p->t[NTHRD]; t++)
    if(t->state == UNUSED)
      goto found;

  return 0;

found:
  t->state = EMBRYO;
  t->tid = tid;

  // Allocate kernel stack.
  if((t->kstack = kalloc()) == 0){
    t->state = UNUSED;
    t->tid = 0;
    return 0;
  }
  sp = t->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *t->tf;
  t->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *t->context;
  t->context = (struct context*)sp;
  memset(t->context, 0, sizeof *t->context);

  t->context->eip = (uint)forkret;

  return t;
} 

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  struct thread *t;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;

  if ((t = allocthread(p, 1)) == 0){
    p->state = UNUSED;
    p->pid = 0;
    p = 0;
  };

  p->tidx = t - p->t;

  release(&ptable.lock);
  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();
  
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;

  memset(p->t[p->tidx].tf, 0, sizeof(*p->t[p->tidx].tf));
  p->t[p->tidx].tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->t[p->tidx].tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->t[p->tidx].tf->es = p->t[p->tidx].tf->ds;
  p->t[p->tidx].tf->ss = p->t[p->tidx].tf->ds;
  p->t[p->tidx].tf->eflags = FL_IF;
  p->t[p->tidx].tf->esp = PGSIZE;
  p->t[p->tidx].tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;
  p->t[p->tidx].state = RUNNABLE;

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if (curproc->memlimit != 0 && curproc->sz + n > curproc->memlimit){
    cprintf("memory limit exceeded\n");
    return -1;
  }
  
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }
  // cprintf("\nFORK %d %d --- %d %d\n", curproc->pid, curproc->t[curproc->tidx].tid, np->pid, np->t[np->tidx].tid);

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    np->state = UNUSED;
    kfree(np->t[np->tidx].kstack);
    np->t[np->tidx].kstack = 0;
    np->t[np->tidx].state = UNUSED;
    np->t[np->tidx].tid = 0;
    return -1;
  }

  np->sz = curproc->sz;
  np->stacksize = curproc->stacksize;
  np->memlimit = curproc->memlimit;
  np->parent = curproc;
  for (i = 0; i < NTHRD; i++){
    if (i == np->tidx) np->ustack[i] = curproc->ustack[curproc->tidx];
    else if (i == curproc->tidx){
      if (curproc->ustack[np->tidx] != 0) np->emptystack[i] = curproc->ustack[np->tidx];
      else np->emptystack[i] = curproc->emptystack[np->tidx];
    }
    else {
      if (curproc->ustack[i] != 0 && i != curproc->tidx) np->emptystack[i] = curproc->ustack[i];
      else np->emptystack[i] = curproc->emptystack[i];
    }
  }
  *np->t[np->tidx].tf = *curproc->t[curproc->tidx].tf;

  // Clear %eax so that fork returns 0 in the child.
  np->t[np->tidx].tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;
  np->t[np->tidx].state = RUNNABLE;

  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  curproc->t[curproc->tidx].state = RUNNABLE;
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  struct thread *t;
  int havekids, pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        // Free all internal threads.
        for (t = p->t; t < &p->t[NTHRD]; t++){
          if(t->state != UNUSED){
            kfree(t->kstack);
            t->kstack = 0;
            t->state = UNUSED;
            t->tid = 0;
            p->emptystack[t - p->t] = 0;
            p->ustack[t - p->t] = 0;
            p->waiting[t - p->t] = 0;
          }
        }
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct thread *t;
  struct cpu *c = mycpu();
  c->proc = 0;
  
  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    // cnt = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state != RUNNABLE)
        continue;

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      for(t = p->t; t < &p->t[NTHRD] && p->state == RUNNABLE; t++){
        if(t->state != RUNNABLE)
          continue;
        // Select one of the runnable thread and copy data.
        p->tidx = t - p->t;

        c->proc = p;
        switchuvm(p);
        p->state = RUNNING;
        p->t[p->tidx].state = RUNNING;

        // if (p->pid == 4 && p->killed == 1) {
        //   cprintf("%d %d\n", p->pid, t->tid);
        // }
        swtch(&(c->scheduler), p->t[p->tidx].context);
        switchkvm();
      }

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
    }
    release(&ptable.lock);

  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING || p->t[p->tidx].state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->t[p->tidx].context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&ptable.lock);  //DOC: yieldlock
  p->state = RUNNABLE;
  p->t[p->tidx].state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  //if (p->pid == 3) cprintf("pid %d tid %d sleep\n", p->pid, p->t[p->tidx].tid);
  
  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->t[p->tidx].chan = chan;
  p->state = RUNNABLE;
  p->t[p->tidx].state = SLEEPING;
  sched();

  // Tidy up.
  p->t[p->tidx].chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;
  struct thread *t;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    for (t = p->t; t < &p->t[NTHRD]; t++){
      if(t->state == SLEEPING && t->chan == chan)
        t->state = RUNNABLE;
    }
  }
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;
  struct thread *t;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      for (t = p->t; t < &(p->t[NTHRD]); t++){
        if (t->state == SLEEPING)
          t->state = RUNNABLE;
      }
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

int
setmemorylimit(int pid, int limit)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      if (limit == 0) {
        p->memlimit = limit;
        return 0;
      } else if (limit < 0) {
        cprintf("memory limit must be positive\n");
        return -1;
      }
      else if (limit < p->sz) {
        cprintf("memory limit can't be smaller than allocated size\n");
        return -1;
      }
      p->memlimit = limit;
      return 0;
    }
  }
  return -1;
}

// thread implementation

int thread_create(thread_t *thread, void *(*start_routine)(void *), void *arg){
  struct proc *p = myproc();
  struct thread *nt;
  uint sp, ustack[3+1];
  int i;

  acquire(&ptable.lock);

  // Allocate thread and kernel stack.
  if((nt = allocthread(p, *thread)) == 0){
    release(&ptable.lock);
    return -1;
  }
  release(&ptable.lock);

  // check if the empty user stack space already exists.
  for (i = 0; i < NTHRD; i++)
    if (p->emptystack[i] != 0) break;
  if (p->emptystack[nt - p->t] != 0) i = nt - p->t;
  
  if (i != NTHRD){
    // write ustack location.
    p->ustack[nt - p->t] = p->emptystack[i];
    p->emptystack[i] = 0;
  }else {
    // test if the curproc will exceed the limit.
    if (p->memlimit != 0 && PGROUNDUP(p->sz) + 2*(p->stacksize + 1) > p->memlimit){
      cprintf("memory limit exceeded.\n");
      goto bad;
    }

    // Allocate user stack.
    p->sz = PGROUNDUP(p->sz);
    if (((p->sz = allocuvm(p->pgdir, p->sz, p->sz + (p->stacksize + 1)*PGSIZE)) == 0))
      goto bad;

    p->ustack[nt - p->t] = p->sz;
  }

  sp = p->ustack[nt - p->t];

  // fetch argument and stack
  ustack[3] = (uint)arg;

  ustack[0] = 0xffffffff; // fake return PC
  ustack[1] = (uint)arg;
  ustack[2] = sp - 8;

  sp = sp - (3+1+1) * 4;
  if(copyout(p->pgdir, sp, ustack, (3+1+1)*4) < 0)
    goto bad;

  // copy trap frame
  *nt->tf = *p->t[p->tidx].tf;

  nt->tf->eip = (uint)start_routine;
  nt->tf->esp = sp;


  acquire(&ptable.lock);

  nt->state = RUNNABLE;

  release(&ptable.lock);
  
  return 0;

  bad:
    kfree(nt->kstack);
    nt->kstack = 0;
    nt->state = UNUSED;
    nt->tid = 0;
    return -1;
}

void thread_exit(void *retval){
  struct proc *curproc = myproc();
  struct thread *t = &(curproc->t[curproc->tidx]);
  int cnt;

  ret[curproc->tidx] = (uint)retval;

  acquire(&ptable.lock);

  if (curproc->waiting[curproc->tidx] != 0){
    wakeup1(curproc);
  }

  t->state = ZOMBIE;
  curproc->state = RUNNABLE;

  // if no thread has left on the process, call exit().
  cnt = 0;
  for(t = curproc->t; t < &(curproc->t[NTHRD]); t++){
    if (t->state != UNUSED && t->state != ZOMBIE) cnt++;
  }
  if (cnt == 0) {
    release(&ptable.lock);
    exit();
  }

  sched();
  panic("zombie exit");
}

int thread_join(thread_t thread, void **retval){
  struct proc *curproc = myproc();
  struct thread *t;
  int havekids;
  int tidx;
  
  // cprintf("\n---JOIN %d %d---\n", curproc->pid, thread);

  if (thread == curproc->t[curproc->tidx].tid){
    cprintf("You cannot join current thread.\n");
    return -1;
  }

  acquire(&ptable.lock);
  for(;;){
    havekids = 0;

    // Find the target thread
    for(t = curproc->t; t < &(curproc->t[NTHRD]); t++){
      if(t->tid != thread) continue;

      tidx = t - curproc->t;
      havekids = 1;
      if(t->state == ZOMBIE){
        // Found one.
        // cprintf("%d %d exit\n", curproc->pid, t->tid);
        kfree(t->kstack);
        t->kstack = 0;
        t->state = UNUSED;
        t->tid = 0;
        curproc->emptystack[t - curproc->t] = curproc->ustack[t - curproc->t];
        curproc->ustack[t - curproc->t] = 0;
        release(&ptable.lock);
        *retval = (void *)ret[t - curproc->t];
        return 0;
      }
    }

    if(!havekids || curproc->killed || thread == 0){
      cprintf("tid doesn't exist.\n");
      release(&ptable.lock);
      return -1;
    }

    // Wait for target thread to exit. (See thread_exit call.)
    curproc->waiting[tidx] = 1;
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
    curproc->waiting[tidx] = 0;
  }
}



//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->t[p->tidx].context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}

void proclist(void){
  struct proc* p;
  // struct thread* t;
  cprintf("Process Name\t pid\tnumofstackpage\tmemsize\t memmax\n");
  cprintf("=========================================================\n");
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if (p->state == UNUSED || p->state == ZOMBIE || p->state == EMBRYO) continue;
      if (strlen(p->name) < 8) cprintf("%s\t\t ", p->name);
      else if (strlen(p->name) < 16) cprintf("%s\t ", p->name);
      else cprintf("%s ", p->name);
      cprintf("%d\t%d\t\t%d\t %d\t\n", p->pid, p->stacksize, p->sz, p->memlimit);
      
      // check process and thread state (debugging)
      // cprintf("%d : ", p->state);
      // for (t = p->t; t < &p->t[NTHRD]; t++){
      //   cprintf("%d ",t->state);
      // }
      // cprintf("\n");
      // for (t = p->t; t < &p->t[NTHRD]; t++){
      //   cprintf("%d ",p->ustack[t - p->t]);
      // }
      // cprintf("\n");
      // for (t = p->t; t < &p->t[NTHRD]; t++){
      //   cprintf("%d ",p->emptystack[t - p->t]);
      // }
      // cprintf("\n");
  }
  return;
}


