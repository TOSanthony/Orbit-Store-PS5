#include "process_identity.h"
#include <string.h>
#if !defined(ORBIT_DESKTOP) && defined(ORBIT_PROCESS_IDENTITY_EXPERIMENT)
#include <ps5/kernel.h>
#include <unistd.h>
typedef int (*get_name_fn)(int, char *);
typedef int (*set_name_fn)(const char *);
#endif

const char *orbit_process_state(OrbitProcessState state) {
    switch (state) {
    case ORBIT_PROCESS_DESKTOP: return "desktop";
    case ORBIT_PROCESS_VALID: return "name-valid";
    case ORBIT_PROCESS_CORRECTED: return "short-name-corrected";
    case ORBIT_PROCESS_SHARED: return "shared-process-unchanged";
    case ORBIT_PROCESS_UNAVAILABLE: return "unavailable";
    case ORBIT_PROCESS_SET_FAILED: return "rename-failed";
    case ORBIT_PROCESS_VERIFY_FAILED: return "verification-failed";
    default: return "not-checked";
    }
}

OrbitProcessIdentity orbit_process_identity(void) {
    OrbitProcessIdentity info = {.state = ORBIT_PROCESS_UNCHECKED,
                                 .original_length = -1, .current_length = -1};
#ifdef ORBIT_DESKTOP
    info.state = ORBIT_PROCESS_DESKTOP;
#elif defined(ORBIT_PROCESS_IDENTITY_EXPERIMENT)
    info.state = ORBIT_PROCESS_UNAVAILABLE;
    /* Use the pinned SDK's own shared/hijacked-process guard (crt/crt.c).
     * Never rename a browser/system process hosting an injected payload. */
    if (kernel_dynlib_dlsym(-1, 0x2001, "sceKernelDlsym")) {
        info.state = ORBIT_PROCESS_SHARED;
        return info;
    }
    /* Optional lookup avoids adding mandatory imports on unfamiliar firmware. */
    get_name_fn get_name = (get_name_fn)kernel_dynlib_dlsym(-1, 0x1, "sceKernelGetProcessName");
    if (!get_name) return info;
    char name[64] = {0};
    info.native_result = get_name(getpid(), name);
    if (info.native_result || !memchr(name, 0, sizeof name)) return info;
    info.original_length = info.current_length = (int)strlen(name);
    if (info.original_length > 2) {
        info.state = ORBIT_PROCESS_VALID;
        return info;
    }
    /* etaHEN's short-name lookup branch can match unrelated payloads. Limit the
     * workaround to our own standalone process, only when its name is short. */
    set_name_fn set_name = (set_name_fn)kernel_dynlib_dlsym(-1, 0x1, "sceKernelSetProcessName");
    if (!set_name) return info;
    info.native_result = set_name("orbit_store.elf");
    info.current_length = -1;
    if (info.native_result) {
        info.state = ORBIT_PROCESS_SET_FAILED;
        return info;
    }
    memset(name, 0, sizeof name);
    info.native_result = get_name(getpid(), name);
    if (!info.native_result && memchr(name, 0, sizeof name))
        info.current_length = (int)strlen(name);
    info.state = info.current_length == (int)sizeof("orbit_store.elf") - 1 && !strcmp(name, "orbit_store.elf")
                     ? ORBIT_PROCESS_CORRECTED : ORBIT_PROCESS_VERIFY_FAILED;
#endif
    return info;
}
