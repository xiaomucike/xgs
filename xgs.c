#include <float.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>
#include <sys/param.h>
#include "XPLMPlugin.h"
#include "XPLMPlanes.h"
#include "XPLMDisplay.h"
#include "XPLMGraphics.h"
#include "XPLMDataAccess.h"
#include "XPLMUtilities.h"
#include "XPLMProcessing.h"
#include "XPLMMenus.h"
#include "XPLMNavigation.h"
#include "XPWidgets.h"
#include "XPStandardWidgets.h"

#include <acfutils/assert.h>
#include <acfutils/airportdb.h>

#define VERSION "3.20-dev"

static float flight_loop_cb(float inElapsedSinceLastCall,
                float inElapsedTimeSinceLastFlightLoop, int inCounter,
                void *inRefcon);

#define MS_2_FPM 196.850
#define M_2_FT 3.2808
#define G 9.80665
#define ACF_STATE_GROUND 1
#define ACF_STATE_AIR 2

#define WINDOW_HEIGHT 140
#define STD_WINDOW_WIDTH 180
#define SIDE_MARGIN 10

#define N_WIN_LINE 7
static int xgs_enabled;
static int init_done;
static int init_failure;

static XPWidgetID main_widget;
static XPLMDataRef gear_faxil_dr, flight_time_dr, acf_num_dr, icao_dr,
        lat_dr, lon_dr, elevation_dr, y_agl_dr, hdg_dr, vy_dr, vr_enabled_dr;

static char landMsg[N_WIN_LINE][100];
static geo_pos3_t cur_pos, last_pos;
static vect3_t last_pos_v;

static int acf_last_state;
static float landing_speed;
static float lastVSpeed;
static float landing_G;
static float lastG;
static float remaining_show_time;
static float remaining_update_time;
static float air_time;

static int win_pos_x = 20;
static int win_pos_y = 600;
static int widget_in_vr;
static XPLMMenuID xgsMenu = NULL;
static int enableLogItem;
static int logEnabled = 0;

static char logAircraftNum[50];
static char acf_icao[40];

typedef struct rating_ { float limit; char txt[100]; } rating_t;
static rating_t std_rating[] = {
	{0.5, "excellent landing"},
	{1.0, "good landing"},
	{1.5, "acceptable landing"},
	{2.0, "hard landing"},
	{2.5, "you are fired!!!"},
	{3.0, "anybody survived?"},
	{FLT_MAX, "R.I.P."},
};

#define NRATING 10
static rating_t cfg_rating[NRATING];

static rating_t *rating = std_rating;
static int window_width;

static char xpdir[512];
static const char *psep;

static airportdb_t airportdb;
static list_t *near_airports;
static float arpt_last_reload;

/* will be set on transitioning into the rwy_bbox, if set then reloading is paused */
static const runway_t *landing_rwy;
static int landing_rwy_end;
static double landing_cross_height;
static double landing_dist;
static int touchdown;
static double landing_cl_delta, landing_cl_angle;


typedef struct ts_val_s {
	float ts;		/* timestamp */
	float vy;		/* vy */
	double g;		/* g as derivative of vy */
	double g_lp;	/* g after low pass filtering */
	} ts_val_t;

/* length of array */
#define N_TS_VY 4
/* order of LP filter */
#define G_LP_ORDER 3

#if G_LP_ORDER > (N_TS_VY-1)
#error G_LP_ORDER too large
#endif

/* initialize so we never get a divide by 0 in compute_g */
static ts_val_t ts_vy[N_TS_VY] = { {-2.0f}, {-1.0f} };
static int ts_val_cur = 2;
static int loops_in_touchdown;

static FILE* getConfigFile(char *mode)
{
    char path[512];

    XPLMGetPrefsPath(path);
    XPLMExtractFileAndPath(path);
    strcat(path, psep);
    strcat(path, "xgs.prf");
    return fopen(path, mode);
}


static void saveConfig()
{
    FILE *f;

    f = getConfigFile("w");
    if (! f)
        return;

    fprintf(f, "%i %i %i", win_pos_x, win_pos_y, logEnabled);

    fclose(f);
}


static void loadConfig()
{
    FILE *f;

    f = getConfigFile("r");
    if (! f)
        return;

    fscanf(f, "%i %i %i", &win_pos_x, &win_pos_y, &logEnabled);

    fclose(f);
}


