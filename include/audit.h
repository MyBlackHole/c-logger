#ifndef AUDIT_H
#define AUDIT_H
#include "logger_export.h"
#include "logger.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { AUDIT_SUCCESS = 0, AUDIT_FAILURE = 1 } audit_result_t;
typedef enum { AUDIT_PHASE_ATTEMPT = 0, AUDIT_PHASE_RESULT = 1 } audit_phase_t;
typedef enum {
	AUDIT_FAIL_REPORT = 0,
	AUDIT_FAIL_DENY = 1
} audit_failure_policy_t;
/* SHA-256 是唯一摘要算法。NONE 表示显式关闭完整性，并不是另一种摘要算法。
 * 已退役的数值 ID 2 永久保留并直接拒绝，绝不会重新映射。
 * 仍在源码中使用已移除算法的调用方必须更新配置；已有链绝不能原地重新计算摘要。 */
typedef enum {
	AUDIT_INTEGRITY_NONE = 0,
	AUDIT_INTEGRITY_SHA256 = 1
} audit_integrity_t;
#define AUDIT_DETAIL_REDACTED "[REDACTED]"
/* Audit 字段只允许承载非敏感标识符/元数据。绝不能把密码、明文密钥、私钥、访问秘密值，
 * 或 bearer/session token 放入 actor/source/resource/operation/detail。 */
typedef struct {
	audit_phase_t phase;
	uint64_t transaction_id;
	const char *event;
	const char *actor;
	const char *source;
	const char *resource;
	const char *operation;
	audit_result_t result;
	int error_code;
	const char *detail;
} audit_event_t;
#define AUDIT_CONFIG_VERSION 1u
typedef struct {
	uint32_t struct_size;
	uint32_t version;
	const char *log_dir;
	const char *name; /* xxx 对应 xxx.audit.log。 */
	const char *chain_state_path; /* NULL 表示 <log_dir>/<name>.audit.state。 */
	logger_rotation_config_t rotation;
	audit_failure_policy_t failure_policy;
	audit_integrity_t integrity;
	int fsync_each_record;
} audit_config_t;
#ifdef __cplusplus
static inline audit_config_t audit_defaults_cpp(void)
{
	audit_config_t c = {};
	c.struct_size = sizeof(c);
	c.version = AUDIT_CONFIG_VERSION;
	c.log_dir = "/var/log";
	c.name = "app";
	c.rotation.mode = LOGGER_ROTATE_SIZE_DAILY;
	c.rotation.max_file_size = 100u * 1024u * 1024u;
	c.rotation.retention_days = 90;
	c.failure_policy = AUDIT_FAIL_REPORT;
	c.integrity = AUDIT_INTEGRITY_SHA256;
	c.fsync_each_record = 1;
	return c;
}
#define AUDIT_DEFAULT_CONFIG() audit_defaults_cpp()
#else
#define AUDIT_DEFAULT_CONFIG()                                       \
	((audit_config_t){                                           \
		.struct_size = sizeof(audit_config_t),               \
		.version = AUDIT_CONFIG_VERSION,                     \
		.log_dir = "/var/log",                               \
		.name = "app",                                       \
		.rotation = { .mode = LOGGER_ROTATE_SIZE_DAILY,      \
			      .max_file_size = 100u * 1024u * 1024u, \
			      .retention_days = 90 },                \
		.failure_policy = AUDIT_FAIL_REPORT,                 \
		.integrity = AUDIT_INTEGRITY_SHA256,                 \
		.fsync_each_record = 1 })
#endif
/* audit_init/audit_write/audit_begin/audit_end/audit_flush 使用 POSIX 风格：
 * 成功返回 0，失败返回 -1 并设置 errno。Audit 是进程全局的单写者子系统；
 * 不得让两个进程针对同一条链目标同时初始化。 */
LOGGER_API int audit_init(const audit_config_t *);
/* 无论结果如何都会释放本地运行时。如果 STOP、对应 checkpoint，或者更早出现结果不确定的
 * log/crypto 操作失败，则返回 -1/errno。void 版本只是兼容包装接口。 */
