/**
 * Marlin 3D Printer Firmware
 * Copyright (c) 2020 MarlinFirmware [https://github.com/MarlinFirmware/Marlin]
 *
 * Based on Sprinter and grbl.
 * Copyright (c) 2011 Camiel Gubbels / Erik van der Zalm
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

//
// Bed Tramming menu
//
// Probe-assisted flow (BED_TRAMMING_USE_PROBE):
//   1. Home, then probe ALL corners in one survey pass (no user interaction).
//   2. The highest corner becomes the baseline; the LCD shows every corner's
//      offset below it.
//   3. "Adjust Corners" walks only the out-of-tolerance corners, positioning the
//      probe at the baseline plane so you raise each one until the probe triggers.
//   4. Re-survey and repeat until every corner is within tolerance.
//
// This replaces the old "chase the highest corner" routine, which interleaved
// adjustment with discovery and never showed the measured values on the LCD.
//

#include "../../inc/MarlinConfigPre.h"

#if ALL(HAS_MARLINUI_MENU, LCD_BED_TRAMMING)

#include "menu_item.h"
#include "../../module/motion.h"
#include "../../module/planner.h"

#if HAS_LEVELING
  #include "../../feature/bedlevel/bedlevel.h"
#endif

#if ALL(HAS_STOWABLE_PROBE, BED_TRAMMING_USE_PROBE) && DISABLED(BLTOUCH)
  #define NEEDS_PROBE_DEPLOY 1
#endif

#if ENABLED(BED_TRAMMING_USE_PROBE)
  #include "../../libs/numtostr.h"
  #include "../../module/probe.h"
  #include "../../module/endstops.h"
  #include "../../gcode/queue.h"
  #if ENABLED(BLTOUCH)
    #include "../../feature/bltouch.h"
  #endif
  #ifndef BED_TRAMMING_PROBE_TOLERANCE
    #define BED_TRAMMING_PROBE_TOLERANCE 0.2
  #endif

  bool tramming_done;
#endif

static_assert(BED_TRAMMING_Z_HOP >= 0, "BED_TRAMMING_Z_HOP must be >= 0. Please update your configuration.");

#define LF 1
#define RF 2
#define RB 3
#define LB 4

#ifndef BED_TRAMMING_LEVELING_ORDER
  #define BED_TRAMMING_LEVELING_ORDER { LF, RF, LB, RB } // Default
  //#define BED_TRAMMING_LEVELING_ORDER { LF, LB, RF  }  // 3 hard-coded points
  //#define BED_TRAMMING_LEVELING_ORDER { LF, RF }       // 3-Point tramming - Rear
  //#define BED_TRAMMING_LEVELING_ORDER { LF, LB }       // 3-Point tramming - Right
  //#define BED_TRAMMING_LEVELING_ORDER { RF, RB }       // 3-Point tramming - Left
  //#define BED_TRAMMING_LEVELING_ORDER { LB, RB }       // 3-Point tramming - Front
#endif

constexpr int lco[] = BED_TRAMMING_LEVELING_ORDER;
constexpr bool tramming_3_points = COUNT(lco) == 2;
static_assert(tramming_3_points || COUNT(lco) == 4, "BED_TRAMMING_LEVELING_ORDER must have exactly 2 or 4 corners.");

constexpr int lcodiff = ABS(lco[0] - lco[1]);
static_assert(COUNT(lco) == 4 || lcodiff == 1 || lcodiff == 3, "The first two BED_TRAMMING_LEVELING_ORDER corners must be on the same edge.");

constexpr int nr_edge_points = tramming_3_points ? 3 : 4;
constexpr int available_points = nr_edge_points + ENABLED(BED_TRAMMING_INCLUDE_CENTER);
constexpr int center_index = TERN(BED_TRAMMING_INCLUDE_CENTER, available_points - 1, -1);
constexpr float inset_lfrb[4] = BED_TRAMMING_INSET_LFRB;
constexpr xy_pos_t lf { (X_MIN_BED) + inset_lfrb[0], (Y_MIN_BED) + inset_lfrb[1] },
                   rb { (X_MAX_BED) - inset_lfrb[2], (Y_MAX_BED) - inset_lfrb[3] };

#if DISABLED(BED_TRAMMING_USE_PROBE)

static int8_t bed_corner;

/**
 * Move to the next corner coordinates (manual, paper-drag tramming)
 */
