#include <module/map.h>
#include "bus.h"
#include "config.h"

#define VALIDATE_PARAMS(m, signature, ...) \
    int r = sd_bus_message_read(m, signature, __VA_ARGS__); \
    if (r < 0) { \
        WARN("Failed to parse parameters: %s\n", strerror(-r)); \
        return r; \
    }

#define CLIGHT_COOKIE -1
#define CLIGHT_INH_KEY "LockClight"

typedef struct {
    int cookie;
    int refs;
    const char *app;
    const char *reason;
} lock_t;

/** org.freedesktop.ScreenSaver spec implementation **/
static void lock_dtor(void *data);
static int start_inhibit_monitor(void);
static void inhibit_parse_msg(sd_bus_message *m);
static int on_bus_name_changed(sd_bus_message *m, UNUSED void *userdata, UNUSED sd_bus_error *ret_error);
static int create_inhibit(int *cookie, const char *key, const char *app_name, const char *reason);
static int drop_inhibit(int *cookie, const char *key, bool force);
static int method_clight_inhibit(sd_bus_message *m, void *userdata, sd_bus_error *ret_error);
static int method_clight_changebl(sd_bus_message *m, void *userdata, sd_bus_error *ret_error);
static int method_inhibit(sd_bus_message *m, void *userdata, sd_bus_error *ret_error);
static int method_uninhibit(sd_bus_message *m, void *userdata, sd_bus_error *ret_error);
static int method_simulate_activity(sd_bus_message *m, void *userdata, sd_bus_error *ret_error);
static int method_get_inhibit(sd_bus_message *m, void *userdata, sd_bus_error *ret_error);

/** Clight bus api **/
static int get_version(sd_bus *b, const char *path, const char *interface, const char *property,
                       sd_bus_message *reply, void *userdata, sd_bus_error *error);
static int method_capture(sd_bus_message *m, void *userdata, sd_bus_error *ret_error);
static int method_load(sd_bus_message *m, void *userdata, sd_bus_error *ret_error);
static int method_unload(sd_bus_message *m, void *userdata, sd_bus_error *ret_error);
static int get_curve(sd_bus *bus, const char *path, const char *interface, const char *property,
                     sd_bus_message *reply, void *userdata, sd_bus_error *error);
static int set_curve(sd_bus *bus, const char *path, const char *interface, const char *property,
                    sd_bus_message *value, void *userdata, sd_bus_error *error);
static int get_location(sd_bus *bus, const char *path, const char *interface, const char *property,
                     sd_bus_message *reply, void *userdata, sd_bus_error *error);
static int set_location(sd_bus *bus, const char *path, const char *interface, const char *property,
                        sd_bus_message *value, void *userdata, sd_bus_error *error);
static int set_timeouts(sd_bus *bus, const char *path, const char *interface, const char *property,
                     sd_bus_message *value, void *userdata, sd_bus_error *error);
static int set_gamma(sd_bus *bus, const char *path, const char *interface, const char *property,
                     sd_bus_message *value, void *userdata, sd_bus_error *error);
static int set_auto_calib(sd_bus *bus, const char *path, const char *interface, const char *property,
                     sd_bus_message *value, void *userdata, sd_bus_error *error);
static int set_event(sd_bus *bus, const char *path, const char *interface, const char *property,
                        sd_bus_message *value, void *userdata, sd_bus_error *error);
static int set_screen_contrib(sd_bus *bus, const char *path, const char *interface, const char *property,
                              sd_bus_message *value, void *userdata, sd_bus_error *error);
static int method_store_conf(sd_bus_message *m, void *userdata, sd_bus_error *ret_error);

static const char object_path[] = "/org/clight/clight";
static const char bus_interface[] = "org.clight.clight";
static const char sc_interface[] = "org.freedesktop.ScreenSaver";

