/* compat: 下游 subsystem_get/put/restart -> 由 compat.c 接 NPU PAS 启动 */
#ifndef _COMPAT_SUBSYSTEM_RESTART_H
#define _COMPAT_SUBSYSTEM_RESTART_H
void *subsystem_get(const char *name);
void subsystem_put(void *subsystem);
int subsystem_restart(const char *name);
#endif