static void _lcd_goto_next_corner() {
  xy_pos_t corner_point = lf;                     // Left front

  if (tramming_3_points) {
    if (bed_corner >= available_points) bed_corner = 0; // Above max position -> move back to first corner
    switch (bed_corner) {
      case 0 ... 1:
        // First two corners set explicitly by the configuration
        switch (lco[bed_corner]) {
          case RF: corner_point.x = rb.x; break;  // Right Front
          case RB: corner_point   = rb;   break;  // Right Back
          case LB: corner_point.y = rb.y; break;  // Left Back
        }
        break;

      case 2:
        // Determine which edge to probe for 3rd point
        corner_point.set(lf.x + (rb.x - lf.x) / 2, lf.y + (rb.y - lf.y) / 2);
        if ((lco[0] == LB && lco[1] == RB) || (lco[0] == RB && lco[1] == LB)) corner_point.y = lf.y; // Front Center
        if ((lco[0] == LF && lco[1] == LB) || (lco[0] == LB && lco[1] == LF)) corner_point.x = rb.x; // Center Right
        if ((lco[0] == RF && lco[1] == RB) || (lco[0] == RB && lco[1] == RF)) corner_point.x = lf.x; // Left Center
        if ((lco[0] == LF && lco[1] == RF) || (lco[0] == RF && lco[1] == LF)) corner_point.y = rb.y; // Center Back
        break;

      #if ENABLED(BED_TRAMMING_INCLUDE_CENTER)
        case 3:
          corner_point.set(X_CENTER, Y_CENTER);
          break;
      #endif
    }
  }
  else {
    // Four-Corner Bed Tramming with optional center
    if (TERN0(BED_TRAMMING_INCLUDE_CENTER, bed_corner == center_index)) {
      corner_point.set(X_CENTER, Y_CENTER);
    }
    else {
      switch (lco[bed_corner]) {
        case RF: corner_point.x = rb.x; break;  // Right Front
        case RB: corner_point   = rb;   break;  // Right Back
        case LB: corner_point.y = rb.y; break;  // Left Back
      }
    }
  }

  float z = _MIN(current_position.z + (BED_TRAMMING_Z_HOP), Z_MAX_POS);
  line_to_z(z);
  do_blocking_move_to_xy(corner_point, manual_feedrate_mm_s.x);
  line_to_z(BED_TRAMMING_HEIGHT);
  if (++bed_corner >= available_points) bed_corner = 0;
}

#endif // DISABLED(BED_TRAMMING_USE_PROBE)

