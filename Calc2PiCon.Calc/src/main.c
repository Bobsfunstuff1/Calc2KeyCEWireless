#include <graphx.h>
#include <keypadc.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/basicusb.h>
#include <usbdrvce.h>

#define SCREEN_COLS 40
#define SCREEN_ROWS 24
#define SCREEN_BYTES (SCREEN_COLS * SCREEN_ROWS)
#define KEY_SCAN_BYTES 7
#define USB_PACKET_HEADER_BYTES 8
#define USB_MAX_PAYLOAD_BYTES 1024

#define PACKET_TYPE_SCREEN 1
#define PACKET_TYPE_INPUT 2

#define CALC_KEY(row, bit) (((row) << 4) | (bit))

typedef struct packet_header {
    uint8_t type;
    uint8_t reserved0;
    uint8_t reserved1;
    uint8_t reserved2;
    uint32_t size;
} packet_header_t;

typedef struct key_stream {
    uint8_t bytes[8];
    uint8_t count;
} key_stream_t;

static usb_error_t event_handler(usb_event_t event, void* event_data, usb_callback_data_t* callback_data);
static usb_error_t input_transfer_callback(usb_endpoint_t endpoint, usb_transfer_status_t status, size_t transferred, usb_transfer_data_t* data);
static usb_error_t output_transfer_callback(usb_endpoint_t endpoint, usb_transfer_status_t status, size_t transferred, usb_transfer_data_t* data);
static void update_keyboard_state(void);
static void process_key_edges(void);
static bool build_key_stream(uint8_t calc_key, key_stream_t* stream);
static void queue_byte(uint8_t value);
static void queue_bytes(const uint8_t* values, uint8_t count);
static void flush_output_queue(void);
static void draw_screen(void);
static void start_input_header_read(void);
static void start_input_payload_read(uint32_t payload_size);
static bool is_pressed(uint8_t row, uint8_t mask);

static usb_device_descriptor_t g_device_descriptor = {
    0x12, USB_DEVICE_DESCRIPTOR, 0x0200, 0x00, 0x00, 0x00, 0x40, 0x0451, 0xE00A, 0x0101, 0x00, 0x00, 0x00, 0x01
};

static uint8_t g_configuration_descriptor[] = {
    0x09, 0x02, 0x20, 0x00, 0x01, 0x01, 0x00, 0x80, 0x32,
    0x09, 0x04, 0x00, 0x00, 0x02, 0xFF, 0x01, 0x01, 0x00,
    0x07, 0x05, 0x81, 0x02, 0x40, 0x00, 0x00,
    0x07, 0x05, 0x02, 0x02, 0x40, 0x00, 0x00
};

static const usb_configuration_descriptor_t* g_configuration_descriptors[] = {
    (usb_configuration_descriptor_t*)g_configuration_descriptor
};

static usb_standard_descriptors_t g_standard_descriptors = {
    .device = &g_device_descriptor,
    .configurations = g_configuration_descriptors,
    .langids = NULL,
    .numStrings = 0,
    .strings = NULL,
};

static usb_endpoint_t g_input_endpoint = NULL;
static usb_endpoint_t g_output_endpoint = NULL;
static bool g_connected = false;
static bool g_input_transfer_pending = false;
static bool g_output_transfer_pending = false;
static bool g_reading_payload = false;
static bool g_screen_dirty = true;
static bool g_alpha_lock = false;
static bool g_alpha_was_down = false;
static bool g_alpha_chord_used = false;

static uint8_t g_scan[KEY_SCAN_BYTES];
static uint8_t g_prev_scan[KEY_SCAN_BYTES];
static uint8_t g_input_header_buffer[USB_PACKET_HEADER_BYTES];
static uint8_t g_input_payload_buffer[USB_MAX_PAYLOAD_BYTES];
static packet_header_t g_current_header;
static uint8_t g_output_packet[USB_PACKET_HEADER_BYTES + 64];
static uint8_t g_output_payload_size = 0;
static char g_screen[SCREEN_BYTES];
static uint8_t g_cursor_col = 0;
static uint8_t g_cursor_row = 0;

