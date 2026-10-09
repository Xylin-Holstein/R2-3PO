#ifndef R2_REMOTE_H
#define R2_REMOTE_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Authenticated HTTP gateway for the remote companion app.
 * R2's core remains the only conversation/identity implementation.
 *
 * Configure R2_REMOTE_TOKEN before launching R2. Optional:
 * R2_REMOTE_PORT (default 8765).
 *
 * Bind is intentionally on all interfaces so it can be reached over
 * a private overlay such as Tailscale. Restrict access with the host
 * firewall/tailnet ACLs; never port-forward this port publicly.
 */
int r2_remote_start(void);
void r2_remote_stop(void);
int r2_remote_is_running(void);

#ifdef __cplusplus
}
#endif
#endif
