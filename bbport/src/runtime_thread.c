/* Guest threads on host pthreads. Each guest thread owns a FreeBSD-style TCB
 * (variant II: static TLS below the TCB). The loader rewrites the eboot's
 * `mov rax, fs:[0]` into `mov rax, gs:[0]`, so GS base = guest TCB while glibc
 * keeps FS. Priorities/affinity are recorded, not enforced by a PS4 scheduler.
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
#include <unistd.h>
#include <stdint.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#else
#include <dirent.h>
#include <sys/syscall.h>
#include <sys/mman.h>
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
#ifdef _WIN32
    bb_jmp_buf exit_jump; /* no SEH unwinding through guest frames */
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
    current=t;
}
/* A thread's record is its ScePthread handle, and the game packs handles into 40-bit fields: the
 * owner of Bloodborne's hybrid lock (guest 0x20368c0) is masked to 0xffffffffff and then compared
 * with the whole handle, so a handle above 40 bits never matches its own lock and the thread sleeps
 * on its semaphore forever (lamp travel hung at loading). Records come from low memory (below 40
 * bits, zeroed), carved out of 256 KiB pieces; they are never freed. From deadinside28's bbport
 * 0.6 (their PR #193). */
static GuestThread *low_record(void) {
    static pthread_mutex_t pool_lock=PTHREAD_MUTEX_INITIALIZER;
    static unsigned char *pool;
    static size_t left;
    const size_t size=(sizeof(GuestThread)+63)&~(size_t)63, piece=256*1024;
    void *record=NULL;
    pthread_mutex_lock(&pool_lock);
    if (left<size) {
        pool=runtime_low_map(piece,PROT_READ|PROT_WRITE);
        left=pool ? piece : 0;
    }
    if (left>=size) { record=pool; pool+=size; left-=size; }
    pthread_mutex_unlock(&pool_lock);
    return record;
}
static GuestThread *new_thread(void) {
    /* Not before the game's image is placed: it takes the start of low memory (the guest code
     * hooks expect it at 0x800000000). A host thread's record before that comes from the heap. */
    GuestThread *t=runtime_image_start ? low_record() : NULL;
    if (!t) {
        if (runtime_image_start)
            fputs("Runtime: no low memory for a thread record; its handle may not fit 40 bits\n",stderr);
        t=calloc(1,sizeof(*t));
    }
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
static ABI int32_t *guest_error(void) { return &guest_errno; }
int32_t *runtime_errno(void) { return &guest_errno; }

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

#ifdef _WIN32
typedef HRESULT (WINAPI *ThreadDescriptionSet)(HANDLE, PCWSTR);
typedef HRESULT (WINAPI *ThreadDescriptionGet)(HANDLE, PWSTR *);
static FARPROC kernel32_proc(const char *name) {
    HMODULE kernel=GetModuleHandleA("kernel32.dll");
    return kernel ? GetProcAddress(kernel,name) : NULL;
}
#endif
/* Linux thread names hold 15 characters; longer ones would be rejected. On Windows the thread's
 * description holds the whole name, so runtime_thread_cpu_report (and a debugger) can show it. */
void runtime_set_thread_name(const char *name) {
    char host[16]={0};
    memcpy(host,name,strnlen(name,sizeof(host)-1));
    pthread_setname_np(pthread_self(),host);
#ifdef _WIN32
    ThreadDescriptionSet set=(ThreadDescriptionSet)(void *)kernel32_proc("SetThreadDescription");
    WCHAR wide[64];
    if (set && MultiByteToWideChar(CP_UTF8,0,name,-1,wide,64)>0) set(GetCurrentThread(),wide);
#endif
}

/* The threads that took the most CPU time since the last call, one line (AtiniirSpectre
 * 2026-10-23: after a death the whole game slowed, the co-op tick's stalls landing in whichever
 * part ran; a thread busy where it should sleep shows here). Called beside the Perf line. */
typedef struct { unsigned long id; uint64_t cpu_ns; char name[48]; } ThreadCpu;
void runtime_thread_cpu_report(void) {
    enum { MaxThreads=512, Shown=5 };
    static ThreadCpu before[MaxThreads];
    static size_t before_count;
    static uint64_t before_wall_ns;
    static ThreadCpu now[MaxThreads];
    size_t count=0;
    struct timespec wall; clock_gettime(CLOCK_MONOTONIC,&wall);
    const uint64_t wall_ns=(uint64_t)wall.tv_sec*1000000000u+(uint64_t)wall.tv_nsec;
#ifdef _WIN32
    ThreadDescriptionGet get=(ThreadDescriptionGet)(void *)kernel32_proc("GetThreadDescription");
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
    if (snapshot==INVALID_HANDLE_VALUE) return;
    THREADENTRY32 entry={.dwSize=sizeof(entry)};
    const DWORD process=GetCurrentProcessId();
    for (BOOL more=Thread32First(snapshot,&entry); more && count<MaxThreads; more=Thread32Next(snapshot,&entry)) {
        if (entry.th32OwnerProcessID!=process) continue;
        HANDLE thread=OpenThread(THREAD_QUERY_LIMITED_INFORMATION,FALSE,entry.th32ThreadID);
        if (!thread) continue;
        FILETIME created, exited, kernel, user;
        if (GetThreadTimes(thread,&created,&exited,&kernel,&user)) {
            ThreadCpu *t=&now[count++];
            t->id=entry.th32ThreadID;
            t->cpu_ns=((((uint64_t)user.dwHighDateTime<<32)|user.dwLowDateTime)+
                       (((uint64_t)kernel.dwHighDateTime<<32)|kernel.dwLowDateTime))*100;
            t->name[0]=0;
            PWSTR description=NULL;
            if (get && SUCCEEDED(get(thread,&description)) && description) {
                WideCharToMultiByte(CP_UTF8,0,description,-1,t->name,sizeof(t->name),NULL,NULL);
                LocalFree(description);
            }
        }
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
#else
    DIR *tasks=opendir("/proc/self/task");
    if (!tasks) return;
    const long ticks=sysconf(_SC_CLK_TCK);
    for (struct dirent *e; (e=readdir(tasks)) && count<MaxThreads;) {
        if (e->d_name[0]<'0' || e->d_name[0]>'9') continue;
        char path[320], line[512];
        snprintf(path,sizeof(path),"/proc/self/task/%s/stat",e->d_name);
        FILE *f=fopen(path,"r");
        if (!f) continue;
        const char *got=fgets(line,sizeof(line),f);
        fclose(f);
        const char *close=got ? strrchr(line,')') : NULL;
        unsigned long long utime=0, stime=0;
        if (!close || sscanf(close+2,"%*c %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %llu %llu",&utime,&stime)!=2) continue;
        ThreadCpu *t=&now[count++];
        t->id=strtoul(e->d_name,NULL,10);
        t->cpu_ns=(utime+stime)*(1000000000u/(uint64_t)ticks);
        t->name[0]=0;
        snprintf(path,sizeof(path),"/proc/self/task/%s/comm",e->d_name);
        if ((f=fopen(path,"r"))) {
            if (fgets(t->name,sizeof(t->name),f)) t->name[strcspn(t->name,"\n")]=0;
            fclose(f);
        }
    }
    closedir(tasks);
#endif
    if (before_wall_ns && wall_ns>before_wall_ns) {
        ThreadCpu top[Shown]={{0}};
        uint64_t total=0;
        for (size_t i=0;i<count;++i) {
            uint64_t used=now[i].cpu_ns;
            for (size_t j=0;j<before_count;++j) if (before[j].id==now[i].id) { used=now[i].cpu_ns-before[j].cpu_ns; break; }
            total+=used;
            ThreadCpu t=now[i]; t.cpu_ns=used;
            for (size_t k=0;k<Shown;++k) if (t.cpu_ns>top[k].cpu_ns) { ThreadCpu out=top[k]; top[k]=t; t=out; }
        }
        const double window=(double)(wall_ns-before_wall_ns);
        char text[512]; size_t at=0;
        for (size_t k=0;k<Shown && top[k].cpu_ns;++k)
            at+=(size_t)snprintf(text+at,sizeof(text)-at,"%s%s %.0f%%",k ? ", " : "",
                                 top[k].name[0] ? top[k].name : "?",100.0*top[k].cpu_ns/window);
        text[at<sizeof(text) ? at : sizeof(text)-1]=0;
        printf("Runtime: CPU over the window: %.0f%% of a core in %zu threads; busiest: %s\n",
               100.0*total/window,count,at ? text : "none");
    }
    memcpy(before,now,count*sizeof(*now));
    before_count=count;
    before_wall_ns=wall_ns;
}
#ifdef _WIN32
#define exit_setjmp(t) bb_setjmp((t)->exit_jump)
#define exit_longjmp(t) bb_longjmp((t)->exit_jump,1)
#else
#define exit_setjmp(t) setjmp((t)->exit_jump)
#define exit_longjmp(t) longjmp((t)->exit_jump,1)
#endif
/* Guest priorities run 256 (highest) to 767; the game's threads are 700 except FMOD's mixer and
 * output threads (256-265). Host threads all ran at normal priority, so on 4-6 core CPUs the
 * draw workers and game threads delayed FMOD's output thread: underruns heard as crackling (its
 * worst gap per 5 s 36 ms against a 5.3 ms period on four CPU threads, 6 ms at
 * THREAD_PRIORITY_HIGHEST; Supermedo 08df6af). BB_THREAD_PRIORITY=0 keeps every thread at
 * normal priority. */
int runtime_thread_host_priority(int guest_prio, const char *env) {
#ifdef _WIN32
    const int enabled=!env || env[0]!='0';
    return enabled && guest_prio<400 ? THREAD_PRIORITY_HIGHEST : THREAD_PRIORITY_NORMAL;
#else
    (void)guest_prio; (void)env;
    return 0;
#endif
}
static void set_host_priority(int guest_prio) {
#ifdef _WIN32
    static const char *env;
    static int read;
    if (!read) { env=getenv("BB_THREAD_PRIORITY"); read=1; }
    const int prio=runtime_thread_host_priority(guest_prio,env);
    if (prio!=THREAD_PRIORITY_NORMAL) SetThreadPriority(GetCurrentThread(),prio);
#else
    (void)guest_prio;
#endif
}
static void *host_start(void *p) {
    GuestThread *t=p;
    attach(t);
    runtime_set_thread_name(t->name);
    set_host_priority(t->attr.prio);
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
    int e=pthread_join(t->host,&value);
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
static ABI int32_t thread_yield(void) { sched_yield(); return 0; }
static ABI int32_t thread_get_prio(GuestThread *t,int *prio) {
    if (!find_thread(t)) return ERR(3);
    if (!prio) return ERR(22);
    *prio=t->attr.prio; return 0;
}
static ABI int32_t thread_set_prio(GuestThread *t,int prio) {
    if (!find_thread(t)) return ERR(3);
    t->attr.prio=prio; return 0;
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
    if (t==current) runtime_set_thread_name(t->name);
    return 0;
}
static ABI int32_t thread_equal(GuestThread *a,GuestThread *b) { return a==b; }

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

/* Co-op: guest threads started by host libraries (NP matching/signaling threads call the
 * game's callbacks). Both return 0 or a positive errno value. */
int32_t runtime_thread_spawn(void **out,void *(ABI *entry)(void *),void *argument,uint64_t stack,const char *name) {
    ThreadAttr *attr=NULL;
    int32_t r=attr_init(&attr);
    if (!r && stack) r=attr_set_stack(&attr,stack);
    if (!r) r=create((GuestThread **)out,&attr,entry,argument,name);
    if (attr) attr_destroy(&attr);
    return posix(r);
}
int32_t runtime_thread_join_spawned(void *thread,void **result) {
    return posix(thread_join(thread,result));
}

uintptr_t runtime_thread_resolve(const char *name) {
    static const struct { const char *nid; void *fn; } table[]={
        {"aI+OeCz8xrQ#p#J",thread_self}, {"EotR8a3ASf4#I#J",thread_self},
        {"9BcDykPmo1I#p#J",guest_error},
        {"nsYoNRywwNg#p#J",attr_init}, {"62KCwEMmzcM#p#J",attr_destroy},
        {"x1X76arYMxU#p#J",attr_get}, {"8+s5BzZjxSg#p#J",attr_affinity},
        {"UTXzJbWhhTE#p#J",attr_set_stack}, {"-Wreprtu0Qs#p#J",attr_set_detach},
        {"4+h9EzwKF4I#p#J",attr_set_policy}, {"DzES9hQF4f4#p#J",attr_set_param},
        {"3qxgM4ezETA#p#J",attr_set_affinity}, {"eXbUSpEaTsA#p#J",attr_set_inherit},
        {"JaRMy+QcpeU#p#J",attr_get_detach}, {"-fA+7ZlGDQs#p#J",attr_get_stack},
        {"FXPWHNk8Of0#p#J",attr_get_param}, {"El+cQ20DynU#p#J",attr_set_guard},
        {"6UgtwV+0zb4#p#J",thread_create}, {"onNY9Byn-W8#p#J",thread_join},
        {"4qGrR6eoP9Y#p#J",thread_detach}, {"3kg7rT0NQIs#p#J",thread_exit},
        {"T72hz6ffq08#p#J",thread_yield}, {"1tKyG7RlMJo#p#J",thread_get_prio},
        {"W0Hpm2X0uPE#p#J",thread_set_prio}, {"bt3CTBKmGyI#p#J",thread_set_affinity}, {"rcrVFJsQWRY#p#J",thread_get_affinity},
        {"GBUY7ywdULE#p#J",thread_rename}, {"3PtV6p3QNX4#p#J",thread_equal},
        {"wtkt-teR1so#I#J",posix_attr_init}, {"zHchY8ft5pk#I#J",posix_attr_destroy},
        {"E+tyo3lp5Lw#I#J",posix_attr_set_detach}, {"2Q0z6rnBrTE#I#J",posix_attr_set_stack},
        {"euKRgm0Vn2M#I#J",posix_attr_set_param}, {"OxhIB8LB-PQ#I#J",posix_create},
        {"Jmi+9w9u0E4#I#J",posix_create_name}, {"h9CcP3J0oVM#I#J",posix_join},
        {"FJrT5LuUBAU#I#J",thread_exit},
    };
    for (size_t i=0;i<sizeof(table)/sizeof(*table);++i)
        if (!strcmp(name,table[i].nid)) return (uintptr_t)table[i].fn;
    return 0;
}
void runtime_thread_report(void) {
    pthread_mutex_lock(&lock);
    printf("Runtime: guest threads created=%zu, exited=%zu, joined=%zu\n",created,exited,joined_count);
    pthread_mutex_unlock(&lock);
}

