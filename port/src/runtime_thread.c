/* Guest threads on host pthreads. Each guest thread owns a FreeBSD-style TCB
 * (variant II: static TLS below the TCB). The loader rewrites the eboot's
 * `mov rax, fs:[0]` into `mov rax, gs:[0]`, so GS base = guest TCB while glibc
 * keeps FS. Priorities/affinity are recorded, not enforced by a PS4 scheduler; TLG_THREAD_PRIO=1
 * additionally carries the priority over to the host scheduler (see apply_prio).
 * Windows: GS is the TEB and cannot be moved. The guest TCB lives in a TLS slot of the TEB
 * instead, and the loader rewrites the load to `mov rax, gs:[0x1480+8*slot]` (TlsSlots),
 * the same 9-byte instruction with another displacement. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#else
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <dirent.h>
#include <asm/prctl.h>
#endif
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define ATTR_MAGIC UINT32_C(0x41545452)
#define STACK_MARGIN (256*1024)
#define MIN_STACK (64*1024)
#define DEFAULT_STACK (1024*1024)
#define DEFAULT_PRIO 700

typedef void *(ABI *GuestEntry)(void *);
typedef struct ThreadAttr {
    uint32_t magic;
    int detached, policy, prio, inherit;
    uint64_t stack, guard, affinity;
    struct ThreadAttr *next;
} ThreadAttr;
typedef struct GuestThread {
    /* Guest-visible TCB is allocated separately; this is the ScePthread handle. */
    uint64_t *tcb;
    unsigned char *tls_block;
    pthread_t host;
    GuestEntry entry;
    void *argument, *result;
    ThreadAttr attr;
    char name[32];
    int detached, finished, joined, host_owned;
    uint64_t os_tid;  /* freeze report: matches the OS thread list */
    uint32_t fp_mxcsr; uint16_t fp_x87cw; /* the creator's FP control: see host_start */
    RuntimeWait wait; /* freeze report: what the thread is blocked on */
#ifdef _WIN32
    tlg_jmp_buf exit_jump; /* no SEH unwinding through guest frames */
#else
    jmp_buf exit_jump;
#endif
    struct GuestThread *next;
} GuestThread;

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static ThreadAttr *attributes;
static GuestThread *threads;
static _Thread_local GuestThread *current;
static _Thread_local int32_t guest_errno;
static const unsigned char *tls_template;
static uint64_t tls_filesz, tls_memsz, tls_align=16;
static size_t created, joined_count, exited;
static int wait_stats(void);        /* TLG_WAIT_STATS: per-thread wait accounting, further down */
static void wait_stats_start(void); /* its monitor thread, started with the main thread */

void runtime_set_main_tls(const void *data,uint64_t filesz,uint64_t memsz,uint64_t align) {
    tls_template=data; tls_filesz=filesz; tls_memsz=memsz; tls_align=align ? align : 16;
}
static uint64_t tls_offset(void) { return (tls_memsz+tls_align-1)&~(tls_align-1); }
#ifdef _WIN32
/* TEB TlsSlots[64] start at gs:0x1480; slots 64+ live elsewhere and cannot be used. */
static DWORD tls_slot=TLS_OUT_OF_INDEXES;
uint32_t runtime_thread_tcb_offset(void) {
    if (tls_slot==TLS_OUT_OF_INDEXES) {
        tls_slot=TlsAlloc();
        if (tls_slot>=64) { fprintf(stderr,"STOP: no TEB TLS slot below 64 (got %lu)\n",tls_slot); exit(21); }
    }
    return 0x1480+8*(uint32_t)tls_slot;
}
static void set_gs(void *base) {
    runtime_thread_tcb_offset();
    if (!TlsSetValue(tls_slot,base)) { fprintf(stderr,"STOP: TlsSetValue failed (%lu)\n",GetLastError()); exit(21); }
}
#define tls_block_alloc(size) _aligned_malloc(size,64)
#else
static void set_gs(void *base) {
    if (syscall(SYS_arch_prctl,ARCH_SET_GS,(unsigned long)base)) { perror("STOP: arch_prctl(ARCH_SET_GS)"); exit(21); }
}
#define tls_block_alloc(size) aligned_alloc(64,size)
#endif
static uint64_t os_thread_id(void) {
#ifdef _WIN32
    return GetCurrentThreadId();
#else
    return (uint64_t)gettid();
#endif
}
/* ---- Guest priority (TLG_THREAD_PRIO=1) ----
 * PS4 priorities run 0x100 (highest) .. 0x2FF (lowest) with 0x2BC (700, the game's default) the
 * normal value (OpenOrbis UserService). Only a value above it lowers the host priority: nice
 * 0..19 (Linux) / BELOW_NORMAL..LOWEST (Windows), one step per 4 guest points. Raising the host
 * priority needs privileges and a wrong guess could starve the frame path, so it is never done;
 * a host that refuses the change keeps the recorded value. Off by default: the value is recorded
 * only, as it is on PS4. */
#define GUEST_PRIO_NORMAL 700
static int thread_prio_enabled(void) {
    static int value=-1;
    if (value<0) { const char *e=getenv("TLG_THREAD_PRIO"); value=e && e[0]=='1'; }
    return value;
}
static int host_nice(int prio) {
    if (prio<=GUEST_PRIO_NORMAL) return 0;
    int nice=(prio-GUEST_PRIO_NORMAL+3)/4;
    return nice>19 ? 19 : nice;
}
/* Applied on the thread itself at start (the creator does not know the host thread id yet) and on
 * the target thread for scePthreadSetprio. */
static void apply_prio(uint64_t os_tid,int prio) {
    if (!thread_prio_enabled() || !os_tid) return;
    int nice=host_nice(prio);
    if (!nice) return; /* the normal band and above: leave the host priority alone */
#ifdef _WIN32
    int level=nice<=5 ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_LOWEST;
    HANDLE thread=os_tid==os_thread_id() ? GetCurrentThread()
                                         : OpenThread(THREAD_SET_INFORMATION,FALSE,(DWORD)os_tid);
    if (!thread) return;
    if (!SetThreadPriority(thread,level) && runtime_verbose())
        fprintf(stderr,"Verbose: SetThreadPriority(%llu, %d) failed (%lu)\n",
                (unsigned long long)os_tid,level,GetLastError());
    if (thread!=GetCurrentThread()) CloseHandle(thread);
#else
    if (setpriority(PRIO_PROCESS,(id_t)os_tid,nice) && runtime_verbose())
        fprintf(stderr,"Verbose: setpriority(%llu, %d) failed (%d)\n",(unsigned long long)os_tid,nice,errno);
#endif
}
/* Build TCB/static TLS for the calling host thread and point GS at it. */
static void attach(GuestThread *t) {
    uint64_t offset=tls_offset();
    size_t total=offset+256;
    unsigned char *block=tls_block_alloc((total+63)&~(size_t)63);
    if (!block) { fputs("Cannot allocate guest TLS\n",stderr); exit(1); }
    memset(block,0,total);
    if (tls_filesz) memcpy(block,tls_template,tls_filesz);
    uint64_t *tcb=(uint64_t *)(block+offset);
    static uint64_t dtv[3];
    tcb[0]=(uint64_t)(uintptr_t)tcb;         /* tcb_self */
    tcb[1]=(uint64_t)(uintptr_t)dtv;         /* tcb_dtv (static module only) */
    tcb[2]=(uint64_t)(uintptr_t)t;           /* tcb_thread */
    t->tls_block=block; t->tcb=tcb;
    set_gs(tcb);
    t->os_tid=os_thread_id();
    current=t;
}
static GuestThread *new_thread(void) {
    GuestThread *t=calloc(1,sizeof(*t));
    if (!t) return NULL;
    t->attr=(ThreadAttr){.magic=ATTR_MAGIC,.policy=1,.prio=DEFAULT_PRIO,.stack=DEFAULT_STACK,.affinity=0x7f};
    return t;
}
static void publish(GuestThread *t) {
    pthread_mutex_lock(&lock); t->next=threads; threads=t; pthread_mutex_unlock(&lock);
}
/* Host threads that call into guest code without being created by the guest
 * (the main thread, test threads) get a record on first use. */
