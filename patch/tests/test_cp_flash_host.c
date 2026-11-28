/* Comprehensive ASan+UBSan host fault-injection test suite for pilot_cp_flash_hardened.c */
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

static char g_dir[256];
static char g_cp_dir[300];
static char g_cp_img[320];
static char g_wbi_dev[320];
static char g_jiritsu_dir[300];
static char g_mode_path[320];
static char g_plan_path[320];
static char g_res_path[320];
static char g_res_tmp[320];

#define PILOT_CP_FLASH_HOST_TEST 1
#define CP_DIR_PATH      g_cp_dir
#define CP_IMAGE_PATH    g_cp_img
#define CP_WBI_DEV_PATH  g_wbi_dev
#define CP_JIRITSU_DIR   g_jiritsu_dir
#define CP_MODE_PATH     g_mode_path
#define CP_PLAN_PATH     g_plan_path
#define CP_RESULT_PATH   g_res_path
#define CP_RESULT_TMP_PATH g_res_tmp

/* Fault injection & call trace state */
static int g_cp_fd = -1;
static unsigned long long g_statfs_magic = 0x4d44ULL; /* vfat */
static unsigned long long g_statfs_flags = 0x0c30ULL; /* rw, noatime, nodiratime, sync */
static unsigned long long g_statfs_free = 12ULL * 1024ULL * 1024ULL;
static long g_statfs_rc = 0;

static int g_fail_pwrite_nth = 0;
static int g_fail_pwrite_from = 0; /* fail every pwrite >= N */
static int g_fail_fsync_nth = 0;
static int g_corrupt_readback_nth = 0;
static int g_eintr_pwrite_once = 0;
static int g_eintr_fsync_once = 0;

static int g_pwrite_count = 0;
static int g_fsync_count = 0;
static int g_pread_after_write = 0;
static int g_write_order_log[32];
static int g_write_order_len = 0;
static int g_events_log[64]; /* 1=pwrite(w0), 2=pwrite(w1), 10=fsync(cp) */
static int g_events_len = 0;

#define TEST_IMG_SIZE 4096ULL
#define W0_OFF 128ULL
#define W0_LEN 4U
#define W1_OFF 1024ULL
#define W1_LEN 64U

static unsigned char g_w0_before[W0_LEN] = {0x30, 0x78, 0x18, 0xb1};
static unsigned char g_w0_after[W0_LEN]  = {0x6a, 0xf3, 0x89, 0xb7};
static unsigned char g_w1_before[W1_LEN];
static unsigned char g_w1_after[W1_LEN];

long cp_host_hook_open(const char *path, long flags, long mode)
{
    int fd = open(path, (int)flags, (mode_t)mode);
    if (fd < 0) return -errno;
    if (strcmp(path, g_cp_img) == 0) g_cp_fd = fd;
    else if (fd == g_cp_fd) g_cp_fd = -1;
    return fd;
}

long cp_host_hook_pread(long fd, void *buf, unsigned long size, unsigned long long off)
{
    ssize_t r = pread((int)fd, buf, size, (off_t)off);
    if (r < 0) return -errno;
    if ((int)fd == g_cp_fd && g_pwrite_count > 0 && (off == W0_OFF || off == W1_OFF)) {
        g_pread_after_write++;
        if (g_corrupt_readback_nth > 0 && g_pread_after_write == g_corrupt_readback_nth) {
            ((unsigned char *)buf)[0] ^= 0xFFu;
        }
    }
    return (long)r;
}

