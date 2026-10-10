/*
 * MIT License
 *
 * Copyright (c) 2025 Eirik S. Morland
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fesk_demo_face.h"
#include "fesk_session.h"
#include "filesystem.h"
#include "movement.h"
#include "watch.h"
#include "watch_tcc.h"
#include "lfs.h"

extern lfs_t eeprom_filesystem;

#define FESK_DEMO_MAX_FILE_SIZE 1024

typedef enum {
    FESK_DEMO_MODE_TEST = 0,    // Transmit the built-in test message
    FESK_DEMO_MODE_LS,          // Transmit the names of all files
    FESK_DEMO_MODE_FILE,        // Transmit the contents of the selected file
    FESK_DEMO_MODE_COUNT
} fesk_demo_mode_t;

typedef struct {
    fesk_session_t session;
    fesk_session_config_t config;
    bool is_countdown;
    bool is_transmitting;
    bool is_debug_playing;
    uint8_t led_color;  // 0 = no LED, 1 = red, 2 = green
    fesk_demo_mode_t mode;
    uint8_t file_index; // Which file FILE mode transmits, in directory order
    char *file_buffer;  // Payload built for LS or FILE mode while transmitting
} fesk_demo_state_t;

static const char test_message[] = "test 😎";

static fesk_demo_state_t *sequence_callback_state = NULL;

// Finds the index-th regular file in the root directory. Returns false if
// there are not that many files.
static bool _fesk_demo_get_file(uint8_t index, struct lfs_info *info) {
    lfs_dir_t dir;
    if (lfs_dir_open(&eeprom_filesystem, &dir, "/") < 0) return false;

    bool found = false;
    uint8_t count = 0;
    while (lfs_dir_read(&eeprom_filesystem, &dir, info) > 0) {
        if (info->type != LFS_TYPE_REG) continue;
        if (count++ == index) {
            found = true;
            break;
        }
    }
    lfs_dir_close(&eeprom_filesystem, &dir);
    return found;
}

static void _fesk_demo_display_ready(fesk_demo_state_t *state) {
    struct lfs_info info;
    char name[7];

    switch (state->mode) {
        case FESK_DEMO_MODE_LS:
            watch_display_text_with_fallback(WATCH_POSITION_TOP, "FESK", "FK");
            watch_display_text(WATCH_POSITION_BOTTOM, "  LS  ");
            break;
        case FESK_DEMO_MODE_FILE:
            // Show the first six characters of the selected file's name.
            watch_display_text_with_fallback(WATCH_POSITION_TOP, "FILE", "FI");
            if (_fesk_demo_get_file(state->file_index, &info)) {
                snprintf(name, sizeof(name), "%-6.6s", info.name);
                watch_display_text(WATCH_POSITION_BOTTOM, name);
            } else {
                watch_display_text(WATCH_POSITION_BOTTOM, "NOFILE");
            }
            break;
        default:
            watch_display_text_with_fallback(WATCH_POSITION_TOP, "FESK", "FK");
            watch_display_text(WATCH_POSITION_BOTTOM, " TEST ");
            break;
    }
}

// In FILE mode, selects the next file, wrapping around after the last one.
static void _fesk_demo_next_file(fesk_demo_state_t *state) {
    struct lfs_info info;
    state->file_index++;
    if (!_fesk_demo_get_file(state->file_index, &info)) state->file_index = 0;
}

// LIGHT long press steps TEST -> LS -> FILE -> TEST.
static void _fesk_demo_next_mode(fesk_demo_state_t *state) {
    struct lfs_info info;

    state->mode = (state->mode + 1) % FESK_DEMO_MODE_COUNT;
    if (state->mode == FESK_DEMO_MODE_FILE) {
        state->file_index = 0;
        // Skip FILE mode entirely when there are no files.
        if (!_fesk_demo_get_file(0, &info)) state->mode = FESK_DEMO_MODE_TEST;
    }
}

static void _fesk_demo_free_file_buffer(fesk_demo_state_t *state) {
    if (state->file_buffer) {
        free(state->file_buffer);
        state->file_buffer = NULL;
    }
}

// Like `ls`: the names of all files, separated by spaces. Names that would
// push the list past FESK_DEMO_MAX_FILE_SIZE are left out.
static fesk_result_t _fesk_demo_build_ls(fesk_demo_state_t *state,
                                         const char **out_text,
                                         size_t *out_length) {
    state->file_buffer = malloc(FESK_DEMO_MAX_FILE_SIZE + 1);
    if (!state->file_buffer) {
        return FESK_ERR_ALLOCATION_FAILED;
    }

    size_t length = 0;
    struct lfs_info info;
    for (uint8_t i = 0; _fesk_demo_get_file(i, &info); i++) {
        size_t name_length = strlen(info.name);
        size_t needed = name_length + (length > 0 ? 1 : 0);
        if (length + needed > FESK_DEMO_MAX_FILE_SIZE) break;
        if (length > 0) state->file_buffer[length++] = ' ';
        memcpy(state->file_buffer + length, info.name, name_length);
        length += name_length;
    }
    state->file_buffer[length] = '\0';

    if (length == 0) {
        _fesk_demo_free_file_buffer(state);
        return FESK_ERR_INVALID_ARGUMENT;
    }

    *out_text = state->file_buffer;
    *out_length = length;
    return FESK_OK;
}

// Like `cat <file>`: read the whole selected file so it can be transmitted.
static fesk_result_t _fesk_demo_read_file(fesk_demo_state_t *state,
                                          const char **out_text,
                                          size_t *out_length) {
    struct lfs_info info;
    if (!_fesk_demo_get_file(state->file_index, &info)) {
        return FESK_ERR_INVALID_ARGUMENT;
    }

    int32_t size = filesystem_get_file_size(info.name);
    if (size <= 0 || size > FESK_DEMO_MAX_FILE_SIZE) {
        return FESK_ERR_INVALID_ARGUMENT;
    }

    state->file_buffer = malloc(size + 1);
    if (!state->file_buffer) {
        return FESK_ERR_ALLOCATION_FAILED;
    }
    if (!filesystem_read_file(info.name, state->file_buffer, size)) {
        _fesk_demo_free_file_buffer(state);
        return FESK_ERR_INVALID_ARGUMENT;
    }
    state->file_buffer[size] = '\0';

    // Drop trailing newlines (as left by `echo`), so they don't force base32.
    while (size > 0 && (state->file_buffer[size - 1] == '\n' || state->file_buffer[size - 1] == '\r')) {
        state->file_buffer[--size] = '\0';
    }
    if (size == 0) {
        _fesk_demo_free_file_buffer(state);
        return FESK_ERR_INVALID_ARGUMENT;
    }

    *out_text = state->file_buffer;
    *out_length = (size_t)size;
    return FESK_OK;
}

static fesk_result_t _fesk_demo_provide_payload(const char **out_text,
                                                size_t *out_length,
                                                void *user_data) {
    fesk_demo_state_t *state = (fesk_demo_state_t *)user_data;

    _fesk_demo_free_file_buffer(state);

    switch (state->mode) {
        case FESK_DEMO_MODE_LS:
            return _fesk_demo_build_ls(state, out_text, out_length);
        case FESK_DEMO_MODE_FILE:
            return _fesk_demo_read_file(state, out_text, out_length);
        default:
            *out_text = test_message;
            *out_length = 0;
            return FESK_OK;
    }
}

static void _fesk_demo_on_ready(void *user_data) {
    fesk_demo_state_t *state = (fesk_demo_state_t *)user_data;
    if (!state) return;
    state->is_countdown = false;
    state->is_transmitting = false;
    _fesk_demo_display_ready(state);
}

static void _fesk_demo_on_countdown_begin(void *user_data) {
    fesk_demo_state_t *state = (fesk_demo_state_t *)user_data;
    if (!state) return;
    state->is_debug_playing = false;
    state->is_countdown = true;
}

static void _fesk_demo_on_transmission_start(void *user_data) {
    fesk_demo_state_t *state = (fesk_demo_state_t *)user_data;
    if (!state) return;

    struct lfs_info info;
    if (state->mode == FESK_DEMO_MODE_FILE && _fesk_demo_get_file(state->file_index, &info)) {
        printf("Transmitting: FILE %s\n", info.name);
    } else {
        printf("Transmitting: %s\n", state->mode == FESK_DEMO_MODE_LS ? "LS" : "TEST");
    }

    state->is_countdown = false;
    state->is_transmitting = true;
    watch_display_text(WATCH_POSITION_BOTTOM, "  TX  ");
}

static void _fesk_demo_on_transmission_end(void *user_data) {
    fesk_demo_state_t *state = (fesk_demo_state_t *)user_data;
    if (!state) return;
    state->is_transmitting = false;
    _fesk_demo_free_file_buffer(state);
    _fesk_demo_display_ready(state);
}

static void _fesk_demo_on_cancelled(void *user_data) {
    fesk_demo_state_t *state = (fesk_demo_state_t *)user_data;
    if (!state) return;
    state->is_countdown = false;
    state->is_transmitting = false;
    _fesk_demo_free_file_buffer(state);
    _fesk_demo_display_ready(state);
}

static void _fesk_demo_on_error(fesk_result_t error, void *user_data) {
    fesk_demo_state_t *state = (fesk_demo_state_t *)user_data;
    printf("FESK error: %d\n", (int)error);
    if (!state) return;
    // The session ends without calling on_transmission_end, so reset here.
    state->is_countdown = false;
    state->is_transmitting = false;
    _fesk_demo_free_file_buffer(state);
    watch_display_text(WATCH_POSITION_BOTTOM, " ERROR");
}

// Debug sequence tick values adjusted for buzzer code version
#ifdef WATCH_BUZZER_PERIOD_REST
// New buzzer code: add 1 to compensate for the -1 in playback
#define DEBUG_TONE_DURATION 41
#else
// Old buzzer code: use original durations
#define DEBUG_TONE_DURATION 40
#endif

// Play all 4 tones for debug (4-FSK)
static int8_t debug_sequence[] = {
    FESK_4FSK_TONE_00_NOTE, DEBUG_TONE_DURATION,
    FESK_4FSK_TONE_01_NOTE, DEBUG_TONE_DURATION,
    FESK_4FSK_TONE_10_NOTE, DEBUG_TONE_DURATION,
    FESK_4FSK_TONE_11_NOTE, DEBUG_TONE_DURATION,
    0
};

static void _fesk_demo_debug_done(void) {
    if (sequence_callback_state) {
        sequence_callback_state->is_debug_playing = false;
        sequence_callback_state = NULL;
    }
}

void fesk_demo_face_setup(uint8_t watch_face_index, void **context_ptr) {
    (void)watch_face_index;
    if (*context_ptr != NULL) {
        return;
    }

    fesk_demo_state_t *state = malloc(sizeof(fesk_demo_state_t));
    if (!state) {
        return;
    }
    memset(state, 0, sizeof(*state));

    state->led_color = 0;  // Start with no LED

    fesk_session_config_t config = fesk_session_config_defaults();
    state->config = config;
    state->config.static_message = test_message;
    state->config.provide_payload = _fesk_demo_provide_payload;
    state->config.on_countdown_begin = _fesk_demo_on_countdown_begin;
    state->config.on_transmission_start = _fesk_demo_on_transmission_start;
    state->config.on_transmission_end = _fesk_demo_on_transmission_end;
    state->config.on_cancelled = _fesk_demo_on_cancelled;
    state->config.on_error = _fesk_demo_on_error;
    state->config.user_data = state;

    fesk_session_init(&state->session, &state->config);
    *context_ptr = state;
}

void fesk_demo_face_activate(void *context) {
    fesk_demo_state_t *state = (fesk_demo_state_t *)context;
    if (!state) return;
    state->is_debug_playing = false;
    _fesk_demo_display_ready(state);

    // Set LED based on current state
    if (state->led_color == 0) {
        watch_set_led_off();
    } else if (state->led_color == 1) {
        watch_set_led_red();
    } else {
        watch_set_led_green();
    }

    // Session is ready, no preparation needed
}

bool fesk_demo_face_loop(movement_event_t event, void *context) {
    fesk_demo_state_t *state = (fesk_demo_state_t *)context;
    if (!state) return true;

    bool handled = false;

    switch (event.event_type) {

        case EVENT_LIGHT_BUTTON_DOWN:
        case EVENT_LIGHT_LONG_UP:
            // Do nothing.
            handled = true;
            break;

        case EVENT_LIGHT_LONG_PRESS:
            // Cycle between modes (what gets transmitted)
            if (!state->is_debug_playing && fesk_session_is_idle(&state->session)) {
                _fesk_demo_next_mode(state);
                _fesk_demo_display_ready(state);
            }
            handled = true;
            break;

        case EVENT_LIGHT_BUTTON_UP:
            if (!state->is_debug_playing && fesk_session_is_idle(&state->session)) {
                if (state->mode == FESK_DEMO_MODE_FILE) {
                    _fesk_demo_next_file(state);
                    _fesk_demo_display_ready(state);
                } else {
                    // Cycle between no LED, red, and green
                    state->led_color = (state->led_color + 1) % 3;
                    state->session.config.static_message = test_message;
                }
            }
            handled = true;
            break;

        case EVENT_MODE_BUTTON_UP:
            if (!state->is_debug_playing && fesk_session_is_idle(&state->session)) {
                movement_move_to_next_face();
                handled = true;
            }
            break;

        case EVENT_ALARM_BUTTON_UP:
            if (state->is_debug_playing) {
                handled = true;
                break;
            }
            if (fesk_session_is_idle(&state->session)) {
                struct lfs_info info;
                if (state->mode != FESK_DEMO_MODE_TEST && !_fesk_demo_get_file(
                        state->mode == FESK_DEMO_MODE_FILE ? state->file_index : 0, &info)) {
                    // The selected file is gone, or there are no files to list.
                    watch_display_text(WATCH_POSITION_BOTTOM, "NOFILE");
                } else if (!state->is_countdown && !state->is_transmitting) {
                    fesk_session_start(&state->session);
                }
            } else {
                fesk_session_cancel(&state->session);
            }
            handled = true;
            break;

        case EVENT_ALARM_LONG_PRESS:
            if (!state->is_debug_playing && !state->is_countdown && !state->is_transmitting) {
                state->is_debug_playing = true;
                sequence_callback_state = state;
                watch_buzzer_play_sequence(debug_sequence, _fesk_demo_debug_done);
            }
            handled = true;
            break;

        case EVENT_TIMEOUT:
            if (fesk_session_is_idle(&state->session) && !state->is_debug_playing) {
                movement_move_to_face(0);
            }
            handled = true;
            break;

        default:
            break;
    }

    if (!handled) {
        movement_default_loop_handler(event);
    }

    if (state->is_debug_playing) {
        return false;
    }

    // We can not sleep if we are transmitting or counting down.
    return fesk_session_is_idle(&state->session);
}

void fesk_demo_face_resign(void *context) {
    fesk_demo_state_t *state = (fesk_demo_state_t *)context;
    if (!state) return;

    if (state->is_debug_playing) {
        watch_buzzer_abort_sequence();
        state->is_debug_playing = false;
        sequence_callback_state = NULL;
    }

    fesk_session_cancel(&state->session);
    _fesk_demo_free_file_buffer(state);

    // Turn off LED when leaving face
    watch_set_led_off();
}