GuestThread *runtime_thread_current(void) {
    if (current) return current;
    GuestThread *t=new_thread();
    if (!t) { fputs("Cannot allocate guest thread\n",stderr); exit(1); }
    t->host=pthread_self(); t->host_owned=1;
    snprintf(t->name,sizeof(t->name),"host");
    attach(t); publish(t);
    return t;
}
/* Host threads that call guest code (HLE decoders invoking guest callbacks) need
 * a TCB/TLS like guest threads; the record lives until process exit. */
void runtime_thread_attach_host(const char *name) {
    GuestThread *t=runtime_thread_current();
    snprintf(t->name,sizeof(t->name),"%s",name ? name : "host");
}
void runtime_thread_attach_main(void) {
    GuestThread *t=runtime_thread_current();
    snprintf(t->name,sizeof(t->name),"main");
    if (wait_stats()) wait_stats_start();
}
static GuestThread *find_thread(void *handle) {
    GuestThread *found=NULL;
    pthread_mutex_lock(&lock);
    for (GuestThread *t=threads;t;t=t->next) if (t==handle) { found=t; break; }
    pthread_mutex_unlock(&lock);
    return found;
}
static ThreadAttr *find_attr(ThreadAttr **slot) {
    if (!slot || !*slot) return NULL;
    ThreadAttr *found=NULL;
    pthread_mutex_lock(&lock);
    for (ThreadAttr *a=attributes;a;a=a->next) if (a==*slot && a->magic==ATTR_MAGIC) { found=a; break; }
    pthread_mutex_unlock(&lock);
    return found;
}

static ABI void *thread_self(void) { return runtime_thread_current(); }
void *runtime_thread_self(void) { return current; }

/* ---- Wait slots (freeze report) ----
 * Guest threads keep theirs in the thread record; host threads (GPU library, decoders) that
 * block in a marked wait get one of a fixed set on first use, matched by OS thread id (a
 * thread that reuses the id of a finished one takes its slot over). When the set runs out, the
 * first slot whose owner has exited is handed over instead of leaving the thread unrecorded
 * (long sessions create and destroy host threads): the new owner starts with empty
 * TLG_WAIT_STATS accounting. */
#define HOST_WAIT_SLOTS 64
static struct { uint64_t os_tid; RuntimeWait wait; } host_waits[HOST_WAIT_SLOTS];
static unsigned host_wait_count;
static _Thread_local RuntimeWait *host_wait;
#define HOST_WAIT_CLAIMING UINT64_MAX /* os_tid of a slot being handed over */
/* 0 for a thread that never existed and for one that exited; a tid reused by a live thread only
 * makes the check pessimistic (that slot is left alone). */
static int thread_alive(uint64_t tid) {
#ifdef _WIN32
    HANDLE thread=OpenThread(SYNCHRONIZE,FALSE,(DWORD)tid);
    if (!thread) return 0;
    int alive=WaitForSingleObject(thread,0)==WAIT_TIMEOUT;
    CloseHandle(thread);
    return alive;
#else
    return syscall(SYS_tgkill,(pid_t)getpid(),(pid_t)tid,0)==0 || errno!=ESRCH;
#endif
}
uint64_t runtime_wait_now_ms(void) {
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC_COARSE,&t);
    return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;
#endif
}
/* TLG_WAIT_STATS=1: per-thread wait accounting (runtime_wait_slot_begin/end) and a summary of it
 * every 5 s from a monitor thread started with the main thread. Off by default: marking a wait
 * costs one store then, no clock read. */
static int wait_stats(void) {
    static int value=-1;
    if (value<0) { const char *e=getenv("TLG_WAIT_STATS"); value=e && e[0]=='1'; }
    return value;
}
static uint64_t wait_now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec;
}
uint64_t runtime_wait_now_ns(void) { return wait_now_ns(); }

/* ---- Wait kinds ----
 * The wait texts are static strings, so the pointer identifies the kind: a small table
 * (pointer -> kind) and a per-thread cache of the last text keep the lookup at a few ns,
 * which matters because the guest waits a million times per minute. */
