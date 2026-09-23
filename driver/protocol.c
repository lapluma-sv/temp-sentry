#include "protocol.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

/* 累计读到的原始字节数，便于判断"有没有数据进来" */
static uint64_t g_rx_bytes;

uint64_t protocol_rx_bytes(void)
{
    return g_rx_bytes;
}

/* 从 4 字节网络序里读出长度（等价于 chatroom 里的 ntohl，顺便避免非对齐指针解引用） */
static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* 解包：从协议包里解析出消息类型和正文，正文自动补 '\0' */
int protocol_unpack(const uint8_t *buf, int buf_len, uint8_t *type, char *msg, int msg_len)
{
    if (buf == NULL || msg == NULL) {
        return PROTOCOL_ERR_INVALID;
    }
    if (buf_len < PROTOCOL_HEADER_SIZE) {
        return PROTOCOL_ERR_MSGSIZE;
    }

    int body_len = (int)read_be32(buf + 3);
    int total_len = PROTOCOL_HEADER_SIZE + body_len + PROTOCOL_TAIL_SIZE;

    if (body_len > PROTOCOL_MAX_BODY_SIZE) {
        return PROTOCOL_ERR_MSGSIZE;
    }
    if (buf_len < total_len) {
        return PROTOCOL_ERR_MSGSIZE;
    }
    if (buf[0] != PROTOCOL_MAGIC || buf[1] != PROTOCOL_MAGIC ||
        buf[total_len - 2] != PROTOCOL_MAGIC || buf[total_len - 1] != PROTOCOL_MAGIC) {
        return PROTOCOL_ERR_MAGIC;
    }

    *type = buf[2];
    if (*type == 0 || *type > MSG_TYPE_COUNT) {
        return PROTOCOL_ERR_TYPE;
    }

    uint8_t checksum = 0;
    for (int i = 0; i < body_len; i++) {
        checksum ^= buf[PROTOCOL_HEADER_SIZE + i];
    }
    if (checksum != buf[total_len - 3]) {
        return PROTOCOL_ERR_CHECKSUM;
    }

    if (msg_len < body_len + 1) {
        return PROTOCOL_ERR_MSGSIZE;
    }

    memcpy(msg, buf + PROTOCOL_HEADER_SIZE, (size_t)body_len);
    msg[body_len] = '\0';
    return body_len;
}

/* 从 fd 里取一个完整帧，顺便处理半帧（缓冲不够就返回 WOULDBLOCK，等下次再读）
 * 流程与 chatroom 的 protocol_recv_msg 一致，只有两点因串口而不同：
 *   1. 串口不是 socket，Linux 上对非 socket 调 recv 会返回 ENOTSOCK，所以用 read
 *   2. read 返回 0 表示"这次没读到"，不是对端关闭，按暂无数据处理
 */
int protocol_recv_msg(int fd, char *recv_buf, int *recv_len, uint8_t *type, char *msg, int msg_len)
{
    while (*recv_len < PROTOCOL_BUF_SIZE) {
        int n = (int)read(fd, recv_buf + *recv_len, (size_t)(PROTOCOL_BUF_SIZE - *recv_len));
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                return PROTOCOL_ERR_UNKNOWN;
            }
            break; /* 暂时读不到，先用缓冲里已有的数据试试 */
        }
        *recv_len += n;
        g_rx_bytes += (uint64_t)n;
    }

    if (*recv_len < PROTOCOL_HEADER_SIZE) {
        return PROTOCOL_ERR_WOULDBLOCK;
    }

    int body_len = (int)read_be32((const uint8_t *)recv_buf + 3);
    if (body_len > PROTOCOL_MAX_BODY_SIZE) {
        return PROTOCOL_ERR_MSGSIZE;
    }

    int total_len = PROTOCOL_HEADER_SIZE + body_len + PROTOCOL_TAIL_SIZE;
    if (*recv_len < total_len) {
        return PROTOCOL_ERR_WOULDBLOCK;
    }

    int ret = protocol_unpack((const uint8_t *)recv_buf, total_len, type, msg, msg_len);
    if (ret < 0) {
        return ret;
    }

    /* 这一帧消费掉，剩余数据前移，供下一次继续取帧（粘包） */
    *recv_len -= total_len;
    memmove(recv_buf, recv_buf + total_len, (size_t)*recv_len);
    return ret;
}