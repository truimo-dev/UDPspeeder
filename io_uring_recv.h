#ifndef IO_URING_RECV_H_
#define IO_URING_RECV_H_

#include "common.h"

#ifdef __linux__

#include <stdint.h>
#include <sys/socket.h>

/* --- Kernel constant fallbacks (for older headers) ----------------------- */

#include <linux/io_uring.h>
#include <sys/syscall.h>

/*
 * Macro fallbacks — these are #define'd in kernel headers (not enums),
 * so #ifndef works reliably.  Struct/enum fallbacks are NOT provided;
 * compilation requires kernel headers 6.0+ (Ubuntu 22.04 HWE or 24.04).
 * Runtime probe handles older kernels gracefully.
 */
#ifndef IORING_RECV_MULTISHOT
#define IORING_RECV_MULTISHOT (1U << 1)
#endif
#ifndef IORING_CQE_F_MORE
#define IORING_CQE_F_MORE (1U << 1)
#endif
#ifndef IORING_CQE_F_BUFFER
#define IORING_CQE_F_BUFFER (1U << 0)
#endif
#ifndef IORING_CQE_BUFFER_SHIFT
#define IORING_CQE_BUFFER_SHIFT 16
#endif

/* --- Public API ---------------------------------------------------------- */

struct uring_recv_buf_t {
    char *data;
    int len;
    struct sockaddr_storage addr;
    socklen_t addr_len;
    int buf_id;
};

struct uring_ctx_t {
    int ring_fd;
    int available;

    /* mmap'd ring pointers */
    void *sq_ring_ptr;
    size_t sq_ring_sz;
    void *cq_ring_ptr;
    size_t cq_ring_sz;
    struct io_uring_sqe *sqes;
    size_t sqes_sz;

    /* SQ ring offsets */
    unsigned *sq_head;
    unsigned *sq_tail;
    unsigned *sq_array;
    unsigned sq_mask;
    unsigned sq_entries;

    /* CQ ring offsets */
    unsigned *cq_head;
    unsigned *cq_tail;
    unsigned cq_mask;
    unsigned cq_entries;
    struct io_uring_cqe *cqes;

    /* Provided buffer ring */
    struct io_uring_buf_ring *buf_ring;
    unsigned short buf_ring_pending; /* shadow tail for deferred recycling */
    char *buf_pool;
    int buf_count;
    int buf_size;       /* size per buffer including header room */
    int bgid;

    /* msghdr template for multishot recvmsg */
    struct msghdr recvmsg_hdr;
    struct sockaddr_storage recvmsg_name;
};

/* User data tag encode/decode */
static inline uint64_t uring_tag(uint8_t type, uint64_t payload) {
    return ((uint64_t)type << 56) | (payload & 0x00FFFFFFFFFFFFFFULL);
}
static inline uint8_t uring_tag_type(uint64_t user_data) {
    return (uint8_t)(user_data >> 56);
}
static inline uint64_t uring_tag_payload(uint64_t user_data) {
    return user_data & 0x00FFFFFFFFFFFFFFULL;
}

/* Tag types */
#define URING_TAG_CLIENT_LOCAL   0x01
#define URING_TAG_CLIENT_REMOTE  0x02
#define URING_TAG_SERVER_LOCAL   0x03
#define URING_TAG_SERVER_REMOTE  0x04

/* Headroom reserved before each provided buffer for in-place conv header */
#define URING_RECV_HEADROOM  4  /* sizeof(u32_t) */

int  uring_init(uring_ctx_t *ctx, int queue_depth, int buf_count, int buf_size);
void uring_destroy(uring_ctx_t *ctx);

int  uring_add_multishot_recvmsg(uring_ctx_t *ctx, int fd, uint64_t user_data);
int  uring_add_multishot_recv(uring_ctx_t *ctx, int fd, uint64_t user_data);
int  uring_cancel(uring_ctx_t *ctx, uint64_t user_data);
int  uring_submit(uring_ctx_t *ctx);

/* Batched CQ drain */
unsigned uring_cq_ready(uring_ctx_t *ctx);
struct io_uring_cqe *uring_cqe_at(uring_ctx_t *ctx, unsigned idx);
void uring_cq_advance(uring_ctx_t *ctx, unsigned n);

int  uring_submit_and_flush(uring_ctx_t *ctx);
void uring_flush(uring_ctx_t *ctx);

int  uring_parse_recvmsg_cqe(uring_ctx_t *ctx, struct io_uring_cqe *cqe,
                              uring_recv_buf_t *out);
int  uring_parse_recv_cqe(uring_ctx_t *ctx, struct io_uring_cqe *cqe,
                           uring_recv_buf_t *out);
void uring_recycle_buf(uring_ctx_t *ctx, int buf_id);
void uring_buf_ring_commit(uring_ctx_t *ctx);

/* Global pointer — set in tunnel event loop, used by connection.cpp cleanup */
extern uring_ctx_t *g_uring_ctx;

#else /* !__linux__ */

/* Stubs for non-Linux — always unavailable */
struct uring_recv_buf_t { char *data; int len; int buf_id; };
struct uring_ctx_t { int available; };
static inline int uring_init(uring_ctx_t *ctx, int, int, int) { ctx->available = 0; return -1; }
static inline void uring_destroy(uring_ctx_t *) {}

/* Tag helpers still available for compilation */
static inline uint64_t uring_tag(uint8_t type, uint64_t payload) {
    return ((uint64_t)type << 56) | (payload & 0x00FFFFFFFFFFFFFFULL);
}
#define URING_TAG_CLIENT_LOCAL   0x01
#define URING_TAG_CLIENT_REMOTE  0x02
#define URING_TAG_SERVER_LOCAL   0x03
#define URING_TAG_SERVER_REMOTE  0x04

extern uring_ctx_t *g_uring_ctx;

#endif /* __linux__ */
#endif /* IO_URING_RECV_H_ */