static struct { const char *text; int kind; } kind_table[32];
static unsigned kind_table_used;
static _Thread_local const char *kind_cache_text;
static _Thread_local int kind_cache_kind;
static int kind_from_text(const char *kind) {
    if (!strncmp(kind,"condition",9)) return RUNTIME_WAIT_COND;
    if (!strncmp(kind,"mutex",5)) return RUNTIME_WAIT_MUTEX;
    if (!strncmp(kind,"semaphore",9)) return RUNTIME_WAIT_SEMAPHORE;
    if (!strncmp(kind,"rwlock",6)) return RUNTIME_WAIT_RWLOCK;
    if (!strncmp(kind,"thread join",11)) return RUNTIME_WAIT_JOIN;
    if (!strncmp(kind,"sleep",5)) return RUNTIME_WAIT_SLEEP;
    if (!strncmp(kind,"event",5)) return RUNTIME_WAIT_EVENT;
    if (!strncmp(kind,"file",4)) return RUNTIME_WAIT_FILE;
    if (!strncmp(kind,"GPU",3)) return RUNTIME_WAIT_GPU;
    return RUNTIME_WAIT_OTHER;
}
int runtime_wait_kind(const char *kind) {
    if (!kind) return RUNTIME_WAIT_OTHER;
    if (kind==kind_cache_text) return kind_cache_kind;
    unsigned used=__atomic_load_n(&kind_table_used,__ATOMIC_ACQUIRE);
    for (unsigned i=0;i<used && i<32;++i) {
        const char *text=__atomic_load_n(&kind_table[i].text,__ATOMIC_ACQUIRE);
        if (text==kind) {
            int k=__atomic_load_n(&kind_table[i].kind,__ATOMIC_ACQUIRE);
            kind_cache_text=kind; kind_cache_kind=k;
            return k;
        }
    }
    int k=kind_from_text(kind);
    if (used<32) {
        unsigned slot=__atomic_fetch_add(&kind_table_used,1,__ATOMIC_ACQ_REL);
        if (slot<32) {
            /* The kind is written before the text: a reader that sees the text sees the kind. */
            __atomic_store_n(&kind_table[slot].kind,k,__ATOMIC_RELAXED);
            __atomic_store_n(&kind_table[slot].text,kind,__ATOMIC_RELEASE);
        }
    }
    kind_cache_text=kind; kind_cache_kind=k;
    return k;
}
RuntimeWait *runtime_wait_slot(void) {
    if (current) return &current->wait;
    if (host_wait) return host_wait;
    /* No lock: this may run inside a fault handler, on a thread that faulted holding one. */
    uint64_t tid=os_thread_id();
    unsigned count=__atomic_load_n(&host_wait_count,__ATOMIC_ACQUIRE);
    for (unsigned i=0;i<count && i<HOST_WAIT_SLOTS && !host_wait;++i)
        if (__atomic_load_n(&host_waits[i].os_tid,__ATOMIC_ACQUIRE)==tid) host_wait=&host_waits[i].wait;
    if (!host_wait) {
        unsigned i=__atomic_fetch_add(&host_wait_count,1,__ATOMIC_ACQ_REL);
        if (i>=HOST_WAIT_SLOTS) {
            i=HOST_WAIT_SLOTS;
            for (unsigned j=0;j<HOST_WAIT_SLOTS;++j) {
                uint64_t owner=__atomic_load_n(&host_waits[j].os_tid,__ATOMIC_ACQUIRE);
                /* 0: a first-use claim is about to store its tid; HOST_WAIT_CLAIMING: another
                 * thread is taking this slot over. */
                if (!owner || owner==HOST_WAIT_CLAIMING || thread_alive(owner)) continue;
                /* Two threads may find the same dead owner: only the one that swaps it out
                 * resets the slot. */
                if (!__atomic_compare_exchange_n(&host_waits[j].os_tid,&owner,HOST_WAIT_CLAIMING,0,
                                                 __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) continue;
                /* The generation stays odd while the slot is cleared, so a report reading it
                 * (runtime_wait_copy) retries instead of taking a torn copy. */
                RuntimeWait *w=&host_waits[j].wait;
                const uint32_t generation=__atomic_load_n(&w->generation,__ATOMIC_ACQUIRE)|1u;
                __atomic_store_n(&w->generation,generation,__ATOMIC_RELEASE);
                RuntimeWait empty={0};
                empty.generation=generation;
                memcpy(w,&empty,sizeof(empty));
                __atomic_store_n(&w->generation,generation+1,__ATOMIC_RELEASE);
                i=j; break;
            }
            if (i>=HOST_WAIT_SLOTS) return NULL; /* every slot is held by a live thread */
        }
        __atomic_store_n(&host_waits[i].os_tid,tid,__ATOMIC_RELEASE);
        host_wait=&host_waits[i].wait;
    }
    return host_wait;
}
/* The name is copied (the object may go away while a report reads it); long names keep
 * their tail, the distinctive part of a path. */
static void copy_wait_name(char *out,size_t size,const char *name) {
    size_t n=name ? strnlen(name,512) : 0;
    if (n<size) { memcpy(out,name ? name : "",n); out[n]=0; return; }
    memcpy(out,"...",3);
    memcpy(out+3,name+n-(size-4),size-4);
    out[size-1]=0;
}
void runtime_wait_slot_begin(RuntimeWait *w,const char *kind,const void *object,const char *name,
                             const void *const *owner,uintptr_t frame,uint64_t now_ms) {
    if (!w) return;
    unsigned depth=w->depth++;
    /* The outermost wait is the time the thread is blocked (nested waits happen inside it). */
    if (!depth && wait_stats()) {
        w->wait_start_ns=wait_now_ns();
        ++w->wait_count;
        int k=runtime_wait_kind(kind);
        w->wait_kind=(unsigned)k;
        ++w->kind_count[k];
    }
    if (depth>=RUNTIME_WAIT_LEVELS) return;
    RuntimeWaitLevel *l=&w->level[depth];
    __atomic_add_fetch(&w->generation,1,__ATOMIC_ACQ_REL); /* odd: being written */
    l->object=object; l->owner=owner; l->frame=frame; l->since_ms=now_ms;
    copy_wait_name(l->name,sizeof(l->name),name);
    __atomic_store_n(&l->kind,kind,__ATOMIC_RELEASE);
    __atomic_add_fetch(&w->generation,1,__ATOMIC_RELEASE);
}
void runtime_wait_slot_end(RuntimeWait *w) {
    if (!w || !w->depth) return;
    unsigned depth=--w->depth;
    if (!depth && w->wait_start_ns) { /* wait_start_ns is set only for a counted begin */
        uint64_t spent=wait_now_ns()-w->wait_start_ns;
        w->wait_start_ns=0;
        w->wait_ns+=spent;
        /* The blocked time goes to the kind of the wait that just ended. */
        unsigned k=w->wait_kind<RUNTIME_WAIT_KINDS ? w->wait_kind : (unsigned)RUNTIME_WAIT_OTHER;
        w->kind_ns[k]+=spent;
        if (spent>w->kind_max_ns[k]) w->kind_max_ns[k]=spent;
        /* The longest wait of the current window (the monitor resets it after each report). */
        if (spent>w->window_max_ns) {
            w->window_max_ns=spent;
            w->window_max_kind=w->level[0].kind;
            copy_wait_name(w->window_max_name,sizeof(w->window_max_name),w->level[0].name);
        }
        if (spent>w->wait_max_ns) {
            w->wait_max_ns=spent; w->wait_max_end_ms=runtime_wait_now_ms();
            w->wait_max_kind=w->level[0].kind;
            copy_wait_name(w->wait_max_name,sizeof(w->wait_max_name),w->level[0].name);
        }
    }
    if (depth>=RUNTIME_WAIT_LEVELS) return;
    __atomic_add_fetch(&w->generation,1,__ATOMIC_ACQ_REL);
    __atomic_store_n(&w->level[depth].kind,NULL,__ATOMIC_RELEASE);
    __atomic_add_fetch(&w->generation,1,__ATOMIC_RELEASE);
}
/* The delay from the post that woke the thread to the moment it ran again (the cost the
 * guest pays per wake-up). The caller measures it with the object's own stamp. */
void runtime_wait_note_wake(uint64_t latency_ns) {
    RuntimeWait *w=runtime_wait_slot();
    if (!w) return;
    w->wake_ns+=latency_ns;
    ++w->wake_count;
    if (latency_ns>w->wake_max_ns) w->wake_max_ns=latency_ns;
    unsigned bucket=latency_ns ? (unsigned)(64-__builtin_clzll(latency_ns)) : 0u;
    if (bucket>=RUNTIME_WAKE_BUCKETS) bucket=RUNTIME_WAKE_BUCKETS-1;
    ++w->wake_hist[bucket];
}
/* The wait returned with no signal: a spurious wake-up (the guest's loop has to re-check). */
void runtime_wait_note_spurious(void) {
    RuntimeWait *w=runtime_wait_slot();
    if (w) ++w->spurious;
}
void runtime_wait_begin(const char *kind,const void *object,const char *name,const void *const *owner,uintptr_t frame) {
    runtime_wait_slot_begin(runtime_wait_slot(),kind,object,name,owner,frame,runtime_wait_now_ms());
}
void runtime_wait_end(void) { runtime_wait_slot_end(runtime_wait_slot()); }
int runtime_wait_copy(const RuntimeWait *from,RuntimeWait *to) {
    for (int attempt=0;attempt<8;++attempt) {
        uint32_t before=__atomic_load_n(&from->generation,__ATOMIC_ACQUIRE);
        if (before&1) continue;
        memcpy(to,(const void *)from,sizeof(*to));
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&from->generation,__ATOMIC_ACQUIRE)==before) return 1;
    }
    memset(to,0,sizeof(*to));
    return 0;
}
size_t runtime_thread_snapshot(RuntimeThreadInfo *out,size_t max) {
    size_t n=0;
    pthread_mutex_lock(&lock);
    for (GuestThread *t=threads;t && n<max;t=t->next,++n) {
        out[n]=(RuntimeThreadInfo){.record=t,.os_tid=t->os_tid,.finished=t->finished};
        memcpy(out[n].name,t->name,sizeof(out[n].name)); out[n].name[sizeof(out[n].name)-1]=0;
        runtime_wait_copy(&t->wait,&out[n].wait);
    }
    unsigned hosts=__atomic_load_n(&host_wait_count,__ATOMIC_ACQUIRE);
    for (unsigned i=0;i<hosts && i<HOST_WAIT_SLOTS && n<max;++i,++n) {
        const uint64_t tid=__atomic_load_n(&host_waits[i].os_tid,__ATOMIC_ACQUIRE);
        out[n]=(RuntimeThreadInfo){.os_tid=tid==HOST_WAIT_CLAIMING ? 0 : tid};
        runtime_wait_copy(&host_waits[i].wait,&out[n].wait);
    }
    pthread_mutex_unlock(&lock);
    return n;
}
static ABI int32_t *guest_error(void) { return &guest_errno; }
int32_t *runtime_errno(void) { return &guest_errno; }

