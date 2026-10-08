#include <stdio.h>
#include <string.h>

#include "app_internal.h"

static int unfinished(const skiff_job *job) {
    return job->state == SKIFF_JOB_QUEUED || job->state == SKIFF_JOB_ACTIVE ||
           job->state == SKIFF_JOB_FAILED;
}

size_t app_queue_unfinished(const skiff_app *app) {
    size_t count = 0;
    for (size_t i = 0; i < app->queue.count; i++) {
        count += (size_t)unfinished(&app->queue.jobs[i]);
    }
    return count;
}

const skiff_job *app_queue_job_for(const skiff_app *app, uint64_t rom_id) {
    for (size_t i = 0; i < app->queue.count; i++) {
        if (app->queue.jobs[i].rom_id == rom_id && unfinished(&app->queue.jobs[i])) {
            return &app->queue.jobs[i];
        }
    }
    return NULL;
}

void app_queue_refresh(skiff_app *app) {
    app_queue_view *queue = &app->queue;
    if (app->jobs == NULL) {
        queue->count = 0;
    } else {
        const size_t count = skiff_jobs_list(app->jobs, queue->jobs, SKIFF_JOBS_MAX);
        queue->count = count < SKIFF_JOBS_MAX ? count : SKIFF_JOBS_MAX;
    }
    for (size_t i = 0; i < queue->count; i++) {
        app_fit(app, queue->jobs[i].title, SKIFF_APP_LABEL_WIDTH, queue->labels[i],
                sizeof queue->labels[i]);
    }
    skiff_ui_list_set_count(&app->queue_list, queue->count);
    app->dirty = 1;
}

void app_queue_event(skiff_app *app, const skiff_jobs_event *event) {
    app_queue_view *queue = &app->queue;
    const uint64_t now = (uint64_t)app_now(app);
    switch (event->kind) {
    case SKIFF_JOBS_EVENT_STATE:
        if (event->job_id == queue->progress_job && event->state != SKIFF_JOB_ACTIVE) {
            queue->progress_job = 0;
            queue->has_recovery = 0;
        }
        app_queue_refresh(app);
        app_library_refresh_markers(app);
        break;
    case SKIFF_JOBS_EVENT_PROGRESS:
        if (event->job_id != queue->progress_job) {
            queue->progress_job = event->job_id;
            skiff_ui_progress_start(&queue->progress, event->done, event->total, now);
        } else {
            if (event->done > queue->progress.done) {
                queue->has_recovery = 0;
            }
            skiff_ui_progress_update(&queue->progress, event->done, now);
        }
        break;
    case SKIFF_JOBS_EVENT_RECOVERY:
        queue->has_recovery = 1;
        queue->recovery = *event;
        app->dirty = 1;
        break;
    }
}

static const skiff_job *selected(const skiff_app *app) {
    return app->queue.count > 0 ? &app->queue.jobs[app->queue_list.selected] : NULL;
}

void app_cancel_confirmed(skiff_app *app, uint32_t id) {
    const skiff_err err = skiff_jobs_cancel(app->jobs, id);
    skiff_log_write(app->log, err == SKIFF_OK ? SKIFF_LOG_INFO : SKIFF_LOG_ERROR, SKIFF_APP_LOG_TAG,
                    "cancel job %u: %s (%d)", (unsigned)id, skiff_err_name(err), (int)err);
    app_queue_refresh(app);
    if (err != SKIFF_OK) {
        app_show_error(app, err, NULL, MESSAGE_BACK, MESSAGE_NONE, SKIFF_TEXT_OK,
                       SKIFF_APP_SCREEN_QUEUE);
        return;
    }
    app_set_screen(app, SKIFF_APP_SCREEN_QUEUE);
}

static void player_change(skiff_app *app, const char *what, uint32_t id, skiff_err err) {
    skiff_log_write(app->log, err == SKIFF_OK ? SKIFF_LOG_INFO : SKIFF_LOG_ERROR, SKIFF_APP_LOG_TAG,
                    "%s job %u: %s (%d)", what, (unsigned)id, skiff_err_name(err), (int)err);
    app_queue_refresh(app);
    if (err != SKIFF_OK) {
        app_show_error(app, err, NULL, MESSAGE_BACK, MESSAGE_NONE, SKIFF_TEXT_OK,
                       SKIFF_APP_SCREEN_QUEUE);
    }
}

void app_queue_update(skiff_app *app, unsigned actions) {
    if (actions & SKIFF_UI_ACTION_BACK) {
        app_set_screen(app, SKIFF_APP_SCREEN_LIBRARY);
        return;
    }
    if (skiff_ui_list_apply(&app->queue_list, actions)) {
        app->dirty = 1;
    }
    const skiff_job *job = selected(app);
    if ((actions & SKIFF_UI_ACTION_MENU) && app->jobs != NULL) {
        player_change(app, "clear finished", 0, skiff_jobs_clear_finished(app->jobs));
        return;
    }
    if (job == NULL) {
        return;
    }
    if ((actions & SKIFF_UI_ACTION_EXTRA) && unfinished(job)) {
        app_confirm(app, SKIFF_TEXT_CONFIRM_CANCEL, CONFIRM_CANCEL_JOB, job->id);
        return;
    }
    if ((actions & SKIFF_UI_ACTION_CONFIRM) &&
        (job->state == SKIFF_JOB_FAILED || job->state == SKIFF_JOB_CANCELLED)) {
        /* A cancelled job gave up its place in installed.json; a failed one still holds it. */
        if (job->state == SKIFF_JOB_CANCELLED &&
            !app_installed_room(app, job->rom_id, job->file_name)) {
            app_show_text(app, SKIFF_TEXT_TITLE_NOTICE, app_text(app, SKIFF_TEXT_INSTALLED_FULL),
                          MESSAGE_BACK, SKIFF_APP_SCREEN_QUEUE);
            return;
        }
        player_change(app, "retry", job->id, skiff_jobs_retry(app->jobs, job->id));
    }
}
