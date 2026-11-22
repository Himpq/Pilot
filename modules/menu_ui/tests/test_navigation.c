#include "../pilot_menu_ui.c"
#include <assert.h>
#include <stdlib.h>
#include <sys/mman.h>

static uint32_t test_group = PILOT_GROUP_ID;
static uint32_t test_row, test_item, test_index;
static unsigned test_focus_commits, test_enters, test_detail_factories;
static int test_patch_failure;
static uint8_t test_ctrl[0x1D000] __attribute__((aligned(16)));
static uintptr_t test_vtable[VIEW_VTABLE_SLOTS];
static uint8_t test_ok_node[0x38] __attribute__((aligned(16)));

static uint32_t *get_group(void) { return &test_group; }
static uint32_t *get_row(void) { return &test_row; }
static uint32_t *get_item(void) { return &test_item; }
static uint32_t get_index(void *cursor)
{
    assert(cursor == test_ctrl + APPFW_293_CURSOR_OBJ_OFF);
    return test_index;
}
static void focus_item(void *cursor, uint32_t item)
{
    assert(cursor == test_ctrl + APPFW_293_CURSOR_OBJ_OFF);
    assert(item >= PILOT_ITEM_ID_BASE && item < PILOT_ITEM_ID_BASE + 4u);
    test_index = 12u + item - PILOT_ITEM_ID_BASE;
    test_item = item;
    ++test_focus_commits;
}
static void native_enter(void *ctrl, const uint32_t *source)
{
    assert(ctrl == test_ctrl && source);
    ++test_enters;
    /* Reproduce native reactivation replacing the global item before view
     * creation; callers must restore through the cursor after this call. */
    test_item = PILOT_ITEM_ID_BASE;
    test_index = 12;
}

static void install_function(uintptr_t offset, void (*function)(void))
{
    uint8_t *destination = (uint8_t *)(g_appfw_base + offset);
    uintptr_t address = (uintptr_t)function;
#if defined(__x86_64__)
    destination[0] = 0x48; destination[1] = 0xB8; /* movabs rax, address */
    memcpy(destination + 2, &address, 8);
    destination[10] = 0xFF; destination[11] = 0xE0; /* jmp rax */
#else
#error Run this host harness with an x86_64 compiler.
#endif
}

static PilotStatus test_log(const char *message)
{
    puts(message);
    return PILOT_STATUS_OK;
}
static PilotStatus test_patch(uintptr_t address, const void *expected,
                              const void *replacement, size_t size)
{
    if (test_patch_failure)
        return PILOT_STATUS_CONFLICT;
    assert(memcmp((void *)address, expected, size) == 0);
    memcpy((void *)address, replacement, size);
    return PILOT_STATUS_OK;
}
static void *detail_factory(void *buf, void *a1, void *a2, void *a3, uint32_t a4)
{
    (void)buf; (void)a1; (void)a2; (void)a3; (void)a4;
    ++test_detail_factories;
    uintptr_t table = *(uintptr_t *)(g_appfw_base + APPFW_GOT_CTRL_ITEMS_1C3);
    assert(table == (uintptr_t)g_ctrl_items_1c3);
    assert(*(uintptr_t *)(table + 7u * APPFW_CTRL_ITEM_SIZE) ==
           *(uintptr_t *)g_ctrl_items_1c3_stock);
    return test_ctrl;
}

