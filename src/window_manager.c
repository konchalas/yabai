#include "window_manager.h"

extern struct event_loop g_event_loop;
extern struct process_manager g_process_manager;
extern struct mouse_state g_mouse_state;
extern char g_sa_socket_file[MAXLEN];

static TABLE_HASH_FUNC(hash_wm)
{
    return *(uint32_t *) key;
}

static TABLE_COMPARE_FUNC(compare_wm)
{
    return *(uint32_t *) key_a == *(uint32_t *) key_b;
}

void window_manager_query_window_rules(FILE *rsp)
{
    fprintf(rsp, "[");
    for (int i = 0; i < buf_len(g_window_manager.rules); ++i) {
        struct rule *rule = &g_window_manager.rules[i];
        rule_serialize(rsp, rule, i);
        if (i < buf_len(g_window_manager.rules) - 1) fprintf(rsp, ",");
    }
    fprintf(rsp, "]\n");
}

void window_manager_query_windows_for_space(FILE *rsp, uint64_t sid)
{
    int window_count;
    uint32_t *window_list = space_window_list(sid, &window_count, true);
    if (!window_list) return;

    struct window **window_aggregate_list = NULL;
    for (int i = 0; i < window_count; ++i) {
        struct window *window = window_manager_find_window(&g_window_manager, window_list[i]);
        if (window) buf_push(window_aggregate_list, window);
    }

    fprintf(rsp, "[");
    for (int i = 0; i < buf_len(window_aggregate_list); ++i) {
        struct window *window = window_aggregate_list[i];
        window_serialize(rsp, window);
        if (i < buf_len(window_aggregate_list) - 1) fprintf(rsp, ",");
    }
    fprintf(rsp, "]\n");

    buf_free(window_aggregate_list);
    free(window_list);
}

void window_manager_query_windows_for_display(FILE *rsp, uint32_t did)
{
    int space_count;
    uint64_t *space_list = display_space_list(did, &space_count);
    if (!space_list) return;

    struct window **window_aggregate_list = NULL;
    for (int i = 0; i < space_count; ++i) {
        int window_count;
        uint32_t *window_list = space_window_list(space_list[i], &window_count, true);
        if (!window_list) continue;

        for (int j = 0; j < window_count; ++j) {
            struct window *window = window_manager_find_window(&g_window_manager, window_list[j]);
            if (window) buf_push(window_aggregate_list, window);
        }

        free(window_list);
    }

    fprintf(rsp, "[");
    for (int i = 0; i < buf_len(window_aggregate_list); ++i) {
        struct window *window = window_aggregate_list[i];
        window_serialize(rsp, window);
        if (i < buf_len(window_aggregate_list) - 1) fprintf(rsp, ",");
    }
    fprintf(rsp, "]\n");

    buf_free(window_aggregate_list);
    free(space_list);
}

void window_manager_query_windows_for_displays(FILE *rsp)
{
    uint32_t display_count;
    uint32_t *display_list = display_manager_active_display_list(&display_count);
    if (!display_list) return;

    struct window **window_aggregate_list = NULL;
    for (int i = 0; i < display_count; ++i) {
        int space_count;
        uint64_t *space_list = display_space_list(display_list[i], &space_count);
        if (!space_list) continue;

        for (int j = 0; j < space_count; ++j) {
            int window_count;
            uint32_t *window_list = space_window_list(space_list[j], &window_count, true);
            if (!window_list) continue;

            for (int k = 0; k < window_count; ++k) {
                struct window *window = window_manager_find_window(&g_window_manager, window_list[k]);
                if (window) buf_push(window_aggregate_list, window);
            }

            free(window_list);
        }

        free(space_list);
    }

    fprintf(rsp, "[");
    for (int i = 0; i < buf_len(window_aggregate_list); ++i) {
        struct window *window = window_aggregate_list[i];
        window_serialize(rsp, window);
        if (i < buf_len(window_aggregate_list) - 1) fprintf(rsp, ",");
    }
    fprintf(rsp, "]\n");

    buf_free(window_aggregate_list);
    free(display_list);
}

void window_manager_apply_rule_to_window(struct space_manager *sm, struct window_manager *wm, struct window *window, struct rule *rule)
{
    int regex_match_app = rule->app_regex_exclude ? REGEX_MATCH_YES : REGEX_MATCH_NO;
    if (regex_match(rule->app_regex_valid,   &rule->app_regex,   window->application->name) == regex_match_app)   return;

    int regex_match_title = rule->title_regex_exclude ? REGEX_MATCH_YES : REGEX_MATCH_NO;
    if (regex_match(rule->title_regex_valid, &rule->title_regex, window_title(window))      == regex_match_title) return;

    if (rule->sid || rule->did) {
        if (!window_is_fullscreen(window) && !space_is_fullscreen(window_space(window))) {
            uint64_t sid = rule->did ? display_space_id(rule->did) : rule->sid;
            window_manager_send_window_to_space(sm, wm, window, sid, true);
            if (rule->follow_space || rule->fullscreen == RULE_PROP_ON) {
                space_manager_focus_space(sid);
            }
        }
    }

    if (in_range_ei(rule->alpha, 0.0f, 1.0f)) {
        window->opacity = rule->alpha;
        window_manager_set_opacity(wm, window, rule->alpha);
    }

    if (rule->manage == RULE_PROP_ON) {
        window->rule_manage = true;
        window->is_floating = false;
        window_manager_make_floating(wm, window, false);
        if ((window_manager_should_manage_window(window)) && (!window_manager_find_managed_window(wm, window))) {
            struct view *view = space_manager_tile_window_on_space(sm, window, space_manager_active_space());
            window_manager_add_managed_window(wm, window, view);
        }
    } else if (rule->manage == RULE_PROP_OFF) {
        window->rule_manage = false;
        struct view *view = window_manager_find_managed_window(wm, window);
        if (view) {
            space_manager_untile_window(sm, view, window);
            window_manager_remove_managed_window(wm, window->id);
            window_manager_purify_window(wm, window);
        }
        window_manager_make_floating(wm, window, true);
        window->is_floating = true;
    }

    if (rule->sticky == RULE_PROP_ON) {
        window_manager_make_sticky(window->id, true);
    } else if (rule->sticky == RULE_PROP_OFF) {
        window_manager_make_sticky(window->id, false);
    }

    if (rule->layer) {
        window_manager_set_window_layer(window, rule->layer);
    }

    if (rule->border == RULE_PROP_ON) {
        border_create(window);
    } else if (rule->border == RULE_PROP_OFF) {
        border_destroy(window);
    }

    if (rule->fullscreen == RULE_PROP_ON) {
        AXUIElementSetAttributeValue(window->ref, kAXFullscreenAttribute, kCFBooleanTrue);
        window->rule_fullscreen = true;
    }

    if (rule->grid[0] != 0 && rule->grid[1] != 0) {
        window_manager_apply_grid(sm, wm, window, rule->grid[0], rule->grid[1], rule->grid[2], rule->grid[3], rule->grid[4], rule->grid[5]);
    }
}

void window_manager_apply_rules_to_window(struct space_manager *sm, struct window_manager *wm, struct window *window)
{
    for (int i = 0; i < buf_len(wm->rules); ++i) {
        window_manager_apply_rule_to_window(sm, wm, window, &wm->rules[i]);
    }
}