#if ENABLED(BED_TRAMMING_USE_PROBE)

  // Farthest the probe may descend below the nominal tramming height before giving up
  #define BED_TRAMMING_PROBE_FLOOR ((BED_TRAMMING_HEIGHT) + (Z_PROBE_LOW_POINT))

  #define VALIDATE_POINT(X, Y, STR) static_assert(Probe::build_time::can_reach((X), (Y)), \
    "BED_TRAMMING_INSET_LFRB " STR " inset is not reachable with the default NOZZLE_TO_PROBE offset and PROBING_MARGIN.")
  VALIDATE_POINT(lf.x, Y_CENTER, "left"); VALIDATE_POINT(X_CENTER, lf.y, "front");
  VALIDATE_POINT(rb.x, Y_CENTER, "right"); VALIDATE_POINT(X_CENTER, rb.y, "back");

  // ---- Survey state ----
  static float   corner_z[nr_edge_points];      // Machine Z where each corner's probe triggered (NAN = never reached)
  static bool    corner_valid[nr_edge_points];
  static float   tram_baseline;                 // Machine Z of the highest corner (the target plane)
  static int8_t  tram_baseline_idx;             // Which corner (leveling-order index) is highest
  static int8_t  tram_active_idx;               // Corner currently being probed/adjusted (for status screens)
  static bool    tram_all_ok;                   // True when every corner is within tolerance
  static bool    wait_for_probe;

  void tramming_results_menu();
  void _tram_survey();

  // Human-readable name for a corner in the configured leveling order
  static FSTR_P _corner_name(const int8_t order_idx) {
    switch (lco[order_idx]) {
      case RF: return F("Front-Right");
      case RB: return F("Back-Right");
      case LB: return F("Back-Left");
      default: return F("Front-Left"); // LF
    }
  }

  // Probe landing XY for a corner in the configured leveling order
  static xy_pos_t _corner_xy(const int8_t order_idx) {
    xy_pos_t p = lf;
    switch (lco[order_idx]) {
      case RF: p.x = rb.x; break;  // Right Front
      case RB: p   = rb;   break;  // Right Back
      case LB: p.y = rb.y; break;  // Left Back
    }
    return p;
  }

  static void _tram_raise_clearance() {
    line_to_z(_MIN(current_position.z + (BED_TRAMMING_Z_HOP), Z_MAX_POS));
  }

  // Move (probe) to a corner with a safe Z hop first
  static void _tram_goto_corner(const int8_t order_idx) {
    float z = _MIN(current_position.z + (BED_TRAMMING_Z_HOP), Z_MAX_POS);
    TERN_(BLTOUCH, z += bltouch.z_extra_clearance());
    line_to_z(z);
    // The corner XY is the probe target; offset the nozzle so the probe lands there
    do_blocking_move_to_xy(_corner_xy(order_idx) - probe.offset_xy, manual_feedrate_mm_s.x);
  }

  // Passive status screen shown while a survey pass runs
  void _tram_draw_probing() {
    if (!ui.should_draw()) return;
    const uint8_t mid = (LCD_HEIGHT - 1) / 2;
    MenuItem_static::draw(mid ? mid - 1 : 0, GET_TEXT_F(MSG_PROBING_POINT));
    MenuItem_static::draw(mid, _corner_name(tram_active_idx), SS_CENTER);
  }

  // Confirm screen shown while the user raises a corner to the baseline plane
  void _tram_draw_raise() {
    if (!ui.should_draw()) return;
    MenuItem_confirm::select_screen(
        GET_TEXT_F(MSG_BUTTON_DONE), GET_TEXT_F(MSG_BUTTON_SKIP)
      , []{ tramming_done = true; wait_for_probe = false; }   // Done: stop tramming entirely
      , []{ wait_for_probe = false; }                         // Skip: leave this corner as-is
      , _corner_name(tram_active_idx), GET_TEXT_F(MSG_BED_TRAMMING_RAISE)
    );
  }

  // Probe the current XY straight down until the Z-probe triggers (live endstop)
  // or the floor is reached. Returns the machine Z at trigger, or NAN.
  static float _tram_probe_here() {
    endstops.enable_z_probe(true);
    TERN_(BLTOUCH, bltouch.deploy());
    float measured = NAN;
    do_blocking_move_to_z(BED_TRAMMING_PROBE_FLOOR, z_probe_slow_mm_s);
    if (TEST(endstops.trigger_state(), Z_MIN_PROBE)) {
      endstops.hit_on_purpose();
      set_current_from_steppers_for_axis(Z_AXIS);
      sync_plan_position();
      measured = current_position.z;
    }
    TERN_(BLTOUCH, bltouch.stow());
    _tram_raise_clearance();
    return measured;
  }

  // Wait for the user to raise the bed until the probe triggers.
  // Returns true if it triggered; false if the user chose Done/Skip.
  static bool _tram_raise_wait() {
    wait_for_probe = true;
    ui.goto_screen(_tram_draw_raise);
    ui.set_selection(true);
    bool triggered = false;
    while (wait_for_probe && !triggered) {
      triggered = PROBE_TRIGGERED();
      if (triggered) {
        endstops.hit_on_purpose();
        TERN_(BED_TRAMMING_AUDIO_FEEDBACK, BUZZ(200, 600));
      }
      idle();
    }
    return triggered;
  }

  // Guide the user to raise every out-of-tolerance corner to the baseline plane
  static void _tram_adjust() {
    ui.defer_status_screen();
    for (int8_t i = 0; i < nr_edge_points; ++i) {
      if (i == tram_baseline_idx) continue;                                             // The baseline corner stays put
      if (corner_valid[i] && (tram_baseline - corner_z[i]) <= BED_TRAMMING_PROBE_TOLERANCE) continue; // Already good

      tram_active_idx = i;
      _tram_goto_corner(i);
      TERN_(BLTOUCH, bltouch.deploy());
      line_to_z(tram_baseline);        // Position the probe at the baseline plane; the low corner sits below it

      const bool triggered = _tram_raise_wait();
      TERN_(BLTOUCH, bltouch.stow());
      _tram_raise_clearance();

      if (!triggered && tramming_done) return;  // User chose Done -> stop entirely
      // On Skip (or trigger) fall through to the next corner
    }
  }

  // Probe every corner, find the highest as baseline, then show the results
  void _tram_survey() {
    ui.defer_status_screen();
    ui.goto_screen(_tram_draw_probing);

    for (int8_t i = 0; i < nr_edge_points; ++i) {
      tram_active_idx = i;
      ui.refresh(LCDVIEW_REDRAW_NOW);
      _tram_draw_probing();
      _tram_goto_corner(i);
      const float z = _tram_probe_here();
      corner_z[i] = z;
      corner_valid[i] = !isnan(z);
    }

    // Highest valid corner becomes the baseline
    tram_baseline = -99.0f;
    tram_baseline_idx = 0;
    for (int8_t i = 0; i < nr_edge_points; ++i)
      if (corner_valid[i] && corner_z[i] > tram_baseline) { tram_baseline = corner_z[i]; tram_baseline_idx = i; }
    if (tram_baseline < -98.0f) tram_baseline = BED_TRAMMING_HEIGHT; // No valid corners (shouldn't happen)

    // Everything within tolerance of the baseline?
    tram_all_ok = true;
    for (int8_t i = 0; i < nr_edge_points; ++i)
      if (!corner_valid[i] || (tram_baseline - corner_z[i]) > BED_TRAMMING_PROBE_TOLERANCE) tram_all_ok = false;

    ui.goto_screen(tramming_results_menu);
    ui.set_selection(true);
  }

  // Restore state and leave the wizard
  void _tram_finish(const bool do_level) {
    TERN_(BLTOUCH, bltouch.stow());
    endstops.enable_z_probe(false);
    SET_SOFT_ENDSTOP_LOOSE(false);
    TERN_(HAS_LEVELING, set_bed_leveling_enabled(menu_leveling_was_active));
    tramming_done = true;
    if (do_level) queue.inject(TERN(HAS_LEVELING, F("G29N"), FPSTR(G28_STR)));
    ui.goto_previous_screen_no_defer();
  }

  // Results table: each corner's offset below the highest (baseline) corner
  void tramming_results_menu() {
    START_MENU();
    STATIC_ITEM(MSG_BED_TRAMMING_RESULTS, SS_DEFAULT | SS_INVERT);

    for (int8_t i = 0; i < nr_edge_points; ++i) {
      const char * const vstr = corner_valid[i]
        ? ftostr43sign(corner_z[i] - tram_baseline, '+')  // 0.000 for the baseline, negative for lower corners
        : "  LOW";
      STATIC_ITEM_F(_corner_name(i), SS_LEFT, vstr);
    }

    if (tram_all_ok) {
      STATIC_ITEM(MSG_BED_TRAMMING_IN_RANGE, SS_LEFT);
      ACTION_ITEM(MSG_BUTTON_DONE, []{ _tram_finish(true); });
    }
    else {
      ACTION_ITEM(MSG_BED_TRAMMING_ADJUST, []{
        _tram_adjust();
        if (tramming_done) _tram_finish(false); else _tram_survey();
      });
      ACTION_ITEM(MSG_BUTTON_DONE, []{ _tram_finish(false); });
    }
    END_MENU();
  }

