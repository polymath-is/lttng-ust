#ifndef _USTCMD_H
#define _USTCMD_H

#include <stdio.h>
#include <unistd.h>
#include <getopt.h>
#include <stdlib.h>
#include <fcntl.h>

#include "ustcomm.h"
#include "ustcmd.h"

#define USTCMD_ERR_CONN		1 /* Process connection error */
#define USTCMD_ERR_ARG		2 /* Invalid function argument */
#define USTCMD_ERR_GEN		3 /* General ustcmd error */

#define USTCMD_MS_CHR_OFF	'0' /* Marker state 'on' character */
#define USTCMD_MS_CHR_ON	'1' /* Marker state 'on' character */
#define USTCMD_MS_OFF		0   /* Marker state 'on' value */
#define USTCMD_MS_ON		1   /* Marker state 'on' value */

#define USTCMD_SOCK_PATH	"/tmp/socks/"

/* Channel/marker/state/format string (cmsf) info. structure */
struct marker_status {
	char *channel; /* Channel name (end of marker_status array if NULL) */
	char *marker; /* Marker name (end of marker_status array if NULL) */
	int state; /* State (0 := marker disabled, 1 := marker enabled) */
	char *fs; /* Format string (end of marker_status array if NULL) */
};

extern pid_t *ustcmd_get_online_pids(void);
extern int ustcmd_set_marker_state(const char *, int, pid_t);
extern int ustcmd_destroy_trace(pid_t);
extern int ustcmd_setup_and_start(pid_t);
extern int ustcmd_stop_trace(pid_t);
extern int ustcmd_start_trace(pid_t);
extern int ustcmd_free_cmsf(struct marker_status *);
extern unsigned int ustcmd_count_nl(const char *);
extern int ustcmd_send_cmd(const char *, pid_t, char **);
extern int ustcmd_get_cmsf(struct marker_status **, pid_t);

#endif /* _USTCMD_H */
