#ifndef ORBIT_PROCESS_IDENTITY_H
#define ORBIT_PROCESS_IDENTITY_H
typedef enum {
    ORBIT_PROCESS_UNCHECKED,
    ORBIT_PROCESS_DESKTOP,
    ORBIT_PROCESS_VALID,
    ORBIT_PROCESS_CORRECTED,
    ORBIT_PROCESS_SHARED,
    ORBIT_PROCESS_UNAVAILABLE,
    ORBIT_PROCESS_SET_FAILED,
    ORBIT_PROCESS_VERIFY_FAILED
} OrbitProcessState;
typedef struct {
    OrbitProcessState state;
    int original_length, current_length;
    int native_result;
} OrbitProcessIdentity;
OrbitProcessIdentity orbit_process_identity(void);
const char *orbit_process_state(OrbitProcessState state);
#endif