/* ---- Wait statistics monitor (TLG_WAIT_STATS=1) ----
 * Every 5 s: the wait map of the window, one line per thread that did something: how many
 * waits it made, how long it stayed blocked (by kind), how long its wake-ups took (the delay
 * from the post to the moment it ran again), how many wake-ups were spurious, how many times
 * it asked to yield (a spin) and how much CPU it used. The guest's threads and the host's
 * threads are apart, and the guest's share of the process CPU says whether the guest is the
 * one taking the frame or the port is. */
#define WAIT_STATS_PERIOD_MS 5000
#define WAIT_STATS_THREADS 256
static const char *wait_kind_name(int kind) {
    static const char *const names[RUNTIME_WAIT_KINDS]={
        "cond","mutex","sema","rwlock","join","sleep","event","file","gpu","other"
    };
    return (kind>=0 && kind<RUNTIME_WAIT_KINDS) ? names[kind] : "wait";
}
static uint64_t delta_u64(uint64_t now,uint64_t before) { return now>=before ? now-before : 0; }
static uint32_t delta_u32(uint32_t now,uint32_t before) { return now>=before ? now-before : 0; }
/* CPU time (user+kernel) of a thread, in nanoseconds; 0 when it cannot be read. */
static uint64_t thread_cpu_ns(uint64_t os_tid) {
    if (!os_tid) return 0;
#ifdef _WIN32
    HANDLE thread=OpenThread(THREAD_QUERY_LIMITED_INFORMATION,FALSE,(DWORD)os_tid);
    if (!thread) return 0;
    FILETIME creation,exit_time,kernel,user;
    uint64_t ns=0;
    if (GetThreadTimes(thread,&creation,&exit_time,&kernel,&user)) {
        uint64_t k=((uint64_t)kernel.dwHighDateTime<<32)|kernel.dwLowDateTime;
        uint64_t u=((uint64_t)user.dwHighDateTime<<32)|user.dwLowDateTime;
        ns=(k+u)*100; /* the FILETIME unit is 100 ns */
    }
    CloseHandle(thread);
    return ns;
#else
    char path[64];
    snprintf(path,sizeof(path),"/proc/self/task/%llu/stat",(unsigned long long)os_tid);
    FILE *f=fopen(path,"r");
    if (!f) return 0;
    char buf[512];
    size_t got=fread(buf,1,sizeof(buf)-1,f);
    fclose(f);
    if (!got) return 0;
    buf[got]=0;
    /* The command name is in parentheses and may hold spaces: the fields start after it. */
    char *p=strrchr(buf,')');
    if (!p) return 0;
    ++p;
    /* state is the first field after it; utime and stime are the 12th and the 13th. */
    unsigned long long utime=0,stime=0;
    for (int field=0;field<13;++field) {
        while (*p==' ') ++p;
        if (!*p) return 0;
        char *end=NULL;
        unsigned long long value=strtoull(p,&end,10);
        if (end==p) return 0;
        if (field==11) utime=value;
        if (field==12) stime=value;
        p=end;
    }
    long ticks=sysconf(_SC_CLK_TCK);
    if (ticks<=0) ticks=100;
    return (uint64_t)(utime+stime)*1000000000ull/(uint64_t)ticks;
#endif
}
/* CPU time of the whole process, in nanoseconds. */
static void process_cpu_ns(uint64_t *user_ns,uint64_t *kernel_ns) {
#ifdef _WIN32
    int64_t user=0,kernel=0;
    compat_cpu_times(0,&user,&kernel);
    if (user_ns) *user_ns=(uint64_t)(user>0?user:0)*1000;
    if (kernel_ns) *kernel_ns=(uint64_t)(kernel>0?kernel:0)*1000;
#else
    struct rusage usage;
    if (getrusage(RUSAGE_SELF,&usage)) {
        if (user_ns) *user_ns=0;
        if (kernel_ns) *kernel_ns=0;
        return;
    }
    if (user_ns) *user_ns=(uint64_t)usage.ru_utime.tv_sec*1000000000ull+(uint64_t)usage.ru_utime.tv_usec*1000;
    if (kernel_ns) *kernel_ns=(uint64_t)usage.ru_stime.tv_sec*1000000000ull+(uint64_t)usage.ru_stime.tv_usec*1000;
#endif
}
/* Threads of the process (the guest's and the port's): the report shows the oversubscription. */
static unsigned process_thread_count(void) {
#ifdef _WIN32
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
    if (snapshot==INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 entry={.dwSize=sizeof(entry)};
    const DWORD process=GetCurrentProcessId();
    unsigned n=0;
    for (BOOL ok=Thread32First(snapshot,&entry);ok;ok=Thread32Next(snapshot,&entry))
        if (entry.th32OwnerProcessID==process) ++n;
    CloseHandle(snapshot);
    return n;
#else
    DIR *dir=opendir("/proc/self/task");
    if (!dir) return 0;
    unsigned n=0;
    struct dirent *entry;
    while ((entry=readdir(dir))) if (entry->d_name[0]!='.') ++n;
    closedir(dir);
    return n;
#endif
}
/* Samples the CPU of every thread into its slot, so the report can show the share of each
 * one in the window. The OS is asked outside the registry lock (a syscall per thread). */
static void sample_thread_cpu(void) {
    enum { MAX_SAMPLE=512 };
    GuestThread *sample[MAX_SAMPLE];
    size_t n=0;
    pthread_mutex_lock(&lock);
    for (GuestThread *t=threads;t && n<MAX_SAMPLE;t=t->next) sample[n++]=t;
    pthread_mutex_unlock(&lock);
    /* The records live as long as the process, so the pointers stay valid outside the lock. */
    for (size_t i=0;i<n;++i) {
        const uint64_t cpu=thread_cpu_ns(sample[i]->os_tid);
        if (cpu) __atomic_store_n(&sample[i]->wait.cpu_ns,cpu,__ATOMIC_RELAXED);
    }
    unsigned hosts=__atomic_load_n(&host_wait_count,__ATOMIC_ACQUIRE);
    for (unsigned i=0;i<hosts && i<HOST_WAIT_SLOTS;++i) {
        const uint64_t tid=__atomic_load_n(&host_waits[i].os_tid,__ATOMIC_ACQUIRE);
        if (!tid || tid==HOST_WAIT_CLAIMING) continue;
        const uint64_t cpu=thread_cpu_ns(tid);
        if (cpu) __atomic_store_n(&host_waits[i].wait.cpu_ns,cpu,__ATOMIC_RELAXED);
    }
}
/* The percentile of the wake histogram (log2 buckets): the upper bound of the bucket. */
static uint64_t wake_percentile(const uint32_t *hist,const uint32_t *base,uint32_t total,double p) {
    if (!total) return 0;
    uint32_t want=(uint32_t)(total*p);
    if (want>=total) want=total-1;
    uint32_t seen=0;
    for (unsigned i=0;i<RUNTIME_WAKE_BUCKETS;++i) {
        uint32_t n=hist[i]-(base?base[i]:0u);
        seen+=n;
        if (seen>want) return (uint64_t)1<<i;
    }
    return (uint64_t)1<<(RUNTIME_WAKE_BUCKETS-1);
}
static const RuntimeThreadInfo *find_previous(const RuntimeThreadInfo *previous,size_t count,
                                              const RuntimeThreadInfo *current) {
    for (size_t i=0;i<count;++i) {
        if (current->record) { if (previous[i].record==current->record) return &previous[i]; }
        else if (!previous[i].record && previous[i].os_tid==current->os_tid) return &previous[i];
    }
    return NULL;
}
/* One line of the map: what this thread waited for, how long its wake-ups took and how much
 * CPU it used in the window. */
static void print_wait_line(const RuntimeThreadInfo *info,const RuntimeThreadInfo *previous,
                             uint64_t window_ms) {
    const RuntimeWait *w=&info->wait;
    const RuntimeWait *p=previous ? &previous->wait : NULL;
    const uint32_t waits=delta_u32(w->wait_count,p?p->wait_count:0u);
    const uint64_t blocked=delta_u64(w->wait_ns,p?p->wait_ns:0);
    const uint64_t cpu=delta_u64(w->cpu_ns,p?p->cpu_ns:0);
    const uint64_t yields=delta_u64(w->yield_count,p?p->yield_count:0);
    const uint64_t yield_ns=delta_u64(w->yield_ns,p?p->yield_ns:0);
    const uint32_t wakes=delta_u32(w->wake_count,p?p->wake_count:0u);
    const uint32_t spurious=delta_u32(w->spurious,p?p->spurious:0u);
    if (!waits && !blocked && !cpu && !wakes && !yields) return;
    /* The kinds with the blocked time, the three first. */
    uint64_t kind_ns[RUNTIME_WAIT_KINDS];
    for (unsigned k=0;k<RUNTIME_WAIT_KINDS;++k)
        kind_ns[k]=delta_u64(w->kind_ns[k],p?p->kind_ns[k]:0);
    printf("  %s %s (tid %llu): %u waits, %.1f ms blocked [",
           info->record ? "guest" : "host ",info->record ? info->name : "thread",
           (unsigned long long)info->os_tid,waits,(double)blocked/1e6);
    int printed_kinds=0;
    for (unsigned round=0;round<RUNTIME_WAIT_KINDS && printed_kinds<3;++round) {
        unsigned best=RUNTIME_WAIT_KINDS;
        uint64_t best_ns=0;
        for (unsigned k=0;k<RUNTIME_WAIT_KINDS;++k)
            if (kind_ns[k]>best_ns) { best_ns=kind_ns[k]; best=k; }
        if (best>=RUNTIME_WAIT_KINDS) break;
        printf("%s%s %.1f ms",printed_kinds?", ":"",wait_kind_name((int)best),(double)best_ns/1e6);
        kind_ns[best]=0;
        ++printed_kinds;
    }
    printf("], longest %.1f ms in '%s'",
           (double)w->window_max_ns/1e6,w->window_max_kind ? w->window_max_kind : "wait");
    if (w->window_max_name[0]) printf(" '%s'",w->window_max_name);
    if (window_ms && blocked>(uint64_t)(window_ms/10)*1000000ull)
        printf(" (%.0f%% of the window)",100.0*(double)blocked/((double)window_ms*1e6));
    if (wakes) {
        const uint64_t p50=wake_percentile(w->wake_hist,p?p->wake_hist:NULL,wakes,0.50);
        const uint64_t p95=wake_percentile(w->wake_hist,p?p->wake_hist:NULL,wakes,0.95);
        printf(", wake %llu/%llu us p50/p95",
               (unsigned long long)(p50/1000),(unsigned long long)(p95/1000));
    }
    if (spurious) printf(", spurious %u",spurious);
    if (yields) printf(", yields %llu (%.1f ms)",(unsigned long long)yields,(double)yield_ns/1e6);
    printf(", cpu %.1f ms\n",(double)cpu/1e6);
}
/* The longest wait of the current window is reset after each report, so the next one shows
 * the waits of its own window and not the longest of the session. */
static void reset_window_max(void) {
    pthread_mutex_lock(&lock);
    for (GuestThread *t=threads;t;t=t->next) {
        __atomic_store_n(&t->wait.window_max_ns,0,__ATOMIC_RELAXED);
        t->wait.window_max_kind=NULL;
        t->wait.window_max_name[0]=0;
    }
    pthread_mutex_unlock(&lock);
    unsigned hosts=__atomic_load_n(&host_wait_count,__ATOMIC_ACQUIRE);
    for (unsigned i=0;i<hosts && i<HOST_WAIT_SLOTS;++i) {
        __atomic_store_n(&host_waits[i].wait.window_max_ns,0,__ATOMIC_RELAXED);
        host_waits[i].wait.window_max_kind=NULL;
        host_waits[i].wait.window_max_name[0]=0;
    }
}
/* The objects the threads are blocked on right now: the guest's "wait for all" pattern shows
 * up as one object with many threads on it (the freeze reports show the five job workers on
 * a single condition variable). */
typedef struct WaitObject { const void *object; const char *kind; char name[48]; unsigned threads; } WaitObject;
static void print_wait_objects(const RuntimeThreadInfo *infos,size_t count) {
    WaitObject objects[32];
    unsigned n=0;
    for (size_t i=0;i<count;++i) {
        const RuntimeWait *w=&infos[i].wait;
        if (!w->depth || !w->level[0].kind) continue;
        const void *object=w->level[0].object;
        const char *kind=w->level[0].kind;
        unsigned slot=n;
        for (unsigned j=0;j<n;++j)
            if (objects[j].object==object && objects[j].kind==kind) { slot=j; break; }
        if (slot==n) {
            if (n>=32) continue;
            objects[n].object=object; objects[n].kind=kind;
            copy_wait_name(objects[n].name,sizeof(objects[n].name),w->level[0].name);
            ++n;
        }
        ++objects[slot].threads;
    }
    for (unsigned round=0;round<n;++round) {
        unsigned best=n;
        for (unsigned j=0;j<n;++j) if (objects[j].threads>1 && (best==n || objects[j].threads>objects[best].threads)) best=j;
        if (best==n) break;
        printf("  %u threads are waiting on the same %s",objects[best].threads,objects[best].kind);
        if (objects[best].name[0]) printf(" '%s'",objects[best].name);
        if (objects[best].object) printf(" 0x%llx",(unsigned long long)(uintptr_t)objects[best].object);
        printf("\n");
        objects[best].threads=1; /* printed */
    }
}
static void *wait_stats_monitor(void *argument) {
    (void)argument;
    static RuntimeThreadInfo infos[WAIT_STATS_THREADS];
    static RuntimeThreadInfo previous[WAIT_STATS_THREADS];
    static size_t previous_count;
    for (;;) {
#ifdef _WIN32
        compat_sleep_ns((uint64_t)WAIT_STATS_PERIOD_MS*1000000);
#else
        struct timespec nap={WAIT_STATS_PERIOD_MS/1000,(long)(WAIT_STATS_PERIOD_MS%1000)*1000000};
        while (nanosleep(&nap,&nap) && errno==EINTR) {}
#endif
        sample_thread_cpu();
        const size_t count=runtime_thread_snapshot(infos,WAIT_STATS_THREADS);
        uint64_t user_ns=0,kernel_ns=0;
        process_cpu_ns(&user_ns,&kernel_ns);
        const uint64_t process_total=user_ns+kernel_ns;
        /* The guest's share of the window: the sum of the guest threads' CPU. */
        uint64_t guest_cpu=0,host_cpu=0;
        for (size_t i=0;i<count;++i) {
            const RuntimeThreadInfo *prev=find_previous(previous,previous_count,&infos[i]);
            const uint64_t cpu=delta_u64(infos[i].wait.cpu_ns,prev?prev->wait.cpu_ns:0);
            if (infos[i].record) guest_cpu+=cpu; else host_cpu+=cpu;
        }
        const unsigned threads=process_thread_count();
        printf("Runtime: wait stats (last %u s, TLG_WAIT_STATS): process CPU %.0f ms, guest %.0f ms, "
               "host %.0f ms, other %.0f ms, %u threads\n",
               WAIT_STATS_PERIOD_MS/1000,(double)process_total/1e6,(double)guest_cpu/1e6,
               (double)host_cpu/1e6,
               (double)(process_total>guest_cpu+host_cpu?process_total-guest_cpu-host_cpu:0)/1e6,threads);
        for (size_t i=0;i<count;++i) {
            const RuntimeThreadInfo *prev=find_previous(previous,previous_count,&infos[i]);
            print_wait_line(&infos[i],prev,WAIT_STATS_PERIOD_MS);
        }
        print_wait_objects(infos,count);
        fflush(stdout);
        /* Keep the counters for the next window and reset the window maxima. */
        memcpy(previous,infos,count*sizeof(*infos));
        previous_count=count;
        reset_window_max();
    }
    return NULL;
}
static void wait_stats_start(void) {
    static int started;
    if (__atomic_exchange_n(&started,1,__ATOMIC_ACQ_REL)) return;
    pthread_t thread;
    if (pthread_create(&thread,NULL,wait_stats_monitor,NULL)) {
        fputs("Runtime: wait stats thread not started\n",stderr);
        return;
    }
#ifndef _WIN32
    pthread_setname_np(thread,"tlg:waitstats");
#endif
    pthread_detach(thread);
    printf("Runtime: wait stats every %u ms (TLG_WAIT_STATS)\n",WAIT_STATS_PERIOD_MS);
}

static ABI int32_t attr_init(ThreadAttr **out) {
    if (!out) return ERR(22);
    ThreadAttr *a=calloc(1,sizeof(*a));
    if (!a) return ERR(12);
    *a=(ThreadAttr){.magic=ATTR_MAGIC,.policy=1,.prio=DEFAULT_PRIO,.inherit=4,.stack=DEFAULT_STACK,.guard=4096,.affinity=0x7f};
    pthread_mutex_lock(&lock); a->next=attributes; attributes=a; pthread_mutex_unlock(&lock);
    *out=a; return 0;
}
static ABI int32_t attr_destroy(ThreadAttr **slot) {
    ThreadAttr *a=find_attr(slot);
    if (!a) return ERR(22);
    pthread_mutex_lock(&lock);
    ThreadAttr **link=&attributes;
    while (*link!=a) link=&(*link)->next;
    *link=a->next; a->magic=0;
    pthread_mutex_unlock(&lock);
    free(a); *slot=NULL; return 0;
}
static ABI int32_t attr_get(void *thread,ThreadAttr **out) {
    ThreadAttr *a=find_attr(out);
    if (!a || !thread) return ERR(22);
    GuestThread *t=find_thread(thread);
    if (!t) return ERR(3);
    ThreadAttr *next=a->next;
    *a=t->attr; a->magic=ATTR_MAGIC; a->next=next;
    a->detached=t->detached;
    return 0;
}
static ABI int32_t attr_set_stack(ThreadAttr **slot,uint64_t size) {
    ThreadAttr *a=find_attr(slot);
    if (!a || size<16384) return ERR(22);
    a->stack=size; return 0;
}
static ABI int32_t attr_get_stack(ThreadAttr **slot,uint64_t *size) {
    ThreadAttr *a=find_attr(slot);
    if (!a || !size) return ERR(22);
    *size=a->stack; return 0;
}
static ABI int32_t attr_set_detach(ThreadAttr **slot,int state) {
    ThreadAttr *a=find_attr(slot);
    if (!a || (state!=0 && state!=1)) return ERR(22);
    a->detached=state; return 0;
}
static ABI int32_t attr_get_detach(ThreadAttr **slot,int *state) {
    ThreadAttr *a=find_attr(slot);
    if (!a || !state) return ERR(22);
    *state=a->detached; return 0;
}
static ABI int32_t attr_set_policy(ThreadAttr **slot,int policy) {
    ThreadAttr *a=find_attr(slot);
    if (!a || policy<1 || policy>3) return ERR(22);
    a->policy=policy; return 0;
}
static ABI int32_t attr_set_inherit(ThreadAttr **slot,int inherit) {
    ThreadAttr *a=find_attr(slot);
    if (!a || (inherit!=0 && inherit!=4)) return ERR(22);
    a->inherit=inherit; return 0;
}
static ABI int32_t attr_set_param(ThreadAttr **slot,const int *param) {
    ThreadAttr *a=find_attr(slot);
    if (!a || !param) return ERR(22);
    a->prio=*param; return 0;
}
static ABI int32_t attr_get_param(ThreadAttr **slot,int *param) {
    ThreadAttr *a=find_attr(slot);
    if (!a || !param) return ERR(22);
    *param=a->prio; return 0;
}
static ABI int32_t attr_set_affinity(ThreadAttr **slot,uint64_t mask) {
    ThreadAttr *a=find_attr(slot);
    if (!a) return ERR(22);
    a->affinity=mask; return 0;
}
static ABI int32_t attr_affinity(ThreadAttr **slot,uint64_t *mask) {
    ThreadAttr *a=find_attr(slot);
    if (!a || !mask) return ERR(22);
    *mask=a->affinity; return 0;
}
static ABI int32_t attr_set_guard(ThreadAttr **slot,uint64_t size) {
    ThreadAttr *a=find_attr(slot);
    if (!a) return ERR(22);
    a->guard=size; return 0;
}

/* Linux thread names hold 15 characters; longer ones would be rejected. */
static void set_host_name(const char *name) {
    char host[16]={0};
    memcpy(host,name,strnlen(name,sizeof(host)-1));
    pthread_setname_np(pthread_self(),host);
}
#ifdef _WIN32
#define exit_setjmp(t) tlg_setjmp((t)->exit_jump)
#define exit_longjmp(t) tlg_longjmp((t)->exit_jump,1)
#else
#define exit_setjmp(t) setjmp((t)->exit_jump)
#define exit_longjmp(t) longjmp((t)->exit_jump,1)
#endif
static void (*thread_start_hook)(void);
void runtime_set_thread_start_hook(void (*hook)(void)) { thread_start_hook=hook; }
static void *host_start(void *p) {
    GuestThread *t=p;
    attach(t);
    if (thread_start_hook) thread_start_hook();
    /* The creator's SSE (MXCSR) and x87 control words, as FreeBSD's libthr gives a new thread
     * and as Linux happens to copy them on clone. Windows starts every thread at its defaults:
     * inFAMOUS turns on flush-to-zero (MXCSR bit 15) in a thread, then creates its job workers
     * from it; without the bit there, denormals reach divisions in the particle/light code
     * (overexposed fire and smoke, flickering headlights, Windows only). */
    __asm__ volatile("ldmxcsr %0\n\tfldcw %1" : : "m"(t->fp_mxcsr), "m"(t->fp_x87cw));
    set_host_name(t->name);
    apply_prio(t->os_tid,t->attr.prio); /* the priority is ours now; the creator had no host tid */
    if (!exit_setjmp(t)) t->result=t->entry(t->argument);
    runtime_thread_keys_cleanup();
    pthread_mutex_lock(&lock); t->finished=1; ++exited; pthread_mutex_unlock(&lock);
    return t->result;
}
static int32_t create(GuestThread **out,ThreadAttr **attr_slot,GuestEntry entry,void *argument,const char *name) {
    if (!out || !entry) return ERR(22);
    ThreadAttr *a=NULL;
    if (attr_slot && !(a=find_attr(attr_slot))) return ERR(22);
    GuestThread *t=new_thread();
    if (!t) return ERR(12);
    if (a) { t->attr=*a; t->attr.next=NULL; }
    t->entry=entry; t->argument=argument; t->detached=t->attr.detached;
    __asm__ volatile("stmxcsr %0\n\tfnstcw %1" : "=m"(t->fp_mxcsr), "=m"(t->fp_x87cw));
    snprintf(t->name,sizeof(t->name),"%s",name ? name : "guest");
    pthread_attr_t host;
    pthread_attr_init(&host);
    uint64_t stack=t->attr.stack<MIN_STACK ? MIN_STACK : t->attr.stack;
    /* Stacks below 1 TiB as on PS4; guest code may pack stack addresses. */
    size_t stack_bytes=(size_t)stack+STACK_MARGIN;
#ifdef _WIN32
    /* Windows places the stack itself (low: no high-entropy ASLR) and commits all of it:
     * guest code has no stack probes and would step over the guard page. */
    pthread_attr_setstacksize(&host,stack_bytes);
#else
    void *stack_memory=runtime_low_map(stack_bytes,PROT_READ|PROT_WRITE);
    if (stack_memory) pthread_attr_setstack(&host,stack_memory,stack_bytes);
    else pthread_attr_setstacksize(&host,stack_bytes);
#endif
    publish(t);
    /* Publish the handle before the thread can run and inspect itself. */
    *out=t;
    int e=pthread_create(&t->host,&host,host_start,t);
    pthread_attr_destroy(&host);
    if (e) { fprintf(stderr,"STOP: host pthread_create failed: %d\n",e); exit(21); }
    if (t->detached) pthread_detach(t->host);
    pthread_mutex_lock(&lock); ++created; pthread_mutex_unlock(&lock);
    printf("Runtime: guest thread '%s' created (stack=%llu, prio=%d)\n",t->name,(unsigned long long)stack,t->attr.prio);
    return 0;
}
static ABI int32_t thread_create(GuestThread **out,ThreadAttr **attr,GuestEntry entry,void *argument,const char *name) {
    return create(out,attr,entry,argument,name);
}
static ABI int32_t thread_join(GuestThread *t,void **result) {
    if (!find_thread(t) || t->host_owned) return ERR(3);
    if (t==current) return ERR(11);
    if (t->detached || t->joined) return ERR(22);
    t->joined=1;
    void *value=NULL;
    /* A join blocks like any other wait: the guest's "wait for all" shows up here. */
    runtime_wait_begin("thread join",t,NULL,NULL,RUNTIME_GUEST_FRAME());
    int e=pthread_join(t->host,&value);
    runtime_wait_end();
    if (e) { fprintf(stderr,"STOP: host pthread_join failed: %d\n",e); exit(21); }
    if (result) *result=t->result;
    pthread_mutex_lock(&lock); ++joined_count; pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t thread_detach(GuestThread *t) {
    if (!find_thread(t)) return ERR(3);
    if (t->detached) return ERR(22);
    t->detached=1;
    if (!t->host_owned) pthread_detach(t->host);
    return 0;
}
static ABI __attribute__((noreturn)) void thread_exit(void *value) {
    GuestThread *t=runtime_thread_current();
    if (t->host_owned) { fputs("STOP: pthread_exit on host-owned/main thread\n",stderr); exit(21); }
    t->result=value;
    /* No host unwinder: guest frames have no registered FDEs. */
    exit_longjmp(t);
}
/* scePthreadYield: the guest asking to yield is a spin, not a wait. It is counted apart, with
 * the time it costs, so a thread that spins on yields does not look idle in the report. */
static ABI int32_t thread_yield(void) {
    const int counted=wait_stats();
    const uint64_t start=counted ? wait_now_ns() : 0;
    sched_yield();
    if (counted) {
        RuntimeWait *w=runtime_wait_slot();
        if (w) { ++w->yield_count; w->yield_ns+=wait_now_ns()-start; }
    }
    return 0;
}
static ABI int32_t thread_get_prio(GuestThread *t,int *prio) {
    if (!find_thread(t)) return ERR(3);
    if (!prio) return ERR(22);
    *prio=t->attr.prio; return 0;
}
static ABI int32_t thread_set_prio(GuestThread *t,int prio) {
    if (!find_thread(t)) return ERR(3);
    t->attr.prio=prio;
    apply_prio(t->os_tid,prio); /* 0 until the thread has started: it applies its own then */
    return 0;
}
static ABI int32_t thread_set_affinity(GuestThread *t,uint64_t mask) {
    if (!find_thread(t)) return ERR(3);
    t->attr.affinity=mask; return 0;
}
static ABI int32_t thread_get_affinity(GuestThread *t,uint64_t *mask) {
    if (!find_thread(t)) return ERR(3);
    if (!mask) return ERR(22);
    *mask=t->attr.affinity; return 0;
}
static ABI int32_t thread_rename(GuestThread *t,const char *name) {
    if (!find_thread(t)) return ERR(3);
    if (!name) return ERR(22);
    snprintf(t->name,sizeof(t->name),"%s",name);
    if (t==current) set_host_name(t->name);
    return 0;
}
static ABI int32_t thread_equal(GuestThread *a,GuestThread *b) { return a==b; }
static ABI int32_t thread_get_name(GuestThread *t,char *name) {
    if (!find_thread(t)) return ERR(3);
    if (!name) return ERR(22);
    snprintf(name,32,"%s",t->name); return 0;
}
/* Small stable per-thread id (the TID the kernel would report is not observable by the game). */
static ABI int32_t thread_get_id(void) {
    static int32_t next=1;
    static _Thread_local int32_t id;
    if (!id) id=__atomic_fetch_add(&next,1,__ATOMIC_RELAXED);
    return id;
}
/* The guest-provided stack memory is ignored: host threads always get a host stack of the requested size. */
static ABI int32_t attr_set_stack_addr(ThreadAttr **slot,void *address) {
    (void)address;
    return find_attr(slot) ? 0 : ERR(22);
}

/* POSIX (libScePosix) variants return positive errno values. */
static int32_t posix(int32_t r) { return r ? (int32_t)((uint32_t)r&0xffff) : 0; }
static ABI int32_t posix_attr_init(ThreadAttr **a) { return posix(attr_init(a)); }
static ABI int32_t posix_attr_destroy(ThreadAttr **a) { return posix(attr_destroy(a)); }
static ABI int32_t posix_attr_set_detach(ThreadAttr **a,int s) { return posix(attr_set_detach(a,s)); }
static ABI int32_t posix_attr_set_stack(ThreadAttr **a,uint64_t s) { return posix(attr_set_stack(a,s)); }
static ABI int32_t posix_attr_set_param(ThreadAttr **a,const int *p) { return posix(attr_set_param(a,p)); }
static ABI int32_t posix_create(GuestThread **t,ThreadAttr **a,GuestEntry e,void *arg) { return posix(create(t,a,e,arg,"posix")); }
static ABI int32_t posix_create_name(GuestThread **t,ThreadAttr **a,GuestEntry e,void *arg,const char *name) { return posix(create(t,a,e,arg,name)); }
static ABI int32_t posix_join(GuestThread *t,void **r) { return posix(thread_join(t,r)); }

uintptr_t runtime_thread_resolve(const char *name) {
    static const struct { const char *nid; void *fn; } table[]={
        {"aI+OeCz8xrQ#libkernel",thread_self}, {"EotR8a3ASf4#libkernel",thread_self},
        {"9BcDykPmo1I#libkernel",guest_error},
        {"nsYoNRywwNg#libkernel",attr_init}, {"62KCwEMmzcM#libkernel",attr_destroy},
        {"x1X76arYMxU#libkernel",attr_get}, {"8+s5BzZjxSg#libkernel",attr_affinity},
        {"UTXzJbWhhTE#libkernel",attr_set_stack}, {"-Wreprtu0Qs#libkernel",attr_set_detach},
        {"4+h9EzwKF4I#libkernel",attr_set_policy}, {"DzES9hQF4f4#libkernel",attr_set_param},
        {"3qxgM4ezETA#libkernel",attr_set_affinity}, {"eXbUSpEaTsA#libkernel",attr_set_inherit},
        {"JaRMy+QcpeU#libkernel",attr_get_detach}, {"-fA+7ZlGDQs#libkernel",attr_get_stack},
        {"FXPWHNk8Of0#libkernel",attr_get_param}, {"El+cQ20DynU#libkernel",attr_set_guard},
        {"6UgtwV+0zb4#libkernel",thread_create}, {"onNY9Byn-W8#libkernel",thread_join},
        {"4qGrR6eoP9Y#libkernel",thread_detach}, {"3kg7rT0NQIs#libkernel",thread_exit},
        {"T72hz6ffq08#libkernel",thread_yield}, {"1tKyG7RlMJo#libkernel",thread_get_prio},
        {"W0Hpm2X0uPE#libkernel",thread_set_prio}, {"bt3CTBKmGyI#libkernel",thread_set_affinity}, {"rcrVFJsQWRY#libkernel",thread_get_affinity},
        {"GBUY7ywdULE#libkernel",thread_rename}, {"3PtV6p3QNX4#libkernel",thread_equal},
        {"wtkt-teR1so#libkernel",posix_attr_init}, {"zHchY8ft5pk#libkernel",posix_attr_destroy},
        {"E+tyo3lp5Lw#libkernel",posix_attr_set_detach}, {"2Q0z6rnBrTE#libkernel",posix_attr_set_stack},
        {"euKRgm0Vn2M#libkernel",posix_attr_set_param}, {"OxhIB8LB-PQ#libkernel",posix_create},
        {"Jmi+9w9u0E4#libkernel",posix_create_name}, {"h9CcP3J0oVM#libkernel",posix_join},
        {"FJrT5LuUBAU#libkernel",thread_exit},
        {"How7B8Oet6k#libkernel",thread_get_name}, {"EI-5-jlq2dE#libkernel",thread_get_id},
        {"F+yfmduIBB8#libkernel",attr_set_stack_addr},
    };
    for (size_t i=0;i<sizeof(table)/sizeof(*table);++i)
        if (!strcmp(name,table[i].nid)) return (uintptr_t)table[i].fn;
    return 0;
}
void runtime_thread_report(void) {
    pthread_mutex_lock(&lock);
    printf("Runtime: guest threads created=%zu, exited=%zu, joined=%zu\n",created,exited,joined_count);
    pthread_mutex_unlock(&lock);
    if (!wait_stats()) return;
    /* The wait map of the whole session: what every thread waited for, how long its wake-ups
     * took and how much CPU it used. The guest's threads and the host's apart. */
    static RuntimeThreadInfo infos[WAIT_STATS_THREADS];
    const size_t count=runtime_thread_snapshot(infos,WAIT_STATS_THREADS);
    uint64_t user_ns=0,kernel_ns=0;
    process_cpu_ns(&user_ns,&kernel_ns);
    uint64_t guest_cpu=0,host_cpu=0;
    uint32_t total_waits=0,total_wakes=0,total_spurious=0;
    uint64_t total_blocked=0,total_wake_ns=0,total_yields=0;
    printf("Runtime: wait map (TLG_WAIT_STATS): %u threads in the process, CPU %.0f ms\n",
           process_thread_count(),(double)(user_ns+kernel_ns)/1e6);
    for (size_t i=0;i<count;++i) {
        const RuntimeWait *w=&infos[i].wait;
        total_waits+=w->wait_count;
        total_wakes+=w->wake_count;
        total_spurious+=w->spurious;
        total_blocked+=w->wait_ns;
        total_wake_ns+=w->wake_ns;
        total_yields+=w->yield_count;
        if (w->cpu_ns) { if (infos[i].record) guest_cpu+=w->cpu_ns; else host_cpu+=w->cpu_ns; }
        if (!w->wait_count && !w->wake_count && !w->yield_count) continue;
        printf("  %s %s (tid %llu): %u waits, %.1f ms blocked [",
               infos[i].record ? "guest" : "host ",infos[i].record ? infos[i].name : "thread",
               (unsigned long long)infos[i].os_tid,w->wait_count,(double)w->wait_ns/1e6);
        int printed_kinds=0;
        uint64_t kind_ns[RUNTIME_WAIT_KINDS];
        for (unsigned k=0;k<RUNTIME_WAIT_KINDS;++k) kind_ns[k]=w->kind_ns[k];
        for (unsigned round=0;round<RUNTIME_WAIT_KINDS && printed_kinds<3;++round) {
            unsigned best=RUNTIME_WAIT_KINDS;
            uint64_t best_ns=0;
            for (unsigned k=0;k<RUNTIME_WAIT_KINDS;++k)
                if (kind_ns[k]>best_ns) { best_ns=kind_ns[k]; best=k; }
            if (best>=RUNTIME_WAIT_KINDS) break;
            printf("%s%s %.1f ms",printed_kinds?", ":"",wait_kind_name((int)best),(double)best_ns/1e6);
            kind_ns[best]=0;
            ++printed_kinds;
        }
        printf("], longest %.1f ms in '%s'",
               (double)w->wait_max_ns/1e6,w->wait_max_kind ? w->wait_max_kind : "wait");
        if (w->wait_max_name[0]) printf(" '%s'",w->wait_max_name);
        if (w->wake_count) {
            const uint64_t p50=wake_percentile(w->wake_hist,NULL,w->wake_count,0.50);
            const uint64_t p95=wake_percentile(w->wake_hist,NULL,w->wake_count,0.95);
            printf(", wake %llu/%llu us p50/p95 (mean %llu, max %llu)",
                   (unsigned long long)(p50/1000),(unsigned long long)(p95/1000),
                   (unsigned long long)(w->wake_ns/w->wake_count/1000),
                   (unsigned long long)(w->wake_max_ns/1000));
        }
        if (w->spurious) printf(", spurious %u",w->spurious);
        if (w->yield_count) printf(", yields %llu (%.1f ms)",
                                    (unsigned long long)w->yield_count,(double)w->yield_ns/1e6);
        printf(", cpu %.1f ms\n",(double)w->cpu_ns/1e6);
    }
    printf("Runtime: wait totals: %u waits, %.1f ms blocked, %u wakes, mean wake %llu us, "
           "%u spurious, %llu yields\n",
           total_waits,(double)total_blocked/1e6,total_wakes,
           (unsigned long long)(total_wakes?total_wake_ns/total_wakes/1000:0),
           total_spurious,(unsigned long long)total_yields);
    printf("Runtime: CPU per thread: guest %.0f ms, host %.0f ms, other %.0f ms\n",
           (double)guest_cpu/1e6,(double)host_cpu/1e6,
           (double)((user_ns+kernel_ns)>guest_cpu+host_cpu?(user_ns+kernel_ns)-guest_cpu-host_cpu:0)/1e6);
}

