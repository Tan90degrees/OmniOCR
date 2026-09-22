#ifndef OMNIOCR_PLUGIN_ABI_H
#define OMNIOCR_PLUGIN_ABI_H
/* Versioned C ABI: no C++ standard-library types, exceptions or ownership
 * cross this boundary. Host and plugin must agree on the ABI version. */
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define OMNIOCR_PLUGIN_ABI_V1 1u
typedef struct omniocr_plugin_api_v1 {
    uint32_t abi_version;
    uint32_t struct_size;
    const char* plugin_id; /* stable, nonempty identifier */
    const char* kind;      /* backend, layout, recognition */
    /* Returns an opaque instance. NULL indicates initialization failure. */
    void* (*create)(const char* json, size_t length, size_t instance_index);
    /* Plugin owns output/error buffers until host calls release. Return 0 on
     * success; on failure place a diagnostic in error buffer, if available. */
    int (*execute)(void* instance, const char* request, size_t request_length,
                   char** output, size_t* output_length,
                   char** error, size_t* error_length);
    void (*release)(void* instance, char* buffer);
    void (*destroy)(void* instance);
} omniocr_plugin_api_v1;
/* Required dlopen symbol; returned descriptor remains valid until dlclose. */
typedef const omniocr_plugin_api_v1* (*omniocr_plugin_entry_v1_fn)(void);
#ifdef __cplusplus
}
#endif
#endif