void window_manager_set_window_border_enabled(struct window_manager *wm, bool enabled)
{
    wm->enable_window_border = enabled;

    for (int window_index = 0; window_index < wm->window.capacity; ++window_index) {
        struct bucket *bucket = wm->window.buckets[window_index];
        while (bucket) {
            if (bucket->value) {
                struct window *window = bucket->value;
                if (enabled) {
                    border_create(window);
                } else {
                    border_destroy(window);
                }
            }

            bucket = bucket->next;
        }
    }

    struct window *window = window_manager_focused_window(wm);
    if (window) border_activate(window);
}

void window_manager_set_window_border_width(struct window_manager *wm, int width)
{
    wm->border_width = width;
    for (int window_index = 0; window_index < wm->window.capacity; ++window_index) {
        struct bucket *bucket = wm->window.buckets[window_index];
        while (bucket) {
            if (bucket->value) {
                struct window *window = bucket->value;
                if (window->border.id) {
                    CGContextSetLineWidth(window->border.context, width);
                    border_redraw(window);
                }
            }

            bucket = bucket->next;
        }
    }
}

void window_manager_set_active_window_border_color(struct window_manager *wm, uint32_t color)
{
    wm->active_border_color = rgba_color_from_hex(color);
    struct window *window = window_manager_focused_window(wm);
    if (window) border_activate(window);
}

void window_manager_set_normal_window_border_color(struct window_manager *wm, uint32_t color)
{
    wm->normal_border_color = rgba_color_from_hex(color);
    for (int window_index = 0; window_index < wm->window.capacity; ++window_index) {
        struct bucket *bucket = wm->window.buckets[window_index];
        while (bucket) {
            if (bucket->value) {
                struct window *window = bucket->value;
                if (window->id != wm->focused_window_id) {
                    border_deactivate(window);
                }
            }

            bucket = bucket->next;
        }
    }
}

void window_manager_center_mouse(struct window_manager *wm, struct window *window)
{
    if (!wm->enable_mff) return;

    CGPoint cursor;
    SLSGetCurrentCursorLocation(g_connection, &cursor);

    CGRect frame = window_frame(window);
    if (CGRectContainsPoint(frame, cursor)) return;

    uint32_t did = window_display_id(window);
    if (!did) return;

    CGPoint center = {
        frame.origin.x + frame.size.width / 2,
        frame.origin.y + frame.size.height / 2
    };

    CGRect bounds = display_bounds(did);
    if (!CGRectContainsPoint(bounds, center)) return;

    CGWarpMouseCursorPosition(center);
}

bool window_manager_should_manage_window(struct window *window)
{
    if (window->is_floating)      return false;
    if (window_is_sticky(window)) return false;
    if (window->rule_manage)      return true;

    return ((window_level_is_standard(window)) &&
            (window_is_standard(window)) &&
            (window_can_move(window)));
}

struct view *window_manager_find_managed_window(struct window_manager *wm, struct window *window)
{
    return table_find(&wm->managed_window, &window->id);
}

void window_manager_remove_managed_window(struct window_manager *wm, uint32_t wid)
{
    table_remove(&wm->managed_window, &wid);
}

void window_manager_add_managed_window(struct window_manager *wm, struct window *window, struct view *view)
{
    if (view->layout != VIEW_BSP) return;
    table_add(&wm->managed_window, &window->id, view);
    window_manager_purify_window(wm, window);
}

enum window_op_error window_manager_adjust_window_ratio(struct window_manager *wm, struct window *window, int type, float ratio)
{
    struct view *view = window_manager_find_managed_window(wm, window);
    if (!view) return WINDOW_OP_ERROR_INVALID_SRC_VIEW;

    struct window_node *node = view_find_window_node(view, window->id);
    if (!node || !node->parent) return WINDOW_OP_ERROR_INVALID_SRC_NODE;

    switch (type) {
    case TYPE_REL: {
        node->parent->ratio = clampf_range(node->parent->ratio + ratio, 0.1f, 0.9f);
    } break;
    case TYPE_ABS: {
        node->parent->ratio = clampf_range(ratio, 0.1f, 0.9f);
    } break;
    }

    window_node_update(view, node->parent);
    window_node_flush(node->parent);

    return WINDOW_OP_ERROR_SUCCESS;
}

enum window_op_error window_manager_move_window_relative(struct window_manager *wm, struct window *window, int type, float dx, float dy)
{
    struct view *view = window_manager_find_managed_window(wm, window);
    if (view) return WINDOW_OP_ERROR_INVALID_SRC_VIEW;

    if (type == TYPE_REL) {
        CGRect frame = window_frame(window);
        dx += frame.origin.x;
        dy += frame.origin.y;
    }

    window_manager_move_window(window, dx, dy);

    return WINDOW_OP_ERROR_SUCCESS;
}

enum window_op_error window_manager_resize_window_relative(struct window_manager *wm, struct window *window, int direction, float dx, float dy)
{
    struct view *view = window_manager_find_managed_window(wm, window);
    if (view) {
        struct window_node *x_fence = NULL;
        struct window_node *y_fence = NULL;

        struct window_node *node = view_find_window_node(view, window->id);
        if (!node) return WINDOW_OP_ERROR_INVALID_SRC_NODE;

        if (direction & HANDLE_TOP)    x_fence = window_node_fence(node, DIR_NORTH);
        if (direction & HANDLE_BOTTOM) x_fence = window_node_fence(node, DIR_SOUTH);
        if (direction & HANDLE_LEFT)   y_fence = window_node_fence(node, DIR_WEST);
        if (direction & HANDLE_RIGHT)  y_fence = window_node_fence(node, DIR_EAST);
        if (!x_fence && !y_fence)      return WINDOW_OP_ERROR_INVALID_DST_NODE;

        if (y_fence) {
            float sr = y_fence->ratio + (float) dx / (float) y_fence->area.w;
            y_fence->ratio = min(1, max(0, sr));
        }

        if (x_fence) {
            float sr = x_fence->ratio + (float) dy / (float) x_fence->area.h;
            x_fence->ratio = min(1, max(0, sr));
        }

        view_update(view);
        view_flush(view);
    } else {
        if (direction == HANDLE_ABS) {
            window_manager_resize_window(window, dx, dy);
        } else {
            int x_mod = (direction & HANDLE_LEFT) ? -1 : (direction & HANDLE_RIGHT)  ? 1 : 0;
            int y_mod = (direction & HANDLE_TOP)  ? -1 : (direction & HANDLE_BOTTOM) ? 1 : 0;

            CGRect frame = window_frame(window);
            float fw = max(1, frame.size.width  + dx * x_mod);
            float fh = max(1, frame.size.height + dy * y_mod);
            float fx = (direction & HANDLE_LEFT) ? frame.origin.x + frame.size.width  - fw : frame.origin.x;
            float fy = (direction & HANDLE_TOP)  ? frame.origin.y + frame.size.height - fh : frame.origin.y;

            window_manager_move_window(window, fx, fy);
            window_manager_resize_window(window, fw, fh);
        }
    }

    return WINDOW_OP_ERROR_SUCCESS;
}

void window_manager_add_to_window_group(uint32_t child_wid, uint32_t parent_wid)
{
    int sockfd;
    char message[MAXLEN];

    if (socket_connect_un(&sockfd, g_sa_socket_file)) {
        snprintf(message, sizeof(message), "window_group_add %d %d", parent_wid, child_wid);
        socket_write(sockfd, message);
        socket_wait(sockfd);
    }
    socket_close(sockfd);
}

