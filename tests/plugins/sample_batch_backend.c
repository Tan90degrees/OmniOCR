#include "omniocr/plugin_abi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void* create(const char* config, size_t length, size_t index) {
    (void)config; (void)length; (void)index;
    return malloc(1);
}
static int output_text(const char* content, char** output, size_t* length) {
    *length = strlen(content);
    *output = malloc(*length);
    if (!*output) return -1;
    memcpy(*output, content, *length);
    return 0;
}
static int execute(void* instance, const char* request, size_t size,
                   char** output, size_t* length, char** error, size_t* error_length) {
    (void)instance; (void)request; (void)size; (void)error; (void)error_length;
    return output_text("{\"text\":\"SINGLE_UNEXPECTED\"}", output, length);
}
static int execute_batch(void* instance, const char* request, size_t size,
                         char** output, size_t* length, char** error, size_t* error_length) {
    (void)instance; (void)error; (void)error_length;
    /* Test fixture counts request images; real plugins parse JSON and validate
     * each item. This intentionally requires at least one batch entry. */
    size_t count = 0;
    for (size_t i = 0; i < size; ++i)
        if (size - i >= 7 && memcmp(request + i, "\"image\"", 7) == 0) ++count;
    if (!count || count > 128) return -1;
    const char* item = "{\"text\":\"EXTERNAL_BATCH\"}";
    const size_t capacity = count * (strlen(item) + 1) + 32;
    char* result = malloc(capacity);
    if (!result) return -1;
    size_t used = (size_t)snprintf(result, capacity, "{\"results\":[");
    for (size_t i = 0; i < count; ++i)
        used += (size_t)snprintf(result + used, capacity - used, "%s%s", i ? "," : "", item);
    used += (size_t)snprintf(result + used, capacity - used, "]}");
    *output = result; *length = used;
    return 0;
}
static void release(void* instance, char* buffer) { (void)instance; free(buffer); }
static void destroy(void* instance) { free(instance); }
static const omniocr_plugin_api_v2 api = {
    {OMNIOCR_PLUGIN_ABI_V2, sizeof(omniocr_plugin_api_v2),
     "example.batch", "backend", create, execute, release, destroy},
    execute_batch
};
const omniocr_plugin_api_v2* omniocr_plugin_entry_v2(void) { return &api; }