int main(void) {
    uint16_t index;

    for (index = 0; index < SCREEN_BYTES; ++index) {
        g_screen[index] = ' ';
    }

    gfx_Begin();
    gfx_SetDrawBuffer();
    gfx_SetMonospaceFont(8);
    gfx_SetTextScale(1, 1);
    gfx_SetTextFGColor(255);
    gfx_SetTextBGColor(0);

    if (usb_Init(event_handler, NULL, &g_standard_descriptors, USB_DEFAULT_INIT_FLAGS) != USB_SUCCESS) {
        gfx_End();
        return 1;
    }

    kb_EnableOnLatch();
    kb_ClearOnLatch();

    while (!kb_On) {
        if (usb_HandleEvents() != USB_SUCCESS) {
            break;
        }

        update_keyboard_state();
        process_key_edges();
        flush_output_queue();

        if (g_screen_dirty) {
            draw_screen();
        }
    }

    usb_Cleanup();
    gfx_End();
    return 0;
}

static void update_keyboard_state(void) {
    uint8_t i;
    kb_Scan();
    for (i = 0; i < KEY_SCAN_BYTES; ++i) {
        g_scan[i] = kb_Data[i + 1];
    }
}

static bool is_pressed(uint8_t row, uint8_t mask) {
    return row < KEY_SCAN_BYTES && (g_scan[row] & mask) != 0;
}