long cp_host_hook_pwrite(long fd, const void *buf, unsigned long size, unsigned long long off)
{
    if ((int)fd == g_cp_fd) {
        if (g_eintr_pwrite_once) {
            g_eintr_pwrite_once = 0;
            return -EINTR;
        }
        g_pwrite_count++;
        int wid = (off == W0_OFF) ? 0 : (off == W1_OFF ? 1 : -1);
        if (g_write_order_len < 32) g_write_order_log[g_write_order_len++] = wid;
        if (g_events_len < 64) g_events_log[g_events_len++] = (wid == 0 ? 1 : 2);
        if ((g_fail_pwrite_nth > 0 && g_pwrite_count == g_fail_pwrite_nth) ||
            (g_fail_pwrite_from > 0 && g_pwrite_count >= g_fail_pwrite_from)) {
            /* Simulate partial write corruption before error! */
            unsigned char junk = 0xAA;
            ssize_t ign = pwrite((int)fd, &junk, 1, (off_t)off);
            (void)ign;
            return -EIO;
        }
    }
    ssize_t r = pwrite((int)fd, buf, size, (off_t)off);
    return r < 0 ? -errno : (long)r;
}

long cp_host_hook_fsync(long fd)
{
    if ((int)fd == g_cp_fd) {
        if (g_eintr_fsync_once) {
            g_eintr_fsync_once = 0;
            return -EINTR;
        }
        g_fsync_count++;
        if (g_events_len < 64) g_events_log[g_events_len++] = 10;
        if (g_fail_fsync_nth > 0 && g_fsync_count == g_fail_fsync_nth) {
            return -EIO;
        }
    }
    int r = fsync((int)fd);
    return r < 0 ? -errno : 0;
}

long cp_host_hook_rename(const char *oldpath, const char *newpath)
{
    int r = rename(oldpath, newpath);
    return r < 0 ? -errno : 0;
}

long cp_host_hook_statfs(const char *path, unsigned long long *m, unsigned long long *fl, unsigned long long *fr)
{
    (void)path;
    *m = g_statfs_magic;
    *fl = g_statfs_flags;
    *fr = g_statfs_free;
    return g_statfs_rc;
}

#include "../pilot_cp_flash_hardened.c"

static void reset_hooks(void)
{
    g_cp_fd = -1;
    g_statfs_magic = 0x4d44ULL;
    g_statfs_flags = 0x0c30ULL;
    g_statfs_free = 12ULL * 1024ULL * 1024ULL;
    g_statfs_rc = 0;
    g_fail_pwrite_nth = 0;
    g_fail_pwrite_from = 0;
    g_fail_fsync_nth = 0;
    g_corrupt_readback_nth = 0;
    g_eintr_pwrite_once = 0;
    g_eintr_fsync_once = 0;
    g_pwrite_count = 0;
    g_fsync_count = 0;
    g_pread_after_write = 0;
    g_write_order_len = 0;
    g_events_len = 0;
}

static void write_file_all(const char *path, const void *buf, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    assert(fd >= 0);
    assert(write(fd, buf, len) == (ssize_t)len);
    close(fd);
}

static void read_result_str(char *buf, size_t cap)
{
    int fd = open(g_res_path, O_RDONLY);
    assert(fd >= 0);
    ssize_t n = read(fd, buf, cap - 1);
    assert(n > 0);
    buf[n] = '\0';
    close(fd);
}

static void seed_wbi(int active_wbi2)
{
    unsigned char hdr[512];
    memset(hdr, 0, sizeof(hdr));
    if (active_wbi2) memcpy(hdr, "WBI2", 4);
    write_file_all(g_wbi_dev, hdr, sizeof(hdr));
}

static void seed_plan_valid(void)
{
    unsigned char plan[512];
    size_t pos = 0;
    memcpy(plan + pos, "PILOTCP1", 8); pos += 8;
    uint32_t ver = 1, cnt = 2;
    uint64_t fsz = TEST_IMG_SIZE;
    memcpy(plan + pos, &ver, 4); pos += 4;
    memcpy(plan + pos, &cnt, 4); pos += 4;
    memcpy(plan + pos, &fsz, 8); pos += 8;
    uint64_t off0 = W0_OFF, off1 = W1_OFF;
    uint32_t len0 = W0_LEN, len1 = W1_LEN, rsv = 0;
    memcpy(plan + pos, &off0, 8); pos += 8;
    memcpy(plan + pos, &len0, 4); pos += 4;
    memcpy(plan + pos, &rsv, 4);  pos += 4;
    memcpy(plan + pos, &off1, 8); pos += 8;
    memcpy(plan + pos, &len1, 4); pos += 4;
    memcpy(plan + pos, &rsv, 4);  pos += 4;
    memcpy(plan + pos, g_w0_before, W0_LEN); pos += W0_LEN;
    memcpy(plan + pos, g_w0_after,  W0_LEN); pos += W0_LEN;
    memcpy(plan + pos, g_w1_before, W1_LEN); pos += W1_LEN;
    memcpy(plan + pos, g_w1_after,  W1_LEN); pos += W1_LEN;
    write_file_all(g_plan_path, plan, pos);
}

