/* qjs_port.h - glue between the QuickJS compat libc and its embedder. */
#ifndef QJS_PORT_H
#define QJS_PORT_H

/* Where printf/fputs/console output goes. NULL restores the terminal. */
typedef void (*QjsSink)(const char *s, int len);
void qjs_set_sink(QjsSink sink);
void qjs_write(const char *s, int len);

#endif
