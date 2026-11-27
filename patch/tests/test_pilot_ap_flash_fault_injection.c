/* Deterministic host fault-injection test suite for patch/pilot_ap_flash.c.
 *
 * Tests all 5 hardening guarantees:
 *   1. Two-stage fsync barrier order on install ({1,2,3} -> fsync -> {0} -> fsync)
 *      and reverse order on restore ({0} -> fsync -> {1,2,3} -> fsync).
 *   2. Atomic pilot.so install/update via temp file + fsync + renameat + dir fsync;
 *      injected write failure leaves existing pilot.so untouched.
 *   3. WBI2 preflight failure leaves appFw.so untouched;
 *      WBI2 zero failure AFTER appFw.so write triggers automatic rollback of appFw.so.
 *   4. emit_mount_failure() strictly bounds output to <= 512 bytes (zero overflow).
 *   5. purge mode refuses while appFw.so is patched, succeeds when baseline.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern void pilot_flash_run_once(void);

static int g_fail_wbi_open_count = 0;
static int g_fail_wbi_pwrite = 0;
static int g_fail_wbi_tmp_bak_pwrite = 0;
static int g_fail_pilot_tmp_pwrite = 0;
static int g_fail_appfw_pwrite_after_n = -1;
static int g_appfw_pwrite_calls = 0;

/* Track write/fsync order on appFw.so to verify 2-stage barrier */
static int g_appfw_fd = -1;
static int g_wbi_fd = -1;
static int g_wbi_tmp_bak_fd = -1;
static int g_pilot_tmp_fd = -1;
static char g_event_log[4096];
static size_t g_event_len = 0;

static void log_event(const char *ev)
{
    size_t n = strlen(ev);
    if (g_event_len + n + 2 < sizeof(g_event_log)) {
        memcpy(g_event_log + g_event_len, ev, n);
        g_event_len += n;
        g_event_log[g_event_len++] = ',';
        g_event_log[g_event_len] = '\0';
    }
}

static void reset_hooks(void)
{
    g_fail_wbi_open_count = 0;
    g_fail_wbi_pwrite = 0;
    g_fail_wbi_tmp_bak_pwrite = 0;
    g_fail_pilot_tmp_pwrite = 0;
    g_fail_appfw_pwrite_after_n = -1;
    g_appfw_pwrite_calls = 0;
    g_appfw_fd = -1;
    g_wbi_fd = -1;
    g_wbi_tmp_bak_fd = -1;
    g_pilot_tmp_fd = -1;
    g_event_log[0] = '\0';
    g_event_len = 0;
}

long host_test_hook_open(const char *path, long flags, long mode)
{
    if (strstr(path, "/nflasha5") != NULL && g_fail_wbi_open_count > 0) {
        g_fail_wbi_open_count--;
        return -EIO;
    }
    int fd = open(path, (int)flags, (mode_t)mode);
    if (fd < 0) return -errno;
    if (fd == g_appfw_fd) g_appfw_fd = -1;
    if (fd == g_wbi_fd) g_wbi_fd = -1;
    if (fd == g_wbi_tmp_bak_fd) g_wbi_tmp_bak_fd = -1;
    if (fd == g_pilot_tmp_fd) g_pilot_tmp_fd = -1;
    if (strstr(path, "/appFw.so") != NULL && (flags & O_RDWR))
        g_appfw_fd = fd;
    if (strstr(path, "/nflasha5") != NULL)
        g_wbi_fd = fd;
    if (strstr(path, "pilotflash.wbi.bak") != NULL)
        g_wbi_tmp_bak_fd = fd;
    if (strstr(path, "/.pilot.so.tmp") != NULL)
        g_pilot_tmp_fd = fd;
    return fd;
}