int main(void)
{
    void *mapping = mmap(NULL, 0x5000000, PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(mapping != MAP_FAILED);
    g_appfw_base = (uintptr_t)mapping;
    install_function(APPFW_GET_CUR_GROUP_OFF, (void (*)(void))get_group);
    install_function(APPFW_GET_SEL_ROW_OFF, (void (*)(void))get_row);
    install_function(APPFW_GET_CUR_ITEM_OFF, (void (*)(void))get_item);
    install_function(APPFW_CURSOR_GET_INDEX_OFF, (void (*)(void))get_index);
    install_function(APPFW_CURSOR_FOCUS_ITEM_OFF, (void (*)(void))focus_item);
    install_function(APPFW_293_ON_ENTER_OFF, (void (*)(void))native_enter);
    static const PilotLogApi log_api = {.print = test_log};
    static const PilotApi api = {.log = &log_api, .memory_patch = test_patch};
    g_api = &api;
    test_vtable[2] = g_appfw_base + APPFW_293_ON_ENTER_OFF;
    *(uintptr_t **)test_ctrl = test_vtable;
    assert(pilot_attach_list_lifecycle(test_ctrl));
    pilot_ctrl_enter_fn enter = (pilot_ctrl_enter_fn)(*(uintptr_t **)test_ctrl)[2];
    uint32_t source = PILOT_DETAIL_SCREEN_ID;

    /* Repeated returns through the SAME controller: no factory is called. */
    for (unsigned repeat = 0; repeat < 5; ++repeat) {
        test_row = test_item = PILOT_ITEM_ID_BASE + 2u;
        test_index = 14;
        pilot_save_list_return(test_ctrl, test_item);
        test_row = test_item = 8u; /* detail page changes shared parameters */
        enter(test_ctrl, &source);
        assert(test_row == PILOT_ITEM_ID_BASE + 2u);
        assert(test_item == PILOT_ITEM_ID_BASE + 2u);
        assert(test_index == 14u && !g_return_list_pending);
    }
    assert(test_focus_commits == 5u && test_enters == 5u);

    /* Recreated controller also receives the lifecycle hook. */
    pilot_save_list_return(test_ctrl, PILOT_ITEM_ID_BASE + 1u);
    *(uintptr_t **)test_ctrl = test_vtable;
    assert(pilot_attach_list_lifecycle(test_ctrl));
    enter(test_ctrl, &source);
    assert(test_item == PILOT_ITEM_ID_BASE + 1u && test_index == 13u);

    /* Foreign groups and ordinary list entry retain native behavior. */
    test_group = 0x3BC;
    enter(test_ctrl, &source);
    assert(test_focus_commits == 6u);
    test_group = PILOT_GROUP_ID;
    pilot_clear_list_return();
    source = 0x292;
    enter(test_ctrl, &source);
    assert(test_focus_commits == 6u && test_index == 12u);

    /* Validate the live native tree walker and reject silent patch failure. */
    *(uintptr_t *)g_layout_1c3_stock = 0x8344360u;
    *(uintptr_t *)(g_layout_1c3_stock + 7u * APPFW_LAYOUT_ITEM_SIZE) = 0x8343DB0u;
    *(uintptr_t *)(g_layout_1c3_stock + 12u * APPFW_LAYOUT_ITEM_SIZE) = 0x8343B90u;
    *(uintptr_t *)g_ctrl_items_1c3_stock = 0x8344360u;
    *(uintptr_t *)(g_ctrl_items_1c3_stock + 7u * APPFW_CTRL_ITEM_SIZE) = 0x8343DB0u;
    bind_detail_page_to_layout(&g_pilot_version_page);
    /* Display types survive hiding; the controller alone loses Update. */
    assert(*(uintptr_t *)(g_layout_1c3 + 7u * APPFW_LAYOUT_ITEM_SIZE) == 0x8343DB0u);
    assert(*(uintptr_t *)(g_layout_1c3 + 12u * APPFW_LAYOUT_ITEM_SIZE) == 0x8343B90u);
    assert(*(uintptr_t *)(g_ctrl_items_1c3 + 7u * APPFW_CTRL_ITEM_SIZE) == 0x8344360u);
    uint8_t *cursor = test_ctrl + APPFW_293_CURSOR_OBJ_OFF;
    *(uintptr_t *)(cursor + 0x18u) = (uintptr_t)test_ok_node;
    *(uint64_t *)(cursor + 0x30u) = 1;
    *(uint32_t *)(test_ok_node + 0x20u) = 8u;
    assert(pilot_detail_focus_mask(test_ctrl) == (1u << 8u));
    *(uint32_t *)(test_ok_node + 0x20u) = 7u;
    assert(pilot_detail_focus_mask(test_ctrl) == (1u << 7u));
    *(uint32_t *)(test_ok_node + 0x20u) = 8u;
    g_orig_ctrl_factory_1c3 = detail_factory;
    g_orig_ctrl_items_1c3 = 0x12345678u;
    *(uintptr_t *)(g_appfw_base + APPFW_GOT_CTRL_ITEMS_1C3) = g_orig_ctrl_items_1c3;
    test_item = PILOT_ITEM_ID_BASE;
    assert(pilot_1c3_ctrl_factory_hook(NULL, NULL, NULL, NULL, 0) == test_ctrl);
    assert(test_detail_factories == 1u);
    assert(*(uintptr_t *)(g_appfw_base + APPFW_GOT_CTRL_ITEMS_1C3) ==
           g_orig_ctrl_items_1c3);
    test_patch_failure = 1;
    assert(pilot_1c3_ctrl_factory_hook(NULL, NULL, NULL, NULL, 0) == NULL);
    assert(test_detail_factories == 1u);
    munmap(mapping, 0x5000000);
    puts("PASS navigation lifecycle, repeated returns, native pass-through, focus diagnostics, patch errors");
    return 0;
}