static bool build_key_stream(uint8_t calc_key, key_stream_t* stream) {
    const bool second = is_pressed(0, kb_2nd);
    const bool alpha_down = is_pressed(1, kb_Alpha);
    const bool alpha_active = g_alpha_lock || alpha_down;

    stream->count = 0;

    switch (calc_key) {
        case CALC_KEY(0, 7): stream->bytes[0] = 0x7F; stream->count = 1; return true;
        case CALC_KEY(5, 0): stream->bytes[0] = '\r'; stream->count = 1; return true;
        case CALC_KEY(5, 6): stream->bytes[0] = 0x1B; stream->count = 1; return true;
        case CALC_KEY(1, 1): stream->bytes[0] = ' '; stream->count = 1; return true;
        case CALC_KEY(0, 6): stream->bytes[0] = '\t'; stream->count = 1; return true;
        case CALC_KEY(6, 3): stream->bytes[0] = 0x1B; stream->bytes[1] = '['; stream->bytes[2] = 'A'; stream->count = 3; return true;
        case CALC_KEY(6, 0): stream->bytes[0] = 0x1B; stream->bytes[1] = '['; stream->bytes[2] = 'B'; stream->count = 3; return true;
        case CALC_KEY(6, 2): stream->bytes[0] = 0x1B; stream->bytes[1] = '['; stream->bytes[2] = 'C'; stream->count = 3; return true;
        case CALC_KEY(6, 1): stream->bytes[0] = 0x1B; stream->bytes[1] = '['; stream->bytes[2] = 'D'; stream->count = 3; return true;
        default: break;
    }

    if (alpha_active) {
        switch (calc_key) {
            case CALC_KEY(1, 6): stream->bytes[0] = second ? 'A' : 'a'; break;
            case CALC_KEY(2, 6): stream->bytes[0] = second ? 'B' : 'b'; break;
            case CALC_KEY(3, 6): stream->bytes[0] = second ? 'C' : 'c'; break;
            case CALC_KEY(1, 5): stream->bytes[0] = second ? 'D' : 'd'; break;
            case CALC_KEY(2, 5): stream->bytes[0] = second ? 'E' : 'e'; break;
            case CALC_KEY(3, 5): stream->bytes[0] = second ? 'F' : 'f'; break;
            case CALC_KEY(4, 5): stream->bytes[0] = second ? 'G' : 'g'; break;
            case CALC_KEY(5, 5): stream->bytes[0] = second ? 'H' : 'h'; break;
            case CALC_KEY(1, 4): stream->bytes[0] = second ? 'I' : 'i'; break;
            case CALC_KEY(2, 4): stream->bytes[0] = second ? 'J' : 'j'; break;
            case CALC_KEY(3, 4): stream->bytes[0] = second ? 'K' : 'k'; break;
            case CALC_KEY(4, 4): stream->bytes[0] = second ? 'L' : 'l'; break;
            case CALC_KEY(5, 4): stream->bytes[0] = second ? 'M' : 'm'; break;
            case CALC_KEY(1, 3): stream->bytes[0] = second ? 'N' : 'n'; break;
            case CALC_KEY(2, 3): stream->bytes[0] = second ? 'O' : 'o'; break;
            case CALC_KEY(3, 3): stream->bytes[0] = second ? 'P' : 'p'; break;
            case CALC_KEY(4, 3): stream->bytes[0] = second ? 'Q' : 'q'; break;
            case CALC_KEY(5, 3): stream->bytes[0] = second ? 'R' : 'r'; break;
            case CALC_KEY(1, 2): stream->bytes[0] = second ? 'S' : 's'; break;
            case CALC_KEY(2, 2): stream->bytes[0] = second ? 'T' : 't'; break;
            case CALC_KEY(3, 2): stream->bytes[0] = second ? 'U' : 'u'; break;
            case CALC_KEY(4, 2): stream->bytes[0] = second ? 'V' : 'v'; break;
            case CALC_KEY(5, 2): stream->bytes[0] = second ? 'W' : 'w'; break;
            case CALC_KEY(2, 1): stream->bytes[0] = second ? 'Y' : 'y'; break;
            case CALC_KEY(3, 1): stream->bytes[0] = second ? 'Z' : 'z'; break;
            default: return false;
        }
        stream->count = 1;
        return true;
    }

    switch (calc_key) {
        case CALC_KEY(2, 0): stream->bytes[0] = second ? ')' : '0'; break;
        case CALC_KEY(2, 1): stream->bytes[0] = second ? '!' : '1'; break;
        case CALC_KEY(3, 1): stream->bytes[0] = second ? '@' : '2'; break;
        case CALC_KEY(4, 1): stream->bytes[0] = second ? '#' : '3'; break;
        case CALC_KEY(2, 2): stream->bytes[0] = second ? '$' : '4'; break;
        case CALC_KEY(3, 2): stream->bytes[0] = second ? '%' : '5'; break;
        case CALC_KEY(4, 2): stream->bytes[0] = second ? '^' : '6'; break;
        case CALC_KEY(2, 3): stream->bytes[0] = second ? '&' : '7'; break;
        case CALC_KEY(3, 3): stream->bytes[0] = second ? '*' : '8'; break;
        case CALC_KEY(4, 3): stream->bytes[0] = second ? '(' : '9'; break;
        case CALC_KEY(2, 4): stream->bytes[0] = second ? '<' : ','; break;
        case CALC_KEY(3, 4): stream->bytes[0] = second ? '[' : '('; break;
        case CALC_KEY(4, 4): stream->bytes[0] = second ? ']' : ')'; break;
        case CALC_KEY(5, 4): stream->bytes[0] = second ? '?' : '/'; break;
        case CALC_KEY(5, 3): stream->bytes[0] = '*'; break;
        case CALC_KEY(5, 2): stream->bytes[0] = '-'; break;
        case CALC_KEY(5, 1): stream->bytes[0] = '+'; break;
        case CALC_KEY(3, 0): stream->bytes[0] = second ? '>' : '.'; break;
        case CALC_KEY(4, 0): stream->bytes[0] = second ? '_' : '-'; break;
        case CALC_KEY(1, 4): stream->bytes[0] = second ? ':' : '='; break;
        case CALC_KEY(1, 3): stream->bytes[0] = second ? '"' : '\''; break;
        case CALC_KEY(1, 2): stream->bytes[0] = second ? ';' : '\\'; break;
        case CALC_KEY(0, 0): stream->bytes[0] = 0x03; break;
        case CALC_KEY(0, 1): stream->bytes[0] = 0x1A; break;
        case CALC_KEY(0, 2): stream->bytes[0] = 0x05; break;
        case CALC_KEY(0, 3): stream->bytes[0] = 0x01; break;
        default: return false;
    }

    stream->count = 1;
    return true;
}