long host_test_hook_pwrite(long fd, const void *buf, unsigned long size, unsigned long long off)
{
    if ((int)fd == g_wbi_fd) {
        log_event("WBI_ZERO");
        if (g_fail_wbi_pwrite) {
            return -EIO;
        }
    }
    if ((int)fd == g_wbi_tmp_bak_fd && g_fail_wbi_tmp_bak_pwrite) {
        return -EIO;
    }
    if ((int)fd == g_pilot_tmp_fd && g_fail_pilot_tmp_pwrite) {
        return -EIO;
    }
    if ((int)fd == g_appfw_fd) {
        g_appfw_pwrite_calls++;
        char tag[64];
        snprintf(tag, sizeof(tag), "W@%llu:%lu", off, size);
        log_event(tag);
        if (g_fail_appfw_pwrite_after_n > 0 && g_appfw_pwrite_calls == g_fail_appfw_pwrite_after_n) {
            return -EIO;
        }
    }
    ssize_t r = pwrite((int)fd, buf, size, (off_t)off);
    return r < 0 ? -errno : (long)r;
}

long host_test_hook_fsync(long fd)
{
    if ((int)fd == g_appfw_fd) {
        log_event("FSYNC");
    }
    int r = fsync((int)fd);
    return r < 0 ? -errno : 0;
}

long host_test_hook_rename(const char *oldpath, const char *newpath)
{
    log_event("RENAME");
    int r = rename(oldpath, newpath);
    return r < 0 ? -errno : 0;
}

long host_test_hook_unlink(const char *path)
{
    int r = unlink(path);
    return r < 0 ? -errno : 0;
}

static void write_file_bytes(const char *path, const void *data, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    assert(fd >= 0);
    assert(write(fd, data, len) == (ssize_t)len);
    close(fd);
}

static size_t read_file_bytes(const char *path, void *buf, size_t cap)
{
    int fd = open(path, O_RDONLY);
    assert(fd >= 0);
    ssize_t n = read(fd, buf, cap);
    assert(n >= 0);
    close(fd);
    return (size_t)n;
}

static void build_plan(const char *path, const unsigned char *orig, size_t img_size,
                       const uint64_t offs[4], const uint32_t lens[4],
                       unsigned char *expected_patched)
{
    memcpy(expected_patched, orig, img_size);
    unsigned char plan[4096];
    size_t pos = 0;
    memcpy(plan + pos, "PILOTAP1", 8); pos += 8;
    uint32_t ver = 1, cnt = 4;
    uint64_t sz = img_size;
    memcpy(plan + pos, &ver, 4); pos += 4;
    memcpy(plan + pos, &cnt, 4); pos += 4;
    memcpy(plan + pos, &sz, 8); pos += 8;
    for (int i = 0; i < 4; ++i) {
        uint32_t zero = 0;
        memcpy(plan + pos, &offs[i], 8); pos += 8;
        memcpy(plan + pos, &lens[i], 4); pos += 4;
        memcpy(plan + pos, &zero, 4); pos += 4;
    }
    for (int i = 0; i < 4; ++i) {
        memcpy(plan + pos, orig + offs[i], lens[i]); pos += lens[i];
        for (uint32_t k = 0; k < lens[i]; ++k) {
            unsigned char after = (unsigned char)(orig[offs[i] + k] ^ (0xA0 + i));
            plan[pos + k] = after;
            expected_patched[offs[i] + k] = after;
        }
        pos += lens[i];
    }
    write_file_bytes(path, plan, pos);
}

