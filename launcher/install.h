#ifndef ORBIT_INSTALL_H
#define ORBIT_INSTALL_H
/* Written only by startup; copy the result before publishing it to the API. */
const char *orbit_launcher_last_error(void);
const char *orbit_launcher_registration_method(void);
void orbit_launcher_record_error(const char *step, int system_error, int platform_error);
int orbit_launcher_ensure(int state_fd);
/* Injectable platform operations permit filesystem tests without a console. */
int orbit_launcher_ensure_at(int state_fd, const char *user_parent, int (*prepare)(void),
                             int (*register_title)(void));
#endif
