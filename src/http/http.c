#include "http.h"

// Pure static allocation. Resides in .bss segment, zero malloc used.
static uint8_t body[4096];

void httpShit(void) {   
    int32_t req = http((const uint8_t *)"GET", (const uint8_t *)"http://example.com/");
    
    if (req < 0) {
        rust_print((const uint8_t *)"Failed to create request\n");
        return;
    }

    int32_t resp = http_go(req);

    if (resp < 0) {
        rust_print((const uint8_t *)"Request failed to send\n");
        return;
    }

    int32_t status = http_response_status(resp);
    if (!http_response_ok(resp) || status != 200) {
        rust_print((const uint8_t *)"Request failed with status: ");
        print_num(status);
        rust_print((const uint8_t *)"\n");
        http_free_response(resp);
        return;
    }

    print_num(status);
    //rust_print((const uint8_t *)"\n");

    // Get the body. We pass sizeof(body) - 1 to leave room for the null terminator.
    int32_t got = http_response_body(resp, body, sizeof(body) - 1);
    
    if (got > 0) {
        // Static allocation method for strings: just strictly null-terminate.
        // This makes the previous 4000 bytes of old data completely invisible.
        body[got] = 0; 
        rust_print(body);
    } else {
        // Failsafe: ensure index 0 is null so nothing prints if got == 0
        body[0] = 0; 
        rust_print((const uint8_t *)"Empty body\n");
    }

    http_free_response(resp);
}