#endif // BED_TRAMMING_USE_PROBE

void _lcd_bed_tramming_homing() {
  if (!all_axes_homed() && TERN1(NEEDS_PROBE_DEPLOY, probe.deploy())) return;

  #if HAS_LEVELING // Disable leveling so the planner won't mess with us
    menu_leveling_was_active = planner.leveling_active;
    set_bed_leveling_enabled(false);
  #endif

  #if ENABLED(BED_TRAMMING_USE_PROBE)

    // Set up probing, loosen soft endstops so corners below Z=0 can be reached,
    // then run the first survey pass (which hands off to the results menu).
    endstops.enable_z_probe(true);
    SET_SOFT_ENDSTOP_LOOSE(true);
    _tram_survey();

  #else // !BED_TRAMMING_USE_PROBE

    bed_corner = 0;
    ui.goto_screen([]{
      MenuItem_confirm::select_screen(
          GET_TEXT_F(MSG_BUTTON_NEXT), GET_TEXT_F(MSG_BUTTON_DONE)
        , _lcd_goto_next_corner
        , []{
            line_to_z(BED_TRAMMING_Z_HOP); // Raise Z off the bed when done
            TERN_(HAS_LEVELING, set_bed_leveling_enabled(menu_leveling_was_active));
            ui.goto_previous_screen_no_defer();
          }
        , GET_TEXT_F(TERN(BED_TRAMMING_INCLUDE_CENTER, MSG_LEVEL_BED_NEXT_POINT, MSG_NEXT_CORNER))
        , (const char*)nullptr, F("?")
      );
    });
    ui.set_selection(true);
    _lcd_goto_next_corner();

  #endif // !BED_TRAMMING_USE_PROBE
}

#if NEEDS_PROBE_DEPLOY

  void deploy_probe() {
    if (!tramming_done) probe.deploy(true);
    ui.goto_screen([]{
      if (ui.should_draw()) MenuItem_static::draw((LCD_HEIGHT - 1) / 2, GET_TEXT_F(MSG_MANUAL_DEPLOY));
      if (!probe.deploy() && !tramming_done) _lcd_bed_tramming_homing();
    });
  }

#endif // NEEDS_PROBE_DEPLOY

void _lcd_bed_tramming() {
  TERN_(BED_TRAMMING_USE_PROBE, tramming_done = false);
  ui.defer_status_screen();
  set_all_unhomed();
  queue.inject(TERN(CAN_SET_LEVELING_AFTER_G28, F("G28L0"), FPSTR(G28_STR)));
  ui.goto_screen([]{
    _lcd_draw_homing();
    if (!all_axes_homed()) return;
    TERN(NEEDS_PROBE_DEPLOY, deploy_probe(), ui.goto_screen(_lcd_bed_tramming_homing));
  });
}

#endif // HAS_MARLINUI_MENU && LCD_BED_TRAMMING
