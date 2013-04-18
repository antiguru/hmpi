#ifdef MPI
#define MPI_FOO
#undef MPI
#endif
#define HMPI_INTERNAL 
#include "hmpi.h"
#ifdef MPI_FOO
#define MPI
#else
#undef MPI
#endif

#include "profile2.h"


//Block size to use when using the accelerated sender-receiver copy.
#ifdef __bg__
#define BLOCK_SIZE_ONE 16384
#define BLOCK_SIZE_TWO 65536
#else
#define BLOCK_SIZE_ONE 4096
#define BLOCK_SIZE_TWO 12288
#endif


#include <malloc.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "lock.h"
#ifdef __bg__
#include <spi/include/kernel/memory.h>
#include "mpix.h"
#endif

#ifdef USE_NUMA
#include <numa.h>
#endif


#ifdef FULL_PROFILE
//PROFILE_DECLARE();
#define FULL_PROFILE_INIT() PROFILE_INIT()
#define FULL_PROFILE_VAR(v) PROFILE_VAR(v)
#define FULL_PROFILE_START(v) PROFILE_START(v)
#define FULL_PROFILE_STOP(v) PROFILE_STOP(v)
#define FULL_PROFILE_RESET(v) PROFILE_RESET(v)
#define FULL_PROFILE_SHOW(v) PROFILE_SHOW(v)
#else
#define FULL_PROFILE_INIT(v)
#define FULL_PROFILE_VAR(v)
#define FULL_PROFILE_START(v)
#define FULL_PROFILE_STOP(v)
#define FULL_PROFILE_RESET(v)
#define FULL_PROFILE_SHOW(v)
#endif

FULL_PROFILE_VAR(MPI_Other);
FULL_PROFILE_VAR(MPI_Isend);
FULL_PROFILE_VAR(MPI_Irecv);
FULL_PROFILE_VAR(MPI_Test);
FULL_PROFILE_VAR(MPI_Testall);
FULL_PROFILE_VAR(MPI_Wait);
FULL_PROFILE_VAR(MPI_Waitall);
FULL_PROFILE_VAR(MPI_Waitany);
FULL_PROFILE_VAR(MPI_Iprobe);

FULL_PROFILE_VAR(MPI_Barrier);
FULL_PROFILE_VAR(MPI_Reduce);
FULL_PROFILE_VAR(MPI_Allreduce);
FULL_PROFILE_VAR(MPI_Scan);
FULL_PROFILE_VAR(MPI_Bcast);
FULL_PROFILE_VAR(MPI_Scatter);
FULL_PROFILE_VAR(MPI_Gather);
FULL_PROFILE_VAR(MPI_Gatherv);
FULL_PROFILE_VAR(MPI_Allgather);
FULL_PROFILE_VAR(MPI_Allgatherv);
FULL_PROFILE_VAR(MPI_Alltoall);


#ifdef ENABLE_OPI
FULL_PROFILE_VAR(OPI_Alloc);
FULL_PROFILE_VAR(OPI_Free);
FULL_PROFILE_VAR(OPI_Give);
FULL_PROFILE_VAR(OPI_Take);

void OPI_Init(void);
void OPI_Finalize(void);
#endif


int g_rank=-1;                      //HMPI world rank
int g_size=-1;                      //HMPI world size
int g_node_rank=-1;                 //HMPI node rank
int g_node_size=-1;                 //HMPI node size
int g_net_rank=-1;                  //HMPI net rank
int g_net_size=-1;                  //HMPI net size
int g_numa_node=-1;                 //HMPI numa node (compute-node scope)
int g_numa_root=-1;                 //HMPI root rank on same numa node
int g_numa_rank=-1;                 //HMPI rank within numa node
int g_numa_size=-1;                 //HMPI numa node size

HMPI_Comm HMPI_COMM_WORLD;


// Debugging functionality

#include <execinfo.h>

static void show_backtrace()
{
    void* buffer[64];
    int nptrs;

    nptrs = backtrace(buffer, 64);
    backtrace_symbols_fd(buffer, nptrs, STDOUT_FILENO);
    fflush(stdout);
}


#ifdef HMPI_CHECKSUM
uint32_t compute_csum(uint8_t* buf, size_t len)
{
    uint32_t csum = 0;

    for(size_t i = 0; i < len; i++) {
        csum = csum * 31 + buf[i];
    }

    return csum;
}
#endif


#ifdef HMPI_LOGCALLS
static int g_log_fd = -1;

#include <stdarg.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#define LOG_MPI_CALL log_mpi_call
static void log_mpi_call(char* fmt, ...)
{
    va_list args;
    char str[1024];

    va_start(args, fmt);
    int len = vsnprintf(str, 1023, fmt, args);
    va_end(args);

    if(len >= 1023) {
        len = 1022;
    }

    strcat(str, "\n");
    write(g_log_fd, str, len + 1);
}
#else
#define LOG_MPI_CALL(fmt, ...)
#endif


// Internal global structures

//Each thread has a list of send and receive requests.
//The receive requests are managed privately by the owning thread.
//The send requests list for a particular thread contains sends whose target is
// that thread.  Other threads place their send requests on this list, and the
// thread owning the list matches receives against them in match_recv().

static HMPI_Item g_recv_reqs_head = {NULL};
static HMPI_Item* g_recv_reqs_tail = NULL;


typedef struct HMPI_Request_list {
    HMPI_Item head;
    HMPI_Item* tail;

    lock_t lock;
    char padding[40];
} HMPI_Request_list;

#ifndef __bg__
static mcs_qnode_t* g_lock_q;                   //Q node for lock
#endif
static HMPI_Request_list* g_send_reqs = NULL;   //Shared: Senders add sends here
static HMPI_Request_list* g_tl_my_send_reqs;    //Shortcut to my global send Q
static HMPI_Request_list g_tl_send_reqs;        //Receiver-local send Q

//Pool of unused reqs to save malloc time.
static HMPI_Item* g_free_reqs = NULL;


#define get_reqstat(req) req->stat

static inline void update_reqstat(HMPI_Request req, int stat) {
#ifdef __bg__
    __lwsync();
    //FENCE();
#endif
    req->stat = stat;
}



//TODO - Maybe send reqs should be allocated on the receiver.  How?
static inline HMPI_Request acquire_req(void)
{
    HMPI_Item* item = g_free_reqs;

    //Malloc a new req only if none are in the pool.
    if(item == NULL) {
        HMPI_Request req = (HMPI_Request)MALLOC(HMPI_Request_info, 1);
        req->match = 0;
        req->do_free = 0;
        return req;
    } else {
        g_free_reqs = item->next;
        return (HMPI_Request)item;
    }
}


static inline void release_req(HMPI_Request req)
{
    //Return a req to the pool -- once allocated, a req is never freed.
    HMPI_Item* item = (HMPI_Item*)req;

    //TODO - is there a better place to put this?
    if(req->do_free == 1) {
        free(req->buf);
        req->do_free = 0;
    }
#ifdef ENABLE_OPI
    else if(req->do_free == 2) {
        OPI_Free(&req->buf);
        req->do_free = 0;
    }
#endif

    item->next = g_free_reqs;
    g_free_reqs = item;
}


static inline void add_send_req(HMPI_Request_list* req_list,
                                HMPI_Request req) {
    //Insert req at tail.
    HMPI_Item* item = (HMPI_Item*)req;

#ifdef DEBUG
    item->next = NULL;
#endif

#ifdef __bg__
    LOCK_ACQUIRE(&req_list->lock);
#else
    mcs_qnode_t* q = g_lock_q;  //Could fold this back into macros..
    __LOCK_ACQUIRE(&req_list->lock, q);
#endif

    //NOTE -- On BG/Q other cores can see these two writes in a different order
    // than what is written here.  Thus update_send_reqs() needs to be careful
    // to acquire the lock before relying on some ordering here.
    req_list->tail->next = item;
    req_list->tail = item;

#ifdef __bg__
    LOCK_RELEASE(&req_list->lock);
#else
    __LOCK_RELEASE(&req_list->lock, q);
#endif
}


