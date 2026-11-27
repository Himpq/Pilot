/* Freestanding AArch64 hardened writer for the Pilot appFw patch windows.
 *
 * Hardening improvements over upstream Pilot v0.2.1 pilot_ap_flash.c:
 *   1. Atomic temp-file + fsync + readback + renameat + parent-dir fsync for
 *      /usr/lib/pilot.so, /usr/lib/pilot_wbi_hdr.bak, and pilotflash.result
 *      (eliminates O_TRUNC truncation and host result-polling race), plus
 *      ELF64/AArch64 header validation on pilot.so.
 *   2. Two-stage fsync barrier on /usr/lib/appFw.so:
 *        - install: write windows {1,2,3} -> fsync -> readback verify ->
 *                   write 4-byte branch window {0} -> fsync.
 *        - restore: write 4-byte branch window {0} -> fsync -> readback ->
 *                   write windows {1,2,3} -> fsync.
 *        - idempotent write: if appFw.so is already patched, NEVER rewrite
 *          appFw.so while window 0 is live.
 *   3. WBI2 preflight + backup BEFORE modifying appFw.so, automatic appFw.so
 *      rollback if zeroing /dev/nflasha5 fails after writing appFw.so, and
 *      cleanup of newly created staging/backup artifacts on rollback paths.
 *   4. Strict capacity bounds checking on all emit_* formatting helpers
 *      (512-byte buffer, zero stack-overflow risk).
 *   5. Safe "purge" mode that removes /usr/lib/pilot.so and
 *      /usr/lib/pilot_wbi_hdr.bak ONLY after confirming appFw.so matches the
 *      stock baseline windows.
 *   6. Free-space preflight (>= 256 KiB free on /usr, safe f_bsize==0 guard),
 *      -EINTR retry loops on all I/O & fsync syscalls, and safe signed file_size
 *      checking.
 */

#ifdef PILOT_FLASH_HOST_TEST
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#endif

typedef unsigned long usize;
typedef unsigned long long u64;

struct pilot_patch_window {
    u64 offset;
    usize length;
    const unsigned char *before;
    const unsigned char *after;
};

#define PILOT_AP_WINDOW_COUNT 4u
#define PILOT_AP_MAX_WINDOW_SIZE 4096u
#define PILOT_PLAN_MAX (24u + 16u * PILOT_AP_WINDOW_COUNT + 2u * PILOT_AP_WINDOW_COUNT * PILOT_AP_MAX_WINDOW_SIZE)
#define PILOT_RESULT_CAP 512u
#define PILOT_USR_MIN_FREE_BYTES (256ULL * 1024ULL)
#define PILOT_MAX_SIGNED_FILE_SIZE 0x7fffffffffffffffULL

__attribute__((visibility("default"), used))
const unsigned int pilot_ap_generic_v1 = 0x50414731u;

__attribute__((visibility("default"), used))
const unsigned int pilot_ap_hardened_v2 = 0x50414832u;

static unsigned char pilot_plan[PILOT_PLAN_MAX];
static struct pilot_patch_window pilot_patch_windows[PILOT_AP_WINDOW_COUNT];
static u64 pilot_ap_file_size;

#ifndef PILOT_USR_LIB_DIR
#ifdef PILOT_FLASH_TEST
#define PILOT_USR_LIB_DIR "/tmp/pilotflash-test"
#else
#define PILOT_USR_LIB_DIR "/usr/lib"
#endif
#endif

#ifndef PILOT_BOOTSTRAP_PATH
#ifdef PILOT_FLASH_TEST
#define PILOT_BOOTSTRAP_PATH "/tmp/pilotflash-test/pilot.so"
#else
#define PILOT_BOOTSTRAP_PATH "/usr/lib/pilot.so"
#endif
#endif

#ifndef PILOT_BOOTSTRAP_TMP_PATH
#ifdef PILOT_FLASH_TEST
#define PILOT_BOOTSTRAP_TMP_PATH "/tmp/pilotflash-test/.pilot.so.tmp"
#else
#define PILOT_BOOTSTRAP_TMP_PATH "/usr/lib/.pilot.so.tmp"
#endif
#endif

#ifndef PILOT_WBI_DEV_PATH
#ifdef PILOT_FLASH_TEST
#define PILOT_WBI_DEV_PATH "/tmp/pilotflash-test/nflasha5"
#else
#define PILOT_WBI_DEV_PATH "/dev/nflasha5"
#endif
#endif

#ifndef PILOT_WBI_BACKUP_PATH
#ifdef PILOT_FLASH_TEST
#define PILOT_WBI_BACKUP_PATH "/tmp/pilotflash-test/pilot_wbi_hdr.bak"
#else
#define PILOT_WBI_BACKUP_PATH "/usr/lib/pilot_wbi_hdr.bak"
#endif
#endif

#ifndef PILOT_WBI_BACKUP_TMP_PATH
#ifdef PILOT_FLASH_TEST
#define PILOT_WBI_BACKUP_TMP_PATH "/tmp/pilotflash-test/.pilot_wbi_hdr.bak.tmp"
#else
#define PILOT_WBI_BACKUP_TMP_PATH "/usr/lib/.pilot_wbi_hdr.bak.tmp"
#endif
#endif

#ifndef PILOT_WBI_TMP_BACKUP_PATH
#ifdef PILOT_FLASH_TEST
#define PILOT_WBI_TMP_BACKUP_PATH "/tmp/pilotflash-test/pilotflash.wbi.bak"
#else
#define PILOT_WBI_TMP_BACKUP_PATH "/tmp/jiritsu/pilotflash.wbi.bak"
#endif
#endif

#ifndef PILOT_WBI_TMP_BACKUP_TMP_PATH
#ifdef PILOT_FLASH_TEST
#define PILOT_WBI_TMP_BACKUP_TMP_PATH "/tmp/pilotflash-test/.pilotflash.wbi.bak.tmp"
#else
#define PILOT_WBI_TMP_BACKUP_TMP_PATH "/tmp/jiritsu/.pilotflash.wbi.bak.tmp"
#endif
#endif

#define PILOT_WBI_HDR_SIZE 512UL

#ifndef PILOT_FLASH_HOST_TEST
#define AT_FDCWD (-100)
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_CREAT 0100
#define O_EXCL 0200
#define O_TRUNC 01000
/* Verified from aarch64-linux-gnu-gcc <fcntl.h>: __O_DIRECTORY == 040000 */
#define O_DIRECTORY 040000
#define ENOENT 2
#define EINTR 4
#endif

#define MS_RDONLY 1
#define MS_REMOUNT 32
#define ST_VALID 32
#define ST_RDONLY 1
#define ST_RELATIME 4096

#ifdef PILOT_FLASH_TEST
#ifndef PILOT_FLASH_TEST_APPFW_PATH
#define PILOT_FLASH_TEST_APPFW_PATH "/tmp/pilotflash-test/appFw.so"
#endif
#ifndef PILOT_FLASH_MODE_PATH
#define PILOT_FLASH_MODE_PATH "/tmp/jiritsu/pilotflash.mode"
#endif
#ifndef PILOT_FLASH_RESULT_PATH
#define PILOT_FLASH_RESULT_PATH "/tmp/jiritsu/pilotflash.result"
#endif
#ifndef PILOT_FLASH_RESULT_TMP_PATH
#define PILOT_FLASH_RESULT_TMP_PATH "/tmp/jiritsu/.pilotflash.result.tmp"
#endif
#ifndef PILOT_FLASH_PLAN_PATH
#define PILOT_FLASH_PLAN_PATH "/tmp/jiritsu/pilotflash-plan.bin"
#endif
#ifndef PILOT_FLASH_PILOT_PATH
#define PILOT_FLASH_PILOT_PATH "/tmp/jiritsu/pilotflash-pilot.so"
#endif
#define APPFW_PATH PILOT_FLASH_TEST_APPFW_PATH
#define MODE_PATH PILOT_FLASH_MODE_PATH
#define RESULT_PATH PILOT_FLASH_RESULT_PATH
#define RESULT_TMP_PATH PILOT_FLASH_RESULT_TMP_PATH
#define PLAN_PATH PILOT_FLASH_PLAN_PATH
#define PILOT_SOURCE_PATH PILOT_FLASH_PILOT_PATH
#else
#define APPFW_PATH "/usr/lib/appFw.so"
#define MODE_PATH "/tmp/jiritsu/pilotflash.mode"
#define RESULT_PATH "/tmp/jiritsu/pilotflash.result"
#define RESULT_TMP_PATH "/tmp/jiritsu/.pilotflash.result.tmp"
#define PLAN_PATH "/tmp/jiritsu/pilotflash-plan.bin"
#define PILOT_SOURCE_PATH "/tmp/jiritsu/pilotflash-pilot.so"
#endif