static void seed_cp_image(int w0_patched, int w1_patched)
{
    unsigned char img[TEST_IMG_SIZE];
    for (size_t i = 0; i < sizeof(img); ++i) img[i] = (unsigned char)((i * 17u) ^ 0x5Au);
    memcpy(img + W0_OFF, w0_patched ? g_w0_after : g_w0_before, W0_LEN);
    memcpy(img + W1_OFF, w1_patched ? g_w1_after : g_w1_before, W1_LEN);
    write_file_all(g_cp_img, img, sizeof(img));
}

static void verify_cp_windows_on_disk(int expect_w0_patched, int expect_w1_patched)
{
    unsigned char img[TEST_IMG_SIZE];
    int fd = open(g_cp_img, O_RDONLY);
    assert(fd >= 0);
    assert(read(fd, img, sizeof(img)) == (ssize_t)sizeof(img));
    close(fd);
    assert(memcmp(img + W0_OFF, expect_w0_patched ? g_w0_after : g_w0_before, W0_LEN) == 0);
    assert(memcmp(img + W1_OFF, expect_w1_patched ? g_w1_after : g_w1_before, W1_LEN) == 0);
}

static void run_mode(const char *mode, char *out, size_t cap)
{
    unlink(g_res_path);
    write_file_all(g_mode_path, mode, strlen(mode));
    pilot_cp_flash_host_entry();
    read_result_str(out, cap);
}

