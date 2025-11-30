// scheduler.c
#include "scheduler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

pthread_mutex_t sched_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t sched_cond = PTHREAD_COND_INITIALIZER;
Job *job_queue = NULL;

// For the summary string at the bottom
static char timeline_buffer[4096];
static bool timeline_empty = true;

// Track last scheduled job to prevent immediate re-selection (unless only 1 left)
static int last_job_id = -1; 

void scheduler_init() {
    timeline_buffer[0] = '\0';
}

void add_job(Job *j) {
    j->next = NULL;
    if (!job_queue) {
        job_queue = j;
    } else {
        Job *curr = job_queue;
        while (curr->next) curr = curr->next;
        curr->next = j;
    }
}

void remove_job(Job *j) {
    if (!job_queue) return;
    if (job_queue == j) {
        job_queue = j->next;
        return;
    }
    Job *curr = job_queue;
    while (curr->next && curr->next != j) {
        curr = curr->next;
    }
    if (curr->next == j) {
        curr->next = j->next;
    }
}

// THE ALGORITHM: Combined SRJF + RR
Job* get_next_job() {
    if (!job_queue) return NULL;

    Job *best = NULL;
    Job *curr = job_queue;
    int count = 0;

    // 1. Check for Shell Commands (-1) - HIGHEST PRIORITY
    // They are non-preemptive, run immediately.
    while (curr) {
        if (curr->status != JOB_FINISHED && curr->is_shell_cmd) {
            return curr;
        }
        count++;
        curr = curr->next;
    }

    // 2. Filter for SRJF (Programs)
    curr = job_queue;
    int min_remaining = 999999;

    while (curr) {
        if (curr->status != JOB_FINISHED) {
            // Constraint: Same process can't be selected 2x consecutive times
            // UNLESS it is the only process left.
            bool skip = (count > 1 && curr->id == last_job_id);

            if (!skip) {
                if (curr->remaining_time < min_remaining) {
                    min_remaining = curr->remaining_time;
                    best = curr;
                }
            }
        }
        curr = curr->next;
    }

    if (best) {
        last_job_id = best->id;
    }
    return best;
}

void append_timeline(int job_id, int duration) {
    char entry[64];
    // Format: P(id)-(duration)-
    if (timeline_empty) {
        snprintf(entry, sizeof(entry), "P%d-(%d)", job_id, duration);
        timeline_empty = false;
    } else {
        snprintf(entry, sizeof(entry), "-P%d-(%d)", job_id, duration);
    }
    strncat(timeline_buffer, entry, sizeof(timeline_buffer) - strlen(timeline_buffer) - 1);
}

void print_timeline() {
    fprintf(stderr, "%s\n", timeline_buffer);
}