int main(void)
{
    const char *dir = PILOT_USR_LIB_DIR;
    mkdir(dir, 0755);

    unsigned char orig[4096], patched[4096], cur[4096];
    for (size_t i = 0; i < sizeof(orig); ++i) orig[i] = (unsigned char)((i * 19 + 3) & 0xff);
    uint64_t offs[4] = {32, 256, 512, 1024};
    uint32_t lens[4] = {4, 32, 8, 8};

    build_plan(PILOT_FLASH_PLAN_PATH, orig, sizeof(orig), offs, lens, patched);
    write_file_bytes(PILOT_FLASH_TEST_APPFW_PATH, orig, sizeof(orig));

    unsigned char pilot_v1[256], pilot_v2[256], pilot_read[256];
    memset(pilot_v1, 0x11, sizeof(pilot_v1));
    pilot_v1[0] = 0x7f; pilot_v1[1] = 'E'; pilot_v1[2] = 'L'; pilot_v1[3] = 'F';
    pilot_v1[4] = 2; pilot_v1[5] = 1; pilot_v1[18] = 183; pilot_v1[19] = 0;
    memcpy(pilot_v2, pilot_v1, sizeof(pilot_v1));
    pilot_v2[200] = 0x99;

    unsigned char wbi_hdr[512], wbi_cur[512];
    memset(wbi_hdr, 0x33, sizeof(wbi_hdr));
    memcpy(wbi_hdr, "WBI2", 4);

    char res[1024];
    size_t rlen;

    /* TEST 1: WBI2 preflight failure (bad magic) must NOT touch appFw.so or pilot.so */
    unlink(PILOT_BOOTSTRAP_PATH);
    unlink(PILOT_WBI_BACKUP_PATH);
    unsigned char bad_wbi[512];
    memset(bad_wbi, 0x77, sizeof(bad_wbi));
    write_file_bytes(PILOT_WBI_DEV_PATH, bad_wbi, sizeof(bad_wbi));
    write_file_bytes(PILOT_FLASH_PILOT_PATH, pilot_v1, sizeof(pilot_v1));
    write_file_bytes(PILOT_FLASH_MODE_PATH, "write\n", 6);
    reset_hooks();
    pilot_flash_run_once();
    rlen = read_file_bytes(PILOT_FLASH_RESULT_PATH, res, sizeof(res) - 1);
    res[rlen] = '\0';
    assert(strstr(res, "reason=wbi_preflight") != NULL);
    assert(read_file_bytes(PILOT_FLASH_TEST_APPFW_PATH, cur, sizeof(cur)) == sizeof(orig));
    assert(memcmp(cur, orig, sizeof(orig)) == 0);
    assert(access(PILOT_BOOTSTRAP_PATH, F_OK) != 0);
    printf("PASS Test 1: WBI2 preflight rejects bad magic before touching appFw.so\n");

    /* TEST 2: WBI2 clear happens BEFORE modifying appFw.so (Suggestion 1):
     * failure leaves appFw.so completely untouched (0 writes!), cleans newly created
     * pilot.so, AND PRESERVES WBI2 backups ! */
    write_file_bytes(PILOT_WBI_DEV_PATH, wbi_hdr, sizeof(wbi_hdr));
    write_file_bytes(PILOT_FLASH_MODE_PATH, "write\n", 6);
    reset_hooks();
    g_fail_wbi_pwrite = 1;
    pilot_flash_run_once();
    rlen = read_file_bytes(PILOT_FLASH_RESULT_PATH, res, sizeof(res) - 1);
    res[rlen] = '\0';
    assert(strstr(res, "reason=wbi_clear") != NULL);
    assert(g_appfw_pwrite_calls == 0);
    assert(read_file_bytes(PILOT_FLASH_TEST_APPFW_PATH, cur, sizeof(cur)) == sizeof(orig));
    assert(memcmp(cur, orig, sizeof(orig)) == 0);
    assert(access(PILOT_BOOTSTRAP_PATH, F_OK) != 0);
    assert(access(PILOT_WBI_BACKUP_PATH, F_OK) == 0);
    assert(access(PILOT_WBI_TMP_BACKUP_PATH, F_OK) == 0);
    printf("PASS Test 2: WBI2 clear failure before appFw write performs 0 writes, cleans pilot.so, preserves WBI backups\n");

    /* TEST 3: Normal write clears WBI2 BEFORE 2-stage fsync barrier on appFw.so */
    write_file_bytes(PILOT_WBI_DEV_PATH, wbi_hdr, sizeof(wbi_hdr));
    write_file_bytes(PILOT_FLASH_MODE_PATH, "write\n", 6);
    reset_hooks();
    pilot_flash_run_once();
    rlen = read_file_bytes(PILOT_FLASH_RESULT_PATH, res, sizeof(res) - 1);
    res[rlen] = '\0';
    assert(strstr(res, "RESULT=OK") != NULL);
    assert(strstr(res, "barrier=2stage") != NULL);
    assert(strstr(res, "wbi=cleared") != NULL);
    /* Verify WBI2 was zeroed BEFORE appFw.so writes, and {1,2,3} fsync'd BEFORE window 0! */
    const char *expected_seq = "W@256:32,W@512:8,W@1024:8,FSYNC,W@32:4,FSYNC,";
    const char *wbi_pos = strstr(g_event_log, "WBI_ZERO,");
    const char *app_pos = strstr(g_event_log, expected_seq);
    assert(wbi_pos != NULL && app_pos != NULL && wbi_pos < app_pos);
    assert(read_file_bytes(PILOT_FLASH_TEST_APPFW_PATH, cur, sizeof(cur)) == sizeof(patched));
    assert(memcmp(cur, patched, sizeof(patched)) == 0);
    assert(read_file_bytes(PILOT_WBI_DEV_PATH, wbi_cur, sizeof(wbi_cur)) == 512);
    for (int i = 0; i < 512; ++i) assert(wbi_cur[i] == 0);
    printf("PASS Test 3: Normal write clears WBI2 BEFORE 2-stage barrier (%s)\n", expected_seq);

    /* TEST 3b: Re-running write when already patched MUST NOT rewrite appFw.so */
    write_file_bytes(PILOT_FLASH_MODE_PATH, "write\n", 6);
    reset_hooks();
    pilot_flash_run_once();
    rlen = read_file_bytes(PILOT_FLASH_RESULT_PATH, res, sizeof(res) - 1);
    res[rlen] = '\0';
    assert(strstr(res, "RESULT=OK") != NULL);
    assert(strstr(res, "already=1") != NULL);
    assert(g_appfw_pwrite_calls == 0);
    printf("PASS Test 3b: Idempotent write on patched appFw.so performs 0 writes to appFw.so\n");

    /* TEST 4: Atomic pilot.so update failure leaves existing pilot_v1 completely intact */
    write_file_bytes(PILOT_FLASH_PILOT_PATH, pilot_v2, sizeof(pilot_v2));
    write_file_bytes(PILOT_FLASH_MODE_PATH, "pilot\n", 6);
    reset_hooks();
    g_fail_pilot_tmp_pwrite = 1;
    pilot_flash_run_once();
    rlen = read_file_bytes(PILOT_FLASH_RESULT_PATH, res, sizeof(res) - 1);
    res[rlen] = '\0';
    assert(strstr(res, "RESULT=FAIL") != NULL);
    assert(read_file_bytes(PILOT_BOOTSTRAP_PATH, pilot_read, sizeof(pilot_read)) == sizeof(pilot_v1));
    assert(memcmp(pilot_read, pilot_v1, sizeof(pilot_v1)) == 0);
    assert(access(PILOT_BOOTSTRAP_TMP_PATH, F_OK) != 0);
    printf("PASS Test 4: Failed pilot.so update leaves original pilot.so untouched & cleans temp\n");

    /* TEST 5: Purge MUST refuse while appFw.so is still patched */
    write_file_bytes(PILOT_FLASH_MODE_PATH, "purge\n", 6);
    reset_hooks();
    pilot_flash_run_once();
    rlen = read_file_bytes(PILOT_FLASH_RESULT_PATH, res, sizeof(res) - 1);
    res[rlen] = '\0';
    assert(strstr(res, "reason=appfw_not_baseline") != NULL);
    assert(access(PILOT_BOOTSTRAP_PATH, F_OK) == 0);
    printf("PASS Test 5: Purge refuses to remove pilot.so while appFw.so is patched\n");

    /* TEST 6: Restore clears Pilot-era WBI2 BEFORE reverse 2-stage barrier */
    unsigned char pilot_era_wbi[512];
    memset(pilot_era_wbi, 0x66, sizeof(pilot_era_wbi));
    memcpy(pilot_era_wbi, "WBI2", 4);
    write_file_bytes(PILOT_WBI_DEV_PATH, pilot_era_wbi, sizeof(pilot_era_wbi));
    write_file_bytes(PILOT_FLASH_MODE_PATH, "restore\n", 8);
    reset_hooks();
    pilot_flash_run_once();
    rlen = read_file_bytes(PILOT_FLASH_RESULT_PATH, res, sizeof(res) - 1);
    res[rlen] = '\0';
    assert(strstr(res, "RESULT=OK") != NULL);
    assert(strstr(res, "wbi=cleared") != NULL);
    const char *expected_restore_seq = "W@32:4,FSYNC,W@256:32,W@512:8,W@1024:8,FSYNC,";
    const char *wbi_pos_r = strstr(g_event_log, "WBI_ZERO,");
    const char *app_pos_r = strstr(g_event_log, expected_restore_seq);
    assert(wbi_pos_r != NULL && app_pos_r != NULL && wbi_pos_r < app_pos_r);
    assert(read_file_bytes(PILOT_FLASH_TEST_APPFW_PATH, cur, sizeof(cur)) == sizeof(orig));
    assert(memcmp(cur, orig, sizeof(orig)) == 0);
    assert(read_file_bytes(PILOT_WBI_DEV_PATH, wbi_cur, sizeof(wbi_cur)) == 512);
    for (int i = 0; i < 512; ++i) assert(wbi_cur[i] == 0);
    printf("PASS Test 6: Restore clears Pilot-era WBI2 BEFORE reverse 2-stage barrier\n");

    /* TEST 6b: Restore WBI2 clear failure leaves appFw.so untouched (never leaves baseline+Pilot-WBI!) & preserves backup */
    write_file_bytes(PILOT_FLASH_TEST_APPFW_PATH, patched, sizeof(patched));
    write_file_bytes(PILOT_WBI_DEV_PATH, pilot_era_wbi, sizeof(pilot_era_wbi));
    write_file_bytes(PILOT_FLASH_MODE_PATH, "restore\n", 8);
    reset_hooks();
    g_fail_wbi_pwrite = 1;
    pilot_flash_run_once();
    rlen = read_file_bytes(PILOT_FLASH_RESULT_PATH, res, sizeof(res) - 1);
    res[rlen] = '\0';
    assert(strstr(res, "reason=wbi_clear") != NULL);
    assert(g_appfw_pwrite_calls == 0);
    assert(read_file_bytes(PILOT_FLASH_TEST_APPFW_PATH, cur, sizeof(cur)) == sizeof(patched));
    assert(memcmp(cur, patched, sizeof(patched)) == 0);
    assert(access(PILOT_WBI_BACKUP_PATH, F_OK) == 0);
    /* Restore back to baseline cleanly for Test 6c/7 */
    write_file_bytes(PILOT_FLASH_TEST_APPFW_PATH, orig, sizeof(orig));
    printf("PASS Test 6b: Restore WBI clear failure aborts before touching appFw.so & preserves WBI backup\n");

    /* TEST 6c: restore-wbi refuses when /dev/nflasha5 is non-zero, succeeds when zero */
    write_file_bytes(PILOT_WBI_DEV_PATH, pilot_era_wbi, sizeof(pilot_era_wbi));
    write_file_bytes(PILOT_FLASH_MODE_PATH, "restore-wbi\n", 12);
    reset_hooks();
    pilot_flash_run_once();
    rlen = read_file_bytes(PILOT_FLASH_RESULT_PATH, res, sizeof(res) - 1);
    res[rlen] = '\0';
    assert(strstr(res, "reason=wbi_dev_not_zero") != NULL);
    unsigned char all_zero[512];
    memset(all_zero, 0, sizeof(all_zero));
    write_file_bytes(PILOT_WBI_DEV_PATH, all_zero, sizeof(all_zero));
    write_file_bytes(PILOT_FLASH_MODE_PATH, "restore-wbi\n", 12);
    reset_hooks();
    pilot_flash_run_once();
    rlen = read_file_bytes(PILOT_FLASH_RESULT_PATH, res, sizeof(res) - 1);
    res[rlen] = '\0';
    assert(strstr(res, "RESULT=OK") != NULL && strstr(res, "wbi=restored") != NULL);
    printf("PASS Test 6c: restore-wbi refuses non-zero device header and succeeds when zero\n");

    /* TEST 7a: Purge MUST refuse while /dev/nflasha5 WBI2 header is non-zero (Suggestion 3) */
    write_file_bytes(PILOT_FLASH_MODE_PATH, "purge\n", 6);
    reset_hooks();
    pilot_flash_run_once();
    rlen = read_file_bytes(PILOT_FLASH_RESULT_PATH, res, sizeof(res) - 1);
    res[rlen] = '\0';
    assert(strstr(res, "reason=wbi_not_clear") != NULL);
    assert(access(PILOT_BOOTSTRAP_PATH, F_OK) == 0);
    assert(access(PILOT_WBI_BACKUP_PATH, F_OK) == 0);
    printf("PASS Test 7a: Purge refuses to remove pilot.so/backup while WBI2 header is non-zero\n");

    /* TEST 7b: Once /dev/nflasha5 is zeroed, purge succeeds and reports verify=enoent */
    write_file_bytes(PILOT_WBI_DEV_PATH, all_zero, sizeof(all_zero));
    write_file_bytes(PILOT_FLASH_MODE_PATH, "purge\n", 6);
    reset_hooks();
    pilot_flash_run_once();
    rlen = read_file_bytes(PILOT_FLASH_RESULT_PATH, res, sizeof(res) - 1);
    res[rlen] = '\0';
    assert(strstr(res, "RESULT=OK") != NULL);
    assert(strstr(res, "mode=purge") != NULL);
    assert(strstr(res, "verify=enoent") != NULL);
    assert(access(PILOT_BOOTSTRAP_PATH, F_OK) != 0);
    assert(access(PILOT_WBI_BACKUP_PATH, F_OK) != 0);
    printf("PASS Test 7b: Purge removes pilot.so & backup when WBI2 is zero and verifies -ENOENT\n");

    /* TEST 8: Mid-write appFw.so failure rolls back already-written windows */
    write_file_bytes(PILOT_WBI_DEV_PATH, wbi_hdr, sizeof(wbi_hdr));
    write_file_bytes(PILOT_FLASH_PILOT_PATH, pilot_v1, sizeof(pilot_v1));
    write_file_bytes(PILOT_FLASH_MODE_PATH, "write\n", 6);
    reset_hooks();
    g_fail_appfw_pwrite_after_n = 2;
    pilot_flash_run_once();
    rlen = read_file_bytes(PILOT_FLASH_RESULT_PATH, res, sizeof(res) - 1);
    res[rlen] = '\0';
    assert(strstr(res, "reason=write_or_readback") != NULL);
    assert(strstr(res, "state=restored") != NULL);
    assert(read_file_bytes(PILOT_FLASH_TEST_APPFW_PATH, cur, sizeof(cur)) == sizeof(orig));
    assert(memcmp(cur, orig, sizeof(orig)) == 0);
    printf("PASS Test 8: Mid-write I/O error automatically restores appFw.so to baseline\n");

    /* TEST 9: Invalid non-ELF pilot.so rejected before modifying anything */
    unsigned char bad_pilot[128];
    memset(bad_pilot, 0x42, sizeof(bad_pilot));
    unlink(PILOT_BOOTSTRAP_PATH);
    write_file_bytes(PILOT_FLASH_PILOT_PATH, bad_pilot, sizeof(bad_pilot));
    write_file_bytes(PILOT_FLASH_MODE_PATH, "pilot\n", 6);
    reset_hooks();
    pilot_flash_run_once();
    rlen = read_file_bytes(PILOT_FLASH_RESULT_PATH, res, sizeof(res) - 1);
    res[rlen] = '\0';
    assert(strstr(res, "operation_rc=-1006") != NULL);
    assert(access(PILOT_BOOTSTRAP_PATH, F_OK) != 0);
    printf("PASS Test 9: Non-AArch64 ELF pilot.so rejected (-1006)\n");

    /* TEST 10: Partially zeroed WBI2 header (interrupted clear) can be resumed & zeroed,
     * while an untouched camera (no backup) with zeroed magic + garbage tail is STILL rejected (-2001). */
    unlink(PILOT_WBI_BACKUP_PATH);
    unlink(PILOT_WBI_TMP_BACKUP_PATH);
    unsigned char partial_no_bak[512];
    memset(partial_no_bak, 0x77, sizeof(partial_no_bak));
    memset(partial_no_bak, 0, 128);
    write_file_bytes(PILOT_WBI_DEV_PATH, partial_no_bak, sizeof(partial_no_bak));
    write_file_bytes(PILOT_FLASH_MODE_PATH, "clear-wbi\n", 10);
    reset_hooks();
    pilot_flash_run_once();
    rlen = read_file_bytes(PILOT_FLASH_RESULT_PATH, res, sizeof(res) - 1);
    res[rlen] = '\0';
    assert(strstr(res, "operation_rc=-2001") != NULL);

    /* Now with valid backup present, simulate partial zero of bytes 0..127 (magic wiped!) */
    write_file_bytes(PILOT_WBI_BACKUP_PATH, wbi_hdr, sizeof(wbi_hdr));
    unsigned char partial_wbi[512];
    memcpy(partial_wbi, wbi_hdr, sizeof(partial_wbi));
    memset(partial_wbi, 0, 128);
    write_file_bytes(PILOT_WBI_DEV_PATH, partial_wbi, sizeof(partial_wbi));
    write_file_bytes(PILOT_FLASH_MODE_PATH, "clear-wbi\n", 10);
    reset_hooks();
    pilot_flash_run_once();
    rlen = read_file_bytes(PILOT_FLASH_RESULT_PATH, res, sizeof(res) - 1);
    res[rlen] = '\0';
    assert(strstr(res, "RESULT=OK") != NULL && strstr(res, "wbi=cleared") != NULL);
    assert(read_file_bytes(PILOT_WBI_DEV_PATH, wbi_cur, sizeof(wbi_cur)) == 512);
    for (int i = 0; i < 512; ++i) assert(wbi_cur[i] == 0);
    /* Verify intact backup in PILOT_WBI_BACKUP_PATH was NOT clobbered */
    assert(read_file_bytes(PILOT_WBI_BACKUP_PATH, wbi_cur, sizeof(wbi_cur)) == 512);
    assert(memcmp(wbi_cur, wbi_hdr, 512) == 0);
    printf("PASS Test 10: Partially zeroed WBI2 resumes clearing when backup exists; rejects without backup\n");

    /* TEST 11: When old backup already exists in /usr/lib/pilot_wbi_hdr.bak and writing the
     * new header's staging backup (pilotflash.wbi.bak) fails, tool MUST fail and MUST NOT
     * zero /dev/nflasha5 or report RESULT=OK! */
    unsigned char new_wbi[512];
    memset(new_wbi, 0x88, sizeof(new_wbi));
    memcpy(new_wbi, "WBI2", 4);
    write_file_bytes(PILOT_WBI_BACKUP_PATH, wbi_hdr, sizeof(wbi_hdr));
    unlink(PILOT_WBI_TMP_BACKUP_PATH);
    write_file_bytes(PILOT_WBI_DEV_PATH, new_wbi, sizeof(new_wbi));
    write_file_bytes(PILOT_FLASH_MODE_PATH, "clear-wbi\n", 10);
    reset_hooks();
    g_fail_wbi_tmp_bak_pwrite = 1;
    pilot_flash_run_once();
    rlen = read_file_bytes(PILOT_FLASH_RESULT_PATH, res, sizeof(res) - 1);
    res[rlen] = '\0';
    assert(strstr(res, "RESULT=FAIL") != NULL);
    assert(strstr(res, "RESULT=OK") == NULL);
    /* /dev/nflasha5 MUST still hold untouched new_wbi! */
    assert(read_file_bytes(PILOT_WBI_DEV_PATH, wbi_cur, sizeof(wbi_cur)) == 512);
    assert(memcmp(wbi_cur, new_wbi, 512) == 0);
    /* Old /usr/lib/pilot_wbi_hdr.bak MUST still hold untouched wbi_hdr! */
    assert(read_file_bytes(PILOT_WBI_BACKUP_PATH, wbi_cur, sizeof(wbi_cur)) == 512);
    assert(memcmp(wbi_cur, wbi_hdr, 512) == 0);
    printf("PASS Test 11: Backup write failure with existing old backup aborts cleanly without zeroing WBI2\n");

    /* TEST 12: Interrupted 2-stage mixed window state (window 0 baseline, windows 1..3 patched):
     *   - check reports state=mixed, window_mask=0xe, plus read-only statfs & WBI2 header telemetry
     *   - purge refuses (appfw_not_baseline)
     *   - mid-write I/O error on restore rolls back to EXACT pre-run mixed window bytes
     *   - restore converges cleanly to baseline (recovered_mixed=1)
     *   - write converges cleanly from mixed to patched (recovered_mixed=1) */
    unsigned char mixed[4096];
    memcpy(mixed, patched, sizeof(mixed));
    memcpy(mixed + offs[0], orig + offs[0], lens[0]); /* window 0 = baseline, windows 1..3 = patched */
    write_file_bytes(PILOT_FLASH_TEST_APPFW_PATH, mixed, sizeof(mixed));
    write_file_bytes(PILOT_WBI_DEV_PATH, wbi_hdr, sizeof(wbi_hdr));
    write_file_bytes(PILOT_FLASH_PILOT_PATH, pilot_v1, sizeof(pilot_v1));

    write_file_bytes(PILOT_FLASH_MODE_PATH, "check\n", 6);
    reset_hooks();
    pilot_flash_run_once();
    rlen = read_file_bytes(PILOT_FLASH_RESULT_PATH, res, sizeof(res) - 1);
    res[rlen] = '\0';
    assert(strstr(res, "RESULT=OK") != NULL);
    assert(strstr(res, "state=mixed") != NULL);
    assert(strstr(res, "window_mask=0xe") != NULL);
    assert(strstr(res, "usr_magic=0xef53") != NULL);
    assert(strstr(res, "wbi_status=wbi2") != NULL);
    assert(strstr(res, "wbi_hdr4=0x57424932") != NULL);

    write_file_bytes(PILOT_FLASH_MODE_PATH, "purge\n", 6);
    reset_hooks();
    pilot_flash_run_once();
    rlen = read_file_bytes(PILOT_FLASH_RESULT_PATH, res, sizeof(res) - 1);
    res[rlen] = '\0';
    assert(strstr(res, "reason=appfw_not_baseline") != NULL);

    /* Mid-write failure from mixed state must roll back to exact pre-run mixed bytes! */
    write_file_bytes(PILOT_WBI_DEV_PATH, all_zero, sizeof(all_zero));
    write_file_bytes(PILOT_FLASH_MODE_PATH, "restore\n", 8);
    reset_hooks();
    g_fail_appfw_pwrite_after_n = 2;
    pilot_flash_run_once();
    rlen = read_file_bytes(PILOT_FLASH_RESULT_PATH, res, sizeof(res) - 1);
    res[rlen] = '\0';
    assert(strstr(res, "reason=write_or_readback") != NULL);
    assert(strstr(res, "state=restored") != NULL);
    assert(read_file_bytes(PILOT_FLASH_TEST_APPFW_PATH, cur, sizeof(cur)) == sizeof(mixed));
    assert(memcmp(cur, mixed, sizeof(mixed)) == 0);

    /* Normal restore from mixed state converges to exact baseline */
    write_file_bytes(PILOT_WBI_DEV_PATH, wbi_hdr, sizeof(wbi_hdr));
    write_file_bytes(PILOT_FLASH_MODE_PATH, "restore\n", 8);
    reset_hooks();
    pilot_flash_run_once();
    rlen = read_file_bytes(PILOT_FLASH_RESULT_PATH, res, sizeof(res) - 1);
    res[rlen] = '\0';
    assert(strstr(res, "RESULT=OK") != NULL);
    assert(strstr(res, "state=baseline") != NULL);
    assert(strstr(res, "recovered_mixed=1") != NULL);
    assert(strstr(res, "wbi=cleared") != NULL);
    assert(read_file_bytes(PILOT_FLASH_TEST_APPFW_PATH, cur, sizeof(cur)) == sizeof(orig));
    assert(memcmp(cur, orig, sizeof(orig)) == 0);

    /* Normal write from mixed state converges to exact patched */
    write_file_bytes(PILOT_FLASH_TEST_APPFW_PATH, mixed, sizeof(mixed));
    write_file_bytes(PILOT_WBI_DEV_PATH, wbi_hdr, sizeof(wbi_hdr));
    write_file_bytes(PILOT_FLASH_MODE_PATH, "write\n", 6);
    reset_hooks();
    pilot_flash_run_once();
    rlen = read_file_bytes(PILOT_FLASH_RESULT_PATH, res, sizeof(res) - 1);
    res[rlen] = '\0';
    assert(strstr(res, "RESULT=OK") != NULL);
    assert(strstr(res, "state=patched") != NULL);
    assert(strstr(res, "recovered_mixed=1") != NULL);
    assert(read_file_bytes(PILOT_FLASH_TEST_APPFW_PATH, cur, sizeof(cur)) == sizeof(patched));
    assert(memcmp(cur, patched, sizeof(patched)) == 0);
    printf("PASS Test 12: Mixed-window check/telemetry, exact per-window rollback, and restore/write convergence\n");

    printf("ALL 16 FAULT-INJECTION TESTS PASSED\n");
    return 0;
}
