#ifndef AUDIT_RECORD_H
#define AUDIT_RECORD_H

#include "audit_internal.h"
#include <stdio.h>

/* 这是既有磁盘格式，不是新 schema：载荷不包含 Logger 前缀/LF。
 * 写入器和两个读取器都使用相同边界；禁止无界 getline()。 */
#define AUDIT_PAYLOAD_MAX 4095u
#define AUDIT_RECORD_MAX 4608u
#define AUDIT_CHECKPOINT_MAX 511u

typedef struct {
	const char *payload; /* 输入数据的借用切片。 */
	size_t canonical_len; /* 截止 prev=，不包含 " hash="。 */
	uint64_t seq;
	uint64_t txn;
	char instance[33];
	unsigned char previous[32];
	unsigned char hash[32];
} audit_record_view_t;

enum audit_line_result {
	AUDIT_LINE_EOF = 0,
	AUDIT_LINE_COMPLETE = 1,
	AUDIT_LINE_PARTIAL = 2
};
/* 最多读取 limit+1 字节，长度包含 LF。PARTIAL 结果只能发生在真正 EOF 之后。
 * 超长、内嵌 NUL 和流错误返回 -errno。
 * buffer 至少必须有 limit+1 字节（额外 1 字节用于便利性的 NUL）。 */
int audit_line_read(FILE *, char *buffer, size_t limit, size_t *length);
/* 严格的规范 KV 语法；只有成功时才修改输出。 */
int audit_record_parse_payload(const char *, size_t, int chained,
			       audit_record_view_t *);
int audit_record_parse_line(const char *, size_t, audit_record_view_t *);
int audit_record_verify(const audit_record_view_t *,
			const unsigned char previous[32],
			const audit_digest_ops_t *, unsigned char next[32]);
/* 编码未参与哈希的载荷。所有字符串都使用引号/转义，不猜测其中是否包含敏感内容。
 * 普通字符串及已有转义继续保持原有字节表示。 */
int audit_payload_encode(const char instance[33], uint64_t seq,
			 const audit_event_t *, char *, size_t, size_t *used);
int audit_hex_parse(const char *, size_t bytes, unsigned char *);
int audit_checkpoint_parse(const char *, size_t, audit_ckpt_t *);
#endif