void window_manager_remove_from_window_group(uint32_t child_wid, uint32_t parent_wid)
{
    int sockfd;
    char message[MAXLEN];

    if (socket_connect_un(&sockfd, g_sa_socket_file)) {
        snprintf(message, sizeof(message), "window_group_remove %d %d", parent_wid, child_wid);
        socket_write(sockfd, message);
        socket_wait(sockfd);
    }
    socket_close(sockfd);
}

void window_manager_move_window_cgs(struct window *window, float x, float y, float dx, float dy)
{
    int sockfd;
    char message[MAXLEN];

    float fx = x + dx;
    float fy = y + dy;

    uint32_t did = display_manager_point_display_id((CGPoint) { fx, fy });
    if (!did) return;

    CGRect bounds = display_bounds(did);
    if (fy < bounds.origin.y) fy = bounds.origin.y;

    if (socket_connect_un(&sockfd, g_sa_socket_file)) {
        snprintf(message, sizeof(message), "window_move %d %d %d", window->id, (int)fx, (int)fy);
        socket_write(sockfd, message);
        socket_wait(sockfd);
    }
    socket_close(sockfd);
}

void window_manager_move_window(struct window *window, float x, float y)
{
    CGPoint position = CGPointMake(x, y);
    CFTypeRef position_ref = AXValueCreate(kAXValueTypeCGPoint, (void *) &position);
    if (!position_ref) return;

    if (AXUIElementSetAttributeValue(window->ref, kAXPositionAttribute, position_ref) == kAXErrorSuccess) {
        if (window->border.id) SLSMoveWindow(g_connection, window->border.id, &position);
    }

    CFRelease(position_ref);
}

void window_manager_resize_window(struct window *window, float width, float height)
{
    CGSize size = CGSizeMake(width, height);
    CFTypeRef size_ref = AXValueCreate(kAXValueTypeCGSize, (void *) &size);
    if (!size_ref) return;

    AXUIElementSetAttributeValue(window->ref, kAXSizeAttribute, size_ref);
    CFRelease(size_ref);
}

void window_manager_set_window_frame(struct window *window, float x, float y, float width, float height)
{
    window_manager_resize_window(window, width, height);
    window_manager_move_window(window, x, y);
    window_manager_resize_window(window, width, height);
}

void window_manager_set_purify_mode(struct window_manager *wm, enum purify_mode mode)
{
    wm->purify_mode = mode;
    for (int window_index = 0; window_index < wm->window.capacity; ++window_index) {
        struct bucket *bucket = wm->window.buckets[window_index];
        while (bucket) {
            if (bucket->value) {
                struct window *window = bucket->value;
                window_manager_purify_window(wm, window);
            }

            bucket = bucket->next;
        }
    }
}

void window_manager_set_opacity(struct window_manager *wm, struct window *window, float opacity)
{
    int sockfd;
    char message[MAXLEN];

    if (opacity == 0.0f) {
        if (wm->enable_window_opacity) {
            opacity = window->id == wm->focused_window_id ? wm->active_window_opacity : wm->normal_window_opacity;
        } else {
            opacity = 1.0f;
        }
    }

    if (socket_connect_un(&sockfd, g_sa_socket_file)) {
        snprintf(message, sizeof(message), "window_alpha_fade %d %f %f", window->id, opacity, wm->window_opacity_duration);
        socket_write(sockfd, message);
        socket_wait(sockfd);
    }
    socket_close(sockfd);
}

void window_manager_set_window_opacity(struct window_manager *wm, struct window *window, float opacity)
{
    if (!wm->enable_window_opacity) return;
    if (window->opacity != 0.0f)    return;
    if ((!window_is_standard(window)) && (!window_is_dialog(window))) return;

    window_manager_set_opacity(wm, window, opacity);
}

void window_manager_set_active_window_opacity(struct window_manager *wm, float opacity)
{
    wm->active_window_opacity = opacity;
    struct window *window = window_manager_focused_window(wm);
    if (window) window_manager_set_window_opacity(wm, window, wm->active_window_opacity);
}

void window_manager_set_normal_window_opacity(struct window_manager *wm, float opacity)
{
    wm->normal_window_opacity = opacity;
    for (int window_index = 0; window_index < wm->window.capacity; ++window_index) {
        struct bucket *bucket = wm->window.buckets[window_index];
        while (bucket) {
            if (bucket->value) {
                struct window *window = bucket->value;
                if (window->id == wm->focused_window_id) goto next;
                window_manager_set_window_opacity(wm, window, wm->normal_window_opacity);
            }

next:
            bucket = bucket->next;
        }
    }
}

void window_manager_set_layer(uint32_t wid, int layer)
{
    int sockfd;
    char message[MAXLEN];

    if (socket_connect_un(&sockfd, g_sa_socket_file)) {
        snprintf(message, sizeof(message), "window_level %d %d", wid, layer);
        socket_write(sockfd, message);
        socket_wait(sockfd);
    }
    socket_close(sockfd);
}

static void window_manager_set_layer_for_window_relation_with_parent(uint32_t *parents, uint32_t *children, int count, uint32_t wid, int layer)
{
    for (int i = 0; i < count; ++i) {
        if (parents[i] == wid) {
            window_manager_set_layer(children[i], layer);
            window_manager_set_layer_for_window_relation_with_parent(parents, children, count, children[i], layer);
        }
    }
}

static void window_manager_set_layer_for_children(int cid, uint32_t wid, uint64_t sid, int layer)
{
    int count;
    uint32_t *window_list = space_window_list(sid, &count, false);
    if (!window_list) return;

    CFArrayRef window_list_ref = cfarray_of_cfnumbers(window_list, sizeof(uint32_t), count, kCFNumberSInt32Type);
    CFTypeRef query = SLSWindowQueryWindows(g_connection, window_list_ref, count);
    CFTypeRef iterator = SLSWindowQueryResultCopyWindows(query);

    int relation_count = 0;
    uint32_t parents[count];
    uint32_t children[count];

    while (SLSWindowIteratorAdvance(iterator)) {
        parents[relation_count]  = SLSWindowIteratorGetParentID(iterator);
        children[relation_count] = SLSWindowIteratorGetWindowID(iterator);
        ++relation_count;
    }

    assert(relation_count == count);
    window_manager_set_layer_for_window_relation_with_parent(parents, children, relation_count, wid, layer);

    CFRelease(query);
    CFRelease(iterator);
    CFRelease(window_list_ref);
    free(window_list);
}

void window_manager_set_window_layer(struct window *window, int layer)
{
    uint64_t sid = window_space(window);
    if (!sid) sid = space_manager_active_space();

    window_manager_set_layer(window->id, layer);
    window_manager_set_layer_for_children(window->connection, window->id, sid, layer);
}

void window_manager_make_floating(struct window_manager *wm, struct window *window, bool floating)
{
    if (!wm->enable_window_topmost) return;

    int layer = floating ? LAYER_ABOVE : LAYER_NORMAL;
    window_manager_set_window_layer(window, layer);
}

void window_manager_make_sticky(uint32_t wid, bool sticky)
{
    int sockfd;
    char message[MAXLEN];

    if (socket_connect_un(&sockfd, g_sa_socket_file)) {
        snprintf(message, sizeof(message), "window_sticky %d %d", wid, sticky);
        socket_write(sockfd, message);
        socket_wait(sockfd);
    }
    socket_close(sockfd);
}