static void process_key_edges(void) {
    uint8_t row;
    uint8_t bit;
    const bool alpha_down = is_pressed(1, kb_Alpha);

    for (row = 0; row < KEY_SCAN_BYTES; ++row) {
        const uint8_t released = (uint8_t)(g_prev_scan[row] & (uint8_t)~g_scan[row]);
        const uint8_t pressed = (uint8_t)(g_scan[row] & (uint8_t)~g_prev_scan[row]);

        if (released) {
            for (bit = 0; bit < 8; ++bit) {
                if (released & (uint8_t)(1u << bit)) {
                    if (CALC_KEY(row, bit) == CALC_KEY(1, 7) && !g_alpha_chord_used) {
                        g_alpha_lock = !g_alpha_lock;
                        g_screen_dirty = true;
                    }
                }
            }
        }

        if (!pressed) {
            continue;
        }

        for (bit = 0; bit < 8; ++bit) {
            key_stream_t stream;
            const uint8_t mask = (uint8_t)(1u << bit);
            const uint8_t calc_key = CALC_KEY(row, bit);

            if (!(pressed & mask)) {
                continue;
            }
            if (calc_key == CALC_KEY(0, 5) || calc_key == CALC_KEY(1, 7)) {
                continue;
            }

            if (build_key_stream(calc_key, &stream)) {
                if (alpha_down || g_alpha_lock) {
                    g_alpha_chord_used = true;
                }
                queue_bytes(stream.bytes, stream.count);
            }
        }
    }

    if (alpha_down && !g_alpha_was_down) {
        g_alpha_chord_used = false;
    }
    g_alpha_was_down = alpha_down;
    memcpy(g_prev_scan, g_scan, sizeof(g_prev_scan));
}

static void queue_byte(uint8_t value) {
    if (g_output_payload_size < 64) {
        g_output_packet[USB_PACKET_HEADER_BYTES + g_output_payload_size] = value;
        ++g_output_payload_size;
    }
}

static void queue_bytes(const uint8_t* values, uint8_t count) {
    uint8_t i;
    for (i = 0; i < count; ++i) {
        if (g_output_payload_size == 64) {
            flush_output_queue();
        }
        queue_byte(values[i]);
    }
}

static void flush_output_queue(void) {
    packet_header_t* header;

    if (!g_connected || !g_output_endpoint || g_output_transfer_pending || !g_output_payload_size) {
        return;
    }

    header = (packet_header_t*)g_output_packet;
    header->type = PACKET_TYPE_INPUT;
    header->reserved0 = 0;
    header->reserved1 = 0;
    header->reserved2 = 0;
    header->size = g_output_payload_size;

    if (usb_ScheduleTransfer(g_output_endpoint, g_output_packet, USB_PACKET_HEADER_BYTES + g_output_payload_size, output_transfer_callback, NULL) == USB_SUCCESS) {
        g_output_transfer_pending = true;
    }
}

static void draw_screen(void) {
    uint8_t row;
    char line[SCREEN_COLS + 1];

    gfx_FillScreen(0);
    for (row = 0; row < SCREEN_ROWS; ++row) {
        memcpy(line, &g_screen[row * SCREEN_COLS], SCREEN_COLS);
        line[SCREEN_COLS] = '\0';
        gfx_PrintStringXY(line, 0, row * 10);
    }

    if (g_cursor_col < SCREEN_COLS && g_cursor_row < SCREEN_ROWS) {
        gfx_SetColor(255);
        gfx_FillRectangle_NoClip(g_cursor_col * 8, g_cursor_row * 10 + 8, 8, 1);
    }

    gfx_BlitBuffer();
    g_screen_dirty = false;
}

