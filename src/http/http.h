#ifndef HTTP_H
#define HTTP_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

int32_t http_new(void);
int32_t http_set_method(int32_t handle, const uint8_t* method);
int32_t http_set_url(int32_t handle, const uint8_t* url);
int32_t http_set_header(int32_t handle, const uint8_t* name, const uint8_t* value);
int32_t http_set_body(int32_t handle, const uint8_t* data, uint32_t len);
void    http_free(int32_t handle);
int32_t http_send(int32_t handle);

bool    http_response_ok(int32_t handle);
int32_t http_response_status(int32_t handle);
int32_t http_response_header(int32_t handle, const uint8_t* name, uint8_t* out, uint32_t out_size);
int32_t http_response_body_len(int32_t handle);
int32_t http_response_body(int32_t handle, uint8_t* out, uint32_t out_size);
void    http_free_response(int32_t handle);

/* Chainable builder API */
int32_t http(const uint8_t* method, const uint8_t* url);
int32_t http_hdr(int32_t handle, const uint8_t* name, const uint8_t* value);
int32_t http_body(int32_t handle, const uint8_t* data, uint32_t len);
int32_t http_go(int32_t handle);
extern void rust_print(const uint8_t *s);
extern void print_num(int32_t n);
void httpShit(void);
#ifdef __cplusplus
}
#endif

#endif /* HTTP_H */