void window_manager_purify_window(struct window_manager *wm, struct window *window)
{
    int value;
    int sockfd;
    char message[MAXLEN];

    if (wm->purify_mode == PURIFY_DISABLED) {
        value = 1;
    } else if (wm->purify_mode == PURIFY_MANAGED) {
        value = window_manager_find_managed_window(wm, window) ? 0 : 1;
    } else if (wm->purify_mode == PURIFY_ALWAYS) {
        value = 0;
    }

    if (socket_connect_un(&sockfd, g_sa_socket_file)) {
        snprintf(message, sizeof(message), "window_shadow %d %d", window->id, value);
        socket_write(sockfd, message);
        socket_wait(sockfd);
        window->has_shadow = value;
    }
    socket_close(sockfd);
}

struct window *window_manager_find_window_on_space_by_rank(struct window_manager *wm, uint64_t sid, int rank)
{
    int count;
    uint32_t *window_list = space_window_list(sid, &count, false);
    if (!window_list) return NULL;

    struct window *result = NULL;
    for (int i = 0, j = 0; i < count; ++i) {
        struct window *window = window_manager_find_window(wm, window_list[i]);
        if (!window) continue;

        if (++j == rank) {
            result = window;
            break;
        }
    }

    free(window_list);
    return result;
}

struct window *window_manager_find_window_at_point_filtering_window(struct window_manager *wm, CGPoint point, uint32_t filter_wid)
{
    CGPoint window_point;
    uint32_t window_id;
    int window_cid;

    SLSFindWindowByGeometry(g_connection, filter_wid, -1, 0, &point, &window_point, &window_id, &window_cid);
    return window_manager_find_window(wm, window_id);
}

struct window *window_manager_find_window_at_point(struct window_manager *wm, CGPoint point)
{
    CGPoint window_point;
    uint32_t window_id;
    int window_cid;

    SLSFindWindowByGeometry(g_connection, 0, 1, 0, &point, &window_point, &window_id, &window_cid);
    if (g_connection == window_cid) SLSFindWindowByGeometry(g_connection, window_id, -1, 0, &point, &window_point, &window_id, &window_cid);

    return window_manager_find_window(wm, window_id);
}

struct window *window_manager_find_window_below_cursor(struct window_manager *wm)
{
    CGPoint cursor;
    SLSGetCurrentCursorLocation(g_connection, &cursor);
    return window_manager_find_window_at_point(wm, cursor);
}

struct window *window_manager_find_closest_managed_window_in_direction(struct window_manager *wm, struct window *window, int direction)
{
    struct view *view = window_manager_find_managed_window(wm, window);
    if (!view) return NULL;

    struct window_node *node = view_find_window_node(view, window->id);
    if (!node) return NULL;

    struct window_node *closest = view_find_window_node_in_direction(view, node, direction);
    if (!closest) return NULL;

    return window_manager_find_window(wm, closest->window_id);
}

struct window *window_manager_find_prev_managed_window(struct space_manager *sm, struct window_manager *wm, struct window *window)
{
    struct view *view = space_manager_find_view(sm, space_manager_active_space());
    if (!view) return NULL;

    struct window_node *node = view_find_window_node(view, window->id);
    if (!node) return NULL;

    struct window_node *prev = window_node_find_prev_leaf(node);
    if (!prev) return NULL;

    return window_manager_find_window(wm, prev->window_id);
}

struct window *window_manager_find_next_managed_window(struct space_manager *sm, struct window_manager *wm, struct window *window)
{
    struct view *view = space_manager_find_view(sm, space_manager_active_space());
    if (!view) return NULL;

    struct window_node *node = view_find_window_node(view, window->id);
    if (!node) return NULL;

    struct window_node *next = window_node_find_next_leaf(node);
    if (!next) return NULL;

    return window_manager_find_window(wm, next->window_id);
}

struct window *window_manager_find_first_managed_window(struct space_manager *sm, struct window_manager *wm)
{
    struct view *view = space_manager_find_view(sm, space_manager_active_space());
    if (!view) return NULL;

    struct window_node *first = window_node_find_first_leaf(view->root);
    if (!first) return NULL;

    return window_manager_find_window(wm, first->window_id);
}

struct window *window_manager_find_last_managed_window(struct space_manager *sm, struct window_manager *wm)
{
    struct view *view = space_manager_find_view(sm, space_manager_active_space());
    if (!view) return NULL;

    struct window_node *last = window_node_find_last_leaf(view->root);
    if (!last) return NULL;

    return window_manager_find_window(wm, last->window_id);
}

struct window *window_manager_find_recent_managed_window(struct space_manager *sm, struct window_manager *wm)
{
    struct window *window = window_manager_find_window(wm, wm->last_window_id);
    if (!window) return NULL;

    struct view *view = window_manager_find_managed_window(wm, window);
    if (!view) return NULL;

    return window;
}

struct window *window_manager_find_largest_managed_window(struct space_manager *sm, struct window_manager *wm)
{
    struct view *view = space_manager_find_view(sm, space_manager_active_space());
    if (!view) return NULL;

    uint32_t best_id   = 0;
    uint32_t best_area = 0;

    for (struct window_node *node = window_node_find_first_leaf(view->root); node != NULL; node = window_node_find_next_leaf(node)) {
        uint32_t area = node->area.w * node->area.h;
        if (area > best_area) {
            best_id   = node->window_id;
            best_area = area;
        }
    }

    return best_id ? window_manager_find_window(wm, best_id) : NULL;
}

struct window *window_manager_find_smallest_managed_window(struct space_manager *sm, struct window_manager *wm)
{
    struct view *view = space_manager_find_view(sm, space_manager_active_space());
    if (!view) return NULL;

    uint32_t best_id   = 0;
    uint32_t best_area = UINT32_MAX;

    for (struct window_node *node = window_node_find_first_leaf(view->root); node != NULL; node = window_node_find_next_leaf(node)) {
        uint32_t area = node->area.w * node->area.h;
        if (area <= best_area) {
            best_id   = node->window_id;
            best_area = area;
        }
    }

    return best_id ? window_manager_find_window(wm, best_id) : NULL;
}

static void window_manager_defer_window_raise(ProcessSerialNumber *window_psn, uint32_t window_id)
{
    uint8_t bytes[0xf8] = {
        [0x04] = 0xf8,
        [0x08] = 0x0d,
        [0x8a] = 0x09
    };

    memcpy(bytes + 0x3c, &window_id, sizeof(uint32_t));
    SLPSPostEventRecordTo(window_psn, bytes);
}

static void window_manager_make_key_window(ProcessSerialNumber *window_psn, uint32_t window_id)
{
    uint8_t bytes1[0xf8] = {
        [0x04] = 0xF8,
        [0x08] = 0x01,
        [0x3a] = 0x10
    };

    uint8_t bytes2[0xf8] = {
        [0x04] = 0xF8,
        [0x08] = 0x02,
        [0x3a] = 0x10
    };

    memcpy(bytes1 + 0x3c, &window_id, sizeof(uint32_t));
    memset(bytes1 + 0x20, 0xFF, 0x10);
    memcpy(bytes2 + 0x3c, &window_id, sizeof(uint32_t));
    memset(bytes2 + 0x20, 0xFF, 0x10);
    SLPSPostEventRecordTo(window_psn, bytes1);
    SLPSPostEventRecordTo(window_psn, bytes2);
}

static void window_manager_deactivate_window(ProcessSerialNumber *window_psn, uint32_t window_id)
{
    uint8_t bytes[0xf8] = {
        [0x04] = 0xf8,
        [0x08] = 0x0d,
        [0x8a] = 0x02
    };

    memcpy(bytes + 0x3c, &window_id, sizeof(uint32_t));
    SLPSPostEventRecordTo(window_psn, bytes);
}

