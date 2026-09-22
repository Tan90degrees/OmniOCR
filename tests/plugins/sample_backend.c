#include "omniocr/plugin_abi.h"
#include <stdlib.h>
#include <string.h>
static void* create(const char* json,size_t length,size_t index) {
    (void)json; (void)length; (void)index;
    return malloc(1);
}
static int execute(void* instance,const char* request,size_t size,
                   char** output,size_t* output_length,char** error,size_t* error_length) {
    (void)instance; (void)request; (void)size;
    (void)error; (void)error_length;
    static const char response[]="{\"text\":\"EXTERNAL_OK\"}";
    *output=malloc(sizeof(response)-1);
    if (!*output) return -1;
    memcpy(*output,response,sizeof(response)-1);
    *output_length=sizeof(response)-1;
    return 0;
}
static void release(void* instance,char* ptr) { (void)instance; free(ptr); }
static void destroy(void* instance) { free(instance); }
static const omniocr_plugin_api_v1 api={
    OMNIOCR_PLUGIN_ABI_V1,sizeof(omniocr_plugin_api_v1),
    "example.external","backend",create,execute,release,destroy
};
const omniocr_plugin_api_v1* omniocr_plugin_entry_v1(void) { return &api; }
