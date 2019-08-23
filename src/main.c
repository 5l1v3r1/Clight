/* BEGIN_COMMON_COPYRIGHT_HEADER
 *
 * clight: C daemon utility to automagically adjust screen backlight to match ambient brightness.
 * https://github.com/FedeDP/Clight/tree/master/clight
 *
 * Copyright (C) 2019  Federico Di Pierro <nierro92@gmail.com>
 *
 * This file is part of clight.
 * clight is free software: you can redistribute it and/or modify
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
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * END_COMMON_COPYRIGHT_HEADER */

#include <opts.h>
#include <bus.h>
#include <log.h>
#include <glob.h>
#include <assert.h>
#include <module/modules_easy.h>

static int init(int argc, char *argv[]);
static void init_state(void);
static void init_topics(void);
static void sigsegv_handler(int signum);
static int check_clightd(void);
static void init_user_mod_path(enum CONFIG file, char *filename);
static void load_user_modules(enum CONFIG file);

state_t state = {0};
conf_t conf = {0};
const char *topics[MSGS_SIZE] = { 0 };

/* Every module needs these; let's init them before any module */
void modules_pre_start(void) {
    state.display = getenv("DISPLAY");
    state.wl_display = getenv("WAYLAND_DISPLAY");
    state.xauthority = getenv("XAUTHORITY");
} 

int main(int argc, char *argv[]) {
    state.quit = setjmp(state.quit_buf);
    if (!state.quit) {
        if (init(argc, argv) == 0) {
            if (conf.no_backlight && conf.no_dimmer && conf.no_dpms && conf.no_gamma) {
                WARN("No functional module running. Leaving...\n");
            } else {
                modules_loop();
            }
        }
    }
    close_log();
    return state.quit == NORM_QUIT ? EXIT_SUCCESS : EXIT_FAILURE;
}

/*
 * First of all loads options from both global and
 * local config file, and from cmdline options.
 * Then init needed modules.
 */
static int init(int argc, char *argv[]) {
    /* 
     * When receiving segfault signal,
     * call our sigsegv handler that just logs
     * a debug message before dying
     */
    signal(SIGSEGV, sigsegv_handler);
    
    /* Init conf and state */
    init_opts(argc, argv);
    init_state();
    init_topics();
    
    /* Init log file */
    open_log();
    log_conf();
    
    /* 
     * Load user custom modules after opening log (thus this information is logged).
     * Note that local (ie: placed in $HOME) modules have higher priority,
     * thus one can override a global module (placed in /usr/share/clight/modules.d/)
     * by creating a module with same name in $HOME.
     * 
     * Clight internal modules cannot be overriden.
     */
    load_user_modules(LOCAL);
    load_user_modules(GLOBAL);
    
    /* Check Clightd version and supported features */
    return check_clightd();
}

static void init_state(void) {
    strncpy(state.version, VERSION, sizeof(state.version));
    memcpy(&state.current_loc, &conf.loc, sizeof(loc_t));
    if (!conf.no_gamma) {
        /* Initial value -> undefined; if GAMMA is disabled instead assume DAY */
        state.time = -1;
    } else {
        state.time = DAY;
    }
    
    /* 
     * Initial state -> undefined; UPower will set this as soon as it is available, 
     * or to ON_AC if UPower is not available 
     */
    state.ac_state = -1;
}

static void init_topics(void) {
    /* BACKLIGHT */
    topics[AMBIENT_BR_UPD] = "CurrentAmbientBr";
    topics[CURRENT_BL_UPD] = "CurrentBlPct";
    topics[CURRENT_KBD_BL_UPD] = "CurrentKbdPct";
    
    /* DIMMER/DPMS */
    topics[DISPLAY_UPD] = "DisplayState";
    
    /* GAMMA */
    topics[TIME_UPD] = "Time";
    topics[EVENT_UPD] = "InEvent";
    topics[SUNRISE_UPD] = "Sunrise";
    topics[SUNSET_UPD] = "Sunset";
    topics[TEMP_UPD] = "CurrentTemp";
    
    /* INHIBIT */
    topics[INHIBIT_UPD] = "PmState";
    
    /* INTERFACE */
    topics[DIMMER_TO_REQ] = "InterfaceDimmerTo";
    topics[DPMS_TO_REQ] = "InterfaceDPMSTo";
    topics[SCR_TO_REQ] = "InterfaceScreenTO";
    topics[BL_TO_REQ] = "InterfaceBLTo";
    topics[TEMP_REQ] = "InterfaceTemp";
    topics[CAPTURE_REQ] = "InterfaceBLCapture";
    topics[CURVE_REQ] = "InterfaceBLCurve";
    topics[AUTOCALIB_REQ] = "InterfaceBLAuto";
    topics[CONTRIB_REQ] = "InterfaceScrContrib";
    topics[LOCATION_REQ] = "InterfaceLocation";
    /* Following are currently unused */
    topics[UPOWER_REQ] = "InterfaceUpower";
    topics[INHIBIT_REQ] = "InterfaceInhibit";
    
    /* LOCATION */
    topics[LOCATION_UPD] = "Location";

    /* SCREEN */
    topics[CURRENT_SCR_BL_UPD] = "CurrentScreenComp";

    /* UPOWER */
    topics[UPOWER_UPD] = "AcState";
    
#ifndef NDEBUG
    /* Runtime check that any topic has been inited; useful in devel */
    for (int i = 0; i < MSGS_SIZE; i++) {
        assert(strlen(topics[i]));
    }
#endif
}