static void window_manager_activate_window(ProcessSerialNumber *window_psn, uint32_t window_id)
{
    uint8_t bytes[0xf8] = {
        [0x04] = 0xf8,
        [0x08] = 0x0d,
        [0x8a] = 0x01
    };

    memcpy(bytes + 0x3c, &window_id, sizeof(uint32_t));
    SLPSPostEventRecordTo(window_psn, bytes);
}

void window_manager_focus_window_without_raise(ProcessSerialNumber *window_psn, uint32_t window_id)
{
    window_manager_defer_window_raise(window_psn, window_id);

    if (psn_equals(window_psn, &g_window_manager.focused_window_psn)) {
        window_manager_deactivate_window(&g_window_manager.focused_window_psn, g_window_manager.focused_window_id);

        // @hack
        // Artificially delay the activation by 1ms. This is necessary
        // because some applications appear to be confused if both of
        // the events appear instantaneously.
        usleep(10000);

        window_manager_activate_window(window_psn, window_id);
    }

    _SLPSSetFrontProcessWithOptions(window_psn, window_id, kCPSUserGenerated);
    window_manager_make_key_window(window_psn, window_id);
}

void window_manager_focus_window_with_raise(ProcessSerialNumber *window_psn, uint32_t window_id, AXUIElementRef window_ref)
{
#if 1
    _SLPSSetFrontProcessWithOptions(window_psn, window_id, kCPSUserGenerated);
    window_manager_make_key_window(window_psn, window_id);
    AXUIElementPerformAction(window_ref, kAXRaiseAction);
#else
    int sockfd;
    char message[MAXLEN];

    if (socket_connect_un(&sockfd, g_sa_socket_file)) {
        snprintf(message, sizeof(message), "window_focus %d", window_id);
        socket_write(sockfd, message);
        socket_wait(sockfd);
    }
    socket_close(sockfd);
#endif
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
struct application *window_manager_focused_application(struct window_manager *wm)
{
    ProcessSerialNumber psn = {};
    _SLPSGetFrontProcess(&psn);

    pid_t pid;
    GetProcessPID(&psn, &pid);

    return window_manager_find_application(wm, pid);
}

struct window *window_manager_focused_window(struct window_manager *wm)
{
    struct application *application = window_manager_focused_application(wm);
    if (!application) return NULL;

    uint32_t window_id = application_focused_window(application);
    return window_manager_find_window(wm, window_id);
}
#pragma clang diagnostic pop

bool window_manager_find_lost_front_switched_event(struct window_manager *wm, pid_t pid)
{
    return table_find(&wm->application_lost_front_switched_event, &pid) != NULL;
}

void window_manager_remove_lost_front_switched_event(struct window_manager *wm, pid_t pid)
{
    table_remove(&wm->application_lost_front_switched_event, &pid);
}

void window_manager_add_lost_front_switched_event(struct window_manager *wm, pid_t pid)
{
    table_add(&wm->application_lost_front_switched_event, &pid, (void *)(intptr_t) 1);
}

bool window_manager_find_lost_focused_event(struct window_manager *wm, uint32_t window_id)
{
    return table_find(&wm->window_lost_focused_event, &window_id) != NULL;
}

void window_manager_remove_lost_focused_event(struct window_manager *wm, uint32_t window_id)
{
    table_remove(&wm->window_lost_focused_event, &window_id);
}

void window_manager_add_lost_focused_event(struct window_manager *wm, uint32_t window_id)
{
    table_add(&wm->window_lost_focused_event, &window_id, (void *)(intptr_t) 1);
}

struct window *window_manager_find_window(struct window_manager *wm, uint32_t window_id)
{
    return table_find(&wm->window, &window_id);
}

void window_manager_remove_window(struct window_manager *wm, uint32_t window_id)
{
    table_remove(&wm->window, &window_id);
}

void window_manager_add_window(struct window_manager *wm, struct window *window)
{
    table_add(&wm->window, &window->id, window);
}

struct application *window_manager_find_application(struct window_manager *wm, pid_t pid)
{
    return table_find(&wm->application, &pid);
}

void window_manager_remove_application(struct window_manager *wm, pid_t pid)
{
    table_remove(&wm->application, &pid);
}

void window_manager_add_application(struct window_manager *wm, struct application *application)
{
    table_add(&wm->application, &application->pid, application);
}

struct window **window_manager_find_application_windows(struct window_manager *wm, struct application *application)
{
    struct window **window_list = NULL;

    for (int window_index = 0; window_index < wm->window.capacity; ++window_index) {
        struct bucket *bucket = wm->window.buckets[window_index];
        while (bucket) {
            if (bucket->value) {
                struct window *window = bucket->value;
                if (window->application == application) {
                    buf_push(window_list, window);
                }
            }

            bucket = bucket->next;
        }
    }

    return window_list;
}

struct window *window_manager_create_and_add_window(struct space_manager *sm, struct window_manager *wm, struct application *application, AXUIElementRef window_ref, uint32_t window_id)
{
    struct window *window = window_create(application, window_ref, window_id);

    if (window_is_unknown(window)) {
        debug("%s: ignoring AXUnknown window %s %d\n", __FUNCTION__, window->application->name, window->id);
        window_manager_remove_lost_focused_event(wm, window->id);
        window_destroy(window);
        return NULL;
    }

    if (window_is_popover(window)) {
        debug("%s: ignoring AXPopover window %s %d\n", __FUNCTION__, window->application->name, window->id);
        window_manager_remove_lost_focused_event(wm, window->id);
        window_destroy(window);
        return NULL;
    }

    window_manager_purify_window(wm, window);
    window_manager_set_window_opacity(wm, window, wm->normal_window_opacity);

    if (!window_observe(window)) {
        debug("%s: could not observe %s %d\n", __FUNCTION__, window->application->name, window->id);
        window_manager_make_floating(wm, window, true);
        window_manager_remove_lost_focused_event(wm, window->id);
        window_unobserve(window);
        window_destroy(window);
        return NULL;
    }

    if (window_manager_find_lost_focused_event(wm, window->id)) {
        struct event *event = event_create(&g_event_loop, WINDOW_FOCUSED, (void *)(intptr_t) window->id);
        event_loop_post(&g_event_loop, event);
        window_manager_remove_lost_focused_event(wm, window->id);
    }

    debug("%s: %s %d\n", __FUNCTION__, window->application->name, window->id);
    window_manager_add_window(wm, window);
    window_manager_apply_rules_to_window(sm, wm, window);

    if ((!application->is_hidden) && (!window->is_minimized) && (!window->is_fullscreen) && (!window->rule_manage)) {
        if (window->rule_fullscreen) {
            window->rule_fullscreen = false;
        } else if ((!window_level_is_standard(window)) ||
                   (!window_is_standard(window)) ||
                   (!window_can_move(window)) ||
                   (window_is_sticky(window)) ||
                   (!window_can_resize(window) && window_is_undersized(window))) {
            window_manager_make_floating(wm, window, true);
            window->is_floating = true;
        }
    }

    return window;
}

void window_manager_add_application_windows(struct space_manager *sm, struct window_manager *wm, struct application *application)
{
    CFArrayRef window_list_ref = application_window_list(application);
    if (!window_list_ref) return;

    int window_count = CFArrayGetCount(window_list_ref);
    for (int i = 0; i < window_count; ++i) {
        AXUIElementRef window_ref = CFArrayGetValueAtIndex(window_list_ref, i);
        uint32_t window_id = ax_window_id(window_ref);
        if (!window_id || window_manager_find_window(wm, window_id)) continue;
        window_manager_create_and_add_window(sm, wm, application, CFRetain(window_ref), window_id);
    }

    CFRelease(window_list_ref);
}

enum window_op_error window_manager_set_window_insertion(struct space_manager *sm, struct window_manager *wm, struct window *window, int direction)
{
    uint64_t sid = window_space(window);
    struct view *view = space_manager_find_view(sm, sid);
    if (view->layout != VIEW_BSP) return WINDOW_OP_ERROR_INVALID_SRC_VIEW;

    struct window_node *node = view_find_window_node(view, window->id);
    if (!node) return WINDOW_OP_ERROR_INVALID_SRC_NODE;

    if (view->insertion_point && view->insertion_point != window->id) {
        struct window_node *insert_node = view_find_window_node(view, view->insertion_point);
        if (insert_node) {
            insert_feedback_destroy(insert_node);
            insert_node->split = SPLIT_NONE;
            insert_node->child = CHILD_NONE;
            insert_node->insert_dir = 0;
        }
    }

    if (direction == node->insert_dir) {
        insert_feedback_destroy(node);
        node->split = SPLIT_NONE;
        node->child = CHILD_NONE;
        node->insert_dir = 0;
        view->insertion_point = 0;
        return WINDOW_OP_ERROR_SUCCESS;
    }

    if (direction == DIR_NORTH) {
        node->split = SPLIT_X;
        node->child = CHILD_FIRST;
    } else if (direction == DIR_EAST) {
        node->split = SPLIT_Y;
        node->child = CHILD_SECOND;
    } else if (direction == DIR_SOUTH) {
        node->split = SPLIT_X;
        node->child = CHILD_SECOND;
    } else if (direction == DIR_WEST) {
        node->split = SPLIT_Y;
        node->child = CHILD_FIRST;
    }

    node->insert_dir = direction;
    view->insertion_point = node->window_id;
    insert_feedback_show(node);

    return WINDOW_OP_ERROR_SUCCESS;
}

enum window_op_error window_manager_warp_window(struct space_manager *sm, struct window_manager *wm, struct window *a, struct window *b)
{
    if (a->id == b->id) return WINDOW_OP_ERROR_SAME_WINDOW;

    uint64_t a_sid = window_space(a);
    struct view *a_view = space_manager_find_view(sm, a_sid);
    if (a_view->layout != VIEW_BSP) return WINDOW_OP_ERROR_INVALID_SRC_VIEW;

    uint64_t b_sid = window_space(b);
    struct view *b_view = space_manager_find_view(sm, b_sid);
    if (b_view->layout != VIEW_BSP) return WINDOW_OP_ERROR_INVALID_DST_VIEW;

    struct window_node *a_node = view_find_window_node(a_view, a->id);
    if (!a_node) return WINDOW_OP_ERROR_INVALID_SRC_NODE;

    struct window_node *b_node = view_find_window_node(b_view, b->id);
    if (!b_node) return WINDOW_OP_ERROR_INVALID_DST_NODE;

    if (a_node->parent == b_node->parent) {
        if (b_view->insertion_point == b_node->window_id) {
            b_node->parent->split = b_node->split;
            b_node->parent->child = b_node->child;
            space_manager_untile_window(sm, a_view, a);
            window_manager_remove_managed_window(wm, a->id);
            window_manager_add_managed_window(wm, a, b_view);
            space_manager_tile_window_on_space_with_insertion_point(sm, a, b_view->sid, b->id);
        } else {
            if (a_view->insertion_point == a_node->window_id) {
                a_view->insertion_point = b->id;
            }

            a_node->window_id = b->id;
            a_node->zoom = NULL;
            b_node->window_id = a->id;
            b_node->zoom = NULL;

            window_node_flush(a_node);
            window_node_flush(b_node);
        }
    } else {
        space_manager_untile_window(sm, a_view, a);

        if (a_view->sid != b_view->sid) {
            window_manager_remove_managed_window(wm, a->id);
            window_manager_add_managed_window(wm, a, b_view);

            if (wm->focused_window_id == a->id) {
                struct window *next = window_manager_find_window_on_space_by_rank(wm, a_view->sid, 2);
                if (next) {
                    window_manager_focus_window_with_raise(&next->application->psn, next->id, next->ref);
                } else {
                    _SLPSSetFrontProcessWithOptions(&g_process_manager.finder_psn, 0, kCPSNoWindows);
                }
            }

            space_manager_move_window_to_space(b_view->sid, a);
        }

        space_manager_tile_window_on_space_with_insertion_point(sm, a, b_view->sid, b->id);
    }

    return WINDOW_OP_ERROR_SUCCESS;
}

enum window_op_error window_manager_swap_window(struct space_manager *sm, struct window_manager *wm, struct window *a, struct window *b)
{
    if (a->id == b->id) return WINDOW_OP_ERROR_SAME_WINDOW;

    uint64_t a_sid = window_space(a);
    struct view *a_view = space_manager_find_view(sm, a_sid);
    if (a_view->layout != VIEW_BSP) return WINDOW_OP_ERROR_INVALID_SRC_VIEW;

    uint64_t b_sid = window_space(b);
    struct view *b_view = space_manager_find_view(sm, b_sid);
    if (b_view->layout != VIEW_BSP) return WINDOW_OP_ERROR_INVALID_DST_VIEW;

    struct window_node *a_node = view_find_window_node(a_view, a->id);
    if (!a_node) return WINDOW_OP_ERROR_INVALID_SRC_NODE;

    struct window_node *b_node = view_find_window_node(b_view, b->id);
    if (!b_node) return WINDOW_OP_ERROR_INVALID_DST_NODE;

    if (a_view->insertion_point == a_node->window_id) {
        a_view->insertion_point = b->id;
    } else if (b_view->insertion_point == b_node->window_id) {
        b_view->insertion_point = a->id;
    }

    a_node->window_id = b->id;
    a_node->zoom = NULL;
    b_node->window_id = a->id;
    b_node->zoom = NULL;

    if (a_view->sid != b_view->sid) {
        window_manager_remove_managed_window(wm, a->id);
        window_manager_add_managed_window(wm, a, b_view);
        window_manager_remove_managed_window(wm, b->id);
        window_manager_add_managed_window(wm, b, a_view);
        space_manager_move_window_to_space(b_view->sid, a);
        space_manager_move_window_to_space(a_view->sid, b);

        if (wm->focused_window_id == a->id) {
            window_manager_focus_window_with_raise(&b->application->psn, b->id, b->ref);
        } else if (wm->focused_window_id == b->id) {
            window_manager_focus_window_with_raise(&a->application->psn, a->id, a->ref);
        }
    }

    window_node_flush(a_node);
    window_node_flush(b_node);

    return WINDOW_OP_ERROR_SUCCESS;
}

enum window_op_error window_manager_minimize_window(struct window *window)
{
    if (!window_can_minimize(window)) return WINDOW_OP_ERROR_CANT_MINIMIZE;
    if (window->is_minimized)         return WINDOW_OP_ERROR_ALREADY_MINIMIZED;

    AXError result = AXUIElementSetAttributeValue(window->ref, kAXMinimizedAttribute, kCFBooleanTrue);
    return result == kAXErrorSuccess ? WINDOW_OP_ERROR_SUCCESS : WINDOW_OP_ERROR_MINIMIZE_FAILED;
}

enum window_op_error window_manager_deminimize_window(struct window *window)
{
    if (!window->is_minimized) return WINDOW_OP_ERROR_NOT_MINIMIZED;

    AXError result = AXUIElementSetAttributeValue(window->ref, kAXMinimizedAttribute, kCFBooleanFalse);
    return result == kAXErrorSuccess ? WINDOW_OP_ERROR_SUCCESS : WINDOW_OP_ERROR_DEMINIMIZE_FAILED;
}

bool window_manager_close_window(struct window *window)
{
    CFTypeRef button = NULL;
    AXUIElementCopyAttributeValue(window->ref, kAXCloseButtonAttribute, &button);
    if (!button) return false;

    AXUIElementPerformAction(button, kAXPressAction);
    CFRelease(button);

    return true;
}

void window_manager_send_window_to_space(struct space_manager *sm, struct window_manager *wm, struct window *window, uint64_t dst_sid, bool moved_by_rule)
{
    uint64_t src_sid = window_space(window);
    if (src_sid == dst_sid) return;

    struct view *view = window_manager_find_managed_window(wm, window);
    if (view) {
        space_manager_untile_window(sm, view, window);
        window_manager_remove_managed_window(wm, window->id);
        window_manager_purify_window(wm, window);
    }

    if ((space_is_visible(src_sid) && (moved_by_rule || wm->focused_window_id == window->id))) {
        struct window *next = window_manager_find_window_on_space_by_rank(wm, src_sid, 2);
        if (next) {
            window_manager_focus_window_with_raise(&next->application->psn, next->id, next->ref);
        } else {
            _SLPSSetFrontProcessWithOptions(&g_process_manager.finder_psn, 0, kCPSNoWindows);
        }
    }

    space_manager_move_window_to_space(dst_sid, window);

    if (window_manager_should_manage_window(window) && !window->is_minimized) {
        struct view *view = space_manager_tile_window_on_space(sm, window, dst_sid);
        window_manager_add_managed_window(wm, window, view);
    }
}

enum window_op_error window_manager_apply_grid(struct space_manager *sm, struct window_manager *wm, struct window *window, unsigned r, unsigned c, unsigned x, unsigned y, unsigned w, unsigned h)
{
    struct view *view = window_manager_find_managed_window(wm, window);
    if (view) return WINDOW_OP_ERROR_INVALID_SRC_VIEW;

    uint32_t did = window_display_id(window);
    if (!did) return WINDOW_OP_ERROR_INVALID_SRC_VIEW;

    if (x >= c)    x = c - 1;
    if (y >= r)    y = r - 1;
    if (w <= 0)    w = 1;
    if (h <= 0)    h = 1;
    if (w > c - x) w = c - x;
    if (h > r - y) h = r - y;

    uint64_t sid = display_space_id(did);
    struct view *dview = space_manager_find_view(sm, sid);

    CGRect bounds = display_bounds_constrained(did);
    if (dview && dview->enable_padding) {
        bounds.origin.x    += dview->left_padding;
        bounds.size.width  -= (dview->left_padding + dview->right_padding);
        bounds.origin.y    += dview->top_padding;
        bounds.size.height -= (dview->top_padding + dview->bottom_padding);
    }

    float cw = bounds.size.width / c;
    float ch = bounds.size.height / r;
    float fx = bounds.origin.x + bounds.size.width  - cw * (c - x);
    float fy = bounds.origin.y + bounds.size.height - ch * (r - y);
    float fw = cw * w;
    float fh = ch * h;

    window_manager_move_window(window, fx, fy);
    window_manager_resize_window(window, fw, fh);

    return WINDOW_OP_ERROR_SUCCESS;
}

void window_manager_toggle_window_topmost(struct window *window)
{
    bool is_topmost = window_is_topmost(window);
    window_manager_set_window_layer(window, is_topmost ? LAYER_NORMAL : LAYER_ABOVE);
}

void window_manager_toggle_window_float(struct space_manager *sm, struct window_manager *wm, struct window *window)
{
    if (window->is_floating) {
        window->is_floating = false;
        window_manager_make_floating(wm, window, false);
        if (window_manager_should_manage_window(window)) {
            struct view *view = space_manager_tile_window_on_space(sm, window, space_manager_active_space());
            window_manager_add_managed_window(wm, window, view);
        }
    } else {
        struct view *view = window_manager_find_managed_window(wm, window);
        if (view) {
            space_manager_untile_window(sm, view, window);
            window_manager_remove_managed_window(wm, window->id);
            window_manager_purify_window(wm, window);
        }
        window_manager_make_floating(wm, window, true);
        window->is_floating = true;
    }
}

void window_manager_toggle_window_sticky(struct space_manager *sm, struct window_manager *wm, struct window *window)
{
    if (window_is_sticky(window)) {
        window_manager_make_sticky(window->id, false);
        if (window_manager_should_manage_window(window)) {
            struct view *view = space_manager_tile_window_on_space(sm, window, space_manager_active_space());
            window_manager_add_managed_window(wm, window, view);
        }
    } else {
        struct view *view = window_manager_find_managed_window(wm, window);
        if (view) {
            space_manager_untile_window(sm, view, window);
            window_manager_remove_managed_window(wm, window->id);
            window_manager_purify_window(wm, window);
        }
        window_manager_make_sticky(window->id, true);
    }
}

void window_manager_toggle_window_shadow(struct space_manager *sm, struct window_manager *wm, struct window *window)
{
    int sockfd;
    char message[MAXLEN];
    bool shadow = !window->has_shadow;

    if (socket_connect_un(&sockfd, g_sa_socket_file)) {
        snprintf(message, sizeof(message), "window_shadow %d %d", window->id, shadow);
        socket_write(sockfd, message);
        socket_wait(sockfd);
        window->has_shadow = shadow;
    }
    socket_close(sockfd);
}

void window_manager_toggle_window_native_fullscreen(struct space_manager *sm, struct window_manager *wm, struct window *window)
{
    uint32_t sid = window_space(window);

    // NOTE(koekeishiya): The window must become the focused window
    // before we can change its fullscreen attribute. We focus the
    // window and spin lock until a potential space animation has finished.
    window_manager_focus_window_with_raise(&window->application->psn, window->id, window->ref);
    while (sid != space_manager_active_space()) { usleep(100000); }

    if (!window_is_fullscreen(window)) {
        AXUIElementSetAttributeValue(window->ref, kAXFullscreenAttribute, kCFBooleanTrue);
    } else {
        AXUIElementSetAttributeValue(window->ref, kAXFullscreenAttribute, kCFBooleanFalse);
    }

    // NOTE(koekeishiya): We toggled the fullscreen attribute and must
    // now spin lock until the post-exit space animation has finished.
    while (sid == space_manager_active_space()) { usleep(100000); }
}

void window_manager_toggle_window_parent(struct space_manager *sm, struct window_manager *wm, struct window *window)
{
    struct view *view = window_manager_find_managed_window(wm, window);
    if (!view || view->layout != VIEW_BSP) return;

    struct window_node *node = view_find_window_node(view, window->id);
    if (node->zoom) {
        window_manager_move_window(window, node->area.x, node->area.y);
        window_manager_resize_window(window, node->area.w, node->area.h);
        node->zoom = NULL;
    } else if (node->parent) {
        window_manager_move_window(window, node->parent->area.x, node->parent->area.y);
        window_manager_resize_window(window, node->parent->area.w, node->parent->area.h);
        node->zoom = node->parent;
    }
}

void window_manager_toggle_window_fullscreen(struct space_manager *sm, struct window_manager *wm, struct window *window)
{
    struct view *view = window_manager_find_managed_window(wm, window);
    if (!view || view->layout != VIEW_BSP) return;

    struct window_node *node = view_find_window_node(view, window->id);
    if (node->zoom) {
        window_manager_move_window(window, node->area.x, node->area.y);
        window_manager_resize_window(window, node->area.w, node->area.h);
        node->zoom = NULL;
    } else {
        window_manager_move_window(window, view->root->area.x, view->root->area.y);
        window_manager_resize_window(window, view->root->area.w, view->root->area.h);
        node->zoom = view->root;
    }
}

void window_manager_toggle_window_expose(struct window_manager *wm, struct window *window)
{
    window_manager_focus_window_with_raise(&window->application->psn, window->id, window->ref);
    CoreDockSendNotification(CFSTR("com.apple.expose.front.awake"), 0);
}

void window_manager_toggle_window_pip(struct space_manager *sm, struct window_manager *wm, struct window *window)
{
    uint32_t did = window_display_id(window);
    if (!did) return;

    uint64_t sid = display_space_id(did);
    struct view *dview = space_manager_find_view(sm, sid);

    CGRect bounds = display_bounds_constrained(did);
    if (dview && dview->enable_padding) {
        bounds.origin.x    += dview->left_padding;
        bounds.size.width  -= (dview->left_padding + dview->right_padding);
        bounds.origin.y    += dview->top_padding;
        bounds.size.height -= (dview->top_padding + dview->bottom_padding);
    }

    int sockfd;
    char message[MAXLEN];

    if (socket_connect_un(&sockfd, g_sa_socket_file)) {
        snprintf(message, sizeof(message), "window_scale %d %f %f %f %f", window->id, bounds.origin.x, bounds.origin.y, bounds.size.width, bounds.size.height);
        socket_write(sockfd, message);
        socket_wait(sockfd);
    }
    socket_close(sockfd);
}

void window_manager_toggle_window_border(struct window_manager *wm, struct window *window)
{
    if (window->border.id) {
        border_destroy(window);
    } else {
        border_create(window);
        if (window->id == wm->focused_window_id) border_activate(window);
    }
}

static void window_manager_validate_windows_on_space(struct space_manager *sm, struct window_manager *wm, uint64_t sid, uint32_t *window_list, int window_count)
{
    struct view *view = space_manager_find_view(sm, sid);
    uint32_t *view_window_list = view_find_window_list(view);

    for (int i = 0; i < buf_len(view_window_list); ++i) {
        bool found = false;

        for (int j = 0; j < window_count; ++j) {
            if (view_window_list[i] == window_list[j]) {
                found = true;
                break;
            }
        }

        if (!found) {
            struct window *window = window_manager_find_window(wm, view_window_list[i]);
            if (!window) continue;

            space_manager_untile_window(sm, view, window);
            window_manager_remove_managed_window(wm, window->id);
            window_manager_purify_window(wm, window);
        }
    }

    buf_free(view_window_list);
}

static void window_manager_check_for_windows_on_space(struct space_manager *sm, struct window_manager *wm, uint64_t sid, uint32_t *window_list, int window_count)
{
    for (int i = 0; i < window_count; ++i) {
        struct window *window = window_manager_find_window(wm, window_list[i]);
        if (!window || !window_manager_should_manage_window(window)) continue;
        if (window->is_minimized || window->application->is_hidden)  continue;

        struct view *existing_view = window_manager_find_managed_window(wm, window);
        if (existing_view && existing_view->layout == VIEW_BSP && existing_view->sid != sid) {
            space_manager_untile_window(sm, existing_view, window);
            window_manager_remove_managed_window(wm, window->id);
            window_manager_purify_window(wm, window);
        }

        if (!existing_view || (existing_view->layout == VIEW_BSP && existing_view->sid != sid)) {
            struct view *view = space_manager_tile_window_on_space(sm, window, sid);
            window_manager_add_managed_window(wm, window, view);
        }
    }
}

void window_manager_validate_and_check_for_windows_on_space(struct space_manager *sm, struct window_manager *wm, uint64_t sid)
{
    int window_count;
    uint32_t *window_list = space_window_list(sid, &window_count, false);
    if (!window_list) return;

    window_manager_validate_windows_on_space(sm, wm, sid, window_list, window_count);
    window_manager_check_for_windows_on_space(sm, wm, sid, window_list, window_count);
    free(window_list);
}

void window_manager_handle_display_add_and_remove(struct space_manager *sm, struct window_manager *wm, uint32_t did)
{
    int space_count;
    uint64_t *space_list = display_space_list(did, &space_count);
    if (!space_list) return;

    for (int i = 0; i < space_count; ++i) {
        if (space_is_user(space_list[i])) {
            int window_count;
            uint32_t *window_list = space_window_list(space_list[i], &window_count, false);
            if (window_list) {
                window_manager_check_for_windows_on_space(sm, wm, space_list[i], window_list, window_count);
                free(window_list);
            }
            break;
        }
    }

    uint64_t sid = display_space_id(did);
    for (int i = 0; i < space_count; ++i) {
        if (space_list[i] == sid) {
            space_manager_refresh_view(sm, sid);
        } else {
            space_manager_mark_view_invalid(sm, space_list[i]);
        }
    }

    free(space_list);
}

void window_manager_init(struct window_manager *wm)
{
    wm->system_element = AXUIElementCreateSystemWide();
    AXUIElementSetMessagingTimeout(wm->system_element, 1.0);

    wm->ffm_mode = FFM_DISABLED;
    wm->purify_mode = PURIFY_DISABLED;
    wm->enable_mff = false;
    wm->enable_window_border = false;
    wm->enable_window_opacity = false;
    wm->enable_window_topmost = false;
    wm->active_window_opacity = 1.0f;
    wm->normal_window_opacity = 1.0f;
    wm->window_opacity_duration = 0.2f;
    wm->insert_feedback_windows = NULL;
    wm->insert_feedback_color = rgba_color_from_hex(0xffd75f5f);
    wm->active_border_color = rgba_color_from_hex(0xff775759);
    wm->normal_border_color = rgba_color_from_hex(0xff555555);
    wm->border_width = 6;

    table_init(&wm->application, 150, hash_wm, compare_wm);
    table_init(&wm->window, 150, hash_wm, compare_wm);
    table_init(&wm->managed_window, 150, hash_wm, compare_wm);
    table_init(&wm->window_lost_focused_event, 150, hash_wm, compare_wm);
    table_init(&wm->application_lost_front_switched_event, 150, hash_wm, compare_wm);
}

void window_manager_begin(struct space_manager *sm, struct window_manager *wm)
{
    for (int process_index = 0; process_index < g_process_manager.process.capacity; ++process_index) {
        struct bucket *bucket = g_process_manager.process.buckets[process_index];
        while (bucket) {
            if (bucket->value) {
                struct process *process = bucket->value;
                struct application *application = application_create(process);

                if (application_observe(application)) {
                    window_manager_add_application(wm, application);
                    window_manager_add_application_windows(sm, wm, application);
                } else {
                    application_unobserve(application);
                    application_destroy(application);
                }
            }

            bucket = bucket->next;
        }
    }

    struct window *window = window_manager_focused_window(wm);
    if (window) {
        wm->last_window_id = window->id;
        wm->focused_window_id = window->id;
        wm->focused_window_psn = window->application->psn;
        window_manager_set_window_opacity(wm, window, wm->active_window_opacity);
        border_activate(window);
    }
}