static inline void remove_send_req(HMPI_Request_list* req_list,
                                   HMPI_Item* prev, const HMPI_Item* cur)
{
    //Since we only remove from the receiver-local send Q, there is no need for
    //locking.

    if(cur->next == NULL) {
        prev->next = NULL;
        req_list->tail = prev;
    } else {
        prev->next = cur->next;
    }
}


static inline void update_send_reqs(HMPI_Request_list* local_list, HMPI_Request_list* shared_list)
{
    HMPI_Item* tail;

    if(shared_list->tail != &shared_list->head) {
        //NOTE - we can safely compare head/tail here, but we need the lock to
        // do more on BQ/Q.  On x86 it's safe to grab the first node in the
        // shared Q, But on BG/Q we can see the two writes in add_send_req() in
        // reverse order.  Thus we need to acquire the lock, ensuring that we
        // see both writes before grabbing head.next in the next statement.  If
        // we move the lock after that statement, it is possible to see the
        // updated tail in add_send_req(), come through and grab the shared
        // head.next before we see the updated head.next.

#ifdef __x86_64__
        //For x86, this statement is safe outside the lock.
        // See comments above and non-x86 statement below.
        // The branch ensures at least one node.  Senders only add at the tail,
        // so head.next won't change out from under us.
        local_list->tail->next = shared_list->head.next;
#endif

#ifdef __bg__
        LOCK_ACQUIRE(&shared_list->lock);
#else
        mcs_qnode_t* q = g_lock_q; //Could fold this back into macros..;
        __LOCK_ACQUIRE(&shared_list->lock, q);
#endif

#ifndef __x86_64__ //NOT x86
        //For non x86 (eg PPC) this statement needs to be protected.
        // See comments and x86 statement above.
        local_list->tail->next = shared_list->head.next;
#endif

        tail = shared_list->tail;
        shared_list->tail = &shared_list->head;

#ifdef __bg__
        LOCK_RELEASE(&shared_list->lock);
#else
        __LOCK_RELEASE(&shared_list->lock, q);
#endif

        //This is safe, the pointers involved here are now only accessible by
        // this core.
        local_list->tail = tail;
        tail->next = NULL;
    }
}



static inline void add_recv_req(HMPI_Request req) {
    HMPI_Item* item = (HMPI_Item*)req;

    //Add at tail to ensure matching occurs in order.
    item->next = NULL;
    g_recv_reqs_tail->next = item;
    g_recv_reqs_tail = item;

}


static inline void remove_recv_req(HMPI_Item* prev, const HMPI_Item* cur) {
    if(cur->next == NULL) {
        g_recv_reqs_tail = prev;
        prev->next = NULL;
    } else {
        prev->next = cur->next;
    }
}


