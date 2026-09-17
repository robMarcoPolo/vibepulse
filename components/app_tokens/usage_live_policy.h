#ifndef USAGE_LIVE_POLICY_H
#define USAGE_LIVE_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#include "agent_status.h"

typedef struct {
  char context[64];
  bool halo_active;
} usage_live_header_view;

typedef struct {
  bool has_total;
  bool has_today;
  int total_px;
  int baseline_px;
  int today_px;
} usage_today_bar_view;

/* The bar's white line is the WINDOW'S OWN CLOCK, not a usage figure: it sits
 * at elapsed/window across the track, so fill left of it means the quota is
 * being spent slower than the window is passing, and fill right of it means
 * faster. Returns false — draw nothing — when the window length is unknown,
 * which is the honest answer whenever the service could not name the window.
 * A reset further out than the window (host/API clock skew, which the
 * statusline bridge explicitly tolerates) clamps to the window's start rather
 * than blinking the marker away. */
bool usage_live_elapsed_marker_px(int window_min, int reset_min,
                                  int track_width, int *marker_px);

typedef enum {
  USAGE_UPDATE_DIRECT,
  USAGE_UPDATE_ANIMATE_FORWARD,
  USAGE_UPDATE_SNAP_BACKWARD,
  USAGE_UPDATE_SILENT,
} usage_update_mode;

void usage_live_build_header(const tk_agent_provider_status *provider,
                             uint64_t packet_age_ms, bool data_stale,
                             bool has_agent_data,
                             usage_live_header_view *out);

bool usage_live_build_today_bar(double total_pct, bool has_total,
                                double today_pct, bool has_today,
                                int track_width, usage_today_bar_view *out);

usage_update_mode usage_live_choose_update(bool initialized, bool stale,
                                           bool visible, bool has_old,
                                           double old_pct, bool has_new,
                                           double new_pct);

#endif
