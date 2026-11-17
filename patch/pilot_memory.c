#define _GNU_SOURCE
#include "pilot_internal.h"

#include <link.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

struct appfw_range_query {
    uintptr_t address;
    uintptr_t end_address;
    uintptr_t segment_start;
    uintptr_t segment_end;
    int protection;
    int found;
};

static pthread_mutex_t memory_patch_lock = PTHREAD_MUTEX_INITIALIZER;

static int appfw_range_callback(struct dl_phdr_info *info,
                               size_t info_size,
                               void *opaque)
{
    (void)info_size;
    struct appfw_range_query *query = opaque;
    if (!info->dlpi_name || !strstr(info->dlpi_name, "appFw.so")) return 0;

    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr) *phdr = &info->dlpi_phdr[i];
        if (phdr->p_type != PT_LOAD || !(phdr->p_flags & PF_R)) continue;
        uintptr_t start = (uintptr_t)info->dlpi_addr + (uintptr_t)phdr->p_vaddr;
        uintptr_t end = start + (uintptr_t)phdr->p_memsz;
        if (end < start || query->address < start || query->end_address > end)
            continue;

        query->segment_start = start;
        query->segment_end = end;
        query->protection = PROT_READ;
        if (phdr->p_flags & PF_W) query->protection |= PROT_WRITE;
        if (phdr->p_flags & PF_X) query->protection |= PROT_EXEC;
        query->found = 1;
        return 1;
    }
    return 0;
}

static int locate_appfw_range(uintptr_t address,
                              size_t length,
                              struct appfw_range_query *query)
{
    if (!address || !length || length > 4096 || address > UINTPTR_MAX - length)
        return 0;
    memset(query, 0, sizeof(*query));
    query->address = address;
    query->end_address = address + length;
    (void)dl_iterate_phdr(appfw_range_callback, query);
    return query->found;
}

PilotStatus pilot_memory_read_impl(uintptr_t address,
                                   void *output,
                                   size_t length)
{
    if (!output || !length) return PILOT_STATUS_INVALID_ARGUMENT;
    if (!__atomic_load_n(&g_pilot.optional_features_enabled, __ATOMIC_ACQUIRE))
        return PILOT_STATUS_DISABLED;

    struct appfw_range_query query;
    if (!locate_appfw_range(address, length, &query)) return PILOT_STATUS_RANGE;
    memcpy(output, (const void *)address, length);
    return PILOT_STATUS_OK;
}

PilotStatus pilot_memory_patch_impl(uintptr_t address,
                                    const void *expected,
                                    const void *replacement,
                                    size_t length)
{
    if (!expected || !replacement || !length || length > 64)
        return PILOT_STATUS_INVALID_ARGUMENT;
    if (!__atomic_load_n(&g_pilot.optional_features_enabled, __ATOMIC_ACQUIRE))
        return PILOT_STATUS_DISABLED;

    pthread_mutex_lock(&memory_patch_lock);
    struct appfw_range_query query;
    if (!locate_appfw_range(address, length, &query)) {
        pthread_mutex_unlock(&memory_patch_lock);
        return PILOT_STATUS_RANGE;
    }
    if (memcmp((const void *)address, expected, length) != 0) {
        pthread_mutex_unlock(&memory_patch_lock);
        return PILOT_STATUS_CONFLICT;
    }

    long page_size_value = sysconf(_SC_PAGESIZE);
    if (page_size_value <= 0) {
        pthread_mutex_unlock(&memory_patch_lock);
        return PILOT_STATUS_SYSTEM;
    }
    uintptr_t page_size = (uintptr_t)page_size_value;
    if (query.end_address > UINTPTR_MAX - (page_size - 1u)) {
        pthread_mutex_unlock(&memory_patch_lock);
        return PILOT_STATUS_RANGE;
    }
    uintptr_t page_start = address & ~(page_size - 1u);
    uintptr_t page_end = (query.end_address + page_size - 1u) & ~(page_size - 1u);
    if (page_end < query.end_address || page_start < query.segment_start ||
        page_end > query.segment_end) {
        pthread_mutex_unlock(&memory_patch_lock);
        return PILOT_STATUS_RANGE;
    }

    size_t page_length = (size_t)(page_end - page_start);
    if (mprotect((void *)page_start, page_length,
                 query.protection | PROT_WRITE) != 0) {
        pthread_mutex_unlock(&memory_patch_lock);
        return PILOT_STATUS_SYSTEM;
    }

    memcpy((void *)address, replacement, length);
    __builtin___clear_cache((char *)address, (char *)query.end_address);
    int restore_result = mprotect((void *)page_start, page_length,
                                  query.protection);
    pthread_mutex_unlock(&memory_patch_lock);
    return restore_result == 0 ? PILOT_STATUS_OK : PILOT_STATUS_SYSTEM;
}