/*
 * If received a sigsegv, log a message, destroy lock then
 * set sigsegv signal handler to default (SIG_DFL),
 * and send again the signal to the process.
 */
static void sigsegv_handler(int signum) {
    WARN("Received sigsegv signal. Aborting.\n");
    close_log();
    signal(signum, SIG_DFL);
    raise(signum);
}

static int check_clightd(void) {
    SYSBUS_ARG(introspect_args, CLIGHTD_SERVICE, "/org/clightd/clightd", "org.freedesktop.DBus.Introspectable", "Introspect");
    SYSBUS_ARG(vers_args, CLIGHTD_SERVICE, "/org/clightd/clightd", "org.clightd.clightd", "Version");
        
    const char *service_list = NULL;
    int r = call(&service_list, "s", &introspect_args, NULL);
    if (r < 0) {
        WARN("Clightd service could not be introspected. Automatic modules detection won't work.\n");
    } else {
        if (!conf.no_gamma && !strstr(service_list, "<node name=\"Gamma\"/>")) {
            conf.no_gamma = true;
            WARN("GAMMA forcefully disabled as Clightd was built without gamma support.\n");
        }
        
        if (!conf.no_screen && !strstr(service_list, "<node name=\"Screen\"/>")) {
            conf.no_screen = true;
            WARN("SCREEN forcefully disabled as Clightd was built without screen support.\n");
        }
        
        if (!conf.no_dpms && !strstr(service_list, "<node name=\"Dpms\"/>")) {
            conf.no_dpms = true;
            WARN("DPMS forcefully disabled as Clightd was built without dpms support.\n");
        }
    }
    
    int ret = -1;
    r = get_property(&vers_args, "s", state.clightd_version, sizeof(state.clightd_version));
    if (r < 0 || !strlen(state.clightd_version)) {
        WARN("No clightd found. Clightd is a mandatory dep.\n");
    } else {
        int maj_val = atoi(state.clightd_version);
        int min_val = atoi(strchr(state.clightd_version, '.') + 1);
        if (maj_val < MINIMUM_CLIGHTD_VERSION_MAJ || (maj_val == MINIMUM_CLIGHTD_VERSION_MAJ && min_val < MINIMUM_CLIGHTD_VERSION_MIN)) {
            WARN("Clightd must be updated. Required version: %d.%d.\n", MINIMUM_CLIGHTD_VERSION_MAJ, MINIMUM_CLIGHTD_VERSION_MIN);
        } else {
            INFO("Clightd found, version: %s.\n", state.clightd_version);
            ret = 0;
        }
    }
    return ret;
}

static void init_user_mod_path(enum CONFIG file, char *filename) {
    switch (file) {
        case LOCAL:
            if (getenv("XDG_DATA_HOME")) {
                snprintf(filename, PATH_MAX, "%s/clight/modules.d/*", getenv("XDG_DATA_HOME"));
            } else {
                snprintf(filename, PATH_MAX, "%s/.local/share/clight/modules.d/*", getpwuid(getuid())->pw_dir);
            }
            break;
        case GLOBAL:
            snprintf(filename, PATH_MAX, "%s/modules.d/*", DATADIR);
            break;
        default:
            break;
    }    
}

static void load_user_modules(enum CONFIG file) {
    char modules_path[PATH_MAX + 1];
    init_user_mod_path(file, modules_path);
    
    glob_t gl = {0};
    if (glob(modules_path, GLOB_NOSORT | GLOB_ERR, NULL, &gl) == 0) {
        for (int i = 0; i < gl.gl_pathc; i++) {
            if (m_load(gl.gl_pathv[i]) == MOD_OK) {
                INFO("'%s' loaded.\n", gl.gl_pathv[i]);
            } else {
                WARN("'%s' failed to load.\n", gl.gl_pathv[i]);
            }
        }
        globfree(&gl);
    }
}
