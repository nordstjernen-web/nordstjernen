/* Nordstjernen — narrow accessors into WAMR internals for the WebAssembly JS API. */

#ifndef NS_WAMR_H
#define NS_WAMR_H

#include <stdbool.h>
#include <stdint.h>

#include "wasm_export.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NS_WAMR_NULL_REF 0xFFFFFFFFu

bool ns_wamr_table_size(wasm_module_inst_t inst, const char *export_name,
                        uint32_t *out_size, uint32_t *out_max);
bool ns_wamr_table_grow(wasm_module_inst_t inst, const char *export_name,
                        uint32_t delta, uint32_t *out_old_size);
bool ns_wamr_table_get_ref(wasm_module_inst_t inst, const char *export_name,
                           uint32_t idx, uint32_t *out_ref);
bool ns_wamr_table_set_ref(wasm_module_inst_t inst, const char *export_name,
                           uint32_t idx, uint32_t ref);
void ns_wamr_externref_reclaim(wasm_module_inst_t inst);

void ns_wamr_set_host_calls_ctors(bool enabled);

void wasm_runtime_set_enlarge_mem_success_callback(
    void (*callback)(wasm_module_inst_t module_inst, void *user_data),
    void *user_data);

#ifdef __cplusplus
}
#endif

#endif
