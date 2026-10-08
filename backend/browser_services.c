#include "browser_platform.h"
#ifndef ORBIT_DESKTOP
#include <dlfcn.h>
#ifndef ORBIT_BROWSER_PROBE
#include "orbit.h"
#define STAGE(phase, status, result) diagnostics_stage(phase, status, result)
#else
#define STAGE(phase, status, result) ((void)(result))
#endif

/* Only the explicit browser-session worker calls this. Keep loaded handles for
 * the process lifetime: opening the browser is asynchronous. No native service
 * import, lookup or initialization belongs on Orbit's normal startup path. */
int probe_browser_open(const char *url) {
    static void *system_service, *user_service;
    if (!url || !*url) return -1;
    STAGE("browser-modules", "started", 0);
    if (!system_service)
        system_service = dlopen("libSceSystemService.sprx", RTLD_NOW | RTLD_LOCAL);
    if (!system_service) {
        STAGE("browser-modules", "error", -1);
        return -1;
    }
    if (!user_service)
        user_service = dlopen("libSceUserService.sprx", RTLD_NOW | RTLD_LOCAL);
    if (!user_service) {
        STAGE("browser-modules", "error", -2);
        return -2;
    }
    int (*initialize)(void *) = (int (*)(void *))dlsym(user_service, "sceUserServiceInitialize");
    int (*terminate)(void) = (int (*)(void))dlsym(user_service, "sceUserServiceTerminate");
    int (*launch)(const char *, void *) =
        (int (*)(const char *, void *))dlsym(system_service, "sceSystemServiceLaunchWebBrowser");
    if (!initialize || !terminate || !launch) {
        STAGE("browser-modules", "error", -3);
        return -3;
    }
    STAGE("browser-modules", "ok", 0);
    STAGE("browser-user-service", "started", 0);
    int result = initialize(NULL);
    STAGE("browser-user-service", result ? "error" : "ok", result);
    if (result) return result;
    STAGE("browser-launch", "started", 0);
    result = launch(url, NULL);
    STAGE("browser-launch", result ? "error" : "ok", result);
    STAGE("browser-user-cleanup", "started", 0);
    int cleanup = terminate();
    STAGE("browser-user-cleanup", cleanup ? "error" : "ok", cleanup);
    return result;
}
#else
int probe_browser_open(const char *url) { (void)url; return -1; }
#endif