int main(void)
{
    const char *tmpdir = getenv("TMPDIR");
    int length = snprintf(g_dir, sizeof(g_dir), "%s/cp-flash-host-test-XXXXXX",
                          tmpdir && *tmpdir ? tmpdir : "/tmp");
    assert(length > 0 && (size_t)length < sizeof(g_dir));
    assert(mkdtemp(g_dir) != NULL);
    snprintf(g_cp_dir, sizeof(g_cp_dir), "%s/cp", g_dir);
    snprintf(g_cp_img, sizeof(g_cp_img), "%s/cpapp-b.bin", g_cp_dir);
    snprintf(g_wbi_dev, sizeof(g_wbi_dev), "%s/nflasha5", g_dir);
    snprintf(g_jiritsu_dir, sizeof(g_jiritsu_dir), "%s/jiritsu", g_dir);
    snprintf(g_mode_path, sizeof(g_mode_path), "%s/cpflash.mode", g_jiritsu_dir);
    snprintf(g_plan_path, sizeof(g_plan_path), "%s/cpflash-plan.bin", g_jiritsu_dir);
    snprintf(g_res_path, sizeof(g_res_path), "%s/cpflash.result", g_jiritsu_dir);
    snprintf(g_res_tmp, sizeof(g_res_tmp), "%s/.cpflash.result.tmp", g_jiritsu_dir);

    assert(mkdir(g_cp_dir, 0755) == 0);
    assert(mkdir(g_jiritsu_dir, 0755) == 0);

    memset(g_w1_before, 0x00, W1_LEN);
    for (size_t i = 0; i < W1_LEN; ++i) g_w1_after[i] = (unsigned char)(0xA0u + i);

    char res[1024];
    int passed = 0;

    /* T1: RO check telemetry on baseline */
    reset_hooks(); seed_wbi(0); seed_plan_valid(); seed_cp_image(0, 0);
    run_mode("check\n", res, sizeof(res));
    assert(strstr(res, "RESULT=OK") && strstr(res, "state=baseline") &&
           strstr(res, "window_mask=0x0") && strstr(res, "cp_magic=0x4d44") &&
           strstr(res, "cp_ro=0") && strstr(res, "wbi_status=clear"));
    assert(g_pwrite_count == 0);
    printf("[PASS 1/16] RO check telemetry (0 writes)\n"); passed++;

    /* T2: Two-stage apply writes Window 1 FIRST -> fsync -> Window 0 SECOND -> fsync (+EINTR) */
    reset_hooks(); seed_wbi(0); seed_plan_valid(); seed_cp_image(0, 0);
    g_eintr_pwrite_once = 1; g_eintr_fsync_once = 1;
    run_mode("apply\n", res, sizeof(res));
    assert(strstr(res, "RESULT=OK") && strstr(res, "state=patched") &&
           strstr(res, "barrier=2stage") && strstr(res, "window_mask=0x3"));
    assert(g_write_order_len == 2 && g_write_order_log[0] == 1 && g_write_order_log[1] == 0);
    /* Verify strict barrier sequence: pwrite(w1) -> fsync -> pwrite(w0) -> fsync */
    assert(g_events_len == 4 && g_events_log[0] == 2 && g_events_log[1] == 10 &&
           g_events_log[2] == 1 && g_events_log[3] == 10);
    verify_cp_windows_on_disk(1, 1);
    printf("[PASS 2/16] Two-stage apply barrier (W1 -> fsync -> W0 -> fsync) + EINTR retry\n"); passed++;

    /* T3: Idempotent apply when already patched -> 0 writes */
    reset_hooks(); seed_wbi(0); seed_plan_valid(); seed_cp_image(1, 1);
    run_mode("apply\n", res, sizeof(res));
    assert(strstr(res, "RESULT=OK") && strstr(res, "idempotent=1") && strstr(res, "written=0"));
    assert(g_pwrite_count == 0);
    printf("[PASS 3/16] Idempotent apply performs 0 writes\n"); passed++;

    /* T4: Two-stage restore writes Window 0 FIRST -> fsync -> Window 1 SECOND -> fsync */
    reset_hooks(); seed_wbi(0); seed_plan_valid(); seed_cp_image(1, 1);
    run_mode("restore\n", res, sizeof(res));
    assert(strstr(res, "RESULT=OK") && strstr(res, "state=baseline") &&
           strstr(res, "barrier=2stage") && strstr(res, "window_mask=0x0"));
    assert(g_write_order_len == 2 && g_write_order_log[0] == 0 && g_write_order_log[1] == 1);
    assert(g_events_len == 4 && g_events_log[0] == 1 && g_events_log[1] == 10 &&
           g_events_log[2] == 2 && g_events_log[3] == 10);
    verify_cp_windows_on_disk(0, 0);
    printf("[PASS 4/16] Two-stage restore barrier (W0 -> fsync -> W1 -> fsync)\n"); passed++;

    /* T5: Idempotent restore when baseline -> 0 writes */
    reset_hooks(); seed_wbi(0); seed_plan_valid(); seed_cp_image(0, 0);
    run_mode("restore\n", res, sizeof(res));
    assert(strstr(res, "RESULT=OK") && strstr(res, "idempotent=1") && strstr(res, "written=0"));
    assert(g_pwrite_count == 0);
    printf("[PASS 5/16] Idempotent restore performs 0 writes\n"); passed++;

    /* T6: Fault on Stage 2 pwrite (W0) during apply -> rolls back W0 + W1 cleanly to baseline! */
    reset_hooks(); seed_wbi(0); seed_plan_valid(); seed_cp_image(0, 0);
    g_fail_pwrite_nth = 2; /* W1 succeeds, W0 partially corrupts then fails */
    run_mode("apply\n", res, sizeof(res));
    assert(strstr(res, "RESULT=FAIL") && strstr(res, "reason=pwrite_fail") &&
           strstr(res, "rollback=ok") && strstr(res, "window_mask=0x0"));
    verify_cp_windows_on_disk(0, 0);
    printf("[PASS 6/16] Stage 2 pwrite fault rolls back both W0 & W1 to baseline\n"); passed++;

    /* T7: Fault on Stage 2 fsync during apply -> rolls back cleanly to baseline! */
    reset_hooks(); seed_wbi(0); seed_plan_valid(); seed_cp_image(0, 0);
    g_fail_fsync_nth = 2;
    run_mode("apply\n", res, sizeof(res));
    assert(strstr(res, "RESULT=FAIL") && strstr(res, "reason=fsync_fail") &&
           strstr(res, "rollback=ok") && strstr(res, "window_mask=0x0"));
    verify_cp_windows_on_disk(0, 0);
    printf("[PASS 7/16] Stage 2 fsync fault rolls back cleanly to baseline\n"); passed++;

    /* T8: Fault on Stage 1 readback mismatch -> rolls back W1 cleanly to baseline! */
    reset_hooks(); seed_wbi(0); seed_plan_valid(); seed_cp_image(0, 0);
    g_corrupt_readback_nth = 1;
    run_mode("apply\n", res, sizeof(res));
    assert(strstr(res, "RESULT=FAIL") && strstr(res, "reason=readback_mismatch") &&
           strstr(res, "rollback=ok") && strstr(res, "window_mask=0x0"));
    verify_cp_windows_on_disk(0, 0);
    printf("[PASS 8/16] Stage 1 readback mismatch rolls back cleanly\n"); passed++;

    /* T9: Fault on Stage 2 pwrite (W1) during restore -> rolls back W1 then W0 to patched! */
    reset_hooks(); seed_wbi(0); seed_plan_valid(); seed_cp_image(1, 1);
    g_fail_pwrite_nth = 2;
    run_mode("restore\n", res, sizeof(res));
    assert(strstr(res, "RESULT=FAIL") && strstr(res, "reason=pwrite_fail") &&
           strstr(res, "rollback=ok") && strstr(res, "window_mask=0x3"));
    verify_cp_windows_on_disk(1, 1);
    printf("[PASS 9/16] Restore Stage 2 fault rolls back W1 then W0 to patched\n"); passed++;

    /* T10: Mixed-window (W1 patched, W0 baseline, mask=0x2) -> check + apply convergence */
    reset_hooks(); seed_wbi(0); seed_plan_valid(); seed_cp_image(0, 1);
    run_mode("check\n", res, sizeof(res));
    assert(strstr(res, "RESULT=OK") && strstr(res, "state=mixed_safe") && strstr(res, "window_mask=0x2"));
    reset_hooks();
    run_mode("apply\n", res, sizeof(res));
    assert(strstr(res, "RESULT=OK") && strstr(res, "state=patched") &&
           strstr(res, "recovered_mixed=1") && g_pwrite_count == 1 && g_write_order_log[0] == 0);
    verify_cp_windows_on_disk(1, 1);
    printf("[PASS 10/16] Mixed-window (0x2) check & apply convergence (writes only W0)\n"); passed++;

    /* T11: Mixed-window (W1 patched, W0 baseline, mask=0x2) -> restore convergence */
    reset_hooks(); seed_wbi(0); seed_plan_valid(); seed_cp_image(0, 1);
    run_mode("restore\n", res, sizeof(res));
    assert(strstr(res, "RESULT=OK") && strstr(res, "state=baseline") &&
           strstr(res, "recovered_mixed=1") && g_pwrite_count == 1 && g_write_order_log[0] == 1);
    verify_cp_windows_on_disk(0, 0);
    printf("[PASS 11/16] Mixed-window (0x2) restore convergence (writes only W1)\n"); passed++;

    /* T12: Refuse when active WBI2 snapshot exists */
    reset_hooks(); seed_wbi(1); seed_plan_valid(); seed_cp_image(0, 0);
    run_mode("apply\n", res, sizeof(res));
    assert(strstr(res, "RESULT=FAIL") && strstr(res, "reason=wbi_not_clear") && g_pwrite_count == 0);
    printf("[PASS 12/16] Refuses apply while WBI2 is active (0 writes)\n"); passed++;

    /* T13: Refuse when /cp is not vfat OR mounted read-only */
    reset_hooks(); seed_wbi(0); seed_plan_valid(); seed_cp_image(0, 0);
    g_statfs_magic = 0xef53ULL; /* ext2 instead of vfat */
    run_mode("apply\n", res, sizeof(res));
    assert(strstr(res, "RESULT=FAIL") && strstr(res, "reason=cp_fs_not_vfat") && g_pwrite_count == 0);
    reset_hooks(); g_statfs_flags = 0x0c31ULL; /* ST_RDONLY set */
    run_mode("apply\n", res, sizeof(res));
    assert(strstr(res, "RESULT=FAIL") && strstr(res, "reason=cp_mount_ro") && g_pwrite_count == 0);
    printf("[PASS 13/16] Refuses non-vfat or read-only /cp mount (0 writes)\n"); passed++;

    /* T14: Refuse unknown/corrupted window bytes */
    reset_hooks(); seed_wbi(0); seed_plan_valid(); seed_cp_image(0, 0);
    int fd = open(g_cp_img, O_RDWR); assert(fd >= 0);
    unsigned char bad = 0xEE; assert(pwrite(fd, &bad, 1, W0_OFF) == 1); close(fd);
    run_mode("apply\n", res, sizeof(res));
    assert(strstr(res, "RESULT=FAIL") && strstr(res, "reason=cp_unknown_windows") && g_pwrite_count == 0);
    printf("[PASS 14/16] Refuses unrecognized window bytes (0 writes)\n"); passed++;

    /* T15: Refuse file size mismatch */
    reset_hooks(); seed_wbi(0); seed_plan_valid();
    unsigned char short_img[512]; memset(short_img, 0, sizeof(short_img));
    write_file_all(g_cp_img, short_img, sizeof(short_img));
    run_mode("apply\n", res, sizeof(res));
    assert(strstr(res, "RESULT=FAIL") && strstr(res, "reason=cp_size_mismatch") && g_pwrite_count == 0);
    printf("[PASS 15/16] Refuses cpapp-b.bin size mismatch (0 writes)\n"); passed++;

    /* T16: Refuse bad plan (wrong magic / wrong window 0 len) & unknown mode */
    reset_hooks(); seed_wbi(0); seed_cp_image(0, 0);
    write_file_all(g_plan_path, "BADMAGIC000000000000000000000000000000000000000000000000", 56);
    run_mode("apply\n", res, sizeof(res));
    assert(strstr(res, "RESULT=FAIL") && strstr(res, "reason=bad_plan") && g_pwrite_count == 0);
    reset_hooks(); seed_plan_valid();
    run_mode("bogus\n", res, sizeof(res));
    assert(strstr(res, "RESULT=FAIL") && strstr(res, "reason=unknown_mode") && g_pwrite_count == 0);
    printf("[PASS 16/19] Refuses bad plan and unknown mode (0 writes)\n"); passed++;

    /* T17 (Audit BLOCKING #2): Restore Stage 2 (W1) fails AND rollback Step 0 (W1) ALSO fails ->
     * MUST ABORT rollback immediately and NEVER re-arm W0 pointing into corrupted W1! */
    reset_hooks(); seed_wbi(0); seed_plan_valid(); seed_cp_image(1, 1);
    g_fail_pwrite_from = 2; /* pwrite #1 (W0->stock) succeeds; pwrite #2 (W1->0) fails; pwrite #3 (W1 rollback) fails! */
    run_mode("restore\n", res, sizeof(res));
    assert(strstr(res, "RESULT=FAIL") && strstr(res, "reason=pwrite_fail") && strstr(res, "rollback=failed"));
    /* Verify W0 was NOT re-armed: total pwrites == 3 (W0, W1, W1-rb), NEVER 4! And W0 is STILL stock! */
    assert(g_write_order_len == 3 && g_write_order_log[0] == 0 &&
           g_write_order_log[1] == 1 && g_write_order_log[2] == 1);
    {
        unsigned char img[TEST_IMG_SIZE];
        int cfd = open(g_cp_img, O_RDONLY); assert(cfd >= 0);
        assert(read(cfd, img, sizeof(img)) == (ssize_t)sizeof(img)); close(cfd);
        assert(memcmp(img + W0_OFF, g_w0_before, W0_LEN) == 0); /* W0 stayed safely unhooked! */
    }
    printf("[PASS 17/19] Restore rollback aborts on W1 failure, NEVER re-arms W0\n"); passed++;

    /* T18 (Audit BLOCKING #2): Apply Stage 2 (W0) fails AND rollback Step 0 (W0) ALSO fails ->
     * MUST ABORT rollback immediately and NEVER wipe W1 while W0 might jump into W1! */
    reset_hooks(); seed_wbi(0); seed_plan_valid(); seed_cp_image(0, 0);
    g_fail_pwrite_from = 2; /* pwrite #1 (W1->after) succeeds; pwrite #2 (W0->after) fails; pwrite #3 (W0 rollback) fails! */
    run_mode("apply\n", res, sizeof(res));
    assert(strstr(res, "RESULT=FAIL") && strstr(res, "reason=pwrite_fail") && strstr(res, "rollback=failed"));
    assert(g_write_order_len == 3 && g_write_order_log[0] == 1 &&
           g_write_order_log[1] == 0 && g_write_order_log[2] == 0);
    {
        unsigned char img[TEST_IMG_SIZE];
        int cfd = open(g_cp_img, O_RDONLY); assert(cfd >= 0);
        assert(read(cfd, img, sizeof(img)) == (ssize_t)sizeof(img)); close(cfd);
        assert(memcmp(img + W1_OFF, g_w1_after, W1_LEN) == 0); /* W1 stayed safely intact! */
    }
    printf("[PASS 18/19] Apply rollback aborts on W0 failure, NEVER wipes W1\n"); passed++;

    /* T19 (Audit BLOCKING #3): WBI2 Fail-Closed on nonzero_other (wbi_st==2) and missing/unreadable dev (wbi_st<0) */
    reset_hooks(); seed_plan_valid(); seed_cp_image(0, 0);
    {
        unsigned char partial[512]; memset(partial, 0, sizeof(partial)); partial[200] = 0x5A;
        write_file_all(g_wbi_dev, partial, sizeof(partial));
    }
    run_mode("apply\n", res, sizeof(res));
    assert(strstr(res, "RESULT=FAIL") && strstr(res, "reason=wbi_nonzero_other") && g_pwrite_count == 0);
    reset_hooks(); unlink(g_wbi_dev);
    run_mode("apply\n", res, sizeof(res));
    assert(strstr(res, "RESULT=FAIL") && strstr(res, "reason=wbi_probe_fail") &&
           strstr(res, "code=-2") && g_pwrite_count == 0);
    printf("[PASS 19/24] WBI2 Fail-Closed on nonzero_other & unreadable nflasha5 (0 writes)\n"); passed++;

    /* T20 (Review C1): force-restore repairs corrupted W0 (0xAA) in strict W0->fsync->W1->fsync order */
    reset_hooks(); seed_wbi(1); /* even if WBI2 is active, emergency repair must work! */
    seed_plan_valid(); seed_cp_image(0, 1);
    {
        int cfd = open(g_cp_img, O_RDWR); assert(cfd >= 0);
        unsigned char bad = 0xAA; assert(pwrite(cfd, &bad, 1, W0_OFF) == 1);
        close(cfd);
    }
    run_mode("force-restore\n", res, sizeof(res));
    assert(strstr(res, "RESULT=OK") && strstr(res, "state=baseline") &&
           strstr(res, "repaired_unknown=1") && strstr(res, "window_mask=0x0"));
    assert(g_write_order_len == 2 && g_write_order_log[0] == 0 && g_write_order_log[1] == 1);
    verify_cp_windows_on_disk(0, 0);
    printf("[PASS 20/24] C1 force-restore repairs corrupted W0 (W0 -> fsync -> W1 -> fsync)\n"); passed++;

    /* T21 (Review C1): force-restore repairs armed W0 + corrupted W1 */
    reset_hooks(); seed_wbi(0); seed_plan_valid(); seed_cp_image(1, 0);
    {
        int cfd = open(g_cp_img, O_RDWR); assert(cfd >= 0);
        unsigned char bad = 0xCC; assert(pwrite(cfd, &bad, 1, W1_OFF + 4) == 1);
        close(cfd);
    }
    run_mode("force-restore\n", res, sizeof(res));
    assert(strstr(res, "RESULT=OK") && strstr(res, "state=baseline") && strstr(res, "repaired_unknown=1"));
    assert(g_write_order_len == 2 && g_write_order_log[0] == 0 && g_write_order_log[1] == 1);
    verify_cp_windows_on_disk(0, 0);
    printf("[PASS 21/24] C1 force-restore repairs armed W0 + corrupted W1\n"); passed++;

    /* T22 (Review C1): force-restore Stage 2 (W1) fails -> leaves W0 safely stock (never rolls back W0!) */
    reset_hooks(); seed_wbi(0); seed_plan_valid(); seed_cp_image(1, 1);
    g_fail_pwrite_nth = 2;
    run_mode("force-restore\n", res, sizeof(res));
    assert(strstr(res, "RESULT=FAIL") && strstr(res, "rollback=aborted_safe_w0_stock"));
    assert(g_write_order_len == 2 && g_write_order_log[0] == 0 && g_write_order_log[1] == 1);
    {
        unsigned char img[TEST_IMG_SIZE];
        int cfd = open(g_cp_img, O_RDONLY); assert(cfd >= 0);
        assert(read(cfd, img, sizeof(img)) == (ssize_t)sizeof(img)); close(cfd);
        assert(memcmp(img + W0_OFF, g_w0_before, W0_LEN) == 0);
    }
    printf("[PASS 22/24] C1 force-restore Stage 2 fault leaves W0 safely stock\n"); passed++;

    /* T23 (Review S1): Dangerous mixed state (mask=0x1: W0 armed, W1 zeros) check & apply-fail never wipes W1 */
    reset_hooks(); seed_wbi(0); seed_plan_valid(); seed_cp_image(1, 0);
    run_mode("check\n", res, sizeof(res));
    assert(strstr(res, "RESULT=OK") && strstr(res, "state=mixed_danger_w0_only") && strstr(res, "window_mask=0x1"));
    reset_hooks(); g_fail_fsync_nth = 1; /* W1 pwrite succeeds, fsync fails -> must NOT roll W1 back to zeros! */
    run_mode("apply\n", res, sizeof(res));
    assert(strstr(res, "RESULT=FAIL") && strstr(res, "rollback=skipped_danger_w0_armed"));
    assert(g_pwrite_count == 1 && g_write_order_log[0] == 1); /* NEVER wiped W1 back to 0! */
    printf("[PASS 23/24] S1 mixed_danger_w0_only (0x1) detected & rollback never wipes W1\n"); passed++;

    /* T24 (Review S3): Idempotent apply/restore succeeds with 0 writes even while WBI2 is active */
    reset_hooks(); seed_wbi(1); seed_plan_valid(); seed_cp_image(1, 1);
    run_mode("apply\n", res, sizeof(res));
    assert(strstr(res, "RESULT=OK") && strstr(res, "idempotent=1") && strstr(res, "written=0") &&
           strstr(res, "wbi_status=wbi2") && g_pwrite_count == 0);
    reset_hooks(); seed_wbi(1); seed_cp_image(0, 0);
    run_mode("restore\n", res, sizeof(res));
    assert(strstr(res, "RESULT=OK") && strstr(res, "idempotent=1") && strstr(res, "written=0") &&
           strstr(res, "wbi_status=wbi2") && g_pwrite_count == 0);
    printf("[PASS 24/24] S3 idempotent apply/restore skips WBI2 gate (0 writes)\n"); passed++;

    printf("\nALL %d/24 ASAN+UBSAN CP FLASHER TESTS PASSED!\n", passed);
    return 0;
}
