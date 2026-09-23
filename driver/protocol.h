#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

/* 应用层协议（从 chatroom 项目移植，串口上同样适用）
 * 帧 = 7 字节头 + 正文 + 3 字节尾
 *   头：A5 A5 | 类型 | 4 字节网络序正文长度
 *   尾：1 字节校验（正文逐字节异或）| A5 A5
 */
#define PROTOCOL_MAGIC         0xA5
#define PROTOCOL_HEADER_SIZE   7
#define PROTOCOL_MAX_BODY_SIZE 1024
#define PROTOCOL_TAIL_SIZE     3
#define PROTOCOL_BUF_SIZE      (PROTOCOL_HEADER_SIZE + PROTOCOL_MAX_BODY_SIZE + PROTOCOL_TAIL_SIZE)

/* 消息类型 */
#define MSG_TYPE_COUNT 5
#define MSG_TEXT      0x01
#define MSG_HEARTBEAT 0x02
#define MSG_QUIT      0x03
#define MSG_NICKNAME  0x04
#define MSG_SYSTEM    0x05

/* 错误码 */
#define PROTOCOL_ERR_INVALID    -2
#define PROTOCOL_ERR_MSGSIZE    -3
#define PROTOCOL_ERR_TYPE       -4
#define PROTOCOL_ERR_UNKNOWN    -5
#define PROTOCOL_ERR_MAGIC      -6
#define PROTOCOL_ERR_CHECKSUM   -7
#define PROTOCOL_ERR_WOULDBLOCK -10 /* 暂无完整帧，等待更多数据 */

/* 函数声明 */
int protocol_unpack(const uint8_t *buf, int buf_len, uint8_t *type, char *msg, int msg_len);
int protocol_recv_msg(int fd, char *recv_buf, int *recv_len, uint8_t *type, char *msg, int msg_len);
uint64_t protocol_rx_bytes(void);

#endif