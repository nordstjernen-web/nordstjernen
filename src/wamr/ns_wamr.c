/* Nordstjernen — narrow accessors into WAMR internals for the WebAssembly JS API. */

#include "ns_wamr.h"

#include "wasm_runtime_common.h"
#include "../interpreter/wasm_runtime.h"

static WASMTableInstance *
ns_wamr_find_export_table(wasm_module_inst_t inst, const char *export_name,
                          uint32 *out_table_idx)
{
    WASMModuleInstance *module_inst = (WASMModuleInstance *)inst;
    WASMModule *module;
    uint32 i;

    if (!module_inst || module_inst->module_type != Wasm_Module_Bytecode)
        return NULL;
    if (!export_name)
        return NULL;

    module = module_inst->module;
    for (i = 0; i < module->export_count; i++) {
        WASMExport *export = module->exports + i;
        if (export->kind == EXPORT_KIND_TABLE
            && !strcmp(export->name, export_name)) {
            if (out_table_idx)
                *out_table_idx = export->index;
            return wasm_get_table_inst(module_inst, export->index);
        }
    }
    return NULL;
}

bool
ns_wamr_table_size(wasm_module_inst_t inst, const char *export_name,
                   uint32_t *out_size, uint32_t *out_max)
{
    WASMTableInstance *table = ns_wamr_find_export_table(inst, export_name,
                                                         NULL);
    if (!table)
        return false;
    if (out_size)
        *out_size = table->cur_size;
    if (out_max)
        *out_max = table->max_size;
    return true;
}

bool
ns_wamr_table_grow(wasm_module_inst_t inst, const char *export_name,
                   uint32_t delta, uint32_t *out_old_size)
{
    uint32 table_idx = 0;
    WASMTableInstance *table = ns_wamr_find_export_table(inst, export_name,
                                                         &table_idx);
    if (!table)
        return false;
    if (out_old_size)
        *out_old_size = table->cur_size;
    return wasm_enlarge_table((WASMModuleInstance *)inst, table_idx, delta,
                              NULL_REF);
}

bool
ns_wamr_table_get_ref(wasm_module_inst_t inst, const char *export_name,
                      uint32_t idx, uint32_t *out_ref)
{
    WASMTableInstance *table = ns_wamr_find_export_table(inst, export_name,
                                                         NULL);
    if (!table || idx >= table->cur_size)
        return false;
    *out_ref = (uint32)table->elems[idx];
    return true;
}

bool
ns_wamr_table_set_ref(wasm_module_inst_t inst, const char *export_name,
                      uint32_t idx, uint32_t ref)
{
    WASMTableInstance *table = ns_wamr_find_export_table(inst, export_name,
                                                         NULL);
    if (!table || idx >= table->cur_size)
        return false;
    table->elems[idx] = ref;
    return true;
}

void
ns_wamr_externref_reclaim(wasm_module_inst_t inst)
{
    if (inst)
        wasm_externref_reclaim(inst);
}

extern bool ns_wamr_host_calls_ctors;

void
ns_wamr_set_host_calls_ctors(bool enabled)
{
    ns_wamr_host_calls_ctors = enabled;
}

void
wasm_trap_delete(void *trap)
{
    (void)trap;
}
