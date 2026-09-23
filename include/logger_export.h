#ifndef LOGGER_EXPORT_H
#define LOGGER_EXPORT_H
/* Linux/ELF public C ABI. Static consumers receive LOGGER_STATIC_DEFINE from
 * Logger::logger; manual static builds may define it themselves. Internal
 * library compilation uses hidden visibility independently of this annotation. */
#if defined(LOGGER_STATIC_DEFINE)
#define LOGGER_API
#elif defined(__GNUC__) || defined(__clang__)
#define LOGGER_API __attribute__((visibility("default")))
#else
#define LOGGER_API
#endif
#endif
