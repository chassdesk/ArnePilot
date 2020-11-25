#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <sys/resource.h>
#include <iostream>

#include <algorithm>

#include "common/util.h"
#include "common/utilpp.h"
#include "common/params.h"
#include "common/touch.h"
#include "common/swaglog.h"

#include "ui.hpp"
#include "paint.hpp"
#include "android/sl_sound.hpp"
#include "dashcam.h"

#include "cereal/gen/cpp/arne182.capnp.h"

volatile sig_atomic_t do_exit = 0;
static void set_do_exit(int sig) {
  do_exit = 1;
}

static void ui_set_brightness(UIState *s, int brightness) {
  static int last_brightness = -1;
  if (last_brightness != brightness && (s->awake || brightness == 0)) {
    if (set_brightness(brightness)) {
      last_brightness = brightness;
    }
  }
}

// e2e model button.
static void send_ml(UIState *s, bool enabled) {
  capnp::MallocMessageBuilder msg;
  auto EventArne182 = msg.initRoot<cereal::EventArne182>();
  EventArne182.setLogMonoTime(nanos_since_boot());
  auto mlStatus = EventArne182.initModelLongButton();
  mlStatus.setEnabled(enabled);
  s->pm->send("modelLongButton", msg);
}

static bool handle_ml_touch(UIState *s, int touch_x, int touch_y) {
  //mlButton manager
  if ((s->awake && s->vision_connected && s->status != STATUS_STOPPED)) {
    int padding = 40;
    int btn_w = 400;
    int btn_h = 138;
    int xs[2] = {1920 / 2 + 75 - btn_w / 2, 1920 / 2 + 75 + btn_w / 2};
    int y_top = 915 - btn_h / 2;
    if (xs[0] <= touch_x + padding && touch_x - padding <= xs[1] && y_top - padding <= touch_y) {
      s->scene.mlButtonEnabled = !s->scene.mlButtonEnabled;
      send_ml(s, s->scene.mlButtonEnabled);
      return true;
    }
  }
    return false;
}

static bool handle_SA_touched(UIState *s, int touch_x, int touch_y) {
  if (s->active_app == cereal::UiLayoutState::App::NONE) {  // if onroad (not settings or home)
    if ((s->awake && s->vision_connected && s->status != STATUS_OFFROAD) || s->ui_debug) {  // if car started or debug mode
      if (handle_df_touch(s, touch_x, touch_y) || handle_ls_touch(s, touch_x, touch_y) || handle_ml_touch(s, touch_x, touch_y)) {
        s->scene.uilayout_sidebarcollapsed = true;  // collapse sidebar when tapping any SA button
        return true;  // only allow one button to be pressed at a time
      }
    }
  }
  return false;
}


static void handle_display_state(UIState *s, bool user_input) {

  static int awake_timeout = 0;
  awake_timeout = std::max(awake_timeout-1, 0);

  // tap detection while display is off
  const float accel_samples = 5*UI_FREQ;
  static float accel_prev, gyro_prev = 0;

  bool accel_trigger = abs(s->accel_sensor - accel_prev) > 0.2;
  bool gyro_trigger = abs(s->gyro_sensor - gyro_prev) > 0.15;
  user_input = user_input || (accel_trigger && gyro_trigger);
  gyro_prev = s->gyro_sensor;
  accel_prev = (accel_prev*(accel_samples - 1) + s->accel_sensor) / accel_samples;

  // determine desired state
  bool should_wake = s->awake;
  if (user_input || s->ignition || s->started) {
    should_wake = true;
    awake_timeout = 30*UI_FREQ;
  } else if (awake_timeout == 0){
    should_wake = false;
  }

  // handle state transition
  if (s->awake != should_wake) {
    s->awake = should_wake;
    int display_mode = s->awake ? HWC_POWER_MODE_NORMAL : HWC_POWER_MODE_OFF;
    LOGW("setting display mode %d", display_mode);
    framebuffer_set_power(s->fb, display_mode);

    if (s->awake) {
      system("service call window 18 i32 1");
    }
  }
}

static void handle_vision_touch(UIState *s, int touch_x, int touch_y) {
  if (s->started && (touch_x >= s->scene.viz_rect.x - bdr_s)
      && (s->active_app != cereal::UiLayoutState::App::SETTINGS)) {
    if (!s->scene.frontview) {
      s->scene.uilayout_sidebarcollapsed = !s->scene.uilayout_sidebarcollapsed;
    } else {
      Params().write_db_value("IsDriverViewEnabled", "0", 1);
    }
  }
}

