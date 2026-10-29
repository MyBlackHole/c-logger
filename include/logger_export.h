#ifndef LOGGER_EXPORT_H
#define LOGGER_EXPORT_H
/*
 * Linux/ELF 公开 C ABI。静态链接使用方从 Logger::logger 获取
 * LOGGER_STATIC_DEFINE；手动静态构建可自行定义。
 * 库内部编译独立于此标注使用隐藏可见性。
 */
#if defined(LOGGER_STATIC_DEFINE)
#define LOGGER_API
#elif defined(__GNUC__) || defined(__clang__)
#define LOGGER_API __attribute__((visibility("default")))
#else
#define LOGGER_API
#endif
#endif