/* Names should match _UPD topic names here as a signal is emitted on each topic */
static const sd_bus_vtable clight_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Version", "s", get_version, offsetof(state_t, version), SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("ClightdVersion", "s", get_version, offsetof(state_t, clightd_version), SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Sunrise", "t", NULL, offsetof(state_t, day_events[SUNRISE]), SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("Sunset", "t", NULL, offsetof(state_t, day_events[SUNSET]), SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("NextEvent", "i", NULL, offsetof(state_t, next_event), SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("DayTime", "i", NULL, offsetof(state_t, day_time), SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("InEvent", "b", NULL, offsetof(state_t, in_event), SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("DisplayState", "i", NULL, offsetof(state_t, display_state), SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("AcState", "i", NULL, offsetof(state_t, ac_state), SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("LidState", "i", NULL, offsetof(state_t, lid_state), SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("Inhibited", "b", NULL, offsetof(state_t, inhibited), SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("PmInhibited", "b", NULL, offsetof(state_t, pm_inhibited), SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("SensorAvail", "b", NULL, offsetof(state_t, sens_avail), SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("BlPct", "d", NULL, offsetof(state_t, current_bl_pct), SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("KbdPct", "d", NULL, offsetof(state_t, current_kbd_pct), SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("AmbientBr", "d", NULL, offsetof(state_t, ambient_br), SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("Temp", "i", NULL, offsetof(state_t, current_temp), SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("Location", "(dd)", get_location, offsetof(state_t, current_loc), SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("ScreenComp", "d", NULL, offsetof(state_t, screen_comp), SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_METHOD("Capture", "bb", NULL, method_capture, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Inhibit", "b", NULL, method_clight_inhibit, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("IncBl", "d", NULL, method_clight_changebl, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("DecBl", "d", NULL, method_clight_changebl, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Load", "s", NULL, method_load, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Unload", "s", NULL, method_unload, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END
};

static const sd_bus_vtable conf_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_WRITABLE_PROPERTY("Verbose", "b", NULL, NULL, offsetof(conf_t, verbose), 0),
    SD_BUS_METHOD("Store", NULL, NULL, method_store_conf, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END
};

static const sd_bus_vtable conf_bl_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_WRITABLE_PROPERTY("NoAutoCalib", "b", NULL, set_auto_calib, offsetof(bl_conf_t, no_auto_calib), 0),
    SD_BUS_WRITABLE_PROPERTY("InhibitOnLidClosed", "b", NULL, NULL, offsetof(bl_conf_t, pause_on_lid_closed), 0),
    SD_BUS_WRITABLE_PROPERTY("BacklightSyspath", "s", NULL, NULL, offsetof(bl_conf_t, screen_path), 0),
    SD_BUS_WRITABLE_PROPERTY("NoSmooth", "b", NULL, NULL, offsetof(bl_conf_t, no_smooth), 0),
    SD_BUS_WRITABLE_PROPERTY("TransStep", "d", NULL, NULL, offsetof(bl_conf_t, trans_step), 0),
    SD_BUS_WRITABLE_PROPERTY("TransDuration", "i", NULL, NULL, offsetof(bl_conf_t, trans_timeout), 0),
    SD_BUS_WRITABLE_PROPERTY("ShutterThreshold", "d", NULL, NULL, offsetof(bl_conf_t, shutter_threshold), 0),
    SD_BUS_WRITABLE_PROPERTY("AcDayTimeout", "i", NULL, set_timeouts, offsetof(bl_conf_t, timeout[ON_AC][DAY]), 0),
    SD_BUS_WRITABLE_PROPERTY("AcNightTimeout", "i", NULL, set_timeouts, offsetof(bl_conf_t, timeout[ON_AC][NIGHT]), 0),
    SD_BUS_WRITABLE_PROPERTY("AcEventTimeout", "i", NULL, set_timeouts, offsetof(bl_conf_t, timeout[ON_AC][IN_EVENT]), 0),
    SD_BUS_WRITABLE_PROPERTY("BattDayTimeout", "i", NULL, set_timeouts, offsetof(bl_conf_t, timeout[ON_BATTERY][DAY]), 0),
    SD_BUS_WRITABLE_PROPERTY("BattNightTimeout", "i", NULL, set_timeouts, offsetof(bl_conf_t, timeout[ON_BATTERY][NIGHT]), 0),
    SD_BUS_WRITABLE_PROPERTY("BattEventTimeout", "i", NULL, set_timeouts, offsetof(bl_conf_t, timeout[ON_BATTERY][IN_EVENT]), 0),
    SD_BUS_VTABLE_END
};

static const sd_bus_vtable conf_sens_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_WRITABLE_PROPERTY("Device", "s", NULL, NULL, offsetof(sensor_conf_t, dev_name), 0),
    SD_BUS_WRITABLE_PROPERTY("Settings", "s", NULL, NULL, offsetof(sensor_conf_t, dev_opts), 0),
    SD_BUS_WRITABLE_PROPERTY("AcCaptures", "i", NULL, NULL, offsetof(sensor_conf_t, num_captures[ON_AC]), 0),
    SD_BUS_WRITABLE_PROPERTY("BattCaptures", "i", NULL, NULL, offsetof(sensor_conf_t, num_captures[ON_BATTERY]), 0),
    SD_BUS_WRITABLE_PROPERTY("AcPoints", "ad", get_curve, set_curve, offsetof(sensor_conf_t, regression_points[ON_AC]), 0),
    SD_BUS_WRITABLE_PROPERTY("BattPoints", "ad", get_curve, set_curve, offsetof(sensor_conf_t, regression_points[ON_BATTERY]), 0),
    SD_BUS_VTABLE_END
};

static const sd_bus_vtable conf_kbd_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_WRITABLE_PROPERTY("Dim", "b", NULL, NULL, offsetof(kbd_conf_t, dim), 0),
    SD_BUS_WRITABLE_PROPERTY("AmbBrThresh", "d", NULL, NULL, offsetof(kbd_conf_t, amb_br_thres), 0),
    SD_BUS_VTABLE_END
};

static const sd_bus_vtable conf_gamma_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_WRITABLE_PROPERTY("AmbientGamma", "b", NULL, NULL, offsetof(gamma_conf_t, ambient_gamma), 0),
    SD_BUS_WRITABLE_PROPERTY("NoSmooth", "b", NULL, NULL, offsetof(gamma_conf_t, no_smooth), 0),
    SD_BUS_WRITABLE_PROPERTY("TransStep", "i", NULL, NULL, offsetof(gamma_conf_t, trans_step), 0),
    SD_BUS_WRITABLE_PROPERTY("TransDuration", "i", NULL, NULL, offsetof(gamma_conf_t, trans_timeout), 0),
    SD_BUS_WRITABLE_PROPERTY("DayTemp", "i", NULL, set_gamma, offsetof(gamma_conf_t, temp[DAY]), 0),
    SD_BUS_WRITABLE_PROPERTY("NightTemp", "i", NULL, set_gamma, offsetof(gamma_conf_t, temp[NIGHT]), 0),
    SD_BUS_WRITABLE_PROPERTY("LongTransition", "b", NULL, NULL, offsetof(gamma_conf_t, long_transition), 0),
    SD_BUS_VTABLE_END
};

static const sd_bus_vtable conf_daytime_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_WRITABLE_PROPERTY("Sunrise", "s", NULL, set_event, offsetof(daytime_conf_t, day_events[SUNRISE]), 0),
    SD_BUS_WRITABLE_PROPERTY("Sunset", "s", NULL, set_event, offsetof(daytime_conf_t, day_events[SUNSET]), 0),
    SD_BUS_WRITABLE_PROPERTY("Location", "(dd)", get_location, set_location, offsetof(daytime_conf_t, loc), 0),
    SD_BUS_WRITABLE_PROPERTY("EventDuration", "i", NULL, NULL, offsetof(daytime_conf_t, event_duration), 0),
    SD_BUS_VTABLE_END
};

static const sd_bus_vtable conf_dimmer_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_WRITABLE_PROPERTY("NoSmoothEnter", "b", NULL, NULL, offsetof(dimmer_conf_t, no_smooth[ENTER]), 0),
    SD_BUS_WRITABLE_PROPERTY("NoSmoothExit", "b", NULL, NULL, offsetof(dimmer_conf_t, no_smooth[EXIT]), 0),
    SD_BUS_WRITABLE_PROPERTY("DimmedPct", "d", NULL, NULL, offsetof(dimmer_conf_t, dimmed_pct), 0),
    SD_BUS_WRITABLE_PROPERTY("TransStepEnter", "d", NULL, NULL, offsetof(dimmer_conf_t, trans_step[ENTER]), 0),
    SD_BUS_WRITABLE_PROPERTY("TransStepExit", "d", NULL, NULL, offsetof(dimmer_conf_t, trans_step[EXIT]), 0),
    SD_BUS_WRITABLE_PROPERTY("TransDurationEnter", "i", NULL, NULL, offsetof(dimmer_conf_t, trans_timeout[ENTER]), 0),
    SD_BUS_WRITABLE_PROPERTY("TransDurationExit", "i", NULL, NULL, offsetof(dimmer_conf_t, trans_timeout[EXIT]), 0),
    SD_BUS_WRITABLE_PROPERTY("AcTimeout", "i", NULL, set_timeouts, offsetof(dimmer_conf_t, timeout[ON_AC]), 0),
    SD_BUS_WRITABLE_PROPERTY("BattTimeout", "i", NULL, set_timeouts, offsetof(dimmer_conf_t, timeout[ON_BATTERY]), 0),
    SD_BUS_VTABLE_END
};

static const sd_bus_vtable conf_dpms_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_WRITABLE_PROPERTY("AcTimeout", "i", NULL, set_timeouts, offsetof(dpms_conf_t, timeout[ON_AC]), 0),
    SD_BUS_WRITABLE_PROPERTY("BattTimeout", "i", NULL, set_timeouts, offsetof(dpms_conf_t, timeout[ON_BATTERY]), 0),
    SD_BUS_VTABLE_END
};

static const sd_bus_vtable conf_screen_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("NumSamples", "i", NULL, offsetof(screen_conf_t, samples), SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_WRITABLE_PROPERTY("Contrib", "d", NULL, set_screen_contrib, offsetof(screen_conf_t, contrib), 0),
    SD_BUS_WRITABLE_PROPERTY("AcTimeout", "i", NULL, set_timeouts, offsetof(screen_conf_t, timeout[ON_AC]), 0),
    SD_BUS_WRITABLE_PROPERTY("BattTimeout", "i", NULL, set_timeouts, offsetof(screen_conf_t, timeout[ON_BATTERY]), 0),
    SD_BUS_VTABLE_END
};

static const sd_bus_vtable conf_inh_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_WRITABLE_PROPERTY("InhibitDocked", "b", NULL, NULL, offsetof(inh_conf_t, inhibit_docked), 0),
    SD_BUS_WRITABLE_PROPERTY("InhibitPM", "b", NULL, NULL, offsetof(inh_conf_t, inhibit_pm), 0),
    SD_BUS_VTABLE_END
};

static const sd_bus_vtable sc_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Inhibit", "ss", "u", method_inhibit, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("UnInhibit", "u", NULL, method_uninhibit, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SimulateUserActivity", NULL, NULL, method_simulate_activity, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD_WITH_OFFSET("GetActive", NULL, "b", method_get_inhibit, offsetof(state_t, inhibited), SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END
};

DECLARE_MSG(bl_to_req, BL_TO_REQ);
DECLARE_MSG(bl_req, BL_REQ);
DECLARE_MSG(dimmer_to_req, DIMMER_TO_REQ);
DECLARE_MSG(dpms_to_req, DPMS_TO_REQ);
DECLARE_MSG(scr_to_req, SCR_TO_REQ);
DECLARE_MSG(inhibit_req, INHIBIT_REQ);
DECLARE_MSG(temp_req, TEMP_REQ);
DECLARE_MSG(capture_req, CAPTURE_REQ);
DECLARE_MSG(curve_req, CURVE_REQ);
DECLARE_MSG(calib_req, NO_AUTOCALIB_REQ);
DECLARE_MSG(loc_req, LOCATION_REQ);
DECLARE_MSG(contrib_req, CONTRIB_REQ);
DECLARE_MSG(sunrise_req, SUNRISE_REQ);
DECLARE_MSG(sunset_req, SUNSET_REQ);
DECLARE_MSG(simulate_req, SIMULATE_REQ);

static map_t *lock_map;
static sd_bus *userbus, *monbus;
static sd_bus_message *curve_message; // this is used to keep curve points data lingering around in set_curve
static sd_bus_slot *lock_slot;

MODULE("INTERFACE");

static void init(void) {
    const char conf_path[] = "/org/clight/clight/Conf";
    const char conf_bl_path[] = "/org/clight/clight/Conf/Backlight";
    const char conf_sens_path[] = "/org/clight/clight/Conf/Sensor";
    const char conf_kbd_path[] = "/org/clight/clight/Conf/Kbd";
    const char conf_gamma_path[] = "/org/clight/clight/Conf/Gamma";
    const char conf_daytime_path[] = "/org/clight/clight/Conf/Daytime";
    const char conf_dim_path[] = "/org/clight/clight/Conf/Dimmer";
    const char conf_dpms_path[] = "/org/clight/clight/Conf/Dpms";
    const char conf_screen_path[] = "/org/clight/clight/Conf/Screen";
    const char conf_inh_path[] = "/org/clight/clight/Conf/Inhibit"; 
    const char sc_path_full[] = "/org/freedesktop/ScreenSaver";
    const char sc_path[] = "/ScreenSaver";
    const char conf_interface[] = "org.clight.clight.Conf";
    const char conf_bl_interface[] = "org.clight.clight.Conf.Backlight";
    const char conf_sens_interface[] = "org.clight.clight.Conf.Sensor";
    const char conf_kbd_interface[] = "org.clight.clight.Conf.Kbd";
    const char conf_gamma_interface[] = "org.clight.clight.Conf.Gamma";
    const char conf_daytime_interface[] = "org.clight.clight.Conf.Daytime";
    const char conf_dim_interface[] = "org.clight.clight.Conf.Dimmer";
    const char conf_dpms_interface[] = "org.clight.clight.Conf.Dpms";
    const char conf_screen_interface[] = "org.clight.clight.Conf.Screen";
    const char conf_inh_interface[] = "org.clight.clight.Conf.Inhibit";
    
    userbus = get_user_bus();
    
    /* Main State interface */
    int r = sd_bus_add_object_vtable(userbus,
                                NULL,
                                object_path,
                                bus_interface,
                                clight_vtable,
                                &state);

    /* Generic Conf interface */
    r += sd_bus_add_object_vtable(userbus,
                                NULL,
                                conf_path,
                                conf_interface,
                                conf_vtable,
                                &conf);

    /* Conf/Backlight interface */
    if (!conf.bl_conf.disabled) {
        r += sd_bus_add_object_vtable(userbus,
                                    NULL,
                                    conf_bl_path,
                                    conf_bl_interface,
                                    conf_bl_vtable,
                                    &conf.bl_conf);

        /* Conf/Sensor interface */
        r += sd_bus_add_object_vtable(userbus,
                                    NULL,
                                    conf_sens_path,
                                    conf_sens_interface,
                                    conf_sens_vtable,
                                    &conf.sens_conf);
    }
    
    /* Conf/Kbd interface */
    if (!conf.kbd_conf.disabled) {
        r += sd_bus_add_object_vtable(userbus,
                                    NULL,
                                    conf_kbd_path,
                                    conf_kbd_interface,
                                    conf_kbd_vtable,
                                    &conf.kbd_conf);
    }
    
    /* Conf/Gamma interface */
    if (!conf.gamma_conf.disabled) {
        r += sd_bus_add_object_vtable(userbus,
                                    NULL,
                                    conf_gamma_path,
                                    conf_gamma_interface,
                                    conf_gamma_vtable,
                                    &conf.gamma_conf);
    }
    
    r += sd_bus_add_object_vtable(userbus,
                                  NULL,
                                  conf_daytime_path,
                                  conf_daytime_interface,
                                  conf_daytime_vtable,
                                  &conf.day_conf);
    
    /* Conf/Dimmer interface */
    if (!conf.dim_conf.disabled) {
        r += sd_bus_add_object_vtable(userbus,
                                    NULL,
                                    conf_dim_path,
                                    conf_dim_interface,
                                    conf_dimmer_vtable,
                                    &conf.dim_conf);
    }
    
    /* Conf/Dpms interface */
    if (!conf.dpms_conf.disabled) {
        r += sd_bus_add_object_vtable(userbus,
                                    NULL,
                                    conf_dpms_path,
                                    conf_dpms_interface,
                                    conf_dpms_vtable,
                                    &conf.dpms_conf);
    }
    
    /* Conf/Screen interface */
    if (!conf.screen_conf.disabled) {
        r += sd_bus_add_object_vtable(userbus,
                                    NULL,
                                    conf_screen_path,
                                    conf_screen_interface,
                                    conf_screen_vtable,
                                    &conf.screen_conf);
    }
    
    if (!conf.inh_conf.disabled) {
        r += sd_bus_add_object_vtable(userbus,
                                      NULL,
                                      conf_inh_path,
                                      conf_inh_interface,
                                      conf_inh_vtable,
                                      &conf.inh_conf);

            /*
            * ScreenSaver implementation:
            * take both /ScreenSaver and /org/freedesktop/ScreenSaver paths
            * as they're both used by applications.
            * Eg: chromium/libreoffice use full path, while vlc uses /ScreenSaver
            *
            * Avoid checking for errors!!
            */
            sd_bus_add_object_vtable(userbus,
                                        NULL,
                                        sc_path,
                                        sc_interface,
                                        sc_vtable,
                                        &state);

            sd_bus_add_object_vtable(userbus,
                                    NULL,
                                    sc_path_full,
                                    sc_interface,
                                    sc_vtable,
                                    &state);
    }

    if (r < 0) {
        WARN("Could not create %s dbus interface: %s\n", bus_interface, strerror(-r));
    } else {
        r = sd_bus_request_name(userbus, bus_interface, 0);
        if (r < 0) {
            WARN("Failed to create %s dbus interface: %s\n", bus_interface, strerror(-r));
        } else {
            /* Subscribe to any topic expept REQUESTS */
            m_subscribe("^[^Req].*");
            
            /** org.freedesktop.ScreenSaver API **/
            if (!conf.inh_conf.disabled) {
                if (sd_bus_request_name(userbus, sc_interface, SD_BUS_NAME_REPLACE_EXISTING) < 0) {
                    WARN("Failed to create %s dbus interface: %s\n", sc_interface, strerror(-r));
                    INFO("Fallback at monitoring requests to %s name owner.\n", sc_interface);
                    if (start_inhibit_monitor() != 0) {
                        WARN("Failed to register %s inhibition monitor.\n", sc_interface);
                    }
                }
                lock_map = map_new(true, lock_dtor);
            }
            /**                                 **/
        }
    }
    
    if (r < 0) {
        WARN("Failed to init.\n");
        m_poisonpill(self());
    }
}

static bool check(void) {
    return true;
}

static bool evaluate() {
    return !conf.wizard;
}

static void receive(const msg_t *const msg, UNUSED const void* userdata) {
    switch (MSG_TYPE()) {
    case FD_UPD: {
        sd_bus *b = (sd_bus *)msg->fd_msg->userptr;
        int r;
        do {
            sd_bus_message *m = NULL;
            r = sd_bus_process(b, &m);
            if (m) {
                inhibit_parse_msg(m);
                sd_bus_message_unref(m);
            }
        } while (r > 0);
        break;
    }
    case SYSTEM_UPD:
        break;
    default:
        if (userbus) {
            DEBUG("Emitting %s property\n", msg->ps_msg->topic);
            sd_bus_emit_properties_changed(userbus, object_path, bus_interface, msg->ps_msg->topic, NULL);
        }
        break;
    }
}

static void destroy(void) {
    if (userbus) {
        sd_bus_release_name(userbus, bus_interface);
        if (!conf.inh_conf.disabled) {
            sd_bus_release_name(userbus, sc_interface);
        }
        userbus = sd_bus_flush_close_unref(userbus);
    }
    if (monbus) {
        monbus = sd_bus_flush_close_unref(monbus);
    }
    map_free(lock_map);
    curve_message = sd_bus_message_unref(curve_message);
}

static void lock_dtor(void *data) {
    lock_t *l = (lock_t *)data;
    free((void *)l->app);
    free((void *)l->reason);
    free(l);
}

/** org.freedesktop.ScreenSaver spec implementation: https://people.freedesktop.org/~hadess/idle-inhibition-spec/re01.html **/

/* 
 * Fallback to monitoring org.freedesktop.ScreenSaver bus name to receive Inhibit/UnhInhibit notifications
 * when org.freedesktop.ScreenSaver name could not be owned by Clight (ie: there is some other app that is owning it).
 * 
 * Stolen from: https://github.com/systemd/systemd/blob/master/src/busctl/busctl.c#L1203 (busctl monitor)
 */
static int start_inhibit_monitor(void) {
    int r = sd_bus_new(&monbus);
    if (r < 0) {
        WARN("Failed to create monitor: %m\n");
        return r;
    }
    
    r = sd_bus_set_monitor(monbus, true);
    if (r < 0) {
        WARN("Failed to set monitor mode: %m\n");
        return r;
    }
    
    r = sd_bus_negotiate_creds(monbus, true, _SD_BUS_CREDS_ALL);
    if (r < 0) {
        WARN("Failed to enable credentials: %m\n");
        return r;
    }
    
    r = sd_bus_negotiate_timestamp(monbus, true);
    if (r < 0) {
        WARN("Failed to enable timestamps: %m\n");
        return r;
    }
    
    r = sd_bus_negotiate_fds(monbus, true);
    if (r < 0) {
        WARN("Failed to enable fds: %m\n");
        return r;
    }
    
    r = sd_bus_set_bus_client(monbus, true);
    if (r < 0) {
        WARN("Failed to set bus client: %m\n");
        return r;
    }
    
    /* Set --user address */
    const char *addr = NULL;
    sd_bus_get_address(userbus, &addr);
    sd_bus_set_address(monbus, addr);
    
    sd_bus_start(monbus);
    
    USERBUS_ARG(args, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus.Monitoring", "BecomeMonitor");
    args.bus = monbus;
    
    r = call(&args, "asu", 1, "destination='org.freedesktop.ScreenSaver'", 0);
    if (r == 0) {
        sd_bus_process(monbus, NULL);
        int fd = sd_bus_get_fd(monbus);
        m_register_fd(dup(fd), true, monbus);
    }
    return r;
}

static void inhibit_parse_msg(sd_bus_message *m) {
    if (sd_bus_message_get_member(m)) {
        const char *member = sd_bus_message_get_member(m);
        const char *signature = sd_bus_message_get_signature(m, false);
        const char *interface = sd_bus_message_get_interface(m);
        
        for (int i = 1; i <= 2; i++) {
            if (!strcmp(interface, sc_interface)
                && !strcmp(member, sc_vtable[i].x.method.member)
                && !strcmp(signature, sc_vtable[i].x.method.signature)) {
                
                switch (i) {
                case 1: { // Inhibit!!
                    int cookie = 0;
                    char *app_name = NULL, *reason = NULL;
                    int r = sd_bus_message_read(m, "ss", &app_name, &reason); 
                    if (r < 0) {
                        WARN("Failed to parse parameters: %s\n", strerror(-r));
                    } else {
                        create_inhibit(&cookie, sd_bus_message_get_sender(m), app_name, reason);
                    }
                    break;
                }
                case 2: // UnInhibit!!
                    drop_inhibit(NULL, sd_bus_message_get_sender(m), false);
                    break;
                default:
                    break;
                }
            }
        }
    }
}

/*
 * org.freedesktop.ScreenSaver spec:
 * Inhibition will stop when the UnInhibit function is called, 
 * or the application disconnects from the D-Bus session bus (which usually happens upon exit).
 * 
 * Polling on NameOwnerChanged dbus signals.
 */
static int on_bus_name_changed(sd_bus_message *m, UNUSED void *userdata, UNUSED sd_bus_error *ret_error) {
    const char *name = NULL, *old_owner = NULL, *new_owner = NULL;
    if (sd_bus_message_read(m, "sss", &name, &old_owner, &new_owner) >= 0) {
        if (map_has_key(lock_map, old_owner) && (!new_owner || !strlen(new_owner))) {
            drop_inhibit(NULL, old_owner, true);
        }
    }
    return 0;
}

static int create_inhibit(int *cookie, const char *key, const char *app_name, const char *reason) {
    lock_t *l = map_get(lock_map, key);
    if (l) {
        l->refs++;
        *cookie = l->cookie;
    } else {
        lock_t *l = malloc(sizeof(lock_t));
        if (l) {
            if (*cookie != CLIGHT_COOKIE) {
                *cookie = random();
            }
            l->cookie = *cookie;
            l->refs = 1;
            l->app = strdup(app_name);
            l->reason = strdup(reason);
            map_put(lock_map, key, l);

            inhibit_req.inhibit.old = state.inhibited;
            inhibit_req.inhibit.new = true;
            inhibit_req.inhibit.force = false;
            inhibit_req.inhibit.app_name = strdup(app_name);
            inhibit_req.inhibit.reason = strdup(reason);
            M_PUB(&inhibit_req);

            if (map_length(lock_map) == 1) {
                /* Start listening on NameOwnerChanged signals */
                USERBUS_ARG(args, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameOwnerChanged");
                add_match(&args, &lock_slot, on_bus_name_changed);
            }
        } else {
            return -1;
        }
    }
    return 0;
}

static int drop_inhibit(int *cookie, const char *key, bool force) {
    lock_t *l = map_get(lock_map, key);
    if (!l && cookie) {
        /* May be another sender is asking to drop a cookie? Linear search */
        for (map_itr_t *itr = map_itr_new(lock_map); itr; itr = map_itr_next(itr)) {
            lock_t *tmp = (lock_t *)map_itr_get_data(itr);
            if (tmp->cookie == *cookie) {
                l = tmp;
                free(itr);
                itr = NULL;
            }
        }
    }

    if (l) {
        if (!force) {
            l->refs--;
        } else {
            l->refs = 0;
        }
        if (l->refs == 0) {
            DEBUG("Dropped ScreenSaver inhibition held by cookie: %d.\n", l->cookie);
            inhibit_req.inhibit.old = state.inhibited;
            inhibit_req.inhibit.new = false;
            inhibit_req.inhibit.force = !strcmp(key, CLIGHT_INH_KEY); // forcefully disable inhibition for Clight INTERFACE Inhibit "false"
            inhibit_req.inhibit.app_name = strdup(l->app);
            inhibit_req.inhibit.reason = strdup(l->reason);
            M_PUB(&inhibit_req);
            
            map_remove(lock_map, key);
            if (map_length(lock_map) == 0) {
                /* Stop listening on NameOwnerChanged signals */
                lock_slot = sd_bus_slot_unref(lock_slot);
            }
        }
        return 0;
    }
    return -1;
}

static int method_clight_inhibit(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    int inhibit;
    VALIDATE_PARAMS(m, "b", &inhibit);
    
    if (!conf.inh_conf.disabled) {
        int ret = 0;
        if (inhibit) {
            int cookie = CLIGHT_COOKIE;
            ret = create_inhibit(&cookie, CLIGHT_INH_KEY, "Clight", "user requested");
        } else {
            ret = drop_inhibit(NULL, CLIGHT_INH_KEY, true);
        }

        if (ret == 0) {
            return sd_bus_reply_method_return(m, NULL);
        }
    } else {
        WARN("Inhibit module is disabled.\n");
    }
    sd_bus_error_set_errno(ret_error, EINVAL);
    return -EINVAL;
}

static int method_clight_changebl(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    double change_pct;
    VALIDATE_PARAMS(m, "d", &change_pct);
    
    if (change_pct > 0.0 && change_pct < 1.0) {
        bl_req.bl.smooth = -1;
        
        if (!strcmp(sd_bus_message_get_member(m), "IncBl")) {
            bl_req.bl.new = state.current_bl_pct + change_pct;
            if (bl_req.bl.new > 1.0) {
                bl_req.bl.new = 1.0;
            }
        } else {
            bl_req.bl.new = state.current_bl_pct - change_pct;
            if (bl_req.bl.new < 0.0) {
                bl_req.bl.new = 0.0;
            }
        }
        M_PUB(&bl_req);
        return sd_bus_reply_method_return(m, NULL);
    }
    sd_bus_error_set_errno(ret_error, EINVAL);
    return -EINVAL;
}

static int method_inhibit(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    char *app_name = NULL, *reason = NULL;
    
    VALIDATE_PARAMS(m, "ss", &app_name, &reason);
    
    int cookie = 0;
    if (create_inhibit(&cookie, sd_bus_message_get_sender(m), app_name, reason) == 0) {
        return sd_bus_reply_method_return(m, "u", cookie);
    }
    sd_bus_error_set_errno(ret_error, ENOMEM);
    return -ENOMEM;
}

static int method_uninhibit(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    int cookie;
    
    VALIDATE_PARAMS(m, "u", &cookie);
    
    if (drop_inhibit(&cookie, sd_bus_message_get_sender(m), false) == 0) {
        return sd_bus_reply_method_return(m, NULL);
    }
    sd_bus_error_set_errno(ret_error, EINVAL);
    return -EINVAL;
}

static int method_simulate_activity(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    M_PUB(&simulate_req);
    return sd_bus_reply_method_return(m, NULL);
}

static int method_get_inhibit(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    return sd_bus_reply_method_return(m, "b", *(bool *)userdata);
}

/** Clight bus api **/

static int get_version(sd_bus *b, const char *path, const char *interface, const char *property,
                       sd_bus_message *reply, void *userdata, sd_bus_error *error) {
    return sd_bus_message_append(reply, "s", userdata);
}
                       
static int method_capture(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    VALIDATE_PARAMS(m, "bb", &capture_req.capture.reset_timer, &capture_req.capture.capture_only);
    M_PUB(&capture_req);
    return sd_bus_reply_method_return(m, NULL);
}

static int method_load(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    const char *module_path;

    VALIDATE_PARAMS(m, "s", &module_path);
    if (m_load(module_path) == MOD_OK) {
        INFO("'%s' loaded.\n", module_path);
        return sd_bus_reply_method_return(m, NULL);
    }

    WARN("'%s' failed to load.\n", module_path);
    sd_bus_error_set_errno(ret_error, EINVAL);
    return -EINVAL;
}

static int method_unload(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    const char *module_path;

    VALIDATE_PARAMS(m, "s", &module_path);
    if (m_unload(module_path) == MOD_OK) {
        INFO("'%s' unloaded.\n", module_path);
        return sd_bus_reply_method_return(m, NULL);
    }

    WARN("'%s' failed to unload.\n", module_path);
    sd_bus_error_set_errno(ret_error, EINVAL);
    return -EINVAL;
}

static int get_curve(sd_bus *bus, const char *path, const char *interface, const char *property,
                     sd_bus_message *reply, void *userdata, sd_bus_error *error) {
    
    enum ac_states st = ON_AC;
    if (userdata == conf.sens_conf.regression_points[ON_BATTERY]) {
        st = ON_BATTERY;
    }
    return sd_bus_message_append_array(reply, 'd', userdata, conf.sens_conf.num_points[st] * sizeof(double));
}

static int set_curve(sd_bus *bus, const char *path, const char *interface, const char *property,
                     sd_bus_message *value, void *userdata, sd_bus_error *error) {

    /* Unref last curve message, if any */
    curve_message = sd_bus_message_unref(curve_message);

    double *data = NULL;
    size_t length;
    int r = sd_bus_message_read_array(value, 'd', (const void**) &data, &length);
    if (r < 0) {
        WARN("Failed to parse parameters: %s\n", strerror(-r));
        return r;
    }
    curve_req.curve.num_points = length / sizeof(double);
    if (curve_req.curve.num_points > MAX_SIZE_POINTS) {
        WARN("Wrong parameters.\n");
        sd_bus_error_set_const(error, SD_BUS_ERROR_FAILED, "Wrong parameters.");
        r = -EINVAL;
    } else {
        curve_req.curve.state = ON_AC;
        if (userdata == conf.sens_conf.regression_points[ON_BATTERY]) {
            curve_req.curve.state = ON_BATTERY;
        }
        curve_req.curve.regression_points = data;
        curve_message = sd_bus_message_ref(value);
        M_PUB(&curve_req);
    }
    return r;
}

static int get_location(sd_bus *bus, const char *path, const char *interface, const char *property,
                        sd_bus_message *reply, void *userdata, sd_bus_error *error) {
    loc_t *l = (loc_t *)userdata;
    return sd_bus_message_append(reply, "(dd)", l->lat, l->lon);
}

static int set_location(sd_bus *bus, const char *path, const char *interface, const char *property,
                        sd_bus_message *value, void *userdata, sd_bus_error *error) {

    VALIDATE_PARAMS(value, "(dd)", &loc_req.loc.new.lat, &loc_req.loc.new.lon);

    DEBUG("New location from BUS api: %.2lf %.2lf\n", loc_req.loc.new.lat, loc_req.loc.new.lat);
    M_PUB(&loc_req);
    return r;
}

static int set_timeouts(sd_bus *bus, const char *path, const char *interface, const char *property,
                            sd_bus_message *value, void *userdata, sd_bus_error *error) {    
    /* Check if we modified currently used timeout! */
    message_t *msg = NULL;
    if (userdata == &conf.bl_conf.timeout[ON_AC][DAY]) {
        msg = &bl_to_req;
        bl_to_req.to.daytime = DAY;
        bl_to_req.to.state = ON_AC;
    } else if (userdata == &conf.bl_conf.timeout[ON_AC][NIGHT]) {
        msg = &bl_to_req;
        bl_to_req.to.daytime = NIGHT;
        bl_to_req.to.state = ON_AC;
    } else if (userdata == &conf.bl_conf.timeout[ON_AC][IN_EVENT]) {
        msg = &bl_to_req;
        bl_to_req.to.daytime = IN_EVENT;
        bl_to_req.to.state = ON_AC;
    } else if (userdata == &conf.bl_conf.timeout[ON_BATTERY][DAY]) {
        msg = &bl_to_req;
        bl_to_req.to.daytime = DAY;
        bl_to_req.to.state = ON_BATTERY;
    } else if (userdata == &conf.bl_conf.timeout[ON_BATTERY][NIGHT]) {
        msg = &bl_to_req;
        bl_to_req.to.daytime = NIGHT;
        bl_to_req.to.state = ON_BATTERY;
    } else if (userdata == &conf.bl_conf.timeout[ON_BATTERY][IN_EVENT]) {
        msg = &bl_to_req;
        bl_to_req.to.daytime = IN_EVENT;
        bl_to_req.to.state = ON_BATTERY;
    } else if (userdata == &conf.dim_conf.timeout[ON_AC]) {
        msg = &dimmer_to_req;
        dimmer_to_req.to.state = ON_AC;
    } else if (userdata == &conf.dim_conf.timeout[ON_BATTERY]) {
        msg = &dimmer_to_req;
        dimmer_to_req.to.state = ON_BATTERY;
    } else if (userdata == &conf.dpms_conf.timeout[ON_AC]) {
        msg = &dpms_to_req;
        dpms_to_req.to.state = ON_AC;
    } else if (userdata == &conf.dpms_conf.timeout[ON_BATTERY]) {
        msg = &dpms_to_req;
        dimmer_to_req.to.state = ON_BATTERY;
    } else if (userdata == &conf.screen_conf.timeout[ON_AC]) {
        msg = &scr_to_req;
        scr_to_req.to.state = ON_AC;
    } else if (userdata == &conf.screen_conf.timeout[ON_BATTERY]) {
        msg = &scr_to_req;
        scr_to_req.to.state = ON_BATTERY;
    }
    
    VALIDATE_PARAMS(value, "i", &msg->to.new);

    if (msg) {
        M_PUB(msg);
    }
    return r;
}

static int set_gamma(sd_bus *bus, const char *path, const char *interface, const char *property,
                     sd_bus_message *value, void *userdata, sd_bus_error *error) {
    VALIDATE_PARAMS(value, "i", &temp_req.temp.new);
    
    temp_req.temp.daytime = userdata == &conf.gamma_conf.temp[DAY] ? DAY : NIGHT;
    temp_req.temp.smooth = -1; // use conf values
    M_PUB(&temp_req);
    return r;
}

static int set_auto_calib(sd_bus *bus, const char *path, const char *interface, const char *property,
                          sd_bus_message *value, void *userdata, sd_bus_error *error) {
    VALIDATE_PARAMS(value, "b", &calib_req.nocalib.new);
    
    M_PUB(&calib_req);
    return r;
}

static int set_event(sd_bus *bus, const char *path, const char *interface, const char *property,
                     sd_bus_message *value, void *userdata, sd_bus_error *error) {
    const char *event = NULL;
    VALIDATE_PARAMS(value, "s", &event);

    message_t *msg = &sunrise_req;
    if (userdata == &conf.day_conf.day_events[SUNSET]) {
        msg = &sunset_req;
    }
    strncpy(msg->event.event, event, sizeof(msg->event.event));
    M_PUB(msg);
    return r;
}

static int set_screen_contrib(sd_bus *bus, const char *path, const char *interface, const char *property,
                     sd_bus_message *value, void *userdata, sd_bus_error *error) {
    VALIDATE_PARAMS(value, "d", &contrib_req.contrib.new);

    M_PUB(&contrib_req);
    return r;
}

static int method_store_conf(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    int r = -1;
    if (store_config(LOCAL) == 0) {
        r = sd_bus_reply_method_return(m, NULL);
    } else {
        sd_bus_error_set_const(ret_error, SD_BUS_ERROR_FAILED, "Failed to store conf.");
    }
    return r;
}
