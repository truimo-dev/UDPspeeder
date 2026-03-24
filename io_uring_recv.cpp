#include "io_uring_recv.h"

uring_ctx_t *g_uring_ctx = NULL;

#ifdef __linux__

#include "log.h"
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

/* --- Raw syscall wrappers ------------------------------------------------ */

static int
sys_io_uring_setup(unsigned entries, struct io_uring_params *p)
{
    return (int)syscall(__NR_io_uring_setup, entries, p);
}

static int
sys_io_uring_enter(int fd, unsigned to_submit, unsigned min_complete,
                   unsigned flags, void *arg, size_t argsz)
{
    return (int)syscall(__NR_io_uring_enter, fd, to_submit, min_complete,
                        flags, arg, argsz);
}

static int
sys_io_uring_register(int fd, unsigned opcode, void *arg, unsigned nr_args)
{
    return (int)syscall(__NR_io_uring_register, fd, opcode, arg, nr_args);
}

/* --- Memory barrier helpers ---------------------------------------------- */

static inline void
io_uring_smp_store_release(unsigned *p, unsigned v)
{
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

static inline unsigned
io_uring_smp_load_acquire(const unsigned *p)
{
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

/* --- SQE helpers --------------------------------------------------------- */

static struct io_uring_sqe *
get_sqe(uring_ctx_t *ctx)
{
    unsigned head = io_uring_smp_load_acquire(ctx->sq_head);
    unsigned tail = *ctx->sq_tail;
    if (tail - head >= ctx->sq_entries)
        return NULL; /* SQ full */
    struct io_uring_sqe *sqe = &ctx->sqes[tail & ctx->sq_mask];
    return sqe;
}

static void
submit_sqe(uring_ctx_t *ctx)
{
    unsigned tail = *ctx->sq_tail;
    ctx->sq_array[tail & ctx->sq_mask] = tail & ctx->sq_mask;
    io_uring_smp_store_release(ctx->sq_tail, tail + 1);
}

/* --- Buffer ring helpers ------------------------------------------------- */

static void
buf_ring_add(uring_ctx_t *ctx, int buf_id)
{
    struct io_uring_buf_ring *br = ctx->buf_ring;
    unsigned short idx = br->tail;
    struct io_uring_buf *buf = &br->bufs[idx & (ctx->buf_count - 1)];
    buf->addr = (unsigned long long)(ctx->buf_pool + (long)buf_id * ctx->buf_size + URING_RECV_HEADROOM);
    buf->len = (__u32)(ctx->buf_size - URING_RECV_HEADROOM);
    buf->bid = (__u16)buf_id;
    /* For init: publish immediately.  For runtime: use buf_ring_add_deferred + commit. */
    __atomic_store_n(&br->tail, (__u16)(idx + 1), __ATOMIC_RELEASE);
}

static void
buf_ring_add_deferred(uring_ctx_t *ctx, int buf_id)
{
    /* Add entry without publishing (no atomic store on tail).
       Caller must call uring_buf_ring_commit() when done. */
    struct io_uring_buf_ring *br = ctx->buf_ring;
    unsigned short idx = ctx->buf_ring_pending;
    struct io_uring_buf *buf = &br->bufs[idx & (ctx->buf_count - 1)];
    buf->addr = (unsigned long long)(ctx->buf_pool + (long)buf_id * ctx->buf_size + URING_RECV_HEADROOM);
    buf->len = (__u32)(ctx->buf_size - URING_RECV_HEADROOM);
    buf->bid = (__u16)buf_id;
    ctx->buf_ring_pending = (__u16)(idx + 1);
}

/* --- Public API ---------------------------------------------------------- */

int
uring_init(uring_ctx_t *ctx, int queue_depth, int buf_count, int buf_size)
{
    if (getenv("UDPSPEEDER_NO_URING")) {
        mylog(log_info, "io_uring: disabled by UDPSPEEDER_NO_URING\n");
        ctx->available = 0;
        return -1;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->ring_fd = -1;
    ctx->available = 0;
    ctx->bgid = 0;
    ctx->buf_count = buf_count;
    ctx->buf_size = buf_size;

    /* buf_count must be power of 2 for the ring */
    if (buf_count & (buf_count - 1)) {
        mylog(log_warn, "io_uring: buf_count must be power of 2\n");
        return -1;
    }

    /* 1. io_uring_setup */
    struct io_uring_params params;
    memset(&params, 0, sizeof(params));
    /* CQ sized 4x buf_count: 2 multishot requests share the CQ, and we need
       headroom for error/cancel CQEs that don't consume buffers. */
    params.flags = IORING_SETUP_CQSIZE;
    params.cq_entries = (unsigned)(buf_count * 4);

    /* Try performance flags.  Fall back if kernel rejects. */
    unsigned opt_flags = 0;
#ifdef IORING_SETUP_COOP_TASKRUN
    opt_flags |= IORING_SETUP_COOP_TASKRUN;
#endif
#ifdef IORING_SETUP_SINGLE_ISSUER
    opt_flags |= IORING_SETUP_SINGLE_ISSUER;
#endif

    params.flags |= opt_flags;
    int fd = sys_io_uring_setup((unsigned)queue_depth, &params);
    if (fd < 0 && opt_flags) {
        /* Retry without optional flags */
        memset(&params, 0, sizeof(params));
        params.flags = IORING_SETUP_CQSIZE;
        params.cq_entries = (unsigned)(buf_count * 4);
        fd = sys_io_uring_setup((unsigned)queue_depth, &params);
    }
    if (fd < 0) {
        mylog(log_info, "io_uring: io_uring_setup failed (errno %d), using fallback\n", errno);
        return -1;
    }
    ctx->ring_fd = fd;
    ctx->sq_entries = params.sq_entries;
    ctx->cq_entries = params.cq_entries;

    /* 2. mmap SQ ring */
    ctx->sq_ring_sz = (size_t)(params.sq_off.array + params.sq_entries * sizeof(unsigned));
    ctx->sq_ring_ptr = mmap(NULL, ctx->sq_ring_sz, PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQ_RING);
    if (ctx->sq_ring_ptr == MAP_FAILED) {
        mylog(log_warn, "io_uring: mmap SQ ring failed\n");
        goto fail;
    }
    ctx->sq_head = (unsigned *)((char *)ctx->sq_ring_ptr + params.sq_off.head);
    ctx->sq_tail = (unsigned *)((char *)ctx->sq_ring_ptr + params.sq_off.tail);
    ctx->sq_mask = *(unsigned *)((char *)ctx->sq_ring_ptr + params.sq_off.ring_mask);
    ctx->sq_array = (unsigned *)((char *)ctx->sq_ring_ptr + params.sq_off.array);

    /* 3. mmap SQEs */
    ctx->sqes_sz = (size_t)(params.sq_entries * sizeof(struct io_uring_sqe));
    ctx->sqes = (struct io_uring_sqe *)mmap(NULL, ctx->sqes_sz, PROT_READ | PROT_WRITE,
                                             MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQES);
    if (ctx->sqes == MAP_FAILED) {
        mylog(log_warn, "io_uring: mmap SQEs failed\n");
        goto fail;
    }

    /* 4. mmap CQ ring */
    ctx->cq_ring_sz = (size_t)(params.cq_off.cqes + params.cq_entries * sizeof(struct io_uring_cqe));
    ctx->cq_ring_ptr = mmap(NULL, ctx->cq_ring_sz, PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_CQ_RING);
    if (ctx->cq_ring_ptr == MAP_FAILED) {
        mylog(log_warn, "io_uring: mmap CQ ring failed\n");
        goto fail;
    }
    ctx->cq_head = (unsigned *)((char *)ctx->cq_ring_ptr + params.cq_off.head);
    ctx->cq_tail = (unsigned *)((char *)ctx->cq_ring_ptr + params.cq_off.tail);
    ctx->cq_mask = *(unsigned *)((char *)ctx->cq_ring_ptr + params.cq_off.ring_mask);
    ctx->cqes = (struct io_uring_cqe *)((char *)ctx->cq_ring_ptr + params.cq_off.cqes);

    /* 5. Allocate buffer pool */
    ctx->buf_pool = (char *)aligned_alloc(4096, (size_t)buf_count * (size_t)buf_size);
    if (!ctx->buf_pool) {
        mylog(log_warn, "io_uring: buf_pool alloc failed\n");
        goto fail;
    }

    /* 6. Set up provided buffer ring */
    {
        size_t ring_sz = sizeof(struct io_uring_buf_ring) +
                         (size_t)buf_count * sizeof(struct io_uring_buf);
        /* Must be page-aligned for kernel registration */
        size_t page = (size_t)sysconf(_SC_PAGESIZE);
        ring_sz = (ring_sz + page - 1) & ~(page - 1);

        ctx->buf_ring = (struct io_uring_buf_ring *)mmap(
            NULL, ring_sz, PROT_READ | PROT_WRITE,
            MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        if (ctx->buf_ring == MAP_FAILED) {
            ctx->buf_ring = NULL;
            mylog(log_warn, "io_uring: buf_ring mmap failed\n");
            goto fail;
        }
        memset(ctx->buf_ring, 0, ring_sz);
        ctx->buf_ring->tail = 0;
        ctx->buf_ring_pending = 0;

        struct io_uring_buf_reg reg;
        memset(&reg, 0, sizeof(reg));
        reg.ring_addr = (unsigned long long)ctx->buf_ring;
        reg.ring_entries = (__u32)buf_count;
        reg.bgid = (__u16)ctx->bgid;

        int ret = sys_io_uring_register(fd, IORING_REGISTER_PBUF_RING, &reg, 1);
        if (ret < 0) {
            mylog(log_info, "io_uring: REGISTER_PBUF_RING failed (errno %d), kernel too old?\n", errno);
            goto fail;
        }

        /* Populate the buffer ring with all buffers */
        for (int i = 0; i < buf_count; i++) {
            buf_ring_add(ctx, i);
        }
        ctx->buf_ring_pending = ctx->buf_ring->tail;
    }

    /* 7. Initialize msghdr template for recvmsg */
    memset(&ctx->recvmsg_hdr, 0, sizeof(ctx->recvmsg_hdr));
    memset(&ctx->recvmsg_name, 0, sizeof(ctx->recvmsg_name));
    ctx->recvmsg_hdr.msg_name = &ctx->recvmsg_name;
    ctx->recvmsg_hdr.msg_namelen = sizeof(ctx->recvmsg_name);

    ctx->available = 1;
    mylog(log_info, "io_uring: initialized (ring_fd=%d, %d buffers × %d bytes, cq=%u)\n",
          fd, buf_count, buf_size, ctx->cq_entries);
    return 0;

fail:
    uring_destroy(ctx);
    return -1;
}

void
uring_destroy(uring_ctx_t *ctx)
{
    if (ctx->buf_ring) {
        size_t page = (size_t)sysconf(_SC_PAGESIZE);
        size_t ring_sz = sizeof(struct io_uring_buf_ring) +
                         (size_t)ctx->buf_count * sizeof(struct io_uring_buf);
        ring_sz = (ring_sz + page - 1) & ~(page - 1);
        munmap(ctx->buf_ring, ring_sz);
        ctx->buf_ring = NULL;
    }
    free(ctx->buf_pool);
    ctx->buf_pool = NULL;

    if (ctx->sqes && ctx->sqes != MAP_FAILED)
        munmap(ctx->sqes, ctx->sqes_sz);
    if (ctx->sq_ring_ptr && ctx->sq_ring_ptr != MAP_FAILED)
        munmap(ctx->sq_ring_ptr, ctx->sq_ring_sz);
    if (ctx->cq_ring_ptr && ctx->cq_ring_ptr != MAP_FAILED)
        munmap(ctx->cq_ring_ptr, ctx->cq_ring_sz);

    if (ctx->ring_fd >= 0)
        close(ctx->ring_fd);

    ctx->ring_fd = -1;
    ctx->available = 0;
}

int
uring_add_multishot_recvmsg(uring_ctx_t *ctx, int fd, uint64_t user_data)
{
    struct io_uring_sqe *sqe = get_sqe(ctx);
    if (!sqe) return -1;

    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = IORING_OP_RECVMSG;
    sqe->fd = fd;
    sqe->user_data = user_data;
    sqe->flags = IOSQE_BUFFER_SELECT;
    sqe->ioprio = IORING_RECV_MULTISHOT;
    sqe->addr = (unsigned long long)&ctx->recvmsg_hdr;
    sqe->buf_group = (__u16)ctx->bgid;

    submit_sqe(ctx);
    return 0;
}

int
uring_add_multishot_recv(uring_ctx_t *ctx, int fd, uint64_t user_data)
{
    struct io_uring_sqe *sqe = get_sqe(ctx);
    if (!sqe) return -1;

    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = IORING_OP_RECV;
    sqe->fd = fd;
    sqe->user_data = user_data;
    sqe->flags = IOSQE_BUFFER_SELECT;
    sqe->ioprio = IORING_RECV_MULTISHOT;
    sqe->buf_group = (__u16)ctx->bgid;

    submit_sqe(ctx);
    return 0;
}

int
uring_cancel(uring_ctx_t *ctx, uint64_t user_data)
{
    struct io_uring_sqe *sqe = get_sqe(ctx);
    if (!sqe) return -1;

    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = IORING_OP_ASYNC_CANCEL;
    sqe->addr = user_data; /* cancels SQE matching this user_data */

    submit_sqe(ctx);
    return 0;
}

int
uring_submit(uring_ctx_t *ctx)
{
    unsigned submitted = *ctx->sq_tail - io_uring_smp_load_acquire(ctx->sq_head);
    if (submitted == 0) return 0;

    int ret = sys_io_uring_enter(ctx->ring_fd, submitted, 0,
                                  IORING_ENTER_SQ_WAKEUP, NULL, 0);
    if (ret < 0) {
        mylog(log_warn, "io_uring: io_uring_enter submit failed (errno %d)\n", errno);
        return -1;
    }
    return ret;
}

int
uring_submit_and_flush(uring_ctx_t *ctx)
{
    unsigned submitted = *ctx->sq_tail - io_uring_smp_load_acquire(ctx->sq_head);
    unsigned flags = IORING_ENTER_GETEVENTS;
    if (submitted > 0)
        flags |= IORING_ENTER_SQ_WAKEUP;

    int ret = sys_io_uring_enter(ctx->ring_fd, submitted, 0, flags, NULL, 0);
    if (ret < 0) {
        mylog(log_warn, "io_uring: io_uring_enter submit+flush failed (errno %d)\n", errno);
        return -1;
    }
    return ret;
}

/* --- Batched CQ drain API ------------------------------------------------ */

unsigned
uring_cq_ready(uring_ctx_t *ctx)
{
    /* Acquire on tail ensures we see CQE data the kernel wrote before updating tail */
    unsigned head = *ctx->cq_head;  /* our variable, no barrier needed */
    unsigned tail = io_uring_smp_load_acquire(ctx->cq_tail);
    return tail - head;
}

struct io_uring_cqe *
uring_cqe_at(uring_ctx_t *ctx, unsigned idx)
{
    /* idx is offset from current cq_head */
    unsigned head = *ctx->cq_head;
    return &ctx->cqes[(head + idx) & ctx->cq_mask];
}

void
uring_cq_advance(uring_ctx_t *ctx, unsigned n)
{
    if (n == 0) return;
    unsigned head = *ctx->cq_head;
    io_uring_smp_store_release(ctx->cq_head, head + n);
}

/* --- Batched buffer ring API --------------------------------------------- */

void
uring_recycle_buf(uring_ctx_t *ctx, int buf_id)
{
    buf_ring_add_deferred(ctx, buf_id);
}

void
uring_buf_ring_commit(uring_ctx_t *ctx)
{
    /* Single atomic publish of all deferred buffer additions */
    __atomic_store_n(&ctx->buf_ring->tail, ctx->buf_ring_pending, __ATOMIC_RELEASE);
}

void
uring_flush(uring_ctx_t *ctx)
{
    /* Trigger deferred completions by entering with GETEVENTS */
    sys_io_uring_enter(ctx->ring_fd, 0, 0, IORING_ENTER_GETEVENTS, NULL, 0);
}

int
uring_parse_recvmsg_cqe(uring_ctx_t *ctx, struct io_uring_cqe *cqe,
                          uring_recv_buf_t *out)
{
    if (cqe->res < 0) return -1;

    if (!(cqe->flags & IORING_CQE_F_BUFFER)) {
        mylog(log_debug, "io_uring: recvmsg CQE missing BUFFER flag\n");
        return -1;
    }

    int buf_id = (int)(cqe->flags >> IORING_CQE_BUFFER_SHIFT);
    if (buf_id < 0 || buf_id >= ctx->buf_count) return -1;

    /* Kernel writes at registered addr = pool + id*buf_size + HEADROOM */
    char *kernel_start = ctx->buf_pool + (long)buf_id * ctx->buf_size + URING_RECV_HEADROOM;
    struct io_uring_recvmsg_out *hdr = (struct io_uring_recvmsg_out *)kernel_start;

    out->buf_id = buf_id;
    out->addr_len = (socklen_t)(hdr->namelen < sizeof(out->addr) ? hdr->namelen : sizeof(out->addr));
    memcpy(&out->addr, kernel_start + sizeof(*hdr), out->addr_len);
    /* Kernel reserves msg_namelen bytes (from template) for name area,
       not hdr->namelen (actual). Use template sizes for offset. */
    int header_len = (int)(sizeof(*hdr) + ctx->recvmsg_hdr.msg_namelen
                           + ctx->recvmsg_hdr.msg_controllen);
    out->data = kernel_start + header_len;
    int max_payload = ctx->buf_size - URING_RECV_HEADROOM - header_len;
    out->len = (int)hdr->payloadlen;
    if (out->len > max_payload) out->len = max_payload;

    return 0;
}

int
uring_parse_recv_cqe(uring_ctx_t *ctx, struct io_uring_cqe *cqe,
                      uring_recv_buf_t *out)
{
    if (cqe->res < 0) return -1;

    if (!(cqe->flags & IORING_CQE_F_BUFFER)) {
        mylog(log_debug, "io_uring: recv CQE missing BUFFER flag\n");
        return -1;
    }

    int buf_id = (int)(cqe->flags >> IORING_CQE_BUFFER_SHIFT);
    if (buf_id < 0 || buf_id >= ctx->buf_count) return -1;

    /* Kernel writes at registered addr = pool + id*buf_size + HEADROOM */
    char *kernel_start = ctx->buf_pool + (long)buf_id * ctx->buf_size + URING_RECV_HEADROOM;
    out->buf_id = buf_id;
    out->data = kernel_start;
    out->len = cqe->res;
    out->addr_len = 0;

    return 0;
}

#endif /* __linux__ */
