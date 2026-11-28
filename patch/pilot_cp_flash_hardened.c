/* Freestanding AArch64 hardened writer for the Sony ILCE-6700 CP UIPC v4 patch.
 *
 * Hardening improvements over upstream Pilot v0.2.1 cpdelta.so:
 *   1. Two-stage fsync barrier on /cp/cpapp-b.bin:
 *        - apply:   write Window 1 (568 B handler @ file 0xB0AD68 / VA 0x00C12D68)
 *                   -> fsync -> readback verify -> ONLY THEN write Window 0
 *                   (4 B branch @ file 0x39FE52 / VA 0x004A7E52) -> fsync ->
 *                   readback verify.
 *                   (Upstream sorted ascending and wrote the 4 B branch FIRST!)
 *        - restore: write Window 0 (4 B stock bytes @ file 0x39FE52 / VA 0x004A7E52)
 *                   -> fsync -> readback verify -> ONLY THEN restore Window 1
 *                   (568 B zeros @ file 0xB0AD68 / VA 0x00C12D68) -> fsync ->
 *                   readback verify.
 *        - idempotent: if already at target state, ZERO writes to cpapp-b.bin.
 *   2. Exact per-window state mask (0x0=baseline, 0x3=patched, 0x1/0x2=mixed,
 *      0xFF=unknown) with safe mixed-window convergence AND exact per-window
 *      rollback to before_mask (in safe reverse order) on pwrite/fsync/readback
 *      failure.
 *   3. Read-only "check" telemetry mode inspecting /cp/cpapp-b.bin size and
 *      window_mask, statfs("/cp") (filesystem magic, mount flags, RO flag,
 *      free space), and /dev/nflasha5 WBI2 header status with ZERO flash writes.
 *   4. Pre-write guards: verifies /cp is mounted vfat (0x4d44) read-write,
 *      exact cpapp-b.bin size (17,892,024 B), and /dev/nflasha5 WBI2 is clear
 *      (prevents booting a stale hibernation snapshot over patched CP).
 *   5. Atomic result publication (.cpflash.result.tmp -> fsync -> renameat ->
 *      parent-dir fsync), -EINTR retry loops on all I/O & fsync syscalls, and
 *      strictly bounded 512-byte formatting helpers.
 */

#ifdef PILOT_CP_FLASH_HOST_TEST
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#endif

typedef unsigned long usize;
typedef unsigned long long u64;

struct cp_patch_window {
    u64 offset;
    usize length;
    const unsigned char *before;
    const unsigned char *after;
};

#define CP_WINDOW_COUNT 2u
#define CP_MAX_WINDOW_SIZE 4096u
#define CP_PLAN_MAX (24u + 16u * CP_WINDOW_COUNT + 2u * CP_WINDOW_COUNT * CP_MAX_WINDOW_SIZE)
#define CP_RESULT_CAP 512u
#define CP_MAX_SIGNED_FILE_SIZE 0x7fffffffffffffffULL
#define CP_WBI_HDR_SIZE 512UL
#define CP_VFAT_SUPER_MAGIC 0x4d44ULL

__attribute__((visibility("default"), used))
const unsigned int pilot_cp_hardened_v2 = 0x50434832u;

static unsigned char cp_plan[CP_PLAN_MAX];
static struct cp_patch_window cp_windows[CP_WINDOW_COUNT];
static u64 cp_file_size;

#ifndef PILOT_CP_FLASH_HOST_TEST
#define AT_FDCWD (-100)
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_CREAT 0100
#define O_TRUNC 01000
/* Verified from aarch64-linux-gnu-gcc <fcntl.h>: __O_DIRECTORY == 040000 */
#define O_DIRECTORY 040000
#define EINTR 4
#endif

#define ST_RDONLY 1ULL

#ifndef CP_DIR_PATH
#define CP_DIR_PATH "/cp"
#endif
#ifndef CP_IMAGE_PATH
#define CP_IMAGE_PATH "/cp/cpapp-b.bin"
#endif
#ifndef CP_WBI_DEV_PATH
#define CP_WBI_DEV_PATH "/dev/nflasha5"
#endif
#ifndef CP_JIRITSU_DIR
#define CP_JIRITSU_DIR "/tmp/jiritsu"
#endif
#ifndef CP_MODE_PATH
#define CP_MODE_PATH "/tmp/jiritsu/cpflash.mode"
#endif
#ifndef CP_PLAN_PATH
#define CP_PLAN_PATH "/tmp/jiritsu/cpflash-plan.bin"
#endif
#ifndef CP_RESULT_PATH
#define CP_RESULT_PATH "/tmp/jiritsu/cpflash.result"
#endif
#ifndef CP_RESULT_TMP_PATH
#define CP_RESULT_TMP_PATH "/tmp/jiritsu/.cpflash.result.tmp"
#endif