static void updateLogItemState()
{
    XPLMCheckMenuItem(xgsMenu, enableLogItem,
        logEnabled ? xplm_Menu_Checked : xplm_Menu_Unchecked);
}


static void xgsMenuCallback(void *menuRef, void *param)
{
    logEnabled = ! logEnabled;
    updateLogItemState();
}


static void trim(char *str)
{
    int len = strlen(str);
    len--;
    while (0 < len) {
        if (('\r' == str[len]) || ('\n' == str[len])) {
            str[len] = 0;
            len--;
        } else
            return;
    }
}


static void update_landing_log()
{
    FILE *f;
    char buf[512];
    char airport_id[50];

	sprintf(buf, "%sOutput%sxgs_landing.log", xpdir, psep);

    f = fopen(buf, "a");
    if (! f) return;

    /* in case we didn't fix a runway... */
    float lat = cur_pos.lat;
    float lon = cur_pos.lon;
    XPLMNavRef ref = XPLMFindNavAid(NULL, NULL, &lat, &lon, NULL, xplm_Nav_Airport);

    if (XPLM_NAV_NOT_FOUND != ref) {
        XPLMGetNavAidInfo(ref, NULL, &lat, &lon, NULL, NULL, NULL, airport_id,
                NULL, NULL);
    } else {
        airport_id[0] = '\0';
    }

    time_t now = time(NULL);
    strftime(buf, sizeof buf, "%c", localtime(&now));
    fprintf(f, "%s %s %s %s %.3f m/s %.0f fpm %.3f G, ", buf, acf_icao, logAircraftNum,
                airport_id, landing_speed,
                landing_speed * MS_2_FPM, landing_G);

	if (0.0 < landing_dist) {
		fprintf(f, "Threshold %s, Above: %.f ft / %.f m, Distance: %.f ft / %.f m, from CL: %.f ft / %.f m / %.1f°, ",
                landing_rwy->ends[landing_rwy_end].id,
                landing_cross_height * M_2_FT, landing_cross_height,
                landing_dist * M_2_FT, landing_dist,
                landing_cl_delta * M_2_FT, landing_cl_delta,
                landing_cl_angle);
	} else {
		fputs("Not on a runway!, ", f);
	}

    fprintf(f, "%s\n", landMsg[0]);
    fclose(f);
}


static void closeEventWindow()
{
    if (main_widget) {
        XPGetWidgetGeometry(main_widget, &win_pos_x, &win_pos_y, NULL, NULL);
		XPHideWidget(main_widget);
        logMsg("widget closed at (%d,%d)", win_pos_x, win_pos_y);
    }

    landing_speed = 0.0f;
    landing_G = 0.0f;
    remaining_show_time = 0.0f;
	air_time = 0.0f;

	landing_rwy = NULL;
    touchdown = 0;
    landing_dist = -1.0;
}


static int load_rating_cfg(const char *path)
{
    logMsg("trying rating config file '%s'", path);

    FILE *f = fopen(path, "r");

    if (f) {
        char line[200];

        int i = 0;
        int firstLine = 1;

        while (fgets(line, sizeof line, f) && i < NRATING) {
            if (line[0] == '#') continue;
            trim(line);
            if ('\0' == line[0])
                continue;

            if (firstLine) {
                firstLine = 0;
                if (0 == strcmp(line, "V30")) {
                    continue;	/* the only version currently supported */
                } else {
                    logMsg("Config file does not start with version number");
                    break;
                }
            }

            char *s2 = NULL;
            char *s1 = strchr(line, ';');
            if (s1) {
                *s1++ = '\0';
                s2 = strchr(s1, ';');
            }
            if (NULL == s1 || NULL == s2) {
                logMsg("ill formed line -> %s", line);
                break;
            }

            s2++;

            float v_ms = fabs(atof(line));
            float v_fpm = fabs(atof(s1));
            logMsg("%f, %f, <%s>", v_ms, v_fpm, s2);

            s2 = strncpy(cfg_rating[i].txt, s2, sizeof(cfg_rating[i].txt));
            cfg_rating[i].txt[ sizeof(cfg_rating[i].txt) -1 ] = '\0';

            if (v_ms > 0) {
                cfg_rating[i].limit = v_ms;
            } else if (v_fpm > 0) {
                cfg_rating[i].limit = v_fpm / MS_2_FPM;
            } else {
                cfg_rating[i].limit = FLT_MAX;
                break;
            }
            i++;
        }

        fclose(f);

        if (i < NRATING && FLT_MAX == cfg_rating[i].limit) {
            rating = cfg_rating;
            logMsg("rating config file '%s' loaded successfully", path);
            return 1;
        }

        logMsg("Invalid config file '%s'", path);
    }

    return 0;
}


