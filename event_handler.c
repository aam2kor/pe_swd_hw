#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <time.h>
#include <stdint.h>

#include "tinyexpr.h"

#define MAX_RULES   32
#define MAX_EVENTS  8
#define FIFO_PATH  "/tmp/motion_fifo"

/* -------------------- Data Structures -------------------- */

typedef enum { MODE_CONT, MODE_END } checkmode_t;

typedef struct {
    int start_min;
    int end_min;
} window_t;

typedef struct {
    window_t    win;
    char        cont_expr_str[128];
    char        end_expr_str[128];
    char        cont_actions[256];
    char        end_actions[256];
} rule_t;

typedef enum {
    EV_WIN_START,
    EV_WIN_END,
    EV_MOTION
} ev_type_t;

typedef struct {
    ev_type_t type;
    int       rule_index;
    int       fd;          /* <-- REQUIRED */
} ev_ctx_t;

/* -------------------- Globals -------------------- */

rule_t rules[MAX_RULES];
int rule_count = 0;

int epfd;
int motion_fd;

int motion = 0;
int window_active = 0;
int active_rule = -1;

//double motion_val;

/* motion epoll context (static lifetime) */
static ev_ctx_t motion_ctx = {
    .type = EV_MOTION,
    .rule_index = -1,
    .fd = -1
};

/* -------------------- Helpers -------------------- */

/* Convert "HH:MM" → minutes from midnight */
int parse_time(const char *s)
{
    int h, m;
    sscanf(s, "%d:%d", &h, &m);
    return h * 60 + m;
}

/* Current time in minutes from midnight */
int now_minutes(void)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    return tm.tm_hour * 60 + tm.tm_min;
}

/* Drain FIFO contents */
void drain_fifo(int fd)
{
    char buf[64];
    while (read(fd, buf, sizeof(buf)) > 0);
}

/* Get current time as formatted string */
const char* get_time_string(void)
{
    static char time_buf[32];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm);
    return time_buf;
}

/* -------------------- Rule Loading -------------------- */

void load_rules(const char *file)
{
    FILE *fp = fopen(file, "r");
    char line[512];

    while (fgets(line, sizeof(line), fp)) {
        rule_t *r = &rules[rule_count];

        char window[64], cont_expr[128], end_expr[128];
        char cont_actions[256], end_actions[256];
        char start[16], end[16];

        sscanf(line,
               " %63[^|] | %127[^|] | %127[^|] | %255[^|] | %255[^\n]",
               window, cont_expr, end_expr, cont_actions, end_actions);

        sscanf(window, "%15[^-]-%15s", start, end);
        r->win.start_min = parse_time(start);
        r->win.end_min = parse_time(end);

        strcpy(r->cont_expr_str, cont_expr);
        strcpy(r->end_expr_str, end_expr);
        strcpy(r->cont_actions, cont_actions);
        strcpy(r->end_actions, end_actions);

        rule_count++;
    }
    printf("[%s] Loaded %d rules\n", get_time_string(), rule_count);
    fclose(fp);
}
/* -------------------- Rule Evaluation -------------------- */

void evaluate_rule(int rule_idx, checkmode_t mode)
{
    if (rule_idx < 0 || rule_idx >= rule_count)
        return;

    rule_t *r = &rules[rule_idx];
    double motion_val = motion;
    te_variable vars[] = { { "motion", &motion_val } };

    char *expr_str = (mode == MODE_CONT) ? r->cont_expr_str : r->end_expr_str;
    char *actions_str = (mode == MODE_CONT) ? r->cont_actions : r->end_actions;

    te_expr *expr = te_compile(expr_str, vars, 1, NULL);
    if (expr && te_eval(expr)) {
        /* Parse and execute comma-separated actions */
        char actions_copy[256];
        strcpy(actions_copy, actions_str);
        
        char *action = strtok(actions_copy, ",");
        while (action != NULL) {
            /* Trim whitespace from action */
            while (*action == ' ') action++;
            char *end_ptr = action + strlen(action) - 1;
            while (end_ptr >= action && *end_ptr == ' ') {
                *end_ptr = '\0';
                end_ptr--;
            }
            
            printf("[%s] ACTION: %s (expr=%s, motion=%d)\n", get_time_string(), action, expr_str, motion);
            action = strtok(NULL, ",");
        }
    }
    if (expr) te_free(expr);
}
/* -------------------- Timer Creation -------------------- */

int create_timer_at(int target_min)
{
    int tfd = timerfd_create(CLOCK_REALTIME, 0);

    struct itimerspec its = {0};

    int delta = target_min - now_minutes();
    if (delta < 0)
        delta += 24 * 60;

    its.it_value.tv_sec = delta * 60;
    timerfd_settime(tfd, 0, &its, NULL);

    return tfd;
}

/* -------------------- Main -------------------- */

int main(void)
{
    load_rules("rules.txt");

    /* Open FIFO */
    motion_fd = open("motion_fifo", O_RDONLY | O_NONBLOCK);

    /* Create epoll */
    epfd = epoll_create1(0);

    /* Create timers for each rule */
    for (int i = 0; i < rule_count; i++) {
        rule_t *r = &rules[i];

        int start_fd = create_timer_at(r->win.start_min);
        int end_fd   = create_timer_at(r->win.end_min);

        ev_ctx_t *start_ctx = malloc(sizeof(*start_ctx));
        start_ctx->type = EV_WIN_START;
        start_ctx->rule_index = i;
        start_ctx->fd = start_fd;

        ev_ctx_t *end_ctx = malloc(sizeof(*end_ctx));
        end_ctx->type = EV_WIN_END;
        end_ctx->rule_index = i;
        end_ctx->fd = end_fd;

        struct epoll_event ev;
        ev.events = EPOLLIN;

        ev.data.ptr = start_ctx;
        epoll_ctl(epfd, EPOLL_CTL_ADD, start_fd, &ev);

        ev.data.ptr = end_ctx;
        epoll_ctl(epfd, EPOLL_CTL_ADD, end_fd, &ev);
    }

    /* Motion event (added dynamically) */
    struct epoll_event motion_ev = {
        .events = EPOLLIN,
        .data.ptr = &motion_ctx
    };

    struct epoll_event events[MAX_EVENTS];

    while (1) {
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1);

        for (int i = 0; i < n; i++) {
            ev_ctx_t *ctx = events[i].data.ptr;

            /* Window start */
            if (ctx->type == EV_WIN_START) {
                uint64_t exp;
                read(ctx->fd, &exp, sizeof(exp));   /* <-- FIX */

                printf("[%s] WINDOW START (Rule %d)\n", get_time_string(), ctx->rule_index);
                window_active = 1;
                active_rule = ctx->rule_index;
                motion = 0;
                drain_fifo(motion_fd);

                epoll_ctl(epfd, EPOLL_CTL_ADD, motion_fd, &motion_ev);
            }

            /* Window end */
            else if (ctx->type == EV_WIN_END) {
                uint64_t exp;
                read(ctx->fd, &exp, sizeof(exp));   /* <-- FIX */

                printf("[%s] WINDOW END (Rule %d)\n", get_time_string(), ctx->rule_index);
                window_active = 0;

                epoll_ctl(epfd, EPOLL_CTL_DEL, motion_fd, NULL);
                evaluate_rule(ctx->rule_index, MODE_END);
                active_rule = -1;
            }

            /* Motion event */
            else if (ctx->type == EV_MOTION && window_active && active_rule >= 0) {
                char buf[32];
                read(motion_fd, buf, sizeof(buf));

                motion++;
                evaluate_rule(active_rule, MODE_CONT);
            }
        }
    }

    return 0;
}