#ifdef PILOT_FLASH_HOST_TEST
extern long host_test_hook_open(const char *path, long flags, long mode);
extern long host_test_hook_pwrite(long fd, const void *buf, usize size, u64 off);
extern long host_test_hook_fsync(long fd);
extern long host_test_hook_rename(const char *oldpath, const char *newpath);
extern long host_test_hook_unlink(const char *path);

static long open_file(const char *path, long flags, long mode)
{
    long r;
    do {
        r = host_test_hook_open(path, flags, mode);
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
    ssize_t r;
    do {
        r = pread((int)fd, buffer, size, (off_t)offset);
    } while (r < 0 && errno == EINTR);
    return r < 0 ? -errno : (long)r;
}

static long pwrite_file(long fd, const void *buffer, usize size, u64 offset)
{
    long r;
    do {
        r = host_test_hook_pwrite(fd, buffer, size, offset);
    } while (r == -EINTR);
    return r;
}

static long sync_file(long fd)
{
    long r;
    do {
        r = host_test_hook_fsync(fd);
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
    return host_test_hook_rename(oldpath, newpath);
}

static long unlink_file(const char *path)
{
    return host_test_hook_unlink(path);
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

#ifndef PILOT_FLASH_TEST
static long statfs_file(const char *path, void *buffer)
{
    long r;
    do {
        r = syscall6(43, (long)path, (long)buffer, 0, 0, 0, 0);
    } while (r == -EINTR);
    return r;
}
#endif

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

static long unlink_file(const char *path)
{
    return syscall6(35, AT_FDCWD, (long)path, 0, 0, 0, 0);
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
    unsigned char *cursor = buffer;
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
    const unsigned char *cursor = buffer;
    usize total = 0;
    while (total < size) {
        long count = pwrite_file(fd, cursor + total, size - total, offset + total);
        if (count <= 0) return 0;
        total += (usize)count;
    }
    return 1;
}

static u64 plan_uint(const unsigned char *data, usize width)
{
    u64 value = 0;
    for (usize i = 0; i < width; ++i) value |= (u64)data[i] << (8u * i);
    return value;
}

static int load_patch_plan(void)
{
    static const unsigned char magic[8] = {'P','I','L','O','T','A','P','1'};
    long fd = open_file(PLAN_PATH, O_RDONLY, 0);
    if (fd < 0) return 0;
    long size = file_size(fd);
    if (size < 88 || size > (long)sizeof(pilot_plan) ||
        !pread_exact(fd, pilot_plan, (usize)size, 0)) {
        (void)close_file(fd);
        return 0;
    }
    (void)close_file(fd);
    for (usize i = 0; i < sizeof(magic); ++i)
        if (pilot_plan[i] != magic[i]) return 0;
    if (plan_uint(pilot_plan + 8, 4) != 1 ||
        plan_uint(pilot_plan + 12, 4) != PILOT_AP_WINDOW_COUNT) return 0;
    pilot_ap_file_size = plan_uint(pilot_plan + 16, 8);
    if (!pilot_ap_file_size || pilot_ap_file_size > PILOT_MAX_SIGNED_FILE_SIZE) return 0;
    usize payload = 24u + 16u * PILOT_AP_WINDOW_COUNT;
    for (usize i = 0; i < PILOT_AP_WINDOW_COUNT; ++i) {
        const unsigned char *row = pilot_plan + 24u + 16u * i;
        u64 offset = plan_uint(row, 8);
        u64 length = plan_uint(row + 8, 4);
        if (plan_uint(row + 12, 4) != 0 || !length || length > PILOT_AP_MAX_WINDOW_SIZE ||
            offset > pilot_ap_file_size || length > pilot_ap_file_size - offset ||
            payload > (usize)size || length * 2u > (usize)size - payload) return 0;
        for (usize j = 0; j < i; ++j) {
            u64 other = pilot_patch_windows[j].offset;
            u64 other_end = other + pilot_patch_windows[j].length;
            if (offset < other_end && other < offset + length) return 0;
        }
        pilot_patch_windows[i].offset = offset;
        pilot_patch_windows[i].length = (usize)length;
        pilot_patch_windows[i].before = pilot_plan + payload;
        pilot_patch_windows[i].after = pilot_plan + payload + (usize)length;
        payload += (usize)length * 2u;
    }
    /* Window 0 must be the 4-byte AArch64 branch instruction for two-stage barrier. */
    if (pilot_patch_windows[0].length != 4u) return 0;
    return payload == (usize)size;
}

static u64 usr_mount_flags;
static u64 usr_filesystem_magic;
static u64 usr_free_bytes;
static long usr_statfs_result;
static int usr_statfs_stage;
static int usr_was_already_rw;
#ifndef PILOT_FLASH_TEST
static int usr_mount_changed;
#endif

#ifndef PILOT_FLASH_TEST
static u64 read_u64_le(const unsigned char *buffer)
{
    u64 value = 0;
    for (unsigned int i = 0; i < 8; ++i)
        value |= (u64)buffer[i] << (i * 8);
    return value;
}
#endif

static int read_usr_mount_flags(void)
{
#ifdef PILOT_FLASH_TEST
    usr_statfs_result = 0;
    usr_filesystem_magic = 0xef53ULL;
    usr_mount_flags = ST_RDONLY | ST_VALID | ST_RELATIME;
    usr_free_bytes = 28ULL * 1024ULL * 1024ULL;
#else
    unsigned char info[120];
    long result = statfs_file("/usr", info);
    usr_statfs_result = result;
    if (result < 0) {
        usr_statfs_stage = 1;
        return (int)result;
    }
    usr_filesystem_magic = read_u64_le(info);
    u64 f_bsize = read_u64_le(info + 8);
    u64 f_bfree = read_u64_le(info + 24);
    usr_mount_flags = read_u64_le(info + 80);
    if (f_bsize == 0)
        usr_free_bytes = 0;
    else if (f_bfree <= (~0ULL) / f_bsize)
        usr_free_bytes = f_bsize * f_bfree;
    else
        usr_free_bytes = ~0ULL;
#endif
    if (usr_filesystem_magic != 0xef53ULL) {
        usr_statfs_stage = 2;
        return -1;
    }
    if (usr_mount_flags & ~(1ULL | 2ULL | 4ULL | 8ULL | 16ULL |
                            ST_VALID | 64ULL | 1024ULL | 2048ULL |
                            ST_RELATIME)) {
        usr_statfs_stage = 3;
        return -1;
    }
    usr_statfs_stage = 0;
    return 0;
}

static u64 usr_remount_options(int readonly)
{
    u64 flags = MS_REMOUNT | (readonly ? MS_RDONLY : 0);
    flags |= usr_mount_flags & (2ULL | 4ULL | 8ULL | 16ULL | 64ULL |
                                1024ULL | 2048ULL);
    if (usr_mount_flags & ST_RELATIME) flags |= 1ULL << 21;
    return flags;
}

static long remount_usr_rw(int require_free_space)
{
    usr_was_already_rw = 0;
#ifdef PILOT_FLASH_TEST
    (void)read_usr_mount_flags();
    (void)usr_remount_options(0);
    if (require_free_space && usr_free_bytes < PILOT_USR_MIN_FREE_BYTES) {
        usr_statfs_stage = 4;
        return -28;
    }
    return 0;
#else
    int result = read_usr_mount_flags();
    if (result < 0) return result;
    if (require_free_space && usr_free_bytes < PILOT_USR_MIN_FREE_BYTES) {
        usr_statfs_stage = 4;
        return -28; /* -ENOSPC */
    }
    if (!(usr_mount_flags & ST_RDONLY)) {
        /* /usr was already rw before we ran; preserve mount state so we do
         * not trigger a spurious -EBUSY failure on remount_ro, and flag it. */
        usr_was_already_rw = 1;
        usr_mount_changed = 0;
        return 0;
    }
    result = (int)syscall6(40, 0, (long)"/usr", 0,
                           (long)usr_remount_options(0), 0, 0);
    if (result == 0) usr_mount_changed = 1;
    return result;
#endif
}

static long remount_usr_ro(void)
{
#ifdef PILOT_FLASH_TEST
    (void)usr_remount_options(1);
    return 0;
#else
    if (!usr_mount_changed) return 0;
    long result = syscall6(40, 0, (long)"/usr", 0,
                          (long)usr_remount_options(1), 0, 0);
    if (result == 0) usr_mount_changed = 0;
    return result;
#endif
}

static int bytes_equal(const unsigned char *left,
                       const unsigned char *right, usize size)
{
    for (usize index = 0; index < size; ++index)
        if (left[index] != right[index]) return 0;
    return 1;
}

static int text_is(const char *left, const char *right)
{
    usize index = 0;
    while (left[index] && right[index] && left[index] == right[index]) ++index;
    return left[index] == 0 && right[index] == 0;
}

/* Strictly bounded formatting helpers (Issue 3 fix).
 * Always leave 1 byte reserved for trailing '\n' (limit = capacity - 1). */
static usize append_char(char *output, usize capacity, usize length, char ch)
{
    if (capacity > 1u && length < capacity - 1u)
        output[length++] = ch;
    return length;
}

static usize append_text(char *output, usize capacity, usize length, const char *text)
{
    if (!text) return length;
    while (*text && capacity > 1u && length < capacity - 1u)
        output[length++] = *text++;
    return length;
}

static usize append_signed(char *output, usize capacity, usize length, long value)
{
    unsigned long magnitude;
    if (value < 0) {
        length = append_char(output, capacity, length, '-');
        magnitude = 0UL - (unsigned long)value;
    } else {
        magnitude = (unsigned long)value;
    }
    char digits[24];
    usize count = 0;
    do {
        digits[count++] = (char)('0' + magnitude % 10UL);
        magnitude /= 10UL;
    } while (magnitude && count < sizeof(digits));
    while (count)
        length = append_char(output, capacity, length, digits[--count]);
    return length;
}

static usize append_hex(char *output, usize capacity, usize length, u64 value)
{
    static const char digits[] = "0123456789abcdef";
    int started = 0;
    length = append_text(output, capacity, length, "0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        unsigned int nibble = (unsigned int)((value >> shift) & 0xfULL);
        if (nibble || started || shift == 0) {
            length = append_char(output, capacity, length, digits[nibble]);
            started = 1;
        }
    }
    return length;
}

/* Atomic result publication via temp file + fsync + renameat (Fix #6).
 * Prevents host polling from ever observing an empty or half-written result file. */
static void write_result_buffer(const char *output, usize length)
{
    (void)unlink_file(RESULT_TMP_PATH);
    long fd = open_file(RESULT_TMP_PATH, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd >= 0) {
        usize written_total = 0;
        int ok = 1;
        while (written_total < length) {
            long count = write_file(fd, output + written_total, length - written_total);
            if (count <= 0) {
                ok = 0;
                break;
            }
            written_total += (usize)count;
        }
        (void)sync_file(fd);
        (void)close_file(fd);
        if (ok && rename_file(RESULT_TMP_PATH, RESULT_PATH) == 0)
            return;
        (void)unlink_file(RESULT_TMP_PATH);
    }
    /* Fallback direct write only if temp-path creation/rename failed. */
    fd = open_file(RESULT_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    usize written_total = 0;
    while (written_total < length) {
        long count = write_file(fd, output + written_total, length - written_total);
        if (count <= 0) break;
        written_total += (usize)count;
    }
    (void)sync_file(fd);
    (void)close_file(fd);
}

static void emit_result(const char *message)
{
    char output[PILOT_RESULT_CAP];
    usize length = 0;
    length = append_text(output, sizeof(output), length, "A6700PILOTFLASH\n");
    length = append_text(output, sizeof(output), length, message);
    if (usr_was_already_rw)
        length = append_text(output, sizeof(output), length, "\nusr_was_rw=1");
    if (length == 0 || output[length - 1] != '\n') {
        if (length < sizeof(output))
            output[length++] = '\n';
        else
            output[sizeof(output) - 1] = '\n';
    }
    write_result_buffer(output, length);
}

static void emit_mount_failure(const char *reason, long result)
{
    char output[PILOT_RESULT_CAP];
    usize length = 0;
    length = append_text(output, sizeof(output), length, "A6700PILOTFLASH\nRESULT=FAIL\nreason=");
    length = append_text(output, sizeof(output), length, reason);
    length = append_text(output, sizeof(output), length, "\noperation_rc=");
    length = append_signed(output, sizeof(output), length, result);
    length = append_text(output, sizeof(output), length, "\nstatfs_stage=");
    length = append_signed(output, sizeof(output), length, usr_statfs_stage);
    length = append_text(output, sizeof(output), length, "\nstatfs_rc=");
    length = append_signed(output, sizeof(output), length, usr_statfs_result);
    length = append_text(output, sizeof(output), length, "\nfilesystem_magic=");
    length = append_hex(output, sizeof(output), length, usr_filesystem_magic);
    length = append_text(output, sizeof(output), length, "\nmount_flags=");
    length = append_hex(output, sizeof(output), length, usr_mount_flags);
    length = append_text(output, sizeof(output), length, "\nfree_bytes=");
    length = append_hex(output, sizeof(output), length, usr_free_bytes);
    if (length < sizeof(output))
        output[length++] = '\n';
    else
        output[sizeof(output) - 1] = '\n';
    write_result_buffer(output, length);
}

static int read_mode(char *mode, usize capacity)
{
    long fd = open_file(MODE_PATH, O_RDONLY, 0);
    if (fd < 0) return 0;
    usize total = 0;
    while (total < capacity - 1) {
        long count = read_file(fd, mode + total, capacity - 1 - total);
        if (count < 0) {
            (void)close_file(fd);
            return 0;
        }
        if (count == 0) break;
        total += (usize)count;
    }
    (void)close_file(fd);
    if (total == 0 || total >= capacity - 1) return 0;
    mode[total] = 0;
    while (total > 0 && (mode[total - 1] == '\n' || mode[total - 1] == '\r'))
        mode[--total] = 0;
    return 1;
}

static int window_matches(long fd, usize window_index, int target_patched)
{
    unsigned char buffer[PILOT_AP_MAX_WINDOW_SIZE];
    const struct pilot_patch_window *window = &pilot_patch_windows[window_index];
    const unsigned char *expected = target_patched ? window->after : window->before;
    if (window->length > sizeof(buffer)) return 0;
    if (!pread_exact(fd, buffer, window->length, window->offset)) return 0;
    return bytes_equal(buffer, expected, window->length);
}

/* Return:
 *   1 = all 4 windows match baseline (mask == 0x0)
 *   2 = all 4 windows match patched  (mask == 0xf)
 *   3 = mixed valid state: every window matches either baseline or patched
 *   0 = unknown: I/O error or at least one window matches neither baseline nor patched
 */
static int image_state_mask(long fd, unsigned int *out_mask)
{
    unsigned char buffer[PILOT_AP_MAX_WINDOW_SIZE];
    unsigned int mask = 0u;
    int all_baseline = 1;
    int all_patched = 1;
    for (usize window_index = 0; window_index < PILOT_AP_WINDOW_COUNT; ++window_index) {
        const struct pilot_patch_window *window = &pilot_patch_windows[window_index];
        if (window->length > sizeof(buffer)) return 0;
        if (!pread_exact(fd, buffer, window->length, window->offset)) return 0;
        int is_before = bytes_equal(buffer, window->before, window->length);
        int is_after = bytes_equal(buffer, window->after, window->length);
        if (!is_before && !is_after) return 0;
        if (is_after && !is_before) {
            mask |= (1u << window_index);
            all_baseline = 0;
        } else if (is_before && !is_after) {
            all_patched = 0;
        }
        /* If before == after for a degenerate window, both flags stay valid. */
    }
    if (out_mask) *out_mask = mask;
    if (all_baseline) return 1;
    if (all_patched) return 2;
    return 3;
}

static int image_state(long fd)
{
    return image_state_mask(fd, (unsigned int *)0);
}

/* Two-stage barrier write for any 4-window target mask (mixed-state hardening):
 *   When window 0 target bit is 1 (branch live / install):
 *     Stage 1: write windows {1, 2, 3} -> fsync -> verify readback
 *     Stage 2: write window {0} (4-byte branch) -> fsync -> verify readback
 *   When window 0 target bit is 0 (branch inactive / restore or mixed rollback):
 *     Stage 1: write window {0} (original 4-byte instruction) -> fsync -> verify
 *     Stage 2: write windows {1, 2, 3} -> fsync -> verify
 */
static int write_mask(long fd, unsigned int target_mask)
{
    static const usize aux_windows[] = {1, 2, 3};
    int w0_patched = (target_mask & 1u) != 0u;
    if (w0_patched) {
        for (usize i = 0; i < 3u; ++i) {
            usize idx = aux_windows[i];
            int patched = ((target_mask >> idx) & 1u) != 0u;
            const struct pilot_patch_window *w = &pilot_patch_windows[idx];
            const unsigned char *src = patched ? w->after : w->before;
            if (!pwrite_exact(fd, src, w->length, w->offset)) return 0;
        }
        if (sync_file(fd) != 0) return 0;
        for (usize i = 0; i < 3u; ++i) {
            usize idx = aux_windows[i];
            int patched = ((target_mask >> idx) & 1u) != 0u;
            if (!window_matches(fd, idx, patched)) return 0;
        }
        const struct pilot_patch_window *w0 = &pilot_patch_windows[0];
        if (!pwrite_exact(fd, w0->after, w0->length, w0->offset)) return 0;
        if (sync_file(fd) != 0) return 0;
        return window_matches(fd, 0, 1);
    } else {
        const struct pilot_patch_window *w0 = &pilot_patch_windows[0];
        if (!pwrite_exact(fd, w0->before, w0->length, w0->offset)) return 0;
        if (sync_file(fd) != 0) return 0;
        if (!window_matches(fd, 0, 0)) return 0;

        for (usize i = 0; i < 3u; ++i) {
            usize idx = aux_windows[i];
            int patched = ((target_mask >> idx) & 1u) != 0u;
            const struct pilot_patch_window *w = &pilot_patch_windows[idx];
            const unsigned char *src = patched ? w->after : w->before;
            if (!pwrite_exact(fd, src, w->length, w->offset)) return 0;
        }
        if (sync_file(fd) != 0) return 0;
        for (usize i = 0; i < 3u; ++i) {
            usize idx = aux_windows[i];
            int patched = ((target_mask >> idx) & 1u) != 0u;
            if (!window_matches(fd, idx, patched)) return 0;
        }
        return 1;
    }
}

static int is_valid_aarch64_elf64(const unsigned char *hdr, usize size)
{
    if (size < 64u) return 0;
    if (hdr[0] != 0x7fu || hdr[1] != 'E' || hdr[2] != 'L' || hdr[3] != 'F') return 0;
    if (hdr[4] != 2u || hdr[5] != 1u) return 0; /* ELFCLASS64, ELFDATA2LSB */
    unsigned int machine = (unsigned int)hdr[18] | ((unsigned int)hdr[19] << 8u);
    return machine == 183u; /* EM_AARCH64 */
}

/* Atomic bootstrap install via temp file -> fsync -> readback -> renameat -> dir fsync.
 * Sets *out_created = 1 if a brand-new /usr/lib/pilot.so was created in this run. */
static long install_bootstrap_mode(int allow_replace, int *out_created)
{
    unsigned char source_buffer[1024], target_buffer[1024];
    if (out_created) *out_created = 0;

    long source = open_file(PILOT_SOURCE_PATH, O_RDONLY, 0);
    if (source < 0) return source;
    long source_size = file_size(source);
    if (source_size < 64 || source_size > 65536) {
        (void)close_file(source);
        return -1005;
    }
    if (!pread_exact(source, source_buffer, 64u, 0) ||
        !is_valid_aarch64_elf64(source_buffer, 64u)) {
        (void)close_file(source);
        return -1006;
    }

    int existed = 0;
    long existing = open_file(PILOT_BOOTSTRAP_PATH, O_RDONLY, 0);
    if (existing >= 0) {
        existed = 1;
        int matches = file_size_equals(existing, (u64)source_size);
        for (usize off = 0; matches && off < (usize)source_size;) {
            usize count = (usize)source_size - off;
            if (count > sizeof(source_buffer)) count = sizeof(source_buffer);
            matches = pread_exact(source, source_buffer, count, off) &&
                      pread_exact(existing, target_buffer, count, off) &&
                      bytes_equal(source_buffer, target_buffer, count);
            off += count;
        }
        (void)close_file(existing);
        if (matches) {
            (void)close_file(source);
            return 0;
        }
        if (!allow_replace) {
            (void)close_file(source);
            return -1001;
        }
    } else if (existing != -ENOENT) {
        (void)close_file(source);
        return existing;
    }

    (void)unlink_file(PILOT_BOOTSTRAP_TMP_PATH);
    long tmp_fd = open_file(PILOT_BOOTSTRAP_TMP_PATH, O_RDWR | O_CREAT | O_EXCL, 0644);
    if (tmp_fd < 0) {
        (void)close_file(source);
        return tmp_fd;
    }

    int ok = 1;
    for (usize off = 0; ok && off < (usize)source_size;) {
        usize count = (usize)source_size - off;
        if (count > sizeof(source_buffer)) count = sizeof(source_buffer);
        ok = pread_exact(source, source_buffer, count, off) &&
             pwrite_exact(tmp_fd, source_buffer, count, off);
        off += count;
    }
    long result = ok ? sync_file(tmp_fd) : -1002;
    if (!result && !file_size_equals(tmp_fd, (u64)source_size)) result = -1003;
    for (usize off = 0; !result && off < (usize)source_size;) {
        usize count = (usize)source_size - off;
        if (count > sizeof(source_buffer)) count = sizeof(source_buffer);
        if (!pread_exact(source, source_buffer, count, off) ||
            !pread_exact(tmp_fd, target_buffer, count, off) ||
            !bytes_equal(source_buffer, target_buffer, count)) {
            result = -1004;
        }
        off += count;
    }
    (void)close_file(tmp_fd);
    if (result) {
        (void)unlink_file(PILOT_BOOTSTRAP_TMP_PATH);
        (void)close_file(source);
        return result;
    }

    result = rename_file(PILOT_BOOTSTRAP_TMP_PATH, PILOT_BOOTSTRAP_PATH);
    if (result < 0) {
        (void)unlink_file(PILOT_BOOTSTRAP_TMP_PATH);
        (void)close_file(source);
        return result;
    }
    if (!existed && out_created) *out_created = 1;

    long dsync = sync_dir(PILOT_USR_LIB_DIR);
    if (dsync < 0) {
        (void)close_file(source);
        return dsync;
    }

    long verify_fd = open_file(PILOT_BOOTSTRAP_PATH, O_RDONLY, 0);
    if (verify_fd < 0) {
        (void)close_file(source);
        return verify_fd;
    }
    if (!file_size_equals(verify_fd, (u64)source_size)) result = -1007;
    for (usize off = 0; !result && off < (usize)source_size;) {
        usize count = (usize)source_size - off;
        if (count > sizeof(source_buffer)) count = sizeof(source_buffer);
        if (!pread_exact(source, source_buffer, count, off) ||
            !pread_exact(verify_fd, target_buffer, count, off) ||
            !bytes_equal(source_buffer, target_buffer, count)) {
            result = -1008;
        }
        off += count;
    }
    (void)close_file(verify_fd);
    (void)close_file(source);
    return result;
}

static long install_bootstrap(int *out_created)
{
    return install_bootstrap_mode(0, out_created);
}

static int wbi_is_magic(const unsigned char *b)
{
    return b[0] == 'W' && b[1] == 'B' && b[2] == 'I' && b[3] == '2';
}

static int wbi_is_all_zero(const unsigned char *b)
{
    for (usize i = 0; i < PILOT_WBI_HDR_SIZE; ++i) {
        if (b[i] != 0)
            return 0;
    }
    return 1;
}

static long save_wbi_backup_atomic(const char *path, const char *tmp_path,
                                   const char *parent_dir, const unsigned char *hdr,
                                   int preserve_existing, int *out_created)
{
    unsigned char check[PILOT_WBI_HDR_SIZE];
    if (out_created) *out_created = 0;
    int existed = 0;
    if (preserve_existing) {
        long rfd = open_file(path, O_RDONLY, 0);
        if (rfd >= 0) {
            existed = 1;
            int valid = file_size_equals(rfd, PILOT_WBI_HDR_SIZE) &&
                        pread_exact(rfd, check, PILOT_WBI_HDR_SIZE, 0) &&
                        wbi_is_magic(check);
            (void)close_file(rfd);
            if (valid)
                return 0;
        }
    }
    if (tmp_path) {
        (void)unlink_file(tmp_path);
        long fd = open_file(tmp_path, O_RDWR | O_CREAT | O_EXCL, 0644);
        if (fd < 0) return fd;
        int ok = pwrite_exact(fd, hdr, PILOT_WBI_HDR_SIZE, 0);
        long rc = ok ? sync_file(fd) : -2002;
        if (!rc && (!pread_exact(fd, check, PILOT_WBI_HDR_SIZE, 0) ||
                    !bytes_equal(check, hdr, PILOT_WBI_HDR_SIZE)))
            rc = -2003;
        (void)close_file(fd);
        if (rc < 0) {
            (void)unlink_file(tmp_path);
            return rc;
        }
        rc = rename_file(tmp_path, path);
        if (rc < 0) {
            (void)unlink_file(tmp_path);
            return rc;
        }
        if (!existed && out_created) *out_created = 1;
        if (parent_dir) {
            long drc = sync_dir(parent_dir);
            if (drc < 0) return drc;
        }
        return 0;
    }
    long fd = open_file(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return fd;
    int ok = pwrite_exact(fd, hdr, PILOT_WBI_HDR_SIZE, 0);
    long rc = ok ? sync_file(fd) : -2002;
    if (!rc && (!pread_exact(fd, check, PILOT_WBI_HDR_SIZE, 0) ||
                !bytes_equal(check, hdr, PILOT_WBI_HDR_SIZE)))
        rc = -2003;
    (void)close_file(fd);
    if (!rc && out_created) *out_created = 1;
    return rc;
}

static int read_valid_wbi_backup(const char *path, unsigned char *out_hdr)
{
    long rfd = open_file(path, O_RDONLY, 0);
    if (rfd < 0) return 0;
    int valid = file_size_equals(rfd, PILOT_WBI_HDR_SIZE) &&
                pread_exact(rfd, out_hdr, PILOT_WBI_HDR_SIZE, 0) &&
                wbi_is_magic(out_hdr);
    (void)close_file(rfd);
    return valid;
}

static int hdr_is_partial_zero_of(const unsigned char *hdr, const unsigned char *bak)
{
    int any_zeroed = 0;
    for (usize i = 0; i < PILOT_WBI_HDR_SIZE; ++i) {
        if (hdr[i] != 0 && hdr[i] != bak[i])
            return 0;
        if (hdr[i] == 0 && bak[i] != 0)
            any_zeroed = 1;
    }
    return any_zeroed;
}

/* Detect a partially zeroed /dev/nflasha5 header left by an interrupted clear:
 *   - Case 1: hdr is a byte-wise partial zero of an existing valid WBI2 backup
 *             in /usr/lib/pilot_wbi_hdr.bak or /tmp/jiritsu/pilotflash.wbi.bak.
 *   - Case 2: /usr/lib/pilot_wbi_hdr.bak holds a valid WBI2 backup AND the
 *             4-byte magic prefix hdr[0..3] is already zeroed (e.g. a later
 *             Pilot-era header whose host backup was already pulled).
 * Returns 1 if safe to resume zeroing, 0 otherwise. */
static int wbi_is_recoverable_partial_zero(const unsigned char *hdr,
                                           unsigned char *intact_bak_out,
                                           int *has_intact_bak)
{
    unsigned char bak[PILOT_WBI_HDR_SIZE];
    if (has_intact_bak) *has_intact_bak = 0;

    if (read_valid_wbi_backup(PILOT_WBI_TMP_BACKUP_PATH, bak) &&
        hdr_is_partial_zero_of(hdr, bak)) {
        if (intact_bak_out) {
            for (usize i = 0; i < PILOT_WBI_HDR_SIZE; ++i) intact_bak_out[i] = bak[i];
        }
        if (has_intact_bak) *has_intact_bak = 1;
        return 1;
    }
    if (read_valid_wbi_backup(PILOT_WBI_BACKUP_PATH, bak)) {
        if (hdr_is_partial_zero_of(hdr, bak)) {
            if (intact_bak_out) {
                for (usize i = 0; i < PILOT_WBI_HDR_SIZE; ++i) intact_bak_out[i] = bak[i];
            }
            if (has_intact_bak) *has_intact_bak = 1;
            return 1;
        }
        if (hdr[0] == 0 && hdr[1] == 0 && hdr[2] == 0 && hdr[3] == 0) {
            return 1;
        }
    }
    return 0;
}

/* Preflight WBI2 device and save backup BEFORE modifying appFw.so.
 * out_state: 0=already_clear, 1=needs_clear, 2=skipped (test env without nflasha5). */
static long preflight_and_backup_wbi(int *out_state, int *out_created_usr_bak)
{
    unsigned char hdr[PILOT_WBI_HDR_SIZE];
    unsigned char intact_bak[PILOT_WBI_HDR_SIZE];
    if (out_state) *out_state = 0;
    if (out_created_usr_bak) *out_created_usr_bak = 0;

    long fd = open_file(PILOT_WBI_DEV_PATH, O_RDWR, 0);
    if (fd < 0) {
#ifdef PILOT_FLASH_TEST
        if (fd == -ENOENT) {
            if (out_state) *out_state = 2;
            return 0;
        }
#endif
        return fd;
    }

    if (!pread_exact(fd, hdr, PILOT_WBI_HDR_SIZE, 0)) {
        (void)close_file(fd);
        return -2000;
    }
    (void)close_file(fd);

    if (wbi_is_all_zero(hdr)) {
        if (out_state) *out_state = 0;
        return 0;
    }

    /* Check partial-zero recovery first so even an out-of-order suffix zero
     * (magic intact, tail zeroed) preserves the intact backup instead of
     * overwriting the host staging copy with a half-zeroed header. */
    int has_intact = 0;
    if (wbi_is_recoverable_partial_zero(hdr, intact_bak, &has_intact)) {
        if (has_intact) {
            (void)save_wbi_backup_atomic(PILOT_WBI_TMP_BACKUP_PATH,
                                         PILOT_WBI_TMP_BACKUP_TMP_PATH,
                                         (const char *)0, intact_bak, 0, (int *)0);
        }
        if (out_state) *out_state = 1;
        return 0;
    }

    if (!wbi_is_magic(hdr)) {
        return -2001;
    }

    long bak_rc = save_wbi_backup_atomic(PILOT_WBI_BACKUP_PATH,
                                         PILOT_WBI_BACKUP_TMP_PATH,
                                         PILOT_USR_LIB_DIR, hdr, 1,
                                         out_created_usr_bak);
    if (bak_rc < 0) return bak_rc;

    /* Always verify staging the current WBI2 header to /tmp/jiritsu/pilotflash.wbi.bak:
     * when /usr/lib/pilot_wbi_hdr.bak already holds an older header, this staged file
     * is the ONLY backup of the current header — NEVER ignore write failures here! */
    long tmp_rc = save_wbi_backup_atomic(PILOT_WBI_TMP_BACKUP_PATH,
                                         PILOT_WBI_TMP_BACKUP_TMP_PATH,
                                         (const char *)0, hdr, 0, (int *)0);
    if (tmp_rc < 0) {
        if (out_created_usr_bak && *out_created_usr_bak) {
            (void)unlink_file(PILOT_WBI_BACKUP_PATH);
            (void)sync_dir(PILOT_USR_LIB_DIR);
            *out_created_usr_bak = 0;
        }
        return tmp_rc;
    }

    if (out_state) *out_state = 1;
    return 0;
}

static void cleanup_created_artifacts(int created_pilot, int created_wbi_bak, int clean_tmp_wbi)
{
    int dirty_dir = 0;
    if (clean_tmp_wbi) {
        (void)unlink_file(PILOT_WBI_TMP_BACKUP_PATH);
        (void)unlink_file(PILOT_WBI_TMP_BACKUP_TMP_PATH);
    }
    if (created_pilot) {
        (void)unlink_file(PILOT_BOOTSTRAP_PATH);
        dirty_dir = 1;
    }
    if (created_wbi_bak) {
        (void)unlink_file(PILOT_WBI_BACKUP_PATH);
        dirty_dir = 1;
    }
    if (dirty_dir)
        (void)sync_dir(PILOT_USR_LIB_DIR);
}

static long zero_wbi_header(void)
{
    unsigned char zeros[PILOT_WBI_HDR_SIZE];
    unsigned char verify[PILOT_WBI_HDR_SIZE];

    long fd = open_file(PILOT_WBI_DEV_PATH, O_RDWR, 0);
    if (fd < 0) return fd;

    for (usize i = 0; i < PILOT_WBI_HDR_SIZE; ++i)
        zeros[i] = 0;
    int ok = pwrite_exact(fd, zeros, PILOT_WBI_HDR_SIZE, 0);
    long rc = ok ? sync_file(fd) : -2004;
    if (!rc && (!pread_exact(fd, verify, PILOT_WBI_HDR_SIZE, 0) ||
                !wbi_is_all_zero(verify)))
        rc = -2005;
    (void)close_file(fd);
    return rc;
}

static void run_clear_wbi(void)
{
    long rw_result = remount_usr_rw(1);
    if (rw_result < 0) {
        emit_mount_failure(usr_statfs_stage ? "statfs_preflight" : "remount_rw",
                           rw_result);
        return;
    }
    int wbi_state = 0;
    int created_wbi_bak = 0;
    long wbi_rc = preflight_and_backup_wbi(&wbi_state, &created_wbi_bak);
    if (wbi_rc == 0 && wbi_state == 1) {
        wbi_rc = zero_wbi_header();
        /* Never delete WBI backups once zero_wbi_header() has been attempted. */
        (void)created_wbi_bak;
    } else if (wbi_rc < 0) {
        cleanup_created_artifacts(0, created_wbi_bak, 1);
    }
    long ro_result = remount_usr_ro();
    if (wbi_rc < 0) {
        emit_mount_failure(ro_result < 0 ? "wbi_clear_and_remount_ro" : "wbi_clear",
                           wbi_rc);
        return;
    }
    if (ro_result < 0) {
        emit_result("RESULT=FAIL\nreason=remount_ro\nmode=clear-wbi\nrecovery=do_not_reboot");
        return;
    }
    emit_result(wbi_state == 1
                    ? "RESULT=OK\nmode=clear-wbi\nwbi=cleared\nremount_ro=0"
                    : "RESULT=OK\nmode=clear-wbi\nwbi=already_clear\nremount_ro=0");
}

static void run_restore_wbi(void)
{
    unsigned char hdr[PILOT_WBI_HDR_SIZE];
    unsigned char current[PILOT_WBI_HDR_SIZE];
    unsigned char verify[PILOT_WBI_HDR_SIZE];

    long bfd = open_file(PILOT_WBI_BACKUP_PATH, O_RDONLY, 0);
    if (bfd < 0) {
        emit_mount_failure("wbi_backup_open", bfd);
        return;
    }
    int ok_read = file_size_equals(bfd, PILOT_WBI_HDR_SIZE) &&
                  pread_exact(bfd, hdr, PILOT_WBI_HDR_SIZE, 0) &&
                  wbi_is_magic(hdr);
    (void)close_file(bfd);
    if (!ok_read) {
        emit_result("RESULT=FAIL\nreason=wbi_backup_invalid");
        return;
    }

    long fd = open_file(PILOT_WBI_DEV_PATH, O_RDWR, 0);
    if (fd < 0) {
        emit_mount_failure("wbi_dev_open", fd);
        return;
    }
    /* Guard: refuse to overwrite a non-zero (active/newer) WBI2 header. */
    if (!pread_exact(fd, current, PILOT_WBI_HDR_SIZE, 0)) {
        (void)close_file(fd);
        emit_mount_failure("wbi_dev_read", -2008);
        return;
    }
    if (!wbi_is_all_zero(current)) {
        (void)close_file(fd);
        emit_result("RESULT=FAIL\nreason=wbi_dev_not_zero\nmode=restore-wbi");
        return;
    }

    int ok_wr = pwrite_exact(fd, hdr, PILOT_WBI_HDR_SIZE, 0);
    long rc = ok_wr ? sync_file(fd) : -2006;
    if (!rc && (!pread_exact(fd, verify, PILOT_WBI_HDR_SIZE, 0) ||
                !bytes_equal(verify, hdr, PILOT_WBI_HDR_SIZE)))
        rc = -2007;
    (void)close_file(fd);
    if (rc < 0) {
        emit_mount_failure("wbi_restore_write", rc);
        return;
    }
    emit_result("RESULT=OK\nmode=restore-wbi\nwbi=restored");
}

static void run_update_pilot(void)
{
    long rw_result = remount_usr_rw(1);
    if (rw_result < 0) {
        emit_mount_failure(usr_statfs_stage ? "statfs_preflight" : "remount_rw",
                           rw_result);
        return;
    }
    long bootstrap_result = install_bootstrap_mode(1, (int *)0);
    long ro_result = remount_usr_ro();
    if (bootstrap_result) {
        emit_mount_failure(ro_result < 0 ? "bootstrap_and_remount_ro" : "bootstrap_install",
                           bootstrap_result);
        return;
    }
    if (ro_result < 0) {
        emit_result("RESULT=FAIL\nreason=remount_ro\nrecovery=do_not_reboot");
        return;
    }
    emit_result("RESULT=OK\nmode=pilot\natomic=1\nfsync=0\nreadback=OK\nremount_ro=0");
}

/* Read-only probe of /dev/nflasha5 for check mode and purge guard.
 * Returns:
 *   0 = all-zero (safe/clear)
 *   1 = valid WBI2 header
 *   2 = recoverable partial-zero header
 *   3 = unknown non-zero header
 *   4 = skipped (PILOT_FLASH_TEST without /dev/nflasha5)
 *  <0 = open/read syscall error (-errno or -2000)
 */
static long probe_wbi_readonly(u64 *out_hdr4)
{
    unsigned char hdr[PILOT_WBI_HDR_SIZE];
    if (out_hdr4) *out_hdr4 = 0ULL;
    long fd = open_file(PILOT_WBI_DEV_PATH, O_RDONLY, 0);
    if (fd < 0) {
#ifdef PILOT_FLASH_TEST
        if (fd == -ENOENT) return 4;
#endif
        return fd;
    }
    if (!pread_exact(fd, hdr, PILOT_WBI_HDR_SIZE, 0)) {
        (void)close_file(fd);
        return -2000;
    }
    (void)close_file(fd);
    if (out_hdr4) {
        *out_hdr4 = ((u64)hdr[0] << 24) | ((u64)hdr[1] << 16) |
                    ((u64)hdr[2] << 8) | (u64)hdr[3];
    }
    if (wbi_is_all_zero(hdr)) return 0;
    if (wbi_is_recoverable_partial_zero(hdr, (unsigned char *)0, (int *)0)) return 2;
    if (wbi_is_magic(hdr)) return 1;
    return 3;
}

static const char *wbi_probe_label(long probe_rc)
{
    if (probe_rc == 0) return "zero";
    if (probe_rc == 1) return "wbi2";
    if (probe_rc == 2) return "partial_zero";
    if (probe_rc == 3) return "unknown";
    if (probe_rc == 4) return "absent_test";
    return "error";
}

static void run_check(const char *mode)
{
    long fd = open_file(APPFW_PATH, O_RDONLY, 0);
    if (fd < 0) {
        emit_result("RESULT=FAIL\nreason=open_read");
        return;
    }
    if (!file_size_equals(fd, pilot_ap_file_size)) {
        (void)close_file(fd);
        emit_result("RESULT=FAIL\nreason=file_size");
        return;
    }
    unsigned int mask = 0u;
    int state = image_state_mask(fd, &mask);
    (void)close_file(fd);

    /* Suggestion 4: collect read-only /usr statfs & /dev/nflasha5 header diagnostics
     * without remounting or touching any flash partition. */
    (void)read_usr_mount_flags();
    u64 wbi_hdr4 = 0ULL;
    long wbi_probe = probe_wbi_readonly(&wbi_hdr4);

    if (!state) {
        char output[PILOT_RESULT_CAP];
        usize length = 0;
        length = append_text(output, sizeof(output), length,
                             "A6700PILOTFLASH\nRESULT=FAIL\nreason=patch_windows_unknown\nusr_statfs_stage=");
        length = append_signed(output, sizeof(output), length, usr_statfs_stage);
        length = append_text(output, sizeof(output), length, "\nusr_flags=");
        length = append_hex(output, sizeof(output), length, usr_mount_flags);
        length = append_text(output, sizeof(output), length, "\nusr_free=");
        length = append_hex(output, sizeof(output), length, usr_free_bytes);
        length = append_text(output, sizeof(output), length, "\nwbi_status=");
        length = append_text(output, sizeof(output), length, wbi_probe_label(wbi_probe));
        length = append_text(output, sizeof(output), length, "\nwbi_hdr4=");
        length = append_hex(output, sizeof(output), length, wbi_hdr4);
        length = append_char(output, sizeof(output), length, '\n');
        write_result_buffer(output, length);
        return;
    }
    if (text_is(mode, "check")) {
        char output[PILOT_RESULT_CAP];
        usize length = 0;
        length = append_text(output, sizeof(output), length, "A6700PILOTFLASH\nRESULT=OK\nmode=check\nstate=");
        length = append_text(output, sizeof(output), length,
                             state == 1 ? "baseline" : (state == 2 ? "patched" : "mixed"));
        length = append_text(output, sizeof(output), length, "\nwindow_mask=");
        length = append_hex(output, sizeof(output), length, (u64)mask);
        length = append_text(output, sizeof(output), length, "\nusr_statfs_stage=");
        length = append_signed(output, sizeof(output), length, usr_statfs_stage);
        length = append_text(output, sizeof(output), length, "\nusr_magic=");
        length = append_hex(output, sizeof(output), length, usr_filesystem_magic);
        length = append_text(output, sizeof(output), length, "\nusr_flags=");
        length = append_hex(output, sizeof(output), length, usr_mount_flags);
        length = append_text(output, sizeof(output), length, "\nusr_free=");
        length = append_hex(output, sizeof(output), length, usr_free_bytes);
        length = append_text(output, sizeof(output), length, "\nwbi_status=");
        length = append_text(output, sizeof(output), length, wbi_probe_label(wbi_probe));
        if (wbi_probe < 0) {
            length = append_text(output, sizeof(output), length, "\nwbi_probe_rc=");
            length = append_signed(output, sizeof(output), length, wbi_probe);
        } else {
            length = append_text(output, sizeof(output), length, "\nwbi_hdr4=");
            length = append_hex(output, sizeof(output), length, wbi_hdr4);
        }
        length = append_char(output, sizeof(output), length, '\n');
        write_result_buffer(output, length);
    }
}

/* Remove /usr/lib/pilot.so and /usr/lib/pilot_wbi_hdr.bak ONLY when:
 *   1. appFw.so matches stock baseline windows (state == 1), AND
 *   2. /dev/nflasha5 WBI2 header is already all-zero (Suggestion 3: refuse while
 *      a live snapshot could still resurrect Pilot-era pages or need the backup), AND
 *   3. Post-unlink verification confirms -ENOENT on both paths. */
static void run_purge(void)
{
    long fd = open_file(APPFW_PATH, O_RDONLY, 0);
    if (fd < 0) {
        emit_result("RESULT=FAIL\nreason=open_read");
        return;
    }
    if (!file_size_equals(fd, pilot_ap_file_size)) {
        (void)close_file(fd);
        emit_result("RESULT=FAIL\nreason=file_size");
        return;
    }
    int state = image_state(fd);
    (void)close_file(fd);
    if (state != 1) {
        emit_result("RESULT=FAIL\nreason=appfw_not_baseline\nmode=purge");
        return;
    }

    long wbi_probe = probe_wbi_readonly((u64 *)0);
    if (wbi_probe < 0) {
        emit_mount_failure("purge_wbi_probe", wbi_probe);
        return;
    }
    if (wbi_probe != 0 && wbi_probe != 4) {
        emit_result("RESULT=FAIL\nreason=wbi_not_clear\nmode=purge");
        return;
    }

    long rw_result = remount_usr_rw(0);
    if (rw_result < 0) {
        emit_mount_failure(usr_statfs_stage ? "statfs_preflight" : "remount_rw",
                           rw_result);
        return;
    }

    long u1 = unlink_file(PILOT_BOOTSTRAP_PATH);
    if (u1 < 0 && u1 != -ENOENT) {
        long ro_result = remount_usr_ro();
        emit_mount_failure(ro_result < 0 ? "purge_pilot_and_remount_ro" : "purge_pilot", u1);
        return;
    }
    (void)unlink_file(PILOT_BOOTSTRAP_TMP_PATH);

    long u2 = unlink_file(PILOT_WBI_BACKUP_PATH);
    if (u2 < 0 && u2 != -ENOENT) {
        long ro_result = remount_usr_ro();
        emit_mount_failure(ro_result < 0 ? "purge_wbi_bak_and_remount_ro" : "purge_wbi_bak", u2);
        return;
    }
    (void)unlink_file(PILOT_WBI_BACKUP_TMP_PATH);

    long dsync = sync_dir(PILOT_USR_LIB_DIR);

    /* Confirm both removed paths now return -ENOENT directly from the kernel. */
    long v1 = open_file(PILOT_BOOTSTRAP_PATH, O_RDONLY, 0);
    if (v1 >= 0) {
        (void)close_file(v1);
        long ro_result = remount_usr_ro();
        emit_mount_failure(ro_result < 0 ? "purge_verify_pilot_and_remount_ro" : "purge_verify_pilot", -17);
        return;
    }
    if (v1 != -ENOENT) {
        long ro_result = remount_usr_ro();
        emit_mount_failure(ro_result < 0 ? "purge_verify_pilot_and_remount_ro" : "purge_verify_pilot", v1);
        return;
    }
    long v2 = open_file(PILOT_WBI_BACKUP_PATH, O_RDONLY, 0);
    if (v2 >= 0) {
        (void)close_file(v2);
        long ro_result = remount_usr_ro();
        emit_mount_failure(ro_result < 0 ? "purge_verify_wbi_and_remount_ro" : "purge_verify_wbi", -17);
        return;
    }
    if (v2 != -ENOENT) {
        long ro_result = remount_usr_ro();
        emit_mount_failure(ro_result < 0 ? "purge_verify_wbi_and_remount_ro" : "purge_verify_wbi", v2);
        return;
    }

    long ro_result = remount_usr_ro();
    if (dsync < 0) {
        emit_mount_failure(ro_result < 0 ? "purge_dirsync_and_remount_ro" : "purge_dirsync", dsync);
        return;
    }
    if (ro_result < 0) {
        emit_result("RESULT=FAIL\nreason=remount_ro\nmode=purge\nrecovery=do_not_reboot");
        return;
    }
    emit_result("RESULT=OK\nmode=purge\nstate=baseline\nbootstrap_removed=1\nwbi_bak_removed=1\nwbi=already_clear\nverify=enoent\nremount_ro=0");
}

static void run_write(const char *mode, int target_patched)
{
    long read_fd = open_file(APPFW_PATH, O_RDONLY, 0);
    if (read_fd < 0) {
        emit_result("RESULT=FAIL\nreason=open_read");
        return;
    }
    if (!file_size_equals(read_fd, pilot_ap_file_size)) {
        (void)close_file(read_fd);
        emit_result("RESULT=FAIL\nreason=file_size");
        return;
    }
    unsigned int before_mask = 0u;
    int before_state = image_state_mask(read_fd, &before_mask);
    (void)close_file(read_fd);

    /* Accept baseline (1), patched (2), AND valid mixed (3) states;
     * reject only unknown (0) where at least one window is unrecognized. */
    if (before_state == 0) {
        emit_result("RESULT=FAIL\nreason=patch_windows_unknown");
        return;
    }

    int desired_state = target_patched ? 2 : 1;
    unsigned int desired_mask = target_patched ? 0xfu : 0x0u;

    long rw_result = remount_usr_rw(1);
    if (rw_result < 0) {
        emit_mount_failure(usr_statfs_stage ? "statfs_preflight" : "remount_rw",
                           rw_result);
        return;
    }

    /* Preflight + back up WBI2 BEFORE modifying pilot.so or appFw.so. */
    int wbi_state = 0;
    int created_wbi_bak = 0;
    int created_pilot = 0;
    long wbi_pre_rc = preflight_and_backup_wbi(&wbi_state, &created_wbi_bak);
    if (wbi_pre_rc < 0) {
        cleanup_created_artifacts(0, created_wbi_bak, 1);
        long ro_result = remount_usr_ro();
        emit_mount_failure(ro_result < 0 ? "wbi_preflight_and_remount_ro" : "wbi_preflight",
                           wbi_pre_rc);
        return;
    }

    if (target_patched) {
        long bootstrap_result = install_bootstrap(&created_pilot);
        if (bootstrap_result) {
            cleanup_created_artifacts(created_pilot, created_wbi_bak, 1);
            long ro_result = remount_usr_ro();
            emit_mount_failure(ro_result < 0 ? "bootstrap_and_remount_ro" : "bootstrap_install",
                               bootstrap_result);
            return;
        }
    }

    /* Suggestion 1: Clear WBI2 BEFORE modifying appFw.so on BOTH install and restore.
     * Eliminates "patched appFw + stale WBI" and "baseline appFw + Pilot-era WBI"
     * intermediate states: if clearing fails, appFw.so is still completely untouched. */
    if (wbi_state == 1) {
        long wbi_rc = zero_wbi_header();
        if (wbi_rc < 0) {
            /* Never delete WBI backups once zero_wbi_header() was attempted;
             * only clean up a newly created pilot.so since appFw.so is untouched. */
            cleanup_created_artifacts(created_pilot, 0, 0);
            long ro_result = remount_usr_ro();
            emit_mount_failure(ro_result < 0 ? "wbi_clear_and_remount_ro" : "wbi_clear",
                               wbi_rc);
            return;
        }
    }

    /* Idempotent fast path: appFw.so already in desired state -> 0 writes to appFw.so;
     * WBI2 has already been cleared above if it was active. */
    if (before_state == desired_state) {
        long ro_result = remount_usr_ro();
        if (ro_result < 0) {
            emit_result(target_patched
                            ? "RESULT=FAIL\nreason=remount_ro\nstate=patched\nrecovery=do_not_reboot"
                            : "RESULT=FAIL\nreason=remount_ro\nstate=baseline\nrecovery=do_not_reboot");
            return;
        }
        if (target_patched) {
            emit_result(wbi_state == 1
                            ? "RESULT=OK\nmode=write\nstate=patched\nalready=1\nwbi=cleared\nremount_ro=0"
                            : "RESULT=OK\nmode=write\nstate=patched\nalready=1\nwbi=already_clear\nremount_ro=0");
        } else {
            emit_result(wbi_state == 1
                            ? "RESULT=OK\nmode=restore\nstate=baseline\nalready=1\nwbi=cleared\nremount_ro=0"
                            : "RESULT=OK\nmode=restore\nstate=baseline\nalready=1\nwbi=already_clear\nremount_ro=0");
        }
        return;
    }

    long fd = open_file(APPFW_PATH, O_RDWR, 0);
    if (fd < 0) {
        cleanup_created_artifacts(created_pilot, 0, 0);
        long ro_result = remount_usr_ro();
        emit_result(ro_result < 0
                        ? "RESULT=FAIL\nreason=open_rw_and_remount_ro"
                        : "RESULT=FAIL\nreason=open_rw");
        return;
    }

    unsigned int mask_after_open = 0u;
    int state_after_open = image_state_mask(fd, &mask_after_open);
    if (state_after_open != before_state || mask_after_open != before_mask) {
        (void)close_file(fd);
        cleanup_created_artifacts(created_pilot, 0, 0);
        long ro_result = remount_usr_ro();
        emit_result(ro_result < 0
                        ? "RESULT=FAIL\nreason=state_changed_and_remount_ro"
                        : "RESULT=FAIL\nreason=state_changed");
        return;
    }

    int write_ok = write_mask(fd, desired_mask);
    unsigned int mask_after_write = 0u;
    int state_after_write = image_state_mask(fd, &mask_after_write);
    if (!write_ok || state_after_write != desired_state || mask_after_write != desired_mask) {
        /* Roll back to exact pre-run per-window contents (before_mask),
         * never assuming the pre-run state was strictly all-baseline or all-patched. */
        int rollback_ok = write_mask(fd, before_mask);
        unsigned int rollback_mask = 0u;
        int rollback_state = image_state_mask(fd, &rollback_mask);
        (void)close_file(fd);
        if (rollback_ok && rollback_state == before_state && rollback_mask == before_mask)
            cleanup_created_artifacts(created_pilot, 0, 0);
        long ro_result = remount_usr_ro();
        if (!rollback_ok || rollback_state != before_state || rollback_mask != before_mask)
            emit_result("RESULT=FAIL\nreason=rollback_failed\nrecovery=do_not_reboot");
        else if (ro_result < 0)
            emit_result("RESULT=FAIL\nreason=remount_ro_after_rollback\nstate=restored\nrecovery=do_not_reboot");
        else
            emit_result("RESULT=FAIL\nreason=write_or_readback\nstate=restored");
        return;
    }

    (void)close_file(fd);
    long ro_result = remount_usr_ro();
    if (ro_result < 0) {
        emit_result(target_patched
                        ? "RESULT=FAIL\nreason=remount_ro\nstate=patched\nrecovery=do_not_reboot"
                        : "RESULT=FAIL\nreason=remount_ro\nstate=baseline\nrecovery=do_not_reboot");
        return;
    }
    char output[PILOT_RESULT_CAP];
    usize length = 0;
    length = append_text(output, sizeof(output), length, "A6700PILOTFLASH\nRESULT=OK\nmode=");
    length = append_text(output, sizeof(output), length, target_patched ? "write" : "restore");
    length = append_text(output, sizeof(output), length, "\nstate=");
    length = append_text(output, sizeof(output), length, target_patched ? "patched" : "baseline");
    if (before_state == 3)
        length = append_text(output, sizeof(output), length, "\nrecovered_mixed=1");
    length = append_text(output, sizeof(output), length, "\nwindows=4\nbarrier=2stage\nfsync=0\nreadback=OK\nwbi=");
    length = append_text(output, sizeof(output), length, wbi_state == 1 ? "cleared" : "already_clear");
    length = append_text(output, sizeof(output), length, "\nremount_ro=0");
    if (usr_was_already_rw)
        length = append_text(output, sizeof(output), length, "\nusr_was_rw=1");
    length = append_char(output, sizeof(output), length, '\n');
    write_result_buffer(output, length);
    (void)mode;
}

#ifdef PILOT_FLASH_HOST_TEST
void pilot_flash_run_once(void)
#else
__attribute__((visibility("default"), used))
void _init(void)
#endif
{
    char mode[24];
    if (!read_mode(mode, sizeof(mode))) {
        emit_result("RESULT=FAIL\nreason=no_mode");
        return;
    }
    if ((text_is(mode, "check") || text_is(mode, "write") ||
         text_is(mode, "restore") || text_is(mode, "purge")) &&
        !load_patch_plan()) {
        emit_result("RESULT=FAIL\nreason=invalid_plan");
        return;
    }
    if (text_is(mode, "check")) {
        run_check(mode);
    } else if (text_is(mode, "write")) {
        run_write(mode, 1);
    } else if (text_is(mode, "restore")) {
        run_write(mode, 0);
    } else if (text_is(mode, "pilot")) {
        run_update_pilot();
    } else if (text_is(mode, "clear-wbi")) {
        run_clear_wbi();
    } else if (text_is(mode, "restore-wbi")) {
        run_restore_wbi();
    } else if (text_is(mode, "purge")) {
        run_purge();
    } else {
        emit_result("RESULT=FAIL\nreason=unknown_mode");
    }
}