static int map_acf_to_cfg(const char *acf, const char *map_path, char *cfg_name)
{
    logMsg("trying to map '%s' to cfg", acf);

    FILE *f = fopen(map_path, "r");

    if (NULL == f) {
        logMsg("mapping file '%s' don't exist", map_path);
        return 0;
    }

    char line[200];

    int ret = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        trim(line);
        if ('\0' == line[0])
            continue;

        char *cptr = strchr(line, ' ');
        if (NULL == cptr) {
            logMsg("bad line: %s", line);
            continue;
        }

        *cptr++ = '\0';
        if (0 == strcmp(acf, line)) {
            strcpy(cfg_name, cptr);
            ret = 1;
            break;
        }
    }

    fclose(f);
    return ret;
}


/* plugin entry points */
PLUGIN_API int XPluginStart(char *outName, char *outSig, char *outDesc)
{
	log_init(XPLMDebugString, "xgs");
    logMsg("startup " VERSION);

 	/* Always use Unix-native paths on the Mac! */
	XPLMEnableFeature("XPLM_USE_NATIVE_PATHS", 1);
    XPLMEnableFeature("XPLM_USE_NATIVE_WIDGET_WINDOWS", 1);

    psep = XPLMGetDirectorySeparator();
	XPLMGetSystemPath(xpdir);

    loadConfig();
    strcpy(outName, "Landing Speed " VERSION);
    strcpy(outSig, "babichev.landspeed - hotbso");
    strcpy(outDesc, "A plugin that shows vertical landing speed.");
    return 1;
}


PLUGIN_API void XPluginDisable(void)
{
    if (xgs_enabled) {
        closeEventWindow();
        saveConfig();
    }

    logMsg("disabled");
    xgs_enabled = 0;
}


PLUGIN_API int XPluginEnable(void)
{
    if (!init_done) {
        logMsg("init start");

        init_done = 1;      /* one time only */

        char cache_path[512];
        sprintf(cache_path, "%sOutput%scaches%sXGS.cache", xpdir, psep, psep);
        fix_pathsep(cache_path);                        /* libacfutils requires a canonical path sep */
        airportdb_create(&airportdb, xpdir, cache_path);
        airportdb.ifr_only = B_FALSE;

        if (!recreate_cache(&airportdb)) {
            logMsg("init failure: recreate_cache failed");
            init_failure = 1;
        } else {
            gear_faxil_dr = XPLMFindDataRef("sim/flightmodel/forces/faxil_gear");
            flight_time_dr = XPLMFindDataRef("sim/time/total_flight_time_sec");
            icao_dr = XPLMFindDataRef("sim/aircraft/view/acf_ICAO");
            acf_num_dr = XPLMFindDataRef("sim/aircraft/view/acf_tailnum");

            lat_dr = XPLMFindDataRef("sim/flightmodel/position/latitude");
            lon_dr = XPLMFindDataRef("sim/flightmodel/position/longitude");
            y_agl_dr = XPLMFindDataRef("sim/flightmodel/position/y_agl");
            hdg_dr = XPLMFindDataRef("sim/flightmodel/position/true_psi");
            vy_dr = XPLMFindDataRef("sim/flightmodel/position/vh_ind");
            elevation_dr = XPLMFindDataRef("sim/flightmodel/position/elevation");
            vr_enabled_dr = XPLMFindDataRef("sim/graphics/VR/enabled");

            XPLMRegisterFlightLoopCallback(flight_loop_cb, 0.05f, NULL);

            XPLMMenuID pluginsMenu = XPLMFindPluginsMenu();
            int subMenuItem = XPLMAppendMenuItem(pluginsMenu, "Landing Speed", NULL, 1);
            xgsMenu = XPLMCreateMenu("Landing Speed", pluginsMenu, subMenuItem,
                        xgsMenuCallback, NULL);
            enableLogItem = XPLMAppendMenuItem(xgsMenu, "Enable Log", NULL, 1);
            updateLogItemState();
            logMsg("init done");
        }
    }

    if (init_failure) {
        logMsg("init failed, can't enable");
        return 0;
    }

    logMsg("xgs enabled");
    xgs_enabled = 1;
    return 1;
}


