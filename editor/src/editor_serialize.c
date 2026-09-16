/* Editor project/prefs text format (self-contained; versioned).
 * Serializes editor-side state only (scene path, viewport, history
 * tuning, focus) — never engine internals. The scene itself travels
 * via le_scene_* canonical text; this file only points at it. */

#include <stdio.h>
#include <string.h>

#include "internal/editor_internal.h"

#define LED_PROJECT_MAGIC "LUMA_EDITOR_PROJECT 1\n"

/* Save editor project sidecar (path + viewport + history tuning).
 * Returns LED codes (IO on write failure). */
led_result led_project_save_sidecar(led_session *session,
                                    const led_viewport *viewport,
                                    const char *path) {
    FILE *f;

    if (session == NULL || path == NULL || path[0] == '\0') {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    f = fopen(path, "w");
    if (f == NULL) {
        return LED_ERROR_IO;
    }
    fprintf(f, "%s", LED_PROJECT_MAGIC);
    fprintf(f, "scene_path %s\n",
            session->has_scene_path ? session->scene_path : "-");
    fprintf(f, "history_capacity %u\n", session->history_capacity);
    fprintf(f, "coalesce %d %llu\n", session->coalesce_enabled,
            (unsigned long long)session->coalesce_window_ms);
    if (viewport != NULL) {
        fprintf(f, "viewport %u %u %.6g %.6g %.6g %.6g %.6g %.6g "
                   "%.6g %.6g %.6g\n",
                viewport->width, viewport->height,
                (double)viewport->target[0],
                (double)viewport->target[1],
                (double)viewport->target[2],
                (double)viewport->yaw_rad,
                (double)viewport->pitch_rad,
                (double)viewport->distance,
                (double)viewport->fov_y_rad,
                (double)viewport->near_plane,
                (double)viewport->far_plane);
    }
    if (fclose(f) != 0) {
        return LED_ERROR_IO;
    }
    return LED_SUCCESS;
}

/* Load editor project sidecar (validates magic; unknown fields
 * tolerated for forward compat). Applies history tuning + viewport
 * + remembered scene path (does NOT open the scene). */
led_result led_project_load_sidecar(led_session *session,
                                    led_viewport *viewport,
                                    const char *path) {
    FILE *f;
    char line[1024];

    if (session == NULL || path == NULL || path[0] == '\0') {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    f = fopen(path, "r");
    if (f == NULL) {
        return LED_ERROR_IO;
    }
    if (fgets(line, sizeof(line), f) == NULL ||
        strcmp(line, LED_PROJECT_MAGIC) != 0) {
        fclose(f);
        return LED_ERROR_PARSE;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, "scene_path ", 11) == 0) {
            size_t n = strlen(line + 11);

            while (n > 0 && (line[11 + n - 1] == '\n' ||
                             line[11 + n - 1] == '\r')) {
                n--;
            }
            if (n == 1 && line[11] == '-') {
                session->has_scene_path = 0;
                session->scene_path[0] = '\0';
            } else if (n > 0 && n < sizeof(session->scene_path)) {
                memcpy(session->scene_path, line + 11, n);
                session->scene_path[n] = '\0';
                session->has_scene_path = 1;
            }
        } else if (strncmp(line, "history_capacity ", 17) == 0) {
            unsigned cap = 0;

            if (sscanf(line + 17, "%u", &cap) == 1 && cap >= 1 &&
                cap <= 65536) {
                led_history_set_capacity(session, cap);
            }
        } else if (strncmp(line, "coalesce ", 9) == 0) {
            int en = 0;
            unsigned long long win = 500;

            if (sscanf(line + 9, "%d %llu", &en, &win) == 2) {
                led_history_set_coalesce(session, en, win);
            }
        } else if (strncmp(line, "viewport ", 9) == 0 &&
                   viewport != NULL) {
            unsigned w = 0;
            unsigned h = 0;
            double tx;
            double ty;
            double tz;
            double yaw;
            double pitch;
            double dist;
            double fov;
            double np;
            double fp;

            if (sscanf(line + 9, "%u %u %lg %lg %lg %lg %lg %lg "
                                 "%lg %lg %lg",
                       &w, &h, &tx, &ty, &tz, &yaw, &pitch, &dist,
                       &fov, &np, &fp) == 11 &&
                w > 0 && h > 0 && dist > 0.0 && fov > 0.0 &&
                np > 0.0 && fp > np) {
                viewport->width = w;
                viewport->height = h;
                viewport->target[0] = (float)tx;
                viewport->target[1] = (float)ty;
                viewport->target[2] = (float)tz;
                viewport->yaw_rad = (float)yaw;
                viewport->pitch_rad = (float)pitch;
                viewport->distance = (float)dist;
                viewport->fov_y_rad = (float)fov;
                viewport->near_plane = (float)np;
                viewport->far_plane = (float)fp;
            }
        }
        /* Unknown fields tolerated (forward compat). */
    }
    fclose(f);
    return LED_SUCCESS;
}