//Match for receives that are *NOT* ANY_SOURCE
static inline HMPI_Request match_recv(HMPI_Request_list* req_list, HMPI_Request recv_req)
{
    HMPI_Item* cur;
    HMPI_Item* prev;
    HMPI_Request req;

    int proc = recv_req->proc;
    int tag = recv_req->tag;

    //So searching takes a long time..
    //Maybe it is taking a while for updates to the send Q to show up at the
    // receiver?  Is there a way to sync/flush/notify?
    for(prev = &req_list->head, cur = prev->next;
            cur != NULL; prev = cur, cur = cur->next) {
        req = (HMPI_Request)cur;

#ifdef ENABLE_OPI
        if(req->type == HMPI_SEND &&
                req->proc == proc && (req->tag == tag || tag == MPI_ANY_TAG)) {
#else
        if(req->proc == proc && (req->tag == tag || tag == MPI_ANY_TAG)) {
#endif
            remove_send_req(req_list, prev, cur);

            //recv_req->proc = req->proc; //Not necessary, no ANY_SRC
            recv_req->tag = req->tag;
            //printf("%d matched recv req %p proc %d tag %d to send req %p\n",
            //        g_rank, recv_req, proc, tag, req);
            return req;
        }
    }

    return HMPI_REQUEST_NULL;
}


//Match for takes
#ifdef ENABLE_OPI
static inline HMPI_Request match_take(HMPI_Request_list* req_list, HMPI_Request recv_req)
{
    HMPI_Item* cur;
    HMPI_Item* prev;
    HMPI_Request req;

    int proc = recv_req->proc;
    int tag = recv_req->tag;

    //So searching takes a long time..
    //Maybe it is taking a while for updates to the send Q to show up at the
    // receiver?  Is there a way to sync/flush/notify?
    for(prev = &req_list->head, cur = prev->next;
            cur != NULL; prev = cur, cur = cur->next) {
        req = (HMPI_Request)cur;

        if(req->type == OPI_GIVE &&
                req->proc == proc && (req->tag == tag || tag == MPI_ANY_TAG)) {
            remove_send_req(req_list, prev, cur);

            //recv_req->proc = req->proc; //Not necessary, no ANY_SRC
            recv_req->tag = req->tag;
            //printf("%d matched recv req %d proc %d tag %d to send req %p\n",
            //        g_hmpi_rank, recv_req, proc, tag, req);
            return req;
        }
    }

    return HMPI_REQUEST_NULL;
}
#endif


//Match for receives with ANY_SOURCE.
//Three things can happen here:
// No matching send is found:
//  return HMPI_REQUEST_NULL, req->u.req != MPI_REQUEST_NULL
// Matching MPI (inter-node) send is found:
//  req->u.req == MPI_REQUEST_NULL and return HMPI_REQUEST_NULL
// Matching local send is found:
//  req->u.req == MPI_REQUEST_NULL and return send_req
//Callers should check return value for local matches, and req->u.req for
// inter-node matches.
static inline HMPI_Request match_recv_any(HMPI_Request_list* req_list, HMPI_Request recv_req)
{
    HMPI_Item* cur;
    HMPI_Item* prev;
    HMPI_Request req;

    int tag = recv_req->tag;

    for(prev = &req_list->head, cur = prev->next;
            cur != NULL; prev = cur, cur = cur->next) {
        req = (HMPI_Request)cur;

        if(req->tag == tag || tag == MPI_ANY_TAG) {
            MPI_Status status;
            int flag;

            //Matched a local message -- try to cancel the MPI-level receive.
            //If not successful, we throw out the local match and use what MPI
            // gave us.  If cancel succeeds, we use the local match.
            MPI_Cancel(&recv_req->u.req);
            MPI_Wait(&recv_req->u.req, &status);
            MPI_Test_cancelled(&status, &flag);
            if(!flag) {
                //Not cancelled - use the inter-node message from MPI.
                int count;
                MPI_Get_count(&status, recv_req->datatype, &count);

                int type_size;
                MPI_Type_size(recv_req->datatype, &type_size);

                recv_req->proc = status.MPI_SOURCE;
                recv_req->tag = status.MPI_TAG;
                recv_req->size = count * type_size;
                update_reqstat(recv_req, HMPI_REQ_COMPLETE);

                //Indicate no local req was matched.
                return HMPI_REQUEST_NULL;
            }

            //Cancel succeeded, use the local send match.
            remove_send_req(req_list, prev, cur);

            recv_req->proc = req->proc;
            recv_req->tag = req->tag;
            return req;
        }
    }

    return HMPI_REQUEST_NULL;
}


static inline int match_probe(int source, int tag, HMPI_Comm comm, HMPI_Request* send_req) {
    HMPI_Item* cur;
    HMPI_Request req;
    HMPI_Request_list* req_list = &g_tl_send_reqs;

    update_send_reqs(req_list, g_tl_my_send_reqs);

    for(cur = req_list->head.next; cur != NULL; cur = cur->next) {
        req = (HMPI_Request)cur;

        //The send request can't have ANY_SOURCE or ANY_TAG,
        // so don't check for that.
        if((req->proc == source || source == MPI_ANY_SOURCE) &&
                (req->tag == tag || tag == MPI_ANY_TAG)) {
            //We don't want to do anything other than return the send req.
            *send_req = req;
            return 1;
        }
    }

    return 0;
}


#ifndef __bg__
#if 0
#include <numa.h>
#include <syscall.h>


void print_numa(void)
{
    if(numa_available() == -1) {
        printf("ERROR numa not available\n");
        return;
    }

    //printf("%d numa_max_node %d\n", g_rank, numa_max_node());
    //printf("%d numa_num_configured_nodes %d\n", g_rank, numa_num_configured_nodes());
    //unsigned long size, unsigned long maskp
#if 0
    struct bitmask* bm = numa_get_mems_allowed();
    printf("%d bm size %d\n", g_rank, bm->size);
    for(int i = 0; i < bm->size / sizeof(unsigned long); i++) {
        printf("%d numa_get_mems_allowed 0x%x\n", g_rank, bm->maskp[i]);
    }
#endif
#if 0
    numa_set_localalloc();
    int preferred = numa_preferred();
    long long freesize;
    long long totalsize = numa_node_size64(preferred, &freesize);

    printf("%d numa_preferred %d total %lld free %lld\n",
            g_rank, preferred, totalsize, freesize);
#endif



    //check some pages using move_pages and sm_lower.
    int pagesize = numa_pagesize();
    void* pages[4];
    int status[4];
    void* data = malloc(pagesize * 4);
    memset(data, 0, pagesize * 4);

    for(int i = 0; i < 4; i++) {
        pages[i] = (void*)((uintptr_t)data + (i * pagesize));
    }

    pages[0] = &pagesize;

    int ret = numa_move_pages(0, 4, pages, NULL, status, 0);
    if(ret > 0) {
        printf("%d uh, move pages couldn't move some pages %d\n", g_rank, ret);
        return;
    } else if(ret < 0) {
        printf("%d ERROR move pages %d\n", g_rank, ret);
    }

    //for(int i = 0; i < 4; i++) {
    //    printf("%d page %p status %d %s\n", g_rank, pages[i], status[i], strerror(-status[i]));
    //}

    numa_set_preferred(status[0]);
    //printf("%d now preferred %d\n", g_rank, numa_preferred());
    free(data);
}
#endif
#endif


int HMPI_Init(int *argc, char ***argv)
{
    MPI_Init(argc, argv);
    FULL_PROFILE_INIT();

#ifdef __bg__
    //On BG/Q, we rely on BG_MAPCOMMONHEAP=1 to get shared memory.
    //Check that it is set before continuing.
    char* tmp = getenv("BG_MAPCOMMONHEAP");
    if(tmp == NULL || atoi(tmp) != 1) {
        printf("ERROR BG_MAPCOMMONHEAP not enabled\n");
        MPI_Abort(MPI_COMM_WORLD, 0);
    }
#endif

    //Set up communicators
    //TODO - this needs to be pulled out into its own routine.
    // A lot of the g_* variables should be moved into the comm struct.
    MPI_Comm_rank(MPI_COMM_WORLD, &g_rank);
    MPI_Comm_size(MPI_COMM_WORLD, &g_size);
    
    HMPI_COMM_WORLD = (HMPI_Comm_info*)MALLOC(HMPI_Comm_info, 1);
    HMPI_COMM_WORLD->comm = MPI_COMM_WORLD;

    //Split into comms containing ranks on the same nodes.
    {
#ifdef __bg__
        MPIX_Hardware_t hw;

        MPIX_Hardware(&hw);

        //printf("%d prank %d psize %d ppn %d coreID %d MHz %d memSize %d\n",
        //        g_rank, hw.prank, hw.psize, hw.ppn, hw.coreID, hw.clockMHz,
        //        hw.memSize);
        int color = 0;
        for(int i = 0; i < hw.torus_dimension; i++) {
            color = (color * hw.Size[i]) + hw.Coords[i];
        }

#else
        //Hash our processor name into a color for Comm_split()
        char proc_name[MPI_MAX_PROCESSOR_NAME];
        int proc_name_len;
        MPI_Get_processor_name(proc_name, &proc_name_len);

        int color = 0;
        for(char* s = proc_name; *s != '\0'; s++) {
            color = *s + 31 * color;
        }
#endif

        //MPI says color must be non-negative.
        color &= 0x7FFFFFFF;

        MPI_Comm_split(MPI_COMM_WORLD, color, g_rank,
                &HMPI_COMM_WORLD->node_comm);
    }

    //Used in HMPI_Comm_node_rank().
    MPI_Comm_size(HMPI_COMM_WORLD->node_comm, &HMPI_COMM_WORLD->node_size);

    {
        MPI_Group node_group;
        MPI_Group world_group;
        MPI_Comm_group(HMPI_COMM_WORLD->node_comm, &node_group);
        MPI_Comm_group(HMPI_COMM_WORLD->comm, &world_group);

        int base_rank = 0;
        MPI_Group_translate_ranks(node_group, 1,
                &base_rank, world_group, &HMPI_COMM_WORLD->node_root);

    }


    MPI_Comm_rank(HMPI_COMM_WORLD->node_comm, &g_node_rank);
    MPI_Comm_size(HMPI_COMM_WORLD->node_comm, &g_node_size);


    //Create a comm that goes across the nodes.
    //This will contain only the procs with node rank 0, or node rank 1, etc.
    MPI_Comm_split(MPI_COMM_WORLD,
            g_node_rank, g_rank, &HMPI_COMM_WORLD->net_comm);

    MPI_Comm_rank(HMPI_COMM_WORLD->net_comm, &g_net_rank);
    MPI_Comm_size(HMPI_COMM_WORLD->net_comm, &g_net_size);


#ifdef USE_NUMA
    //Split the node comm into per-NUMA-domain (ie socket) comms.
    //Look up the NUMA node of a stack page -- this should be local.
    int ret = 0;
    void* page = &ret;

    ret = numa_move_pages(0, 1, &page, NULL, &g_numa_node, 0);
    if(ret != 0) {
        printf("ERROR numa_move_pages %s\n", strerror(ret));
        MPI_Abort(MPI_COMM_WORLD, 0);
    }
#else
    //Without a way to find the local NUMA node, assume one NUMA node.
    g_numa_node = 0;
#endif //USE_NUMA

    MPI_Comm_split(HMPI_COMM_WORLD->node_comm, g_numa_node, g_node_rank,
            &HMPI_COMM_WORLD->numa_comm);

    MPI_Comm_rank(HMPI_COMM_WORLD->numa_comm, &g_numa_rank);
    MPI_Comm_size(HMPI_COMM_WORLD->numa_comm, &g_numa_size);

    {
        MPI_Group numa_group;
        MPI_Group world_group;
        MPI_Comm_group(HMPI_COMM_WORLD->numa_comm, &numa_group);
        MPI_Comm_group(HMPI_COMM_WORLD->comm, &world_group);

        int base_rank = 0;
        MPI_Group_translate_ranks(numa_group, 1,
                &base_rank, world_group, &g_numa_root);
    }

#if 0
    printf("%5d rank=%3d size=%3d node_rank=%2d node_size=%2d node_root=%4d "
            "net_rank=%2d net_size=%2d numa_node=%d numa_root=%3d "
            "numa_rank=%2d\n",
            getpid(), g_rank, g_size, g_node_rank, g_node_size,
            HMPI_COMM_WORLD->node_root, g_net_rank, g_net_size, g_numa_node,
            g_numa_root, g_numa_rank);
#endif


    //Ensure every rank on a node has the same shared region address.
    {
        void* result = NULL;

        MPI_Allreduce(&sm_lower, &result, 1, MPI_LONG,
                MPI_MAX, HMPI_COMM_WORLD->node_comm);
        if(result != sm_lower) {
            printf("%d ERROR sm_lower %p doesn't agree with MAX %p\n",
                    g_rank, sm_lower, result);
            MPI_Abort(MPI_COMM_WORLD, 0);
        }

        MPI_Allreduce(&sm_upper, &result, 1, MPI_LONG,
                MPI_MAX, HMPI_COMM_WORLD->node_comm);
        if(result != sm_upper) {
            printf("%d ERROR sm_upper %p doesn't agree with MAX %p\n",
                    g_rank, sm_upper, result);
            MPI_Abort(MPI_COMM_WORLD, 0);
        }
    }


    //Set up intra-node shared memory structures.
    if(g_node_rank == 0) {
        //One rank per node allocates shared send request lists.
        g_send_reqs = MALLOC(HMPI_Request_list, g_node_size);
    }

    MPI_Bcast(&g_send_reqs, 1, MPI_LONG, 0, HMPI_COMM_WORLD->node_comm);


    // Initialize request lists and lock
    g_recv_reqs_tail = &g_recv_reqs_head;

#ifndef __bg__
    //Except on BGQ, allocate a SHARED lock Q for use with MCS locks.
    //Used in Qing sends on the receiver and clearing that Q.
    g_lock_q = MALLOC(mcs_qnode_t, 1);
    memset(g_lock_q, 0, sizeof(mcs_qnode_t));
#endif

    g_send_reqs[g_node_rank].head.next = NULL;
    g_send_reqs[g_node_rank].tail = &g_send_reqs[g_node_rank].head;

    g_tl_my_send_reqs = &g_send_reqs[g_node_rank];
    LOCK_INIT(&g_send_reqs[g_node_rank].lock);

    g_tl_send_reqs.head.next = NULL;
    g_tl_send_reqs.tail = &g_tl_send_reqs.head;

    //print_numa();

    hmpi_coll_t* coll = HMPI_COMM_WORLD->coll = MALLOC(hmpi_coll_t, 1);

    MPI_Bcast(&HMPI_COMM_WORLD->coll, 1, MPI_LONG, 0,
            HMPI_COMM_WORLD->node_comm);

    if(g_node_rank == 0) {
        coll->sbuf = MALLOC(volatile void*, g_node_size);
        coll->rbuf = MALLOC(volatile void*, g_node_size);
        coll->tmp = MALLOC(volatile void*, g_node_size);


        for(int i=0; i<PTOP; i++) {
            coll->ptop[i] = MALLOC(padptop, g_node_size);
        }

        // for(int i =0; i<g_node_size; i++)
        // {
        //   coll->ptop_0[i] = 0;
        //   coll->ptop_1[i] = 0;
        //   coll->ptop_2[i] = 0;
        //   coll->ptop_3[i] = 0;
        //   coll->ptop_4[i] = 0;
        // }

        for(int j = 0; j < PTOP; j++) {
            for(int i = 0; i < g_node_size; i++) {
                coll->ptop[j][i].ptopsense = 0;
            }
        }

        //for(int i =0; i<g_node_size; i++)
        //{
        //  coll->ptop_0[i].ptopsense = 0;
        //  coll->ptop_1[i].ptopsense = 0;
        //  coll->ptop_2[i].ptopsense = 0;
        //  coll->ptop_3[i].ptopsense = 0;
        //  coll->ptop_4[i].ptopsense = 0;
        //}

        //FANINEQUAL1(t_barrier_init_fanin1(&coll->t_barr, g_node_size);) 
        //PFANIN(t_barrier_init(&coll->t_barr, g_node_size););
    }


#ifdef ENABLE_OPI
    OPI_Init();
#endif

    //Set up debugging stuff
#ifdef HMPI_LOGCALLS
    {
        char filename[1024];
        snprintf(filename, 1024, "hmpi-%d.log", getpid());
        g_log_fd = open(filename, O_CREAT|O_DIRECT|O_TRUNC|O_WRONLY);
    }
#endif

    MPI_Barrier(MPI_COMM_WORLD);
    FULL_PROFILE_START(MPI_Other);
    return MPI_SUCCESS;
}


int HMPI_Finalize()
{
    FULL_PROFILE_STOP(MPI_Other);
    FULL_PROFILE_START(MPI_Other);
    FULL_PROFILE_SHOW(MPI_Isend);
    FULL_PROFILE_SHOW(MPI_Irecv);
    FULL_PROFILE_SHOW(MPI_Test);
    FULL_PROFILE_SHOW(MPI_Testall);
    FULL_PROFILE_SHOW(MPI_Wait);
    FULL_PROFILE_SHOW(MPI_Waitall);
    FULL_PROFILE_SHOW(MPI_Waitany);
    FULL_PROFILE_SHOW(MPI_Iprobe);

    FULL_PROFILE_SHOW(MPI_Barrier);
    FULL_PROFILE_SHOW(MPI_Reduce);
    FULL_PROFILE_SHOW(MPI_Allreduce);
    FULL_PROFILE_SHOW(MPI_Scan);
    FULL_PROFILE_SHOW(MPI_Bcast);
    FULL_PROFILE_SHOW(MPI_Scatter);
    FULL_PROFILE_SHOW(MPI_Gather);
    FULL_PROFILE_SHOW(MPI_Gatherv);
    FULL_PROFILE_SHOW(MPI_Allgather);
    FULL_PROFILE_SHOW(MPI_Allgatherv);
    FULL_PROFILE_SHOW(MPI_Alltoall);

    FULL_PROFILE_SHOW(MPI_Other);

#ifdef ENABLE_OPI
    OPI_Finalize();
#endif

    //Seems to prevent a segfault in MPI_Finalize()
    MPI_Barrier(HMPI_COMM_WORLD->comm);

    MPI_Finalize();
    return 0;
}


//We assume req->type == HMPI_SEND
static inline int HMPI_Progress_send(const HMPI_Request send_req)
{
    if(get_reqstat(send_req) == HMPI_REQ_COMPLETE) {
        return HMPI_REQ_COMPLETE;
    }

    //Write blocks on this send req if receiver has matched it.
    //If mesage is short, receiver won't bother clearing the match lock, and
    // instead just does the copy and marks completion.
    if(send_req->match &&
            CAS_T_BOOL(volatile uint32_t, &send_req->match, 1, 0)) {
        HMPI_Request recv_req = (HMPI_Request)send_req->u.match_req;
        uintptr_t rbuf = (uintptr_t)recv_req->buf;

        //Receiver does any size sanity checking.
        size_t size = send_req->size;
        size_t block_size = BLOCK_SIZE_ONE;
        if(size >= BLOCK_SIZE_TWO << 1) {
            block_size = BLOCK_SIZE_TWO;
        }

        uintptr_t sbuf = (uintptr_t)send_req->buf;
        volatile ssize_t* offsetptr = &recv_req->u.offset;
        ssize_t offset;

        //length to copy is min of len - offset and BLOCK_SIZE
        while((offset = FETCH_ADD64(offsetptr, block_size)) < size) {
            size_t left = size - offset;
            memcpy((void*)(rbuf + offset), (void*)(sbuf + offset),
                    (left < block_size ? left : block_size));
        }

        //Signal that the sender is done copying.
        //Possible for the receiver to still be copying here.
#ifdef __bg__
        STORE_FENCE();
#endif
        send_req->match = 1;

        //Receiver will set completion soon, wait rather than running off.
        while(get_reqstat(send_req) != HMPI_REQ_COMPLETE);

        return HMPI_REQ_COMPLETE;
    }

    return HMPI_REQ_ACTIVE;
}


//For req->type == HMPI_RECV
static inline void HMPI_Complete_recv(HMPI_Request recv_req, HMPI_Request send_req)
{
    size_t send_size = send_req->size;
    size_t size = recv_req->size;

#ifdef DEBUG
    if(unlikely(send_size > size)) {
        printf("%d ERROR recv message from %d of size %ld truncated to %ld\n", g_rank, send_req->proc, send_size, size);
        MPI_Abort(MPI_COMM_WORLD, 5);
    }
#endif

    if(send_size < size) {
        //Adjust receive count
        //printf("%d WARNING recv from %d is %d bytes, recv is %d bytes\n",
        //        g_rank, send_req->proc, send_size, size);
        recv_req->size = send_size;
        size = send_size;
    }

    uintptr_t rbuf = (uintptr_t)recv_req->buf;
    uintptr_t sbuf = (uintptr_t)send_req->buf;

    if(size < BLOCK_SIZE_ONE << 1 || !IS_SM_BUF((void*)rbuf)) {
        //Use memcpy for small messages, and when the user's receive buf isn't
        // in the SM region.  On the recv path, buf is always the user's recv
        // buf, whether it's an SM region or not.
        memcpy((void*)rbuf, (void*)sbuf, size);
    } else {
        //The setting of send_req->match_req signals to sender that they can
        // start doing copying as well, if they are testing the req.

        recv_req->u.offset = 0;
        send_req->u.match_req = recv_req;
        STORE_FENCE();
        send_req->match = 1;

        size_t block_size = BLOCK_SIZE_ONE;
        if(size >= BLOCK_SIZE_TWO << 1) {
            block_size = BLOCK_SIZE_TWO;
        }

        volatile ssize_t* offsetptr = &recv_req->u.offset;
        ssize_t offset = 0;

        //length to copy is min of len - offset and BLOCK_SIZE
        while((offset = FETCH_ADD64(offsetptr, block_size)) < size) {
            size_t left = size - offset;

            memcpy((void*)(rbuf + offset), (void*)(sbuf + offset),
                    (left < block_size ? left : block_size));
        }

        //Wait if the sender is copying.
        while(!CAS_T_BOOL(volatile uint32_t, &send_req->match, 1, 0));
    }

#ifdef HMPI_CHECKSUM
#warning "csum enabled"
    uint32_t recv_csum = compute_csum(recv_req->buf, size);
    if(recv_csum != send_req->csum) {
        printf("%d csum %d mismatched sender %d csum %d\n",
                g_rank, recv_csum, send_req->proc, send_req->csum);
    }
#endif

#ifdef DEBUG
    printf("%d completed local-level RECV buf %p size %lu source %d tag %d\n",
            g_rank, recv_req->buf, recv_req->size, recv_req->proc, recv_req->tag);
    printf("%d completed local-level SEND buf %p size %lu dest %d tag %d\n",
            send_req->proc, send_req->buf, send_req->size, g_rank, send_req->tag);
#endif

    //Mark send and receive requests done
    update_reqstat(send_req, HMPI_REQ_COMPLETE);
    update_reqstat(recv_req, HMPI_REQ_COMPLETE);
}


#ifdef ENABLE_OPI
//For req->type == OPI_TAKE
static inline void HMPI_Complete_take(HMPI_Request recv_req, HMPI_Request send_req)
{
    size_t send_size = send_req->size;
    size_t size = recv_req->size;

    if(send_size < size) {
        //Adjust receive count
        recv_req->size = send_size;
        size = send_size;
    }

#if DEBUG
    if(unlikely(send_size > size)) {
        printf("%d ERROR recv message from %d of size %ld truncated to %ld\n", g_rank, send_req->proc, send_size, size);
        MPI_Abort(MPI_COMM_WORLD, 5);
    }
#endif

#if 0
    if(size < 256) {
        //Size is too small - just memcpy the buffer instead of doing OP.
        //But, I have to alloc and free.. blah
        OPI_Alloc(recv_req->buf, size);
        memcpy(*((void**)recv_req->buf), send_req->buf, size);
        OPI_Free(&send_req->buf);
    } else {
#endif
        //Easy OP
        *((void**)recv_req->buf) = send_req->buf;
    //}

    //Mark send and receive requests done
    update_reqstat(send_req, HMPI_REQ_COMPLETE);
    update_reqstat(recv_req, HMPI_REQ_COMPLETE);
}
#endif


//For req->type == MPI_SEND || req->type == MPI_RECV
static int HMPI_Progress_mpi(HMPI_Request req)
{
    int flag = 0;
    MPI_Status status = {0};

#if DEBUG
    if(req->u.req == MPI_REQUEST_NULL) {
        printf("%d Progress_mpi on null request!\n", g_rank);
    }
#endif

    MPI_Test(&req->u.req, &flag, &status);

    if(flag) {
        //Update status
        int count = 0;
        MPI_Get_count(&status, req->datatype, &count);

        int type_size;
        MPI_Type_size(req->datatype, &type_size);

        req->proc = status.MPI_SOURCE;

        req->tag = status.MPI_TAG;
        req->size = count * type_size;
        update_reqstat(req, HMPI_REQ_COMPLETE);
    }

    return flag;
}


//Progress local receive requests.
static void HMPI_Progress(HMPI_Item* recv_reqs_head,
        HMPI_Request_list* local_list, HMPI_Request_list* shared_list) {
    HMPI_Item* cur;
    HMPI_Item* prev;
    HMPI_Request req;

    //TODO - poll MPI here?

    update_send_reqs(local_list, shared_list);

    //Progress receive requests.
    //We remove items from the list, but they are still valid; nothing in this
    //function will free or modify a req.  So, it's safe to do cur = cur->next.
    //Note the careful updating of prev; we need to leave it alone on iterations
    //where cur is matched successfully and only update it otherwise.
    // This prevents the recv_reqs list from getting corrupted due to a bad
    // prev pointer.
    for(prev = recv_reqs_head, cur = prev->next;
            cur != NULL; cur = cur->next) {
        req = (HMPI_Request)cur;

        if(likely(req->type == HMPI_RECV)) {
            HMPI_Request send_req = match_recv(local_list, req);
            if(send_req != HMPI_REQUEST_NULL) {
                HMPI_Complete_recv(req, send_req);

                remove_recv_req(prev, cur);
                continue; //Whenever we remove a req, dont update prev
            }
#ifdef ENABLE_OPI
        } else if(req->type == OPI_TAKE) {
            HMPI_Request send_req = match_take(local_list, req);
            if(send_req != HMPI_REQUEST_NULL) {
                HMPI_Complete_take(req, send_req);

                remove_recv_req(prev, cur);
                continue; //Whenever we remove a req, dont update prev
            }
#endif
        } else { //req->type == HMPI_RECV_ANY_SOURCE
            //First, check for a local match.
            // match_recv_any() may complete the MPI-level receive here.
            // In that case, it returns REQUEST_NULL indicating no match,
            // but the request will be in completed state.
            HMPI_Request send_req = match_recv_any(local_list, req);
            if(send_req != HMPI_REQUEST_NULL) {
                HMPI_Complete_recv(req, send_req);

                remove_recv_req(prev, cur);
                continue; //Whenever we remove a req, dont update prev
            } else if(req->u.req == MPI_REQUEST_NULL) {
                //This means match_recv_any tried to cancel the MPI recv and
                // failed, so we completed the MPI request.
                remove_recv_req(prev, cur);
                continue;
            } else /*if(g_net_size > 1)*/ {
                if(HMPI_Progress_mpi(req)) {
                    remove_recv_req(prev, cur);
                    continue;
                }
            }
        }

        //Update prev -- we only do this if cur wasn't matched.
        prev = cur;
    }

    //TODO - probe/progress MPI here if nothing was completed?
}



int HMPI_Test(HMPI_Request *request, int *flag, HMPI_Status *status)
{
    FULL_PROFILE_STOP(MPI_Other);
    FULL_PROFILE_START(MPI_Test);
    HMPI_Request req = *request;

    LOG_MPI_CALL("MPI_Test(request=%p, flag=%p, status=%p) type=%d",
            request, flag, status, req);

    if(unlikely(req == HMPI_REQUEST_NULL)) {
        if(status != HMPI_STATUS_IGNORE) {
            //Make Get_count return 0 count
            status->size = 0;
        }

        *flag = 1;
        return MPI_SUCCESS;
    } else if(get_reqstat(req) != HMPI_REQ_COMPLETE) {
        int f;

        HMPI_Progress(&g_recv_reqs_head, &g_tl_send_reqs, g_tl_my_send_reqs);

        if(req->type == HMPI_SEND) {
            f = HMPI_Progress_send(req);
        //OPI GIVE/TAKE will need an entry here; just check its stat
#ifdef ENABLE_OPI
        } else if(req->type == HMPI_RECV || req->type == OPI_GIVE || req->type == OPI_TAKE) {
#else
        } else if(req->type == HMPI_RECV) {
#endif
            f = get_reqstat(req);
        } else if(req->type == MPI_SEND || req->type == MPI_RECV) {
            f = HMPI_Progress_mpi(req);
        } else { //req->type == HMPI_RECV_ANY_SOURCE
            f = get_reqstat(req);
        }

        if(f != HMPI_REQ_COMPLETE) {
            *flag = 0;
            FULL_PROFILE_STOP(MPI_Test);
            FULL_PROFILE_START(MPI_Other);
            return MPI_SUCCESS;
        }
    }

    if(status != HMPI_STATUS_IGNORE) {
        status->size = req->size;
        status->MPI_SOURCE = req->proc;
        status->MPI_TAG = req->tag;
        status->MPI_ERROR = MPI_SUCCESS;
    }

    release_req(req);
    *request = HMPI_REQUEST_NULL;
    *flag = 1;

    FULL_PROFILE_STOP(MPI_Test);
    FULL_PROFILE_START(MPI_Other);
    return MPI_SUCCESS;
}


int HMPI_Testall(int count, HMPI_Request *requests, int* flag, HMPI_Status *statuses)
{
    FULL_PROFILE_STOP(MPI_Other);
    FULL_PROFILE_START(MPI_Testall);
    LOG_MPI_CALL(
            "MPI_Testall(count=%d, requests=%p, flag=%p, statuses=%p)",
            count, requests, flag, statuses);

    *flag = 1;

    //Return as soon as any one request isn't complete.
    //TODO - poll each request anyway, to try and progress?
    for(int i = 0; i < count && *flag; i++) {
        if(requests[i] == HMPI_REQUEST_NULL) {
            continue;
        } else if(statuses == HMPI_STATUSES_IGNORE) {
            HMPI_Test(&requests[i], flag, HMPI_STATUS_IGNORE);
        } else {
            HMPI_Test(&requests[i], flag, &statuses[i]);
        }

        if(!(*flag)) {
            FULL_PROFILE_STOP(MPI_Testall);
            FULL_PROFILE_START(MPI_Other);
            return MPI_SUCCESS;
        }
    }

    FULL_PROFILE_STOP(MPI_Testall);
    FULL_PROFILE_START(MPI_Other);
    return MPI_SUCCESS;
}


int HMPI_Wait(HMPI_Request *request, HMPI_Status *status) {
    FULL_PROFILE_STOP(MPI_Other);
    FULL_PROFILE_START(MPI_Wait);

    HMPI_Request req = *request;

    LOG_MPI_CALL("MPI_Wait(request=%p, statuses=%p) type=%d",
            request, status, req->type);

    if(unlikely(req == HMPI_REQUEST_NULL)) {
        if(status != HMPI_STATUS_IGNORE) {
            //Make Get_count return 0 count
            status->size = 0;
        }

        FULL_PROFILE_STOP(MPI_Wait);
        FULL_PROFILE_START(MPI_Other);
        return MPI_SUCCESS;
    }

    HMPI_Item* recv_reqs_head = &g_recv_reqs_head;
    HMPI_Request_list* local_list = &g_tl_send_reqs;
    HMPI_Request_list* shared_list = g_tl_my_send_reqs;

    if(req->type == HMPI_SEND) {
        do {
            HMPI_Progress(recv_reqs_head, local_list, shared_list);
        } while(HMPI_Progress_send(req) != HMPI_REQ_COMPLETE);
        //OPI GIVE/TAKE will need an entry here; just check its stat
#ifdef ENABLE_OPI
    } else if(req->type == HMPI_RECV || req->type == OPI_GIVE || req->type == OPI_TAKE) {
#else
    } else if(req->type == HMPI_RECV) {
#endif
        do {
            HMPI_Progress(recv_reqs_head, local_list, shared_list);
        } while(get_reqstat(req) != HMPI_REQ_COMPLETE);
    } else if(req->type == MPI_RECV || req->type == MPI_SEND) {
        do {
            HMPI_Progress(recv_reqs_head, local_list, shared_list);
        } while(HMPI_Progress_mpi(req) != HMPI_REQ_COMPLETE);
    } else { //HMPI_RECV_ANY_SOURCE
        do {
            HMPI_Progress(recv_reqs_head, local_list, shared_list);
        } while(get_reqstat(req) != HMPI_REQ_COMPLETE);
    }

    //Req is complete at this point
    if(status != HMPI_STATUS_IGNORE) {
        status->size = req->size;
        status->MPI_SOURCE = req->proc;
        status->MPI_TAG = req->tag;
        status->MPI_ERROR = MPI_SUCCESS;
    }

    release_req(req);
    *request = HMPI_REQUEST_NULL;

    FULL_PROFILE_STOP(MPI_Wait);
    FULL_PROFILE_START(MPI_Other);
    return MPI_SUCCESS;
}


int HMPI_Waitall(int count, HMPI_Request *requests, HMPI_Status *statuses)
{
    FULL_PROFILE_STOP(MPI_Other);
    FULL_PROFILE_START(MPI_Waitall);
    LOG_MPI_CALL("MPI_Waitall(count=%d, requests=%p, statuses=%p)",
            count, requests, statuses);

    HMPI_Item* recv_reqs_head = &g_recv_reqs_head;
    HMPI_Request_list* local_list = &g_tl_send_reqs;
    HMPI_Request_list* shared_list = g_tl_my_send_reqs;
    int done;

    do {
        HMPI_Progress(recv_reqs_head, local_list, shared_list);
        done = 0;

        for(int i = 0; i < count; i++) {
            HMPI_Request req = requests[i];

            if(req == HMPI_REQUEST_NULL) {
                done += 1;
                continue;
            }

            if(get_reqstat(req) != HMPI_REQ_COMPLETE) {
                if(req->type == HMPI_SEND) {
                    if(HMPI_Progress_send(req) != HMPI_REQ_COMPLETE) {
                        continue;
                    }
        //OPI GIVE/TAKE will need an entry here; just continue
#ifdef ENABLE_OPI
                } else if(req->type == HMPI_RECV || req->type == OPI_GIVE || req->type == OPI_TAKE) {
#else
                } else if(req->type == HMPI_RECV) {
#endif
                    continue;
                } else if(req->type == MPI_SEND || req->type == MPI_RECV) {
                    if(!HMPI_Progress_mpi(req)) {
                        continue;
                    }
                } else {
                    continue;
                }
            }

            //req is complete but status not handled
            if(statuses != HMPI_STATUSES_IGNORE) {
                HMPI_Status* st = &statuses[i];
                st->size = req->size;
                st->MPI_SOURCE = req->proc;
                st->MPI_TAG = req->tag;
                st->MPI_ERROR = MPI_SUCCESS;
            }

            release_req(req);
            requests[i] = HMPI_REQUEST_NULL;
            done += 1;
        }
    } while(done < count);

    FULL_PROFILE_STOP(MPI_Waitall);
    FULL_PROFILE_START(MPI_Other);
    return MPI_SUCCESS;
}


int HMPI_Waitany(int count, HMPI_Request* requests, int* index, HMPI_Status *status)
{
    FULL_PROFILE_STOP(MPI_Other);
    FULL_PROFILE_START(MPI_Waitany);
    LOG_MPI_CALL("MPI_Waitany(count=%d, requests=%p, index=%p, status=%p)",
            count, requests, index, status);

    HMPI_Item* recv_reqs_head = &g_recv_reqs_head;
    HMPI_Request_list* local_list = &g_tl_send_reqs;
    HMPI_Request_list* shared_list = g_tl_my_send_reqs;
    int done;

    //Call Progress once, then check each req.
    do {
        done = 1;

        //What if I have progress return whether a completion was made?
        //Then I can skip the poll loop unless something finishes.
        HMPI_Progress(recv_reqs_head, local_list, shared_list);

        //TODO - is there a way to speed this up?
        // Maybe keep my own list of non-null entries that i can reorder to
        // skip null requests..
        for(int i = 0; i < count; i++) {
            HMPI_Request req = requests[i];
            if(req == HMPI_REQUEST_NULL) {
                continue;
            }

            //Not done! at least one req was not NULL.
            done = 0;

            //Check completion of the req.
            if(get_reqstat(req) != HMPI_REQ_COMPLETE) {
                if(req->type == HMPI_SEND) {
                    if(HMPI_Progress_send(req) != HMPI_REQ_COMPLETE) {
                        continue;
                    }
                //OPI GIVE/TAKE will need an entry here; just continue
#ifdef ENABLE_OPI
                } else if(req->type == HMPI_RECV || req->type == OPI_GIVE || req->type == OPI_TAKE) {
#else
                } else if(req->type == HMPI_RECV) {
#endif
                    continue;
                } else if(req->type == MPI_SEND || req->type == MPI_RECV) {
                    if(!HMPI_Progress_mpi(req)) {
                        continue;
                    }
                } else {
                    continue;
                }
            }

            //req is complete, handle status.
            if(status != HMPI_STATUS_IGNORE) {
                status->size = req->size;
                status->MPI_SOURCE = req->proc;
                status->MPI_TAG = req->tag;
                status->MPI_ERROR = MPI_SUCCESS;
            }

            release_req(req);
            requests[i] = HMPI_REQUEST_NULL;
            *index = i;

            FULL_PROFILE_STOP(MPI_Waitany);
            FULL_PROFILE_START(MPI_Other);
            return MPI_SUCCESS;
        }
    } while(done == 0);

    //All requests were NULL.
    *index = MPI_UNDEFINED;
    if(status != HMPI_STATUS_IGNORE) {
        status->size = 0;
    }

    FULL_PROFILE_STOP(MPI_Waitany);
    FULL_PROFILE_START(MPI_Other);
    return MPI_SUCCESS;
}


int HMPI_Get_count(HMPI_Status* status, MPI_Datatype datatype, int* count)
{
    LOG_MPI_CALL("MPI_Get_count(status=%d, datatype=%d, count=%p)",
            status, datatype, count);

    int type_size;

    MPI_Type_size(datatype, &type_size);

    if(unlikely(type_size == 0)) {
        *count = 0;
    } else if(unlikely(status->size % type_size != 0)) {
        *count = MPI_UNDEFINED;
    } else {
        *count = status->size / (size_t)type_size;
    }

    return MPI_SUCCESS;
}


int HMPI_Iprobe(int source, int tag, HMPI_Comm comm, int* flag, HMPI_Status* status)
{
    FULL_PROFILE_STOP(MPI_Other);
    FULL_PROFILE_START(MPI_Iprobe);
    LOG_MPI_CALL("MPI_Iprobe(source=%d, tag=%d, comm=%d, flag=%p, status=%p)",
            source, tag, comm, flag, status);

    HMPI_Request send_req = NULL;

    //Progress here prevents deadlocks.
    HMPI_Progress(&g_recv_reqs_head, &g_tl_send_reqs, g_tl_my_send_reqs);

    //Probe HMPI (on-node) layer
    *flag = match_probe(source, tag, comm, &send_req);
    if(*flag) {
        if(status != HMPI_STATUS_IGNORE) {
            status->size = send_req->size;
            status->MPI_SOURCE = send_req->proc;
            status->MPI_TAG = send_req->tag;
            status->MPI_ERROR = MPI_SUCCESS;
        }
    } else /*if(g_net_size > 1)*/ {
        //Probe MPI (off-node) layer only if more than one MPI rank
        MPI_Status st;
        MPI_Iprobe(source, tag, comm->comm, flag, &st);

        if(*flag && status != HMPI_STATUS_IGNORE) {
            int count;
            MPI_Get_count(&st, MPI_BYTE, &count);
            status->size = count;
            status->MPI_SOURCE = st.MPI_SOURCE;
            status->MPI_TAG = st.MPI_TAG;
            status->MPI_ERROR = st.MPI_ERROR;
        }
    }

    FULL_PROFILE_STOP(MPI_Iprobe);
    FULL_PROFILE_START(MPI_Other);
    return MPI_SUCCESS;
}


int HMPI_Probe(int source, int tag, HMPI_Comm comm, HMPI_Status* status)
{
    //LOG_MPI_CALL("MPI_Probe(source=%d, tag=%d, comm=%d, status=%p)",
    //        source, tag, comm, status);

    int flag;

    do {
        HMPI_Iprobe(source, tag, comm, &flag, status);
    } while(flag == 0);

    return MPI_SUCCESS;
}


#ifdef __bg__
//BGQ speed hack assumes 64 ranks/node
#define RANKS_NODE 64
#define RANKS_NODE_SHIFT 6
#define RANKS_NODE_MASK ((1 << RANKS_NODE_SHIFT) - 1)
inline void HMPI_Comm_node_rank(HMPI_Comm comm, int rank, int* node_rank)
{
    //First determine if rank is on the same node.
    if(rank >> RANKS_NODE_SHIFT == g_rank >> RANKS_NODE_SHIFT) {
        *node_rank = rank & RANKS_NODE_MASK; 
    } else {
        *node_rank = MPI_UNDEFINED;
    }
}

#else
//Covers MPI_ANY_SOURCE, but no other special values like MPI_PROC_NULL.
inline void HMPI_Comm_node_rank(HMPI_Comm comm, int rank, int* node_rank)
{
    if(rank == MPI_ANY_SOURCE) {
        *node_rank = MPI_ANY_SOURCE;
        return;
    } 
    
    int diff = rank - comm->node_root;
    
    //if(rank >= comm->node_root &&
    //        (diff = rank - comm->node_root) < comm->node_size) {
    if(diff >= 0 && diff < comm->node_size) {
        *node_rank = diff;
        return;
    }

    *node_rank = MPI_UNDEFINED;
}
#endif


int HMPI_Isend(void* buf, int count, MPI_Datatype datatype, int dest, int tag, HMPI_Comm comm, HMPI_Request *request)
{
    FULL_PROFILE_STOP(MPI_Other);
    FULL_PROFILE_START(MPI_Isend);
    LOG_MPI_CALL("MPI_Isend(buf=%p, count=%d, datatype=%d, dest=%d, tag=%d, comm=%p, request=%p)",
            buf, count, datatype, dest, tag, comm, request);

    //Freed when req completion is signaled back to the user.
    HMPI_Request req = *request = acquire_req();

#if 0
    int size;
    MPI_Type_size(datatype, &size);
#ifdef HMPI_SAFE
    MPI_Aint extent, lb;
    MPI_Type_get_extent(datatype, &lb, &extent);
    if(extent != size) {
        printf("non-contiguous derived datatypes are not supported yet!\n");
        MPI_Abort(comm->comm, 0);
    }

    if(comm->comm != MPI_COMM_WORLD) {
        printf("only MPI_COMM_WORLD is supported so far\n");
        MPI_Abort(comm->comm, 0);
    }
#endif
#endif

#if DEBUG
    if(dest == < 0) {
        printf("%d dest %d MPI_PROC_NULL %d MPI_ANY_SOURCE %d\n",
                g_rank, dest, MPI_PROC_NULL, MPI_ANY_SOURCE);
        abort();
    }
#endif

    if(unlikely(dest == MPI_PROC_NULL)) { 
        req->type = HMPI_SEND;
        update_reqstat(req, HMPI_REQ_COMPLETE);
        FULL_PROFILE_STOP(MPI_Isend);
        FULL_PROFILE_START(MPI_Other);
        return MPI_SUCCESS;
    }

    int dest_node_rank;
    HMPI_Comm_node_rank(comm, dest, &dest_node_rank);

    int type_size;
    MPI_Type_size(datatype, &type_size);
    size_t size = (size_t)count * (size_t)type_size;

#ifdef HMPI_CHECKSUM
    req->csum = compute_csum((uint8_t*)buf, size);
#endif

    //update_reqstat() has a memory fence on BGQ, avoid it here.
    req->stat = HMPI_REQ_ACTIVE;
    req->datatype = datatype;

    if(dest_node_rank != MPI_UNDEFINED) {
        req->type = HMPI_SEND;

        req->proc = g_rank; // always sender's world-level rank
        req->tag = tag;
        req->size = size;
        req->buf = buf;

        //For small messages, copy into the eager buffer.
        //If the user's send buffer is not in the SM region, allocate an SM buf
        // and copy the data over.
        //On BGQ, immediate doesn't help, and the buf is always an SM buf.

#ifndef __bg__
        if(size < EAGER_LIMIT) {
            memcpy(req->eager, buf, size);
            req->buf = req->eager;
        } else if(buf != NULL && !IS_SM_BUF(buf)) {
            //printf("%d warning, non-SM buf %p size %ld\n", g_rank, buf, size);
            //show_backtrace();
            req->do_free = 1;
            req->buf = MALLOC(uint8_t, size);
            memcpy(req->buf, buf, size);
        } 
#endif

        add_send_req(&g_send_reqs[dest_node_rank], req);
    } else {
        MPI_Isend(buf, count, datatype,
                dest, tag, comm->comm, &req->u.req);

        req->type = MPI_SEND;
    }

    FULL_PROFILE_STOP(MPI_Isend);
    FULL_PROFILE_START(MPI_Other);
    return MPI_SUCCESS;
}


int HMPI_Send(void* buf, int count, MPI_Datatype datatype, int dest, int tag, HMPI_Comm comm) {
  HMPI_Request req;
  HMPI_Isend(buf, count, datatype, dest, tag, comm, &req);
  HMPI_Wait(&req, HMPI_STATUS_IGNORE);
  return MPI_SUCCESS;
}


int HMPI_Irecv(void* buf, int count, MPI_Datatype datatype, int source, int tag, HMPI_Comm comm, HMPI_Request *request)
{
    FULL_PROFILE_STOP(MPI_Other);
    FULL_PROFILE_START(MPI_Irecv);
    LOG_MPI_CALL("MPI_Irecv(buf=%p, count=%d, datatype=%d, src=%d, tag=%d, comm=%p, request=%p)",
            buf, count, datatype, source, tag, comm, request);

    //Freed when req completion is signaled back to the user.
    HMPI_Request req = *request = acquire_req();

#if 0
#ifdef HMPI_SAFE
    MPI_Aint extent, lb;
    MPI_Type_get_extent(datatype, &lb, &extent);
    if(extent != size) {
        printf("non-contiguous derived datatypes are not supported yet!\n");
        MPI_Abort(comm->comm, 0);
    }

    if(comm->comm != MPI_COMM_WORLD) {
        printf("only MPI_COMM_WORLD is supported so far\n");
        MPI_Abort(comm->comm, 0);
    }
#endif 
#endif

    if(unlikely(source == MPI_PROC_NULL)) { 
        req->type = HMPI_RECV;
        update_reqstat(req, HMPI_REQ_COMPLETE);
        FULL_PROFILE_STOP(MPI_Irecv);
        FULL_PROFILE_START(MPI_Other);
        return MPI_SUCCESS;
    }

    int src_node_rank;
    HMPI_Comm_node_rank(comm, source, &src_node_rank);

    int type_size;
    MPI_Type_size(datatype, &type_size);

    //update_reqstat() has a memory fence on BGQ, avoid it here.
    req->stat = HMPI_REQ_ACTIVE;
    req->datatype = datatype;

#if 0
        if(buf != NULL && !IS_SM_BUF(buf)) {
            printf("%d warning, non-SM buf %p size %ld\n", g_rank, buf, req->size);
        }
#endif

    if(unlikely(source == MPI_ANY_SOURCE)) {
        MPI_Irecv(buf, count, datatype,
                source, tag, comm->comm, &req->u.req);

        req->type = HMPI_RECV_ANY_SOURCE;

        req->proc = source; //Always sender's world-level rank
        req->tag = tag;
        req->size = count * type_size;
        req->buf = buf;

        add_recv_req(req);
    } else if(src_node_rank != MPI_UNDEFINED) {
        req->type = HMPI_RECV;

        req->proc = source; //Always sender's world-level rank
        req->tag = tag;
        req->size = count * type_size;
        req->buf = buf;

        add_recv_req(req);
    } else { //Recv off-node, but not ANY_SOURCE
        MPI_Irecv(buf, count, datatype,
                source, tag, comm->comm, &req->u.req);

        req->type = MPI_RECV;
    }

    FULL_PROFILE_STOP(MPI_Irecv);
    FULL_PROFILE_START(MPI_Other);
    return MPI_SUCCESS;
}


int HMPI_Recv(void* buf, int count, MPI_Datatype datatype, int source, int tag, HMPI_Comm comm, HMPI_Status *status)
{
    HMPI_Request req;

    HMPI_Irecv(buf, count, datatype, source, tag, comm, &req);
    HMPI_Wait(&req, status);
    return MPI_SUCCESS;
}


#ifdef ENABLE_OPI
int OPI_Give(void** ptr, int count, MPI_Datatype datatype, int dest, int tag, HMPI_Comm comm, HMPI_Request* request)
{
    FULL_PROFILE_STOP(MPI_Other);
    FULL_PROFILE_START(OPI_Give);
    LOG_MPI_CALL("OPI_Give(ptr=%p, count=%d, datatype=%d, dest=%d, tag=%d, comm=%p, request=%p)",
            ptr, count, datatype, dest, tag, comm, request);

    //Freed when req completion is signaled back to the user.
    HMPI_Request req = *request = acquire_req();

    if(unlikely(dest == MPI_PROC_NULL)) { 
        req->type = OPI_GIVE;
        update_reqstat(req, HMPI_REQ_COMPLETE);
        FULL_PROFILE_STOP(OPI_Give);
        FULL_PROFILE_START(MPI_Other);
        return MPI_SUCCESS;
    }

    int dest_node_rank;
    HMPI_Comm_node_rank(comm, dest, &dest_node_rank);

    int type_size;
    MPI_Type_size(datatype, &type_size);
    uint64_t size = (uint64_t)count * (uint64_t)type_size;

    //update_reqstat() has a memory fence on BGQ, avoid it here.
    req->stat = HMPI_REQ_ACTIVE;

    req->proc = g_rank; // my local rank
    req->tag = tag;
    req->size = size;
    req->buf = *ptr; //For OP Give, we store the pointer being shared directly.
    req->datatype = datatype;

    if(dest_node_rank != MPI_UNDEFINED) {
        req->type = OPI_GIVE;

        add_send_req(&g_send_reqs[dest_node_rank], req);
    } else {
        //This is easy, just convert to a send.
        MPI_Isend(*ptr, count, datatype,
                dest, tag, comm->comm, &req->u.req);
        req->type = MPI_SEND;

        //OPI_Free will be called when the req is released.
        req->do_free = 2;
    }

    //*ptr = NULL;
    FULL_PROFILE_STOP(OPI_Give);
    FULL_PROFILE_START(MPI_Other);
    return MPI_SUCCESS;
}


int OPI_Take(void** ptr, int count, MPI_Datatype datatype, int source, int tag, HMPI_Comm comm, HMPI_Request* request)
{
    FULL_PROFILE_STOP(MPI_Other);
    FULL_PROFILE_START(OPI_Take);
    LOG_MPI_CALL("OPI_Take(ptr=%p, count=%d, datatype=%d, src=%d, tag=%d, comm=%p, request=%p)",
            ptr, count, datatype, source, tag, comm, request);

    //Freed when req completion is signaled back to the user.
    HMPI_Request req = *request = acquire_req();

    if(unlikely(source == MPI_PROC_NULL)) { 
        req->type = OPI_TAKE;
        update_reqstat(req, HMPI_REQ_COMPLETE);
        FULL_PROFILE_STOP(MPI_Irecv);
        FULL_PROFILE_START(MPI_Other);
        return MPI_SUCCESS;
    }

    int src_node_rank;
    HMPI_Comm_node_rank(comm, source, &src_node_rank);

    int type_size;
    MPI_Type_size(datatype, &type_size);

    //update_reqstat() has a memory fence on BGQ, avoid it here.
    req->stat = HMPI_REQ_ACTIVE;

    req->proc = source;
    req->tag = tag;
    req->size = count * type_size;
    req->buf = ptr;
    req->datatype = datatype;

    if(unlikely(source == MPI_ANY_SOURCE)) {
        //Take ANY_SOURCE not supported right now
        //TODO - what would i have to do for this?
        // We could match a local give, or a remote send.
        abort();

        // test both layers and pick first 
        req->type = OPI_TAKE_ANY_SOURCE;

        add_recv_req(req);
    } else if(src_node_rank != MPI_UNDEFINED) {
        //Recv on-node, but not ANY_SOURCE
        req->type = OPI_TAKE;

        add_recv_req(req);
    } else { //Recv off-node, but not ANY_SOURCE
        OPI_Alloc(ptr, req->size);
        MPI_Irecv(*ptr, count, datatype,
                source, tag, comm->comm, &req->u.req);
        req->buf = *ptr;
        req->type = MPI_RECV;
    }

    FULL_PROFILE_STOP(OPI_Take);
    FULL_PROFILE_START(MPI_Other);
    return MPI_SUCCESS;
}
#endif //ENABLE_OPI