#ifdef PILOT_CP_FLASH_HOST_TEST
extern long cp_host_hook_open(const char *path, long flags, long mode);
extern long cp_host_hook_pread(long fd, void *buf, usize size, u64 off);
extern long cp_host_hook_pwrite(long fd, const void *buf, usize size, u64 off);
extern long cp_host_hook_fsync(long fd);
extern long cp_host_hook_rename(const char *oldpath, const char *newpath);
extern long cp_host_hook_statfs(const char *path, u64 *magic_out, u64 *flags_out, u64 *free_out);

static long open_file(const char *path, long flags, long mode)
{
    long r;
    do {
        r = cp_host_hook_open(path, flags, mode);
    } while (r == -EINTR);
    return r;
}

static long close_file(long fd)
{
    int r = close((int)fd);
    return r < 0 ? -errno : 0;
}

static long read_file(long fd, void *buffer, usize size)
{
    ssize_t r;
    do {
        r = read((int)fd, buffer, size);
    } while (r < 0 && errno == EINTR);
    return r < 0 ? -errno : (long)r;
}

static long write_file(long fd, const void *buffer, usize size)
{
    ssize_t r;
    do {
        r = write((int)fd, buffer, size);
    } while (r < 0 && errno == EINTR);
    return r < 0 ? -errno : (long)r;
}

static long pread_file(long fd, void *buffer, usize size, u64 offset)
{
    long r;
    do {
        r = cp_host_hook_pread(fd, buffer, size, offset);
    } while (r == -EINTR);
    return r;
}

static long pwrite_file(long fd, const void *buffer, usize size, u64 offset)
{
    long r;
    do {
        r = cp_host_hook_pwrite(fd, buffer, size, offset);
    } while (r == -EINTR);
    return r;
}

static long sync_file(long fd)
{
    long r;
    do {
        r = cp_host_hook_fsync(fd);
    } while (r == -EINTR);
    return r;
}

static long file_size(long fd)
{
    off_t r = lseek((int)fd, 0, SEEK_END);
    return r < 0 ? -errno : (long)r;
}

static long rename_file(const char *oldpath, const char *newpath)
{
    return cp_host_hook_rename(oldpath, newpath);
}

#else

/* Verified AArch64 Linux syscall numbers (asm-generic/unistd.h, Linux 4.9+) */
static long syscall6(long number, long a0, long a1, long a2,
                     long a3, long a4, long a5)
{
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    register long x3 __asm__("x3") = a3;
    register long x4 __asm__("x4") = a4;
    register long x5 __asm__("x5") = a5;
    register long x8 __asm__("x8") = number;
    __asm__ volatile("svc 0"
                     : "+r"(x0)
                     : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8)
                     : "memory", "cc");
    return x0;
}

static long open_file(const char *path, long flags, long mode)
{
    long r;
    do {
        r = syscall6(56, AT_FDCWD, (long)path, flags, mode, 0, 0);
    } while (r == -EINTR);
    return r;
}

static long close_file(long fd)
{
    return syscall6(57, fd, 0, 0, 0, 0, 0);
}

static long read_file(long fd, void *buffer, usize size)
{
    long r;
    do {
        r = syscall6(63, fd, (long)buffer, (long)size, 0, 0, 0);
    } while (r == -EINTR);
    return r;
}

static long write_file(long fd, const void *buffer, usize size)
{
    long r;
    do {
        r = syscall6(64, fd, (long)buffer, (long)size, 0, 0, 0);
    } while (r == -EINTR);
    return r;
}

static long pread_file(long fd, void *buffer, usize size, u64 offset)
{
    long r;
    do {
        r = syscall6(67, fd, (long)buffer, (long)size, (long)offset, 0, 0);
    } while (r == -EINTR);
    return r;
}

static long pwrite_file(long fd, const void *buffer, usize size, u64 offset)
{
    long r;
    do {
        r = syscall6(68, fd, (long)buffer, (long)size, (long)offset, 0, 0);
    } while (r == -EINTR);
    return r;
}

static long statfs_raw(const char *path, void *buffer)
{
    long r;
    do {
        r = syscall6(43, (long)path, (long)buffer, 0, 0, 0, 0);
    } while (r == -EINTR);
    return r;
}