LOGGER_API int audit_shutdown_status(void);
LOGGER_API void audit_shutdown(void);

typedef enum {
	AUDIT_STATE_IDLE = 0,
	AUDIT_STATE_STARTING,
	AUDIT_STATE_RUNNING,
	AUDIT_STATE_CHECKPOINT_FAILED,
	AUDIT_STATE_IO_FAILED,
	AUDIT_STATE_STOPPING,
	AUDIT_STATE_FORKED,
	AUDIT_STATE_CRYPTO_FAILED /* 追加在末尾：此前 enum 数值保持不变。 */
} audit_state_t;
/* 这里只是快照，不是事务回执。STARTING/STOPPING/FORKED 不暴露会话计数器。
 * IDLE 状态会保留最近一次关闭/初始化失败信息。
 * committed_seq 表示已经确认的严格日志提交，**不**代表 checkpoint 成功。
 * IO_FAILED 操作仍可能已经写出/持久化部分字节。
 * CRYPTO_FAILED 发生在对应记录输出之前，因此不会推进该记录的 seq。 */
typedef struct {
	audit_state_t state;
	int error_code;
	int checkpoint_dirty;
	uint64_t committed_seq;
	uint64_t checkpoint_seq;
	char instance_id[33];
} audit_status_t;
LOGGER_API int audit_get_status(audit_status_t *);
/* 仅做失效标记。**不会**重置继承锁、销毁运行时或写出 STOP。
 * fork 子进程在 exec 前重新初始化会返回 ECHILD。优先使用“任何库运行时使用之前的单线程 fork”
 * 或 fork+exec。进程防护与 Logger/Console 共享；见 API.md。 */
/* 使用 logger_fork_reinit 时，进入前必须先关闭 Audit；函数返回后可以正常重新初始化。
 * 在这条干净路径上不要调用本失效辅助接口。 */
LOGGER_API void audit_after_fork_child(void);
LOGGER_API int audit_write(const audit_event_t *);
LOGGER_API int audit_begin(audit_event_t *event);
LOGGER_API int audit_end(audit_event_t *event, audit_result_t result,
			 int error_code);
/* 修复待处理 checkpoint，但不会重复写入已经提交的日志记录。
 * flush 不会清除 IO_FAILED / CRYPTO_FAILED；应先修复原因，再关闭并重新初始化。
 * IO_FAILED 还需要额外执行数据协调。 */
LOGGER_API int audit_flush(void);
LOGGER_API audit_failure_policy_t audit_failure_policy(void);
/* 返回线程本地快照（出错时为空），同一线程下一次调用会覆盖。
 * 优先使用能够报告错误的复制 API。 */
LOGGER_API const char *audit_instance_id(void);
LOGGER_API int audit_instance_id_copy(char out[33]);
/* 摘要引擎失败和输入损坏都会使验证返回 -1/errno。
 * 不允许全零摘要替代，也不存在后端回退。见 API.md。 */
/* 严格使用当前 KV 记录格式，并要求最终 LF。验证器不执行修复。
 * 返回 0 / -1 + errno；格式非法/链损坏返回 EBADMSG，记录过大返回 EOVERFLOW。
 * 历史展示前缀**不**在密码学覆盖范围内。任何失败都会让 *_from() 保持 final_hash_hex 不变。 */
LOGGER_API int audit_verify_file(const char *path);
/* 兼容入口：只接受 SHA256；NONE 和不支持的 ID 返回 EPROTONOSUPPORT。
 * audit_verify_file() 是默认 SHA-256 API。 */
LOGGER_API int audit_verify_file_with(const char *path,
				      audit_integrity_t algorithm);
/* 兼容/诊断访问接口：固定返回静态字符串 "builtin"。 */
LOGGER_API const char *audit_crypto_backend(void);
LOGGER_API int audit_verify_file_from(const char *path,
				      const char *expected_prev_hex,
				      char final_hash_hex[65]);
#ifdef __cplusplus
}
#endif
#endif