static void start_input_header_read(void) {
    if (!g_connected || !g_input_endpoint || g_input_transfer_pending) {
        return;
    }
    g_reading_payload = false;
    if (usb_ScheduleTransfer(g_input_endpoint, g_input_header_buffer, sizeof(g_input_header_buffer), input_transfer_callback, NULL) == USB_SUCCESS) {
        g_input_transfer_pending = true;
    }
}

static void start_input_payload_read(uint32_t payload_size) {
    if (!g_connected || !g_input_endpoint || g_input_transfer_pending || payload_size > USB_MAX_PAYLOAD_BYTES) {
        return;
    }
    g_reading_payload = true;
    if (usb_ScheduleTransfer(g_input_endpoint, g_input_payload_buffer, payload_size, input_transfer_callback, NULL) == USB_SUCCESS) {
        g_input_transfer_pending = true;
    }
}

static usb_error_t input_transfer_callback(usb_endpoint_t endpoint, usb_transfer_status_t status, size_t transferred, usb_transfer_data_t* data) {
    (void)endpoint;
    (void)data;

    g_input_transfer_pending = false;
    if (status != USB_TRANSFER_COMPLETED) {
        g_connected = false;
        g_input_endpoint = NULL;
        g_output_endpoint = NULL;
        return USB_SUCCESS;
    }

    if (!g_reading_payload) {
        if (transferred != sizeof(g_input_header_buffer)) {
            start_input_header_read();
            return USB_SUCCESS;
        }

        memcpy(&g_current_header, g_input_header_buffer, sizeof(g_current_header));
        if (g_current_header.size == 0 || g_current_header.size > USB_MAX_PAYLOAD_BYTES) {
            start_input_header_read();
            return USB_SUCCESS;
        }

        start_input_payload_read(g_current_header.size);
        return USB_SUCCESS;
    }

    if (transferred == g_current_header.size && g_current_header.type == PACKET_TYPE_SCREEN && g_current_header.size >= SCREEN_BYTES + 2) {
        memcpy(g_screen, g_input_payload_buffer, SCREEN_BYTES);
        g_cursor_col = g_input_payload_buffer[SCREEN_BYTES];
        g_cursor_row = g_input_payload_buffer[SCREEN_BYTES + 1];
        g_screen_dirty = true;
    }

    start_input_header_read();
    return USB_SUCCESS;
}

static usb_error_t output_transfer_callback(usb_endpoint_t endpoint, usb_transfer_status_t status, size_t transferred, usb_transfer_data_t* data) {
    (void)endpoint;
    (void)status;
    (void)transferred;
    (void)data;
    g_output_transfer_pending = false;
    g_output_payload_size = 0;
    return USB_SUCCESS;
}

static usb_error_t event_handler(usb_event_t event, void* event_data, usb_callback_data_t* callback_data) {
    (void)event_data;
    (void)callback_data;

    if (event == USB_HOST_CONFIGURE_EVENT) {
        usb_device_t host_device = usb_FindDevice(NULL, NULL, USB_SKIP_HUBS);
        if (!host_device) {
            return USB_ERROR_NO_DEVICE;
        }

        g_input_endpoint = usb_GetDeviceEndpoint(host_device, 0x02);
        g_output_endpoint = usb_GetDeviceEndpoint(host_device, 0x81);
        g_connected = (g_input_endpoint != NULL && g_output_endpoint != NULL);
        g_output_payload_size = 0;
        g_screen_dirty = true;
        start_input_header_read();
    } else if (event == USB_DEVICE_DISCONNECTED_EVENT || event == USB_DEVICE_DISABLED_EVENT) {
        g_connected = false;
        g_input_endpoint = NULL;
        g_output_endpoint = NULL;
        g_input_transfer_pending = false;
        g_output_transfer_pending = false;
        g_output_payload_size = 0;
    }

    return USB_SUCCESS;
}