PLUGIN_API void	XPluginStop(void)
{
    XPluginDisable();       /* just in case */
}


PLUGIN_API void XPluginReceiveMessage(XPLMPluginID from, long msg, void *param)
{
	if ((XPLM_MSG_PLANE_LOADED == msg) && (0 == param)) {
        int n = XPLMGetDatab(acf_num_dr, logAircraftNum, 0, 49);
        logAircraftNum[n] = '\0';

        n = XPLMGetDatab(icao_dr, acf_icao, 0, 39);
        acf_icao[n] = '\0';

        char path[512];
        char acf_file[256];

        /* default to compiled in values */
        rating = std_rating;

        /* try acf specific config */
        XPLMGetNthAircraftModel(XPLM_USER_AIRCRAFT, acf_file, path);
        char *s = strrchr(path, psep[0]);
        if (NULL != s) {
            strcpy(s+1, "xgs_rating.cfg");
            if (load_rating_cfg(path))
                return;
        }

        char plugin_path[512];
        snprintf(plugin_path, sizeof plugin_path, "%sResources%splugins%sxgs%s",
                 xpdir, psep, psep, psep);

         /* try mapping */
        strcpy(path, plugin_path); strcat(path, "acf_mapping.cfg");
        if (map_acf_to_cfg(acf_icao, path, &path[strlen(plugin_path)]) && load_rating_cfg(path))
            return;

        /* try system wide config */
        strcpy(path, plugin_path); strcat(path, "std_xgs_rating.cfg");
        if (load_rating_cfg(path))
            return;
    }
}


static int getCurrentState()
{
    return 0.0f != XPLMGetDataf(gear_faxil_dr) ? ACF_STATE_GROUND : ACF_STATE_AIR;
}


static int printLandingMessage(float vy, float g)
{
	ASSERT(NULL != landing_rwy);

	int w_width = STD_WINDOW_WIDTH;

	/* rating terminates with FLT_MAX */
	int i = 0;
	while (fabs(vy) > rating[i].limit) i++;

    strcpy(landMsg[0], rating[i].txt);
	w_width = MAX(w_width, (int)(2*SIDE_MARGIN + ceil(XPLMMeasureString(xplmFont_Basic, landMsg[0], strlen(landMsg[0])))));

    sprintf(landMsg[1], "Vy: %.0f fpm / %.2f m/s", vy * MS_2_FPM, vy);
    sprintf(landMsg[2], "G:  %.2f", g);
	if (landing_dist > 0.0) {
		sprintf(landMsg[3], "Threshold %s/%s", landing_rwy->arpt->icao, landing_rwy->ends[landing_rwy_end].id);
		sprintf(landMsg[4], "Above:    %.f ft / %.f m", landing_cross_height * M_2_FT, landing_cross_height);
		sprintf(landMsg[5], "Distance: %.f ft / %.f m", landing_dist * M_2_FT, landing_dist);
		sprintf(landMsg[6], "from CL:  %.f ft / %.f m / %.1f°",
							landing_cl_delta * M_2_FT, landing_cl_delta, landing_cl_angle);
		w_width = MAX(w_width, (int)(2*SIDE_MARGIN + ceil(XPLMMeasureString(xplmFont_Basic, landMsg[6], strlen(landMsg[6])))));

	} else {
		strcpy(landMsg[3], "Not on a runway!");
		landMsg[4][0] = '\0';
	}

	return w_width;
}


static void updateLandingResult()
{
    int changed = 0;

    if (landing_speed > lastVSpeed) {
        landing_speed = lastVSpeed;
        changed = 1;
    }

    if (landing_G < lastG) {
        landing_G = lastG;
        changed = 1;
    }

    if (changed || landMsg[0][0]) {
        int w = printLandingMessage(landing_speed, landing_G);
		if (w > window_width) {
			window_width = w;
			XPSetWidgetGeometry(main_widget, win_pos_x, win_pos_y,
                    win_pos_x + window_width, win_pos_y - WINDOW_HEIGHT);
		}
	}
}


