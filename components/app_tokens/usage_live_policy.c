#include "usage_live_policy.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "agent_monitor_policy.h"

static bool is_effectively_active(tk_agent_state state,
                                  uint64_t packet_age_ms) {
  if (state == TK_AGENT_WORKING) return true;
  return (state == TK_AGENT_WAITING || state == TK_AGENT_ERROR) &&
         packet_age_ms <= TK_AGENT_WORKING_LEASE_MS;
}

static void build_now_context(const tk_agent_status *working,
                              usage_live_header_view *out) {
  const bool has_model = working->has_model && working->model[0];
  const bool has_effort = working->has_effort && working->effort[0];
  if (has_model && has_effort) {
    snprintf(out->context, sizeof out->context, "NOW · %s · %s",
             working->model, working->effort);
  } else if (has_model) {
    snprintf(out->context, sizeof out->context, "NOW · %s", working->model);
  } else if (has_effort) {
    snprintf(out->context, sizeof out->context, "NOW · %s", working->effort);
  } else {
    snprintf(out->context, sizeof out->context, "NOW");
  }
}

void usage_live_build_header(const tk_agent_provider_status *provider,
                             uint64_t packet_age_ms, bool data_stale,
                             bool has_agent_data,
                             usage_live_header_view *out) {
  if (!out) return;
  memset(out, 0, sizeof *out);
  if (!has_agent_data || !provider || data_stale) return;

  const uint8_t job_count = provider->job_count < TK_AGENT_JOBS_MAX
                                ? provider->job_count
                                : TK_AGENT_JOBS_MAX;
  unsigned active_count = 0;
  const tk_agent_status *working = NULL;
  for (uint8_t i = 0; i < job_count; i++) {
    tk_agent_state state =
        tk_agent_monitor_effective_state(&provider->jobs[i], packet_age_ms);
    if (!is_effectively_active(state, packet_age_ms)) continue;
    active_count++;
    if (state == TK_AGENT_WORKING) working = &provider->jobs[i];
  }

  out->halo_active = working != NULL;
  if (active_count == 0) {
    snprintf(out->context, sizeof out->context, "NO ACTIVE AGENT");
  } else if (active_count == 1 && working) {
    build_now_context(working, out);
  } else if (active_count == 1) {
    snprintf(out->context, sizeof out->context, "1 AGENT ACTIVE");
  } else {
    snprintf(out->context, sizeof out->context, "%u AGENTS ACTIVE",
             active_count);
  }
}

bool usage_live_elapsed_marker_px(int window_min, int reset_min,
                                  int track_width, int *marker_px) {
  if (!marker_px || window_min <= 0 || track_width <= 0 || reset_min < 0) {
    return false;
  }
  /* The service may report a reset slightly beyond the window's own length —
   * the statusline bridge allows fifteen minutes of clock slack for exactly
   * this. Treat that as "the window just began" rather than dropping the
   * marker, which would make it blink out at every window boundary. */
  int elapsed_min = window_min - reset_min;
  if (elapsed_min < 0) elapsed_min = 0;
  if (elapsed_min > window_min) elapsed_min = window_min;
  *marker_px = (int)llround((double)elapsed_min * (double)track_width /
                            (double)window_min);
  return true;
}

bool usage_live_build_today_bar(double total_pct, bool has_total,
                                double today_pct, bool has_today,
                                int track_width, usage_today_bar_view *out) {
  if (!out) return false;
  memset(out, 0, sizeof *out);
  if (!has_total || !isfinite(total_pct) || total_pct < 0.0 ||
      total_pct > 100.0 || track_width <= 0) {
    return false;
  }

  out->has_total = true;
  out->total_px = (int)llround(total_pct * (double)track_width / 100.0);
  out->baseline_px = out->total_px;
  if (!has_today) return true;
  if (!isfinite(today_pct) || today_pct < 0.0 || today_pct > total_pct) {
    memset(out, 0, sizeof *out);
    return false;
  }

  out->has_today = true;
  out->baseline_px =
      (int)llround((total_pct - today_pct) * (double)track_width / 100.0);
  out->today_px = out->total_px - out->baseline_px;
  return true;
}

usage_update_mode usage_live_choose_update(bool initialized, bool stale,
                                           bool visible, bool has_old,
                                           double old_pct, bool has_new,
                                           double new_pct) {
  if (!has_new || !isfinite(new_pct)) return USAGE_UPDATE_SILENT;
  if (stale || !visible) return USAGE_UPDATE_SILENT;
  if (!initialized || !has_old || !isfinite(old_pct)) return USAGE_UPDATE_DIRECT;
  if (new_pct > old_pct) return USAGE_UPDATE_ANIMATE_FORWARD;
  if (new_pct < old_pct) return USAGE_UPDATE_SNAP_BACKWARD;
  return USAGE_UPDATE_DIRECT;
}

bool usage_flick_page_step(usage_flick flick, int *step) {
  if (!step) return false;
  if (flick == USAGE_FLICK_LEFT) {
    *step = 1;
    return true;
  }
  if (flick == USAGE_FLICK_RIGHT) {
    *step = -1;
    return true;
  }
  return false;
}
