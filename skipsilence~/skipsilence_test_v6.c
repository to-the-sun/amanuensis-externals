#include <stdio.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>

typedef struct {
    double bar_ms;
    double last_ramp_ms;
    int sync_waiting;
    int playing;
    int next_bar_ready;
    int next_bar_needs_sync;
    char pending_play_sym[64];
    char current_buf[64];
    double playhead;
    double current_bar_start;
    double current_bar_end;
    double next_bar_start;
    double next_bar_end;

    int pending_bar_ready;
    double pending_bar_start;
    double pending_bar_end;
} t_skipsilence_mock;

bool simulate_perform(t_skipsilence_mock *x, double ramp) {
    int sync_event = 0;
    if (x->bar_ms > 0) {
        if (ramp == 0.0) sync_event = 1;
        else if (x->last_ramp_ms >= 0) {
            if (ramp < x->last_ramp_ms) sync_event = 1;
            else if (floor(ramp / x->bar_ms) > floor(x->last_ramp_ms / x->bar_ms)) sync_event = 1;
        }
    } else sync_event = 1;
    x->last_ramp_ms = ramp;

    if (sync_event) {
        if (x->playing && x->pending_bar_ready) {
            printf("  PERFORM: synchronized switch to pending buffer '%s'\n", x->pending_play_sym);
            strcpy(x->current_buf, x->pending_play_sym);
            x->pending_play_sym[0] = '\0';
            x->current_bar_start = x->pending_bar_start;
            x->current_bar_end = x->pending_bar_end;
            x->playhead = x->current_bar_start;
            x->pending_bar_ready = 0;
            x->next_bar_ready = 0;
            x->sync_waiting = 0;
            return true;
        } else if (x->sync_waiting && x->next_bar_ready) {
            printf("  PERFORM: sync achieved, next bar transition\n");
            x->current_bar_start = x->next_bar_start;
            x->current_bar_end = x->next_bar_end;
            x->playhead = x->current_bar_start;
            x->next_bar_ready = 0;
            x->sync_waiting = 0;
            return true;
        }
    }

    if (x->playing) {
        x->playhead += 1000.0; // Simulate time passing
        if (x->playhead >= x->current_bar_end) {
            if (x->next_bar_ready) {
                if (x->next_bar_needs_sync) {
                    printf("  PERFORM: bar end reached, needs sync. Holding playhead at end (SILENCE).\n");
                    x->sync_waiting = 1;
                    x->playhead = x->current_bar_end;
                } else {
                    printf("  PERFORM: normal bar transition\n");
                    x->current_bar_start = x->next_bar_start;
                    x->current_bar_end = x->next_bar_end;
                    x->playhead = x->current_bar_start;
                    x->next_bar_ready = 0;
                }
            } else {
                 printf("  PERFORM: no next bar ready. Holding at end.\n");
                 x->playhead = x->current_bar_end;
            }
        }
    }
    return false;
}

void test_refined_sync() {
    printf("Test: Refined Synchronization Behavior\n");
    t_skipsilence_mock x = {0};
    x.bar_ms = 1000.0;
    x.last_ramp_ms = 500.0;
    x.playing = 1;
    x.current_bar_start = 0;
    x.current_bar_end = 5000;
    x.playhead = 4000;
    strcpy(x.current_buf, "buf1");

    printf("\nScenario 1: Repeat needs Sync (No Looping)\n");
    x.next_bar_ready = 1;
    x.next_bar_needs_sync = 1;
    x.next_bar_start = 0;
    x.next_bar_end = 44100;

    printf("  Ramp at 600.0...\n");
    simulate_perform(&x, 600.0);
    printf("  playhead: %.1f\n", x.playhead);

    printf("  Ramp at 700.0 (reached bar end)...\n");
    simulate_perform(&x, 700.0);
    printf("  playhead: %.1f, sync_waiting: %d\n", x.playhead, x.sync_waiting);

    printf("  Ramp at 1005.0 (SYNC!)...\n");
    simulate_perform(&x, 1005.0);
    printf("  playhead: %.1f, sync_waiting: %d\n", x.playhead, x.sync_waiting);

    printf("\nScenario 2: Buffer Switch during Playback (Normal play interim)\n");
    x.playing = 1;
    x.current_bar_start = 44100;
    x.current_bar_end = 88200;
    x.playhead = 50000;
    strcpy(x.current_buf, "buf1");
    strcpy(x.pending_play_sym, "buf2");
    x.pending_bar_start = 0;
    x.pending_bar_end = 44100;
    x.pending_bar_ready = 1;
    x.last_ramp_ms = 1200.0;

    printf("  Ramp at 1300.0 (no sync)...\n");
    simulate_perform(&x, 1300.0);
    printf("  current_buf: %s, playhead: %.1f\n", x.current_buf, x.playhead);

    printf("  Ramp at 2005.0 (SYNC!)...\n");
    simulate_perform(&x, 2005.0);
    printf("  current_buf: %s, playhead: %.1f\n", x.current_buf, x.playhead);
}

int main() {
    test_refined_sync();
    return 0;
}