static long sync_file(long fd)
{
    long r;
    do {
        r = syscall6(82, fd, 0, 0, 0, 0, 0);
    } while (r == -EINTR);
    return r;
}

static long file_size(long fd)
{
    return syscall6(62, fd, 0, 2, 0, 0, 0);
}

static long rename_file(const char *oldpath, const char *newpath)
{
    return syscall6(38, AT_FDCWD, (long)oldpath, AT_FDCWD, (long)newpath, 0, 0);
}

static u64 read_u64_le(const unsigned char *buffer)
{
    u64 value = 0;
    for (unsigned int i = 0; i < 8; ++i)
        value |= (u64)buffer[i] << (i * 8u);
    return value;
}
#endif

static int file_size_equals(long fd, u64 expected)
{
    long sz = file_size(fd);
    return sz >= 0 && (u64)sz == expected;
}

static long sync_dir(const char *dir_path)
{
    long dfd = open_file(dir_path, O_RDONLY | O_DIRECTORY, 0);
    if (dfd < 0) return dfd;
    long rc = sync_file(dfd);
    (void)close_file(dfd);
    return rc;
}

static int pread_exact(long fd, void *buffer, usize size, u64 offset)
{
    unsigned char *cursor = (unsigned char *)buffer;
    usize total = 0;
    while (total < size) {
        long count = pread_file(fd, cursor + total, size - total, offset + total);
        if (count <= 0) return 0;
        total += (usize)count;
    }
    return 1;
}

static int pwrite_exact(long fd, const void *buffer, usize size, u64 offset)
{
    const unsigned char *cursor = (const unsigned char *)buffer;
    usize total = 0;
    while (total < size) {
        long count = pwrite_file(fd, cursor + total, size - total, offset + total);
        if (count <= 0) return 0;
        total += (usize)count;
    }
    return 1;
}

static int bytes_equal(const unsigned char *left,
                       const unsigned char *right, usize size)
{
    for (usize i = 0; i < size; ++i)
        if (left[i] != right[i]) return 0;
    return 1;
}

static int text_is(const char *left, const char *right)
{
    usize i = 0;
    while (left[i] && right[i] && left[i] == right[i]) ++i;
    return left[i] == 0 && right[i] == 0;
}

/* Strictly bounded formatting helpers (1 byte reserved for trailing '\n'). */
static usize append_char(char *out, usize cap, usize len, char ch)
{
    if (cap > 1u && len < cap - 1u)
        out[len++] = ch;
    return len;
}

static usize append_text(char *out, usize cap, usize len, const char *text)
{
    if (!text) return len;
    while (*text && cap > 1u && len < cap - 1u)
        out[len++] = *text++;
    return len;
}

static usize append_signed(char *out, usize cap, usize len, long value)
{
    unsigned long mag;
    if (value < 0) {
        len = append_char(out, cap, len, '-');
        mag = 0UL - (unsigned long)value;
    } else {
        mag = (unsigned long)value;
    }
    char digits[24];
    usize n = 0;
    do {
        digits[n++] = (char)('0' + mag % 10UL);
        mag /= 10UL;
    } while (mag > 0);
    while (n > 0)
        len = append_char(out, cap, len, digits[--n]);
    return len;
}

static usize append_hex64(char *out, usize cap, usize len, u64 value)
{
    static const char hex[] = "0123456789abcdef";
    len = append_text(out, cap, len, "0x");
    char digits[16];
    usize n = 0;
    do {
        digits[n++] = hex[value & 0xfu];
        value >>= 4;
    } while (value > 0);
    while (n > 0)
        len = append_char(out, cap, len, digits[--n]);
    return len;
}

static usize append_kv_text(char *out, usize cap, usize len, const char *k, const char *v)
{
    len = append_text(out, cap, len, k);
    len = append_char(out, cap, len, '=');
    len = append_text(out, cap, len, v);
    len = append_char(out, cap, len, '\n');
    return len;
}

static usize append_kv_signed(char *out, usize cap, usize len, const char *k, long v)
{
    len = append_text(out, cap, len, k);
    len = append_char(out, cap, len, '=');
    len = append_signed(out, cap, len, v);
    len = append_char(out, cap, len, '\n');
    return len;
}

static usize append_kv_hex(char *out, usize cap, usize len, const char *k, u64 v)
{
    len = append_text(out, cap, len, k);
    len = append_char(out, cap, len, '=');
    len = append_hex64(out, cap, len, v);
    len = append_char(out, cap, len, '\n');
    return len;
}

