/*
 * eduke32 entry point on the SF2000 / GB300 -- shared declarations.
 */
#ifndef EDUKE32_ENTRY_H
#define EDUKE32_ENTRY_H

#ifdef __cplusplus
extern "C" {
#endif

/* Spawns eduke32's main() on a libco coroutine. The returned handle is passed
 * back to co_resume() once per retro_run(). NULL on failure. */
void *sf2k_entry_start(const char *dir);

/* Destroys the coroutine. */
void sf2k_entry_stop(void);

/* 0 if <dir>/DUKE3D.GRP exists, -1 otherwise. */
int sf2k_game_data_present(const char *dir);

/* Writes the shipped default bindings to <dir>/autoexec.cfg unless one is
 * already there. 0 on success (including "already present"), -1 on failure. */
int sf2k_install_default_config(const char *dir);

#ifdef __cplusplus
}
#endif

#endif /* EDUKE32_ENTRY_H */