static int widget_cb(XPWidgetMessage msg, XPWidgetID widget_id, intptr_t param1, intptr_t param2)
{
	if (widget_id == main_widget) {
		if (msg == xpMessage_CloseButtonPushed) {
			closeEventWindow();
			return 1;
		}

		return 0;
	}

	/* for the embedded custom widget */
	if (xpMsg_Draw == msg) {
		int left, top;
		XPGetWidgetGeometry(widget_id, &left, &top, NULL, NULL);
		static float color[] = { 1.0, 1.0, 1.0 }; 	/* RGB White */

        for (int i = 0; i < 7; i++) {
			if ('\0' == landMsg[i][0])
				break;

			XPLMDrawString(color, left, top - (i+1)*15, landMsg[i], NULL, xplmFont_Basic);
		}
		return 1;
	}

	return 0;
}

static void createEventWindow()
{
	remaining_show_time = 60.0f;
	window_width = STD_WINDOW_WIDTH;

	int left = win_pos_x;
	int top = win_pos_y;

	if (NULL == main_widget) {
		main_widget = XPCreateWidget(left, top, left + STD_WINDOW_WIDTH, top - WINDOW_HEIGHT,
			0, "Landing Speed", 1, NULL, xpWidgetClass_MainWindow);
		XPSetWidgetProperty(main_widget, xpProperty_MainWindowType, xpMainWindowStyle_Translucent);
		XPSetWidgetProperty(main_widget, xpProperty_MainWindowHasCloseBoxes, 1);
		XPAddWidgetCallback(main_widget, widget_cb);

		left += SIDE_MARGIN; top -= 20;
		(void)XPCreateCustomWidget(left, top, left + STD_WINDOW_WIDTH, top - WINDOW_HEIGHT,
			1, "", 0, main_widget, widget_cb);

	} else {
		/* reset to standard width */
        window_width = STD_WINDOW_WIDTH;
		XPSetWidgetGeometry(main_widget, win_pos_x, win_pos_y,
							win_pos_x + window_width, win_pos_y - WINDOW_HEIGHT);
	}

	updateLandingResult();
   	XPShowWidget(main_widget);

    int in_vr = (NULL != vr_enabled_dr) && XPLMGetDatai(vr_enabled_dr);
    if (in_vr) {
        logMsg("VR mode detected");
        XPLMWindowID window =  XPGetWidgetUnderlyingWindow(main_widget);
        XPLMSetWindowPositioningMode(window, xplm_WindowVR, -1);
        widget_in_vr = 1;
    } else {
        if (widget_in_vr) {
            logMsg("widget now out of VR, map at (%d,%d)", win_pos_x, win_pos_y);
            XPLMWindowID window =  XPGetWidgetUnderlyingWindow(main_widget);
            XPLMSetWindowPositioningMode(window, xplm_WindowPositionFree, -1);

            /* A resize is necessary so it shows up on the main screen again */
            XPSetWidgetGeometry(main_widget, win_pos_x, win_pos_y,
                    win_pos_x + window_width, win_pos_y - WINDOW_HEIGHT);
            widget_in_vr = 0;
        }
    }
}


static void get_near_airports()
{
	ASSERT(NULL == landing_rwy);

	if (near_airports)
		free_nearest_airport_list(near_airports);

	load_nearest_airport_tiles(&airportdb, GEO3_TO_GEO2(cur_pos));
	unload_distant_airport_tiles(&airportdb, GEO3_TO_GEO2(cur_pos));

	near_airports = find_nearest_airports(&airportdb, GEO3_TO_GEO2(cur_pos));
}


/*
 * Catch the transition into the rwy_bbox of the nearest threshold.
 *
 */
static void fix_landing_rwy()
{
	/* have it already */
	if (landing_rwy)
		return;

	double thresh_dist_min = 1.0E12;

	float hdg = XPLMGetDataf(hdg_dr);

	int in_rwy_bb = 0;
	const airport_t *min_arpt;
	const runway_t *min_rwy;
	int min_end;

	ASSERT(NULL != near_airports);

	/* loop over all runway ends */
	for (const airport_t *arpt = list_head(near_airports);
		arpt != NULL; arpt = list_next(near_airports, arpt)) {
		ASSERT(arpt->load_complete);

		vect2_t pos_v = geo2fpp(GEO3_TO_GEO2(cur_pos), &arpt->fpp);

		for (const runway_t *rwy = avl_first(&arpt->rwys); rwy != NULL;
			rwy = AVL_NEXT(&arpt->rwys, rwy)) {

			if (point_in_poly(pos_v, rwy->rwy_bbox)) {
				for (int e = 0; e <=1; e++) {
					const runway_end_t *rwy_end = &rwy->ends[e];
					double rhdg = fabs(rel_hdg(hdg, rwy_end->hdg));
					if (rhdg > 20)
						continue;

					vect2_t thr_v = rwy_end->dthr_v;
					double dist = vect2_abs(vect2_sub(thr_v, pos_v));
					if (dist < thresh_dist_min) {
						thresh_dist_min = dist;
						in_rwy_bb = 1;
						min_arpt = arpt;
						min_rwy = rwy;
						min_end = e;
					}
				}
			}
		}
	}

	if (in_rwy_bb) {
		landing_rwy = min_rwy;
		landing_rwy_end = min_end;
		landing_cross_height = XPLMGetDataf(y_agl_dr);
		logMsg("fix runway airport: %s, runway: %s, distance: %0.0f",
			   min_arpt->icao, landing_rwy->ends[landing_rwy_end].id, thresh_dist_min);
	}
}


#ifdef DEBUG_G_LP

/* put values in CSV format into log so it can be grepped out easily */

#define MAX_GREC 200
typedef struct grec_s {double t,v,g; } grec_t;
static grec_t grec[MAX_GREC];
static int n_grec;

static void dump_grec()
{
	grec_t *p = &grec[0];
	double tstart = p->t;

	for (int i = 1; i < n_grec; i++) {
		grec_t *gr = &grec[i];
		logMsg("grec# %f;%f;%f", gr->t - tstart, gr->v * MS_2_FPM, gr->g);
		p = gr;
	}

	n_grec = 0;
}


static void record_grec(const ts_val_t *p)
{
	if (n_grec < MAX_GREC) {
		grec_t *gr = &grec[n_grec++];
		gr->t = p->ts;
		gr->v = p->vy;
		gr->g = p->g_lp;
	}
}
#else
#define dump_grec() do {} while(0)
#define record_grec(p) do {} while(0)
#endif


/* g as derivative of vy per second order approximation */
static void compute_g()
{
	ts_val_t *p0, *p1, *p2;
	p0 = &ts_vy[(ts_val_cur + (-2 + N_TS_VY)) % N_TS_VY];
	p1 = &ts_vy[(ts_val_cur + (-1 + N_TS_VY)) % N_TS_VY];
	p2 = &ts_vy[ts_val_cur];

	double h10 = p1->ts - p0->ts;
	double h20 = p2->ts - p0->ts;
	double h21 = p2->ts - p1->ts;

	p1 -> g = 1.0 + (-p0->vy * h21 / (h10 * h20) + p1->vy / h10 - p1->vy / h21 + p2->vy * h10 / (h21 * h20)) / G;
}


/* low pass filter for g */
static void compute_g_lp()
{
	ts_val_t *p[G_LP_ORDER+1];

	for (int i = 0; i < G_LP_ORDER + 1; i++) {
		/* */
		p[i] = &ts_vy[(ts_val_cur - G_LP_ORDER + i + N_TS_VY) % N_TS_VY];
	}

	/* low pass as integral over g considered as step function. With loop delay >= 0.25 and ~ 30 frames/sec
	   this filters below ~ 0.1 Hz */
	double sum = 0.0;
	for (int i = 0; i < G_LP_ORDER; i++)
		sum += p[i]->g * (p[i+1]->ts - p[i]->ts);

	p[G_LP_ORDER - 2]->g_lp = sum / (p[G_LP_ORDER]->ts - p[0]->ts);
}

/* to be called on initial ground contact */
static void record_touchdown()
{
    if (NULL != landing_rwy) {
        vect2_t pos_v = geo2fpp(GEO3_TO_GEO2(cur_pos), &landing_rwy->arpt->fpp);

        /* check whether we are really on a runway */

        if (point_in_poly(pos_v, landing_rwy->rwy_bbox)) {
            const runway_end_t *near_end = &landing_rwy->ends[landing_rwy_end];
            const runway_end_t *far_end = &landing_rwy->ends[(0 == landing_rwy_end ? 1 : 0)];

            vect2_t center_line_v = vect2_sub(far_end->dthr_v, near_end->dthr_v);
            vect2_t my_v = vect2_sub(pos_v, near_end->dthr_v);
            landing_dist = vect2_abs(my_v);
            double cl_len = vect2_abs(center_line_v);
            if (cl_len > 0.0) {
                vect2_t cl_unit_v = vect2_scmul(center_line_v, 1/cl_len);

                double dprod = vect2_dotprod(cl_unit_v, my_v);
                vect2_t p_v = vect2_scmul(cl_unit_v, dprod);
                vect2_t dev_v = vect2_sub(my_v, p_v);

                /* get signed deviation, + -> right, - -> left */
                landing_cl_delta = vect2_abs(dev_v);
                double xprod_z = my_v.x * cl_unit_v.y - my_v.y * cl_unit_v.x;
                /* by sign of cross product */
                landing_cl_delta = xprod_z > 0 ? landing_cl_delta : -landing_cl_delta;

                /* angle between cl and my heading */
                landing_cl_angle = rel_hdg(near_end->hdg, XPLMGetDataf(hdg_dr));
            }
        } else {
            landing_dist = -1.0;  /* did not land on runway */
        }
    }
}


static float flight_loop_cb(float inElapsedSinceLastCall,
                float inElapsedTimeSinceLastFlightLoop, int inCounter,
                void *inRefcon)
{
    if (! xgs_enabled)
        return 2.0;

    float timeFromStart = XPLMGetDataf(flight_time_dr);
	float loop_delay = 0.025f;

	cur_pos = GEO_POS3(XPLMGetDataf(lat_dr), XPLMGetDataf(lon_dr), XPLMGetDataf(elevation_dr));
	float height = XPLMGetDataf(y_agl_dr);

    vect3_t cur_pos_v = sph2ecef(cur_pos);

    /* if we go supersonic it's a teleportation */
    int teleportation = (vect3_dist(cur_pos_v, last_pos_v) / inElapsedSinceLastCall > 340.0);
    if (teleportation)
        logMsg("Teleportation detected");

    /* independent of state */
    if (0.0f < remaining_show_time) {
        remaining_show_time -= inElapsedSinceLastCall;
        if (teleportation || (0.0f >= remaining_show_time))
            closeEventWindow();
    }

    int acf_state = getCurrentState();

	if (ACF_STATE_AIR == acf_state) {
		if (height > 10.0)
			air_time += inElapsedSinceLastCall;

		/* low, alert mode */
		if (height < 150) {
			if (NULL == landing_rwy) {
				if (arpt_last_reload + 10.0 < timeFromStart) {
					arpt_last_reload = timeFromStart;
					get_near_airports();
				}

				if (NULL != near_airports)
					fix_landing_rwy();

			}
		} else if (height > 200) {
			landing_rwy = NULL;		/* may be a go around */
            touchdown = 0;
            landing_dist = -1.0;
		}

		if (height > 500)
			loop_delay = 1.0f;		/* we can be lazy */
	}

	/* ensure we have a real flight (and not teleportation or a bumpy takeoff) */
    if (15.0 < air_time && height < 20) {
        ts_val_cur = (ts_val_cur + 1) % N_TS_VY;
        ts_vy[ts_val_cur].ts = timeFromStart;
        ts_vy[ts_val_cur].vy = XPLMGetDataf(vy_dr);

        compute_g();
        compute_g_lp();

		if (0.0 < remaining_update_time) {
            remaining_update_time -= inElapsedSinceLastCall;

			/* we start with the last value prior to ground contact.
			   This is 2 back from current at touchdown */
			if (1 <= loops_in_touchdown) {
				const ts_val_t *tsv = &ts_vy[(ts_val_cur - 2 + N_TS_VY) % N_TS_VY];
				lastVSpeed = tsv->vy;
				lastG = tsv->g_lp;
				record_grec(tsv);
			}

			updateLandingResult();

			if (20 == loops_in_touchdown)
				createEventWindow();

            if (0.0 > remaining_update_time) {
				dump_grec();

                if (logEnabled)
                    update_landing_log();

                remaining_update_time = 0.0;
			}

		loops_in_touchdown++;
        loop_delay = -1.0;  /* highest resolution */
        }

        /* catch only first TD, i.e. no bouncing,
           landing_rwy can be NULL here after a teleportation or when not landing on a rwy */

        if (!touchdown && ACF_STATE_AIR == acf_last_state && ACF_STATE_GROUND == acf_state) {
            touchdown = 1;
            record_touchdown();
            remaining_update_time = 3.0f;
            loops_in_touchdown = 0;
            loop_delay = -1.0;  /* highest resolution */
		}
    }

    acf_last_state = acf_state;
    last_pos = cur_pos;
    last_pos_v = cur_pos_v;
    return loop_delay;
}