/* Atomic result publication: write .cpflash.result.tmp -> fsync -> renameat -> sync_dir */
static void write_result_atomic(const char *data, usize size)
{
    long fd = open_file(CP_RESULT_TMP_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    usize written = 0;
    while (written < size) {
        long n = write_file(fd, data + written, size - written);
        if (n <= 0) {
            (void)close_file(fd);
            return;
        }
        written += (usize)n;
    }
    (void)sync_file(fd);
    (void)close_file(fd);
    (void)rename_file(CP_RESULT_TMP_PATH, CP_RESULT_PATH);
    (void)sync_dir(CP_JIRITSU_DIR);
}

static u64 plan_uint(const unsigned char *data, usize width)
{
    u64 v = 0;
    for (usize i = 0; i < width; ++i)
        v |= (u64)data[i] << (8u * i);
    return v;
}

static int load_cp_plan(void)
{
    static const unsigned char magic[8] = {'P','I','L','O','T','C','P','1'};
    long fd = open_file(CP_PLAN_PATH, O_RDONLY, 0);
    if (fd < 0) return 0;
    long sz = file_size(fd);
    if (sz < 56 || sz > (long)sizeof(cp_plan) ||
        !pread_exact(fd, cp_plan, (usize)sz, 0)) {
        (void)close_file(fd);
        return 0;
    }
    (void)close_file(fd);

    for (usize i = 0; i < sizeof(magic); ++i)
        if (cp_plan[i] != magic[i]) return 0;
    if (plan_uint(cp_plan + 8, 4) != 1u ||
        plan_uint(cp_plan + 12, 4) != CP_WINDOW_COUNT) return 0;

    cp_file_size = plan_uint(cp_plan + 16, 8);
    if (!cp_file_size || cp_file_size > CP_MAX_SIGNED_FILE_SIZE) return 0;

    usize payload = 24u + 16u * CP_WINDOW_COUNT;
    for (usize i = 0; i < CP_WINDOW_COUNT; ++i) {
        const unsigned char *row = cp_plan + 24u + 16u * i;
        u64 offset = plan_uint(row, 8);
        u64 length = plan_uint(row + 8, 4);
        if (plan_uint(row + 12, 4) != 0u || !length || length > CP_MAX_WINDOW_SIZE ||
            offset > cp_file_size || length > cp_file_size - offset ||
            payload > (usize)sz || length * 2u > (usize)sz - payload) {
            return 0;
        }
        for (usize j = 0; j < i; ++j) {
            u64 o2 = cp_windows[j].offset;
            u64 e2 = o2 + cp_windows[j].length;
            if (offset < e2 && o2 < offset + length) return 0;
        }
        cp_windows[i].offset = offset;
        cp_windows[i].length = (usize)length;
        cp_windows[i].before = cp_plan + payload;
        cp_windows[i].after = cp_plan + payload + (usize)length;
        payload += (usize)length * 2u;
    }
    /* Window 0 MUST be the 4-byte Thumb-2 receiver branch hook; Window 1 MUST be the handler body (>4 B). */
    if (cp_windows[0].length != 4u || cp_windows[1].length <= 4u) return 0;
    /* Before and after must differ on both windows. */
    if (bytes_equal(cp_windows[0].before, cp_windows[0].after, cp_windows[0].length)) return 0;
    if (bytes_equal(cp_windows[1].before, cp_windows[1].after, cp_windows[1].length)) return 0;
    return payload == (usize)sz;
}

static u64 cp_fs_magic;
static u64 cp_fs_flags;
static u64 cp_fs_free;
static long cp_statfs_rc;

static int read_cp_mount_info(void)
{
    cp_fs_magic = 0;
    cp_fs_flags = 0;
    cp_fs_free = 0;
#ifdef PILOT_CP_FLASH_HOST_TEST
    cp_statfs_rc = cp_host_hook_statfs(CP_DIR_PATH, &cp_fs_magic, &cp_fs_flags, &cp_fs_free);
    return (int)cp_statfs_rc;
#else
    unsigned char info[120];
    cp_statfs_rc = statfs_raw(CP_DIR_PATH, info);
    if (cp_statfs_rc < 0) return (int)cp_statfs_rc;
    cp_fs_magic = read_u64_le(info);
    u64 f_bsize = read_u64_le(info + 8);
    u64 f_bfree = read_u64_le(info + 24);
    cp_fs_flags = read_u64_le(info + 80);
    if (f_bsize == 0)
        cp_fs_free = 0;
    else if (f_bfree <= (~0ULL) / f_bsize)
        cp_fs_free = f_bsize * f_bfree;
    else
        cp_fs_free = ~0ULL;
    return 0;
#endif
}

/* Read-only /dev/nflasha5 WBI2 status probe.
 * Returns: 0 = zero/clear, 1 = active WBI2, 2 = non-zero other, <0 = negative errno on open/read error. */
static long probe_wbi_status(u64 *hdr4_out)
{
    unsigned char hdr[CP_WBI_HDR_SIZE];
    *hdr4_out = 0;
    long fd = open_file(CP_WBI_DEV_PATH, O_RDONLY, 0);
    if (fd < 0) return fd;
    int ok = pread_exact(fd, hdr, sizeof(hdr), 0);
    (void)close_file(fd);
    if (!ok) return -5L; /* -EIO */
    *hdr4_out = ((u64)hdr[0] << 24) | ((u64)hdr[1] << 16) | ((u64)hdr[2] << 8) | (u64)hdr[3];
    if (hdr[0] == 'W' && hdr[1] == 'B' && hdr[2] == 'I' && hdr[3] == '2')
        return 1L;
    for (usize i = 0; i < sizeof(hdr); ++i)
        if (hdr[i] != 0) return 2L;
    return 0L;
}

static const char *wbi_status_name(long s)
{
    if (s == 0L) return "clear";
    if (s == 1L) return "wbi2";
    if (s == 2L) return "nonzero_other";
    return "unavailable";
}

/* Inspect CP windows.
 * Sets *mask_out to bitmask of windows matching `after` (bit 0 = window 0, bit 1 = window 1).
 * Returns:
 *   1 = baseline (mask == 0x0)
 *   2 = patched  (mask == 0x3)
 *   3 = mixed    (mask == 0x1 or 0x2, every window matches either before or after)
 *   0 = unknown  (at least one window matches neither before nor after)
 *  -1 = read error
 */
static int inspect_cp_windows(long fd, unsigned int *mask_out)
{
    unsigned char buf[CP_MAX_WINDOW_SIZE];
    unsigned int mask = 0;
    for (usize i = 0; i < CP_WINDOW_COUNT; ++i) {
        if (!pread_exact(fd, buf, cp_windows[i].length, cp_windows[i].offset))
            return -1;
        if (bytes_equal(buf, cp_windows[i].after, cp_windows[i].length)) {
            mask |= (1u << i);
        } else if (!bytes_equal(buf, cp_windows[i].before, cp_windows[i].length)) {
            *mask_out = 0xFFu;
            return 0;
        }
    }
    *mask_out = mask;
    if (mask == 0u) return 1;
    if (mask == ((1u << CP_WINDOW_COUNT) - 1u)) return 2;
    return 3;
}

static const char *state_name(int st, unsigned int mask)
{
    if (st == 1) return "baseline";
    if (st == 2) return "patched";
    if (st == 3) return (mask == 2u) ? "mixed_safe" : "mixed_danger_w0_only";
    return "unknown";
}

/* Write a single window, fsync, and readback-verify.
 * Returns: 0 = OK, -1 = pwrite failed, -2 = fsync failed, -3 = readback read failed, -4 = readback mismatch. */
static int write_and_verify_window(long fd, usize idx, int to_after, usize *bytes_written_acc)
{
    unsigned char verify[CP_MAX_WINDOW_SIZE];
    const unsigned char *src = to_after ? cp_windows[idx].after : cp_windows[idx].before;
    usize len = cp_windows[idx].length;
    u64 off = cp_windows[idx].offset;

    if (!pwrite_exact(fd, src, len, off))
        return -1;
    if (bytes_written_acc)
        *bytes_written_acc += len;
    if (sync_file(fd) < 0)
        return -2;
    if (!pread_exact(fd, verify, len, off))
        return -3;
    if (!bytes_equal(verify, src, len))
        return -4;
    return 0;
}

/* Roll back ONLY windows in modified_mask back to their original state in before_mask.
 * Safe order with STRICT barrier abort:
 *   - S1 guard: if before_mask == 0x1 (W0 armed while W1 was stock zeros), NEVER wipe
 *     W1 back to zeros while W0 stays armed! Return -1 (skipped_danger_w0_armed).
 *   - If window 0's original state was `before` (0): restore W0 FIRST; if W0 fails,
 *     ABORT IMMEDIATELY (return 0) so we NEVER wipe W1 while W0 might still jump into W1!
 *   - If window 0's original state was `after`  (1): restore W1 FIRST; if W1 fails,
 *     ABORT IMMEDIATELY (return 0) so we NEVER re-arm W0 pointing into a corrupted W1! */
static int rollback_windows(long fd, unsigned int before_mask, unsigned int modified_mask)
{
    if (!modified_mask) return 1;
    if (before_mask == 1u) {
        /* Starting from dangerous mixed state (W0 armed, W1 zeros): never roll back W1 to zeros! */
        return -1;
    }
    for (usize step = 0; step < 2u; ++step) {
        usize idx = ((before_mask & 1u) == 0u) ? step : (1u - step);
        if (!(modified_mask & (1u << idx))) continue;
        int orig_is_after = (before_mask & (1u << idx)) != 0u;
        if (write_and_verify_window(fd, idx, orig_is_after, (usize *)0) != 0)
            return 0;
    }
    return 1;
}

static void emit_fail(const char *mode, const char *reason, long detail_code,
                      unsigned int mask, const char *rollback_str)
{
    char out[CP_RESULT_CAP];
    usize n = 0;
    n = append_text(out, sizeof(out), n, "A6700CPFLASH\n");
    n = append_kv_text(out, sizeof(out), n, "RESULT", "FAIL");
    n = append_kv_text(out, sizeof(out), n, "mode", mode);
    n = append_kv_text(out, sizeof(out), n, "reason", reason);
    n = append_kv_signed(out, sizeof(out), n, "code", detail_code);
    n = append_kv_hex(out, sizeof(out), n, "window_mask", (u64)mask);
    if (rollback_str)
        n = append_kv_text(out, sizeof(out), n, "rollback", rollback_str);
    write_result_atomic(out, n);
}

static void run_check(void)
{
    int statfs_rc = read_cp_mount_info();
    u64 wbi_hdr4 = 0;
    long wbi_st = probe_wbi_status(&wbi_hdr4);

    long fd = open_file(CP_IMAGE_PATH, O_RDONLY, 0);
    if (fd < 0) {
        emit_fail("check", "open_cp_fail", fd, 0xFFu, (const char *)0);
        return;
    }
    long actual_size = file_size(fd);
    if (actual_size < 0 || (u64)actual_size != cp_file_size) {
        (void)close_file(fd);
        emit_fail("check", "cp_size_mismatch", actual_size, 0xFFu, (const char *)0);
        return;
    }
    unsigned int mask = 0xFFu;
    int st = inspect_cp_windows(fd, &mask);
    (void)close_file(fd);
    if (st < 0) {
        emit_fail("check", "pread_fail", st, 0xFFu, (const char *)0);
        return;
    }

    char out[CP_RESULT_CAP];
    usize n = 0;
    n = append_text(out, sizeof(out), n, "A6700CPFLASH\n");
    n = append_kv_text(out, sizeof(out), n, "RESULT", "OK");
    n = append_kv_text(out, sizeof(out), n, "mode", "check");
    n = append_kv_text(out, sizeof(out), n, "state", state_name(st, mask));
    n = append_kv_hex(out, sizeof(out), n, "window_mask", (u64)mask);
    n = append_kv_signed(out, sizeof(out), n, "cp_size", actual_size);
    n = append_kv_signed(out, sizeof(out), n, "cp_statfs_rc", statfs_rc);
    n = append_kv_hex(out, sizeof(out), n, "cp_magic", cp_fs_magic);
    n = append_kv_hex(out, sizeof(out), n, "cp_flags", cp_fs_flags);
    n = append_kv_signed(out, sizeof(out), n, "cp_ro", (cp_fs_flags & ST_RDONLY) ? 1 : 0);
    n = append_kv_hex(out, sizeof(out), n, "cp_free", cp_fs_free);
    n = append_kv_text(out, sizeof(out), n, "wbi_status", wbi_status_name(wbi_st));
    n = append_kv_hex(out, sizeof(out), n, "wbi_hdr4", wbi_hdr4);
    write_result_atomic(out, n);
}

static void run_mutate(int is_apply, int force_restore)
{
    const char *mode_str = force_restore ? "force-restore"
                         : (is_apply ? "apply" : "restore");
    int statfs_rc = read_cp_mount_info();
    if (statfs_rc < 0) {
        emit_fail(mode_str, "cp_statfs_fail", statfs_rc, 0xFFu, (const char *)0);
        return;
    }
    if (cp_fs_magic != CP_VFAT_SUPER_MAGIC) {
        emit_fail(mode_str, "cp_fs_not_vfat", (long)cp_fs_magic, 0xFFu, (const char *)0);
        return;
    }
    if (cp_fs_flags & ST_RDONLY) {
        emit_fail(mode_str, "cp_mount_ro", (long)cp_fs_flags, 0xFFu, (const char *)0);
        return;
    }

    long fd = open_file(CP_IMAGE_PATH, O_RDWR, 0);
    if (fd < 0) {
        emit_fail(mode_str, "open_cp_rw_fail", fd, 0xFFu, (const char *)0);
        return;
    }
    if (!file_size_equals(fd, cp_file_size)) {
        long sz = file_size(fd);
        (void)close_file(fd);
        emit_fail(mode_str, "cp_size_mismatch", sz, 0xFFu, (const char *)0);
        return;
    }

    unsigned int before_mask = 0xFFu;
    int st = inspect_cp_windows(fd, &before_mask);
    if (st < 0) {
        (void)close_file(fd);
        emit_fail(mode_str, "pread_fail", st, 0xFFu, (const char *)0);
        return;
    }
    if (st == 0 && !force_restore) {
        (void)close_file(fd);
        emit_fail(mode_str, "cp_unknown_windows", 0, before_mask, (const char *)0);
        return;
    }

    u64 wbi_hdr4 = 0;
    long wbi_st = probe_wbi_status(&wbi_hdr4);

    /* S3: If already at exact target state (idempotent, 0 writes), succeed cleanly
     * without requiring the WBI2 gate (still report wbi_status for visibility). */
    unsigned int target_mask = is_apply ? 3u : 0u;
    if (st != 0 && before_mask == target_mask) {
        (void)close_file(fd);
        char out[CP_RESULT_CAP];
        usize n = 0;
        n = append_text(out, sizeof(out), n, "A6700CPFLASH\n");
        n = append_kv_text(out, sizeof(out), n, "RESULT", "OK");
        n = append_kv_text(out, sizeof(out), n, "mode", mode_str);
        n = append_kv_text(out, sizeof(out), n, "state", state_name(st, before_mask));
        n = append_kv_hex(out, sizeof(out), n, "window_mask", (u64)before_mask);
        n = append_kv_text(out, sizeof(out), n, "barrier", "2stage");
        n = append_kv_signed(out, sizeof(out), n, "idempotent", 1);
        n = append_kv_signed(out, sizeof(out), n, "written", 0);
        n = append_kv_text(out, sizeof(out), n, "wbi_status", wbi_status_name(wbi_st));
        write_result_atomic(out, n);
        return;
    }

    /* Fail-Closed WBI2 gate before any actual write in normal apply/restore.
     * Emergency force-restore (C1) bypasses WBI2 gate because repairing corrupted
     * CP windows back to known-good stock bytes is strictly safer than leaving
     * corrupted flash in place; wbi_status is still reported for follow-up cleanup. */
    if (wbi_st != 0L && !force_restore) {
        (void)close_file(fd);
        const char *reason = (wbi_st == 1L) ? "wbi_not_clear"
                           : (wbi_st == 2L) ? "wbi_nonzero_other"
                                            : "wbi_probe_fail";
        long detail = (wbi_st < 0L) ? wbi_st : (long)wbi_hdr4;
        emit_fail(mode_str, reason, detail, before_mask, (const char *)0);
        return;
    }

    int recovered_mixed = (st == 3) ? 1 : 0;
    int repaired_unknown = (st == 0 && force_restore) ? 1 : 0;
    /* Strict two-stage order:
     *   apply:         Stage 1 = Window 1 (568 B handler), Stage 2 = Window 0 (4 B branch).
     *   restore/force: Stage 1 = Window 0 (4 B stock),     Stage 2 = Window 1 (568 B zeros). */
    usize order[2];
    order[0] = is_apply ? 1u : 0u;
    order[1] = is_apply ? 0u : 1u;

    unsigned int modified_mask = 0u;
    usize bytes_written = 0u;

    for (usize step = 0; step < 2u; ++step) {
        usize idx = order[step];
        unsigned int bit = 1u << idx;
        if (!force_restore && (before_mask & bit) == (target_mask & bit))
            continue;
        int rc = write_and_verify_window(fd, idx, is_apply, &bytes_written);
        if (rc != 0) {
            modified_mask |= bit;
            const char *rb_str;
            if (force_restore) {
                /* Never roll back a force-restore into corrupted bytes!
                 * If step==1 (W1) failed, W0 is already safely stock. */
                rb_str = (step == 1u) ? "aborted_safe_w0_stock" : "none";
            } else {
                int rb = rollback_windows(fd, before_mask, modified_mask);
                rb_str = (rb == 1) ? "ok"
                       : (rb < 0)  ? "skipped_danger_w0_armed"
                                   : "failed";
            }
            unsigned int post_mask = 0xFFu;
            (void)inspect_cp_windows(fd, &post_mask);
            (void)close_file(fd);
            const char *reason = (rc == -1) ? "pwrite_fail"
                               : (rc == -2) ? "fsync_fail"
                               : (rc == -3) ? "readback_fail"
                                            : "readback_mismatch";
            emit_fail(mode_str, reason, (long)idx, post_mask, rb_str);
            return;
        }
        modified_mask |= bit;
    }

    /* Final full-window verification after both stages completed */
    unsigned int after_mask = 0xFFu;
    int after_st = inspect_cp_windows(fd, &after_mask);
    if (after_st < 0 || after_mask != target_mask) {
        const char *rb_str;
        if (force_restore) {
            rb_str = "none";
        } else {
            int rb = rollback_windows(fd, before_mask, modified_mask);
            rb_str = (rb == 1) ? "ok"
                   : (rb < 0)  ? "skipped_danger_w0_armed"
                               : "failed";
        }
        unsigned int post_mask = 0xFFu;
        (void)inspect_cp_windows(fd, &post_mask);
        (void)close_file(fd);
        emit_fail(mode_str, "final_verify_mismatch", (long)after_mask,
                  post_mask, rb_str);
        return;
    }

    (void)close_file(fd);
    long dir_sync_rc = sync_dir(CP_DIR_PATH);

    char out[CP_RESULT_CAP];
    usize n = 0;
    n = append_text(out, sizeof(out), n, "A6700CPFLASH\n");
    n = append_kv_text(out, sizeof(out), n, "RESULT", "OK");
    n = append_kv_text(out, sizeof(out), n, "mode", mode_str);
    n = append_kv_text(out, sizeof(out), n, "state", state_name(after_st, after_mask));
    n = append_kv_hex(out, sizeof(out), n, "before_mask", (u64)before_mask);
    n = append_kv_hex(out, sizeof(out), n, "window_mask", (u64)after_mask);
    n = append_kv_text(out, sizeof(out), n, "barrier", "2stage");
    n = append_kv_signed(out, sizeof(out), n, "recovered_mixed", recovered_mixed);
    n = append_kv_signed(out, sizeof(out), n, "repaired_unknown", repaired_unknown);
    n = append_kv_signed(out, sizeof(out), n, "written", (long)bytes_written);
    n = append_kv_text(out, sizeof(out), n, "readback", "OK");
    n = append_kv_signed(out, sizeof(out), n, "dir_sync", dir_sync_rc);
    n = append_kv_text(out, sizeof(out), n, "wbi_status", wbi_status_name(wbi_st));
    write_result_atomic(out, n);
}

static void pilot_cp_flash_run(void)
{
    char mode[32];
    long mfd = open_file(CP_MODE_PATH, O_RDONLY, 0);
    if (mfd < 0) {
        emit_fail("unknown", "no_mode_file", mfd, 0xFFu, (const char *)0);
        return;
    }
    long mlen = read_file(mfd, mode, sizeof(mode) - 1u);
    (void)close_file(mfd);
    if (mlen <= 0) {
        emit_fail("unknown", "empty_mode", mlen, 0xFFu, (const char *)0);
        return;
    }
    while (mlen > 0 && (mode[mlen - 1] == '\n' || mode[mlen - 1] == '\r' || mode[mlen - 1] == ' '))
        --mlen;
    mode[mlen] = '\0';

    if (!load_cp_plan()) {
        emit_fail(mode, "bad_plan", 0, 0xFFu, (const char *)0);
        return;
    }

    if (text_is(mode, "check")) {
        run_check();
    } else if (text_is(mode, "apply")) {
        run_mutate(1, 0);
    } else if (text_is(mode, "restore")) {
        run_mutate(0, 0);
    } else if (text_is(mode, "force-restore")) {
        run_mutate(0, 1);
    } else {
        emit_fail(mode, "unknown_mode", 0, 0xFFu, (const char *)0);
    }
}

#ifdef PILOT_CP_FLASH_HOST_TEST
void pilot_cp_flash_host_entry(void)
{
    pilot_cp_flash_run();
}
#else
__attribute__((visibility("default")))
void _init(void)
{
    pilot_cp_flash_run();
}
#endif