static void handle_sidebar_touch(UIState *s, int touch_x, int touch_y) {
  if (!s->scene.uilayout_sidebarcollapsed && touch_x <= sbr_w) {
    if (settings_btn.ptInRect(touch_x, touch_y)) {
      s->active_app = cereal::UiLayoutState::App::SETTINGS;
    } else if (home_btn.ptInRect(touch_x, touch_y)) {
      if (s->started) {
        s->active_app = cereal::UiLayoutState::App::NONE;
        s->scene.uilayout_sidebarcollapsed = true;
      } else {
        s->active_app = cereal::UiLayoutState::App::HOME;
      }
    }
  }
}

static void update_offroad_layout_state(UIState *s, PubMaster *pm) {
  static int timeout = 0;
  static bool prev_collapsed = false;
  static cereal::UiLayoutState::App prev_app = cereal::UiLayoutState::App::NONE;
  if (timeout > 0) {
    timeout--;
  }
  if (prev_collapsed != s->scene.uilayout_sidebarcollapsed || prev_app != s->active_app || timeout == 0) {
    MessageBuilder msg;
    auto layout = msg.initEvent().initUiLayoutState();
    layout.setActiveApp(s->active_app);
    layout.setSidebarCollapsed(s->scene.uilayout_sidebarcollapsed);
    pm->send("offroadLayout", msg);
    LOGD("setting active app to %d with sidebar %d", (int)s->active_app, s->scene.uilayout_sidebarcollapsed);
    prev_collapsed = s->scene.uilayout_sidebarcollapsed;
    prev_app = s->active_app;
    timeout = 2 * UI_FREQ;
  }
}

int main(int argc, char* argv[]) {
  setpriority(PRIO_PROCESS, 0, -14);
  signal(SIGINT, (sighandler_t)set_do_exit);
  SLSound sound;

  UIState uistate = {};
  UIState *s = &uistate;
  ui_init(s);
  sa_init(s, true);
  s->sound = &sound;

  TouchState touch = {0};
  touch_init(&touch);
  handle_display_state(s, true);

  PubMaster *pm = new PubMaster({"offroadLayout"});

  // light sensor scaling and volume params
  const bool LEON = util::read_file("/proc/cmdline").find("letv") != std::string::npos;

  float brightness_b = 0, brightness_m = 0;
  int result = read_param(&brightness_b, "BRIGHTNESS_B", true);
  result += read_param(&brightness_m, "BRIGHTNESS_M", true);
  if(result != 0) {
    brightness_b = LEON ? 10.0 : 5.0;
    brightness_m = LEON ? 2.6 : 1.3;
    write_param_float(brightness_b, "BRIGHTNESS_B", true);
    write_param_float(brightness_m, "BRIGHTNESS_M", true);
  }
  float smooth_brightness = brightness_b;

  const int MIN_VOLUME = LEON ? 12 : 9;
  const int MAX_VOLUME = LEON ? 15 : 12;
  s->sound->setVolume(MIN_VOLUME);

  bool last_started = s->started;
  while (!do_exit) {
    if (!s->started) {
      usleep(50 * 1000);
    }

    if (s->started && !last_started) {
      sa_init(s, false);  // reset ml button and regrab params
    }
    last_started = s->started;

    double u1 = millis_since_boot();

    ui_update(s);

    // poll for touch events
    int touch_x = -1, touch_y = -1;
    int touched = touch_poll(&touch, &touch_x, &touch_y, 0);
    if (touched == 1) {
      if (s->ui_debug) { printf("touched x: %d, y: %d\n", touch_x, touch_y); }
      handle_sidebar_touch(s, touch_x, touch_y);
      if (!handle_SA_touched(s, touch_x, touch_y)) {  // if SA button not touched
        handle_vision_touch(s, touch_x, touch_y);
      }
    }

    if (s->awake) {
      dashcam(s, touch_x, touch_y);
    }

    // Don't waste resources on drawing in case screen is off
    handle_display_state(s, touched == 1);
    if (!s->awake) {
      continue;
    }

    // up one notch every 5 m/s
    s->sound->setVolume(fmin(MAX_VOLUME, MIN_VOLUME + s->scene.controls_state.getVEgo() / 5));

    // set brightness
    float clipped_brightness = fmin(512, (s->light_sensor*brightness_m) + brightness_b);
    smooth_brightness = fmin(255, clipped_brightness * 0.01 + smooth_brightness * 0.99);
    ui_set_brightness(s, (int)smooth_brightness);

    update_offroad_layout_state(s, pm);

    ui_draw(s);
    double u2 = millis_since_boot();
    if (!s->scene.frontview && (u2-u1 > 66)) {
      // warn on sub 15fps
      LOGW("slow frame(%llu) time: %.2f", (s->sm)->frame, u2-u1);
    }
    framebuffer_swap(s->fb);
  }

  handle_display_state(s, true);
  delete s->sm;
  delete pm;
  return 0;
}
