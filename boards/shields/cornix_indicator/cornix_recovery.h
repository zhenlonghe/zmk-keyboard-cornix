#ifndef CORNIX_RECOVERY_H
#define CORNIX_RECOVERY_H

/* Arms the hardware watchdog on first call, then feeds it. Call from the
 * indicator work loop so the watchdog only runs while the feeder is alive.
 */
void cornix_recovery_feed(void);

#endif
