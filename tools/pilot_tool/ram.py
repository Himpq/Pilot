"""Generate/build a Pilot module from a checked RAM patch description."""
from __future__ import annotations

import hashlib
import json
import subprocess
from pathlib import Path

from .spec import hex_bytes, integer, validate_spec


def _array(name, data):
    return f"static const uint8_t {name}[] = {{" + ",".join(f"0x{x:02x}" for x in data) + "};"


def generate_module(spec, restore=False):
    validate_spec(spec)
    if spec["kind"] != "ram":
        raise ValueError("RAM description required")
    arrays, rows = [], []
    for index, site in enumerate(spec["patches"]):
        for key in ("before", "after"):
            arrays.append(_array(f"{key}_{index}", hex_bytes(site[key])))
        space = "PILOT_PATCH_SPACE_AP" if site["space"] == "ap" else "PILOT_PATCH_SPACE_CP"
        rows.append(f"{{{json.dumps(site['id'])},{space},UINT64_C({integer(site['address'])}),before_{index},after_{index},sizeof(before_{index})}}")
    arrays.append(_array("target_hash", bytes.fromhex(spec["target"]["sha256"])))
    return '''#define _GNU_SOURCE
#include "pilot_patch_runtime.h"
#include "pilot_sha256.h"
#include <link.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

''' + "\n".join(arrays) + '''
static const PilotPatchSite templates[] = {
''' + ",\n".join(rows) + '''
};
struct image_base { uintptr_t base; int found; };
static int find_appfw(struct dl_phdr_info *info, size_t size, void *opaque) {
    (void)size;
    const char *name=info->dlpi_name;
    if (!name) return 0;
    const char *slash=strrchr(name,'/');
    if (strcmp(slash ? slash+1 : name,"appFw.so") != 0) return 0;
    struct image_base *image=opaque; image->base=info->dlpi_addr; image->found=1; return 1;
}
__attribute__((visibility("default")))
PilotStatus pilot_module_init(const PilotApi *api) {
    if (!api || api->abi_version != PILOT_ABI_VERSION || api->struct_size < sizeof(PilotApi)) return PILOT_STATUS_UNSUPPORTED;
    uint8_t hash[32];
    if (pilot_sha256_file("/usr/lib/appFw.so",UINT64_C(''' + str(spec["target"]["size_bytes"]) + '''),hash) != 0 || memcmp(hash,target_hash,32) != 0) {
        if (api->log && api->log->print) api->log->print("PILOT_PATCH target appFw SHA-256 mismatch");
        return PILOT_STATUS_CONFLICT;
    }
    PilotPatchSite sites[sizeof(templates)/sizeof(templates[0])];
    memcpy(sites,templates,sizeof(sites));
    struct image_base image={0,0};
    dl_iterate_phdr(find_appfw,&image);
    if (!image.found) return PILOT_STATUS_NOT_FOUND;
    for (size_t i=0;i<sizeof(sites)/sizeof(sites[0]);i++) {
        if (sites[i].space == PILOT_PATCH_SPACE_AP) {
            if (sites[i].address > UINTPTR_MAX-image.base) return PILOT_STATUS_RANGE;
            sites[i].address += image.base;
        }
    }
    PilotPatchReport report;
    PilotStatus status=pilot_patch_apply_sites(api,sites,sizeof(sites)/sizeof(sites[0]),''' + str(int(restore)) + ''',&report);
    char line[160];
    snprintf(line,sizeof(line),"PILOT_PATCH complete checked=%zu attempted=%zu changed=%zu failed_index=%zu status=%d",report.checked,report.attempted,report.changed,report.failed_index,(int)status);
    if (api->log && api->log->print) api->log->print(line);
    return status;
}
'''


def build_module(spec, out: Path, cc: str, sysroot: Path, restore=False):
    source = generate_module(spec, restore)
    root = Path(__file__).resolve().parents[2]
    out = out.resolve()
    if out.exists():
        raise FileExistsError(out)
    if not sysroot.is_dir():
        raise ValueError("explicit target sysroot directory required")
    out.parent.mkdir(parents=True, exist_ok=True)
    generated = out.with_suffix(".c")
    if generated.exists():
        raise FileExistsError(generated)
    generated.write_text(source, encoding="utf-8", newline="\n")
    runtime = root / "tools/runtime"
    command = [cc, f"--sysroot={sysroot}", "-std=c11", "-O2", "-march=armv8-a", "-mno-outline-atomics",
               "-fno-stack-protector", "-fPIC", "-fvisibility=hidden", "-Wall", "-Wextra", "-Werror",
               "-shared", "-nostdlib", "-Wl,--no-undefined", "-Wl,--no-as-needed", "-Wl,--build-id=none",
               "-I", str(root / "patch"), "-I", str(runtime), str(generated),
               str(runtime / "pilot_patch_runtime.c"), str(runtime / "pilot_sha256.c"),
               "-L", str(sysroot / "lib"), "-l:libdl.so.2", "-l:libc.so.6", "-lgcc", "-o", str(out)]
    subprocess.run(command, check=True)
    blob = out.read_bytes()
    if len(blob) < 64 or blob[:6] != b"\x7fELF\x02\x01" or int.from_bytes(blob[18:20], "little") != 183:
        raise ValueError("compiler did not produce an AArch64 ELF64 module")
    return {"module": str(out), "source": str(generated), "sha256": hashlib.sha256(blob).hexdigest(), "size_bytes": len(blob)}
