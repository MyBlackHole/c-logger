#ifndef LOGGER_EXPORT_H
#define LOGGER_EXPORT_H
/* Linux/ELF 公开 C ABI。静态消费方通过 Logger::logger 获得 LOGGER_STATIC_DEFINE；
 * 手工静态构建也可以自行定义。库内部编译的隐藏可见性与该标注相互独立。 */
#if defined(LOGGER_STATIC_DEFINE)
#define LOGGER_API
#elif defined(__GNUC__) || defined(__clang__)
#define LOGGER_API __attribute__((visibility("default")))
#else
#define LOGGER_API
#endif
#endif
