#include <stdbool.h>
#include <stdint.h>

#include <kos/framebuffer.h>
#include <kos/heap.h>
#include <kos/timer.h>

enum {
    FRAMEBUFFER_BLUR_CACHE_SLOTS = 8,
};

struct framebuffer_blur_cache_entry {
    uint32_t *pixels;
    uint64_t bytes;
    uint64_t x;
    uint64_t y;
    uint64_t width;
    uint64_t height;
    uint64_t radius;
    uint64_t source_hash;
    uint64_t last_used;
    bool valid;
};

struct kos_framebuffer {
    volatile uint8_t *address;
    uint32_t *backbuffer;
    uint32_t *background_cache;
    uint32_t *blur_source;
    uint32_t *blur_horizontal;
    uint32_t *blur_result;
    uint64_t width;
    uint64_t height;
    uint64_t pitch;
    uint64_t backbuffer_bytes;
    uint64_t background_cache_bytes;
    uint64_t blur_capacity_bytes;
    uint64_t blur_cache_bytes;
    uint64_t blur_cache_clock;
    uint64_t blur_cache_hit_count;
    uint64_t blur_cache_miss_count;
    uint64_t cursor_draw_count;
    uint64_t cursor_integrity_repair_count;
    uint64_t cursor_integrity_skip_count;
    uint64_t cursor_refresh_count;
    uint64_t cursor_last_refresh_ms;
    struct framebuffer_blur_cache_entry blur_cache[FRAMEBUFFER_BLUR_CACHE_SLOTS];
    uint8_t red_mask_shift;
    uint8_t green_mask_shift;
    uint8_t blue_mask_shift;
    bool initialized;
    bool frame_active;
    bool frame_damage_active;
    uint64_t frame_damage_x;
    uint64_t frame_damage_y;
    uint64_t frame_damage_width;
    uint64_t frame_damage_height;
    bool background_cache_valid;
    bool cursor_visible;
    bool cursor_saved;
    int32_t cursor_x;
    int32_t cursor_y;
    uint64_t cursor_saved_x;
    uint64_t cursor_saved_y;
    uint32_t cursor_saved_width;
    uint32_t cursor_saved_height;
    uint32_t cursor_underlay[15 * 11];
};

static struct kos_framebuffer framebuffer;
static volatile uint32_t *framebuffer_row(uint64_t row);
static volatile uint32_t *framebuffer_scanout_row(uint64_t row);

#define FRAMEBUFFER_CACHE_MAX_BYTES (64ull * 1024ull * 1024ull)
#define FRAMEBUFFER_BLUR_MAX_BYTES (16ull * 1024ull * 1024ull)
#define FRAMEBUFFER_BLUR_CACHE_MAX_BYTES (16ull * 1024ull * 1024ull)
#define FRAMEBUFFER_BLUR_CACHE_ENTRY_MAX_BYTES (4ull * 1024ull * 1024ull)

static const uint16_t cursor_rows[13] = {
    0x0003, 0x0007, 0x000f, 0x001f, 0x003f, 0x007f, 0x00ff,
    0x01ff, 0x00ff, 0x00db, 0x0198, 0x0300, 0x0300,
};

static bool cursor_shape_pixel(int32_t x, int32_t y) {
    if (x < 0 || x >= 9 || y < 0 || y >= 13) return false;
    return (cursor_rows[y] & (1u << (uint32_t)x)) != 0;
}

static void cursor_restore(void) {
    if (!framebuffer.cursor_saved) return;
    for (uint32_t row = 0; row < framebuffer.cursor_saved_height; ++row) {
        volatile uint32_t *destination = framebuffer_scanout_row(
            framebuffer.cursor_saved_y + row) + framebuffer.cursor_saved_x;
        const uint32_t *source = framebuffer.cursor_underlay
            + (uint64_t)row * 11;
        for (uint32_t column = 0; column < framebuffer.cursor_saved_width; ++column) {
            destination[column] = source[column];
        }
    }
    framebuffer.cursor_saved = false;
}

static void cursor_paint(void) {
    if (!framebuffer.initialized || !framebuffer.cursor_visible
        || framebuffer.frame_active || framebuffer.width == 0
        || framebuffer.height == 0) return;
    int64_t left = (int64_t)framebuffer.cursor_x - 1;
    int64_t top = (int64_t)framebuffer.cursor_y - 1;
    int64_t right = (int64_t)framebuffer.cursor_x + 10;
    int64_t bottom = (int64_t)framebuffer.cursor_y + 14;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if ((uint64_t)right > framebuffer.width) right = (int64_t)framebuffer.width;
    if ((uint64_t)bottom > framebuffer.height) bottom = (int64_t)framebuffer.height;
    if (right <= left || bottom <= top) return;
    uint32_t outline = framebuffer_make_color(5, 12, 22);
    uint32_t fill = framebuffer_make_color(244, 249, 255);
    for (int64_t y = top; y < bottom; ++y) {
        volatile uint32_t *destination = framebuffer_scanout_row((uint64_t)y);
        for (int64_t x = left; x < right; ++x) {
            int32_t shape_x = (int32_t)(x - framebuffer.cursor_x);
            int32_t shape_y = (int32_t)(y - framebuffer.cursor_y);
            if (cursor_shape_pixel(shape_x, shape_y)) {
                destination[x] = fill;
                continue;
            }
            bool edge = false;
            for (int32_t dy = -1; dy <= 1 && !edge; ++dy) {
                for (int32_t dx = -1; dx <= 1; ++dx) {
                    if ((dx != 0 || dy != 0)
                        && cursor_shape_pixel(shape_x + dx, shape_y + dy)) {
                        edge = true;
                        break;
                    }
                }
            }
            if (edge) destination[x] = outline;
        }
    }
}

static void cursor_draw(void) {
    if (!framebuffer.initialized || !framebuffer.cursor_visible
        || framebuffer.cursor_saved || framebuffer.frame_active
        || framebuffer.width == 0 || framebuffer.height == 0) return;
    int64_t left = (int64_t)framebuffer.cursor_x - 1;
    int64_t top = (int64_t)framebuffer.cursor_y - 1;
    int64_t right = (int64_t)framebuffer.cursor_x + 10;
    int64_t bottom = (int64_t)framebuffer.cursor_y + 14;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if ((uint64_t)right > framebuffer.width) right = (int64_t)framebuffer.width;
    if ((uint64_t)bottom > framebuffer.height) bottom = (int64_t)framebuffer.height;
    if (right <= left || bottom <= top) return;

    framebuffer.cursor_saved_x = (uint64_t)left;
    framebuffer.cursor_saved_y = (uint64_t)top;
    framebuffer.cursor_saved_width = (uint32_t)(right - left);
    framebuffer.cursor_saved_height = (uint32_t)(bottom - top);
    for (uint32_t row = 0; row < framebuffer.cursor_saved_height; ++row) {
        volatile uint32_t *source = framebuffer_scanout_row(
            framebuffer.cursor_saved_y + row) + framebuffer.cursor_saved_x;
        uint32_t *destination = framebuffer.cursor_underlay + (uint64_t)row * 11;
        for (uint32_t column = 0; column < framebuffer.cursor_saved_width; ++column)
            destination[column] = source[column];
    }
    cursor_paint();
    framebuffer.cursor_saved = true;
    if (framebuffer.cursor_draw_count != UINT64_MAX) ++framebuffer.cursor_draw_count;
}

static bool cursor_overlay_intact(void) {
    if (!framebuffer.cursor_saved) return false;
    uint32_t fill = framebuffer_make_color(244, 249, 255);
    bool found_pixel = false;
    for (int32_t y = 0; y < 13; ++y) {
        for (int32_t x = 0; x < 9; ++x) {
            if (!cursor_shape_pixel(x, y)) continue;
            int64_t screen_x = (int64_t)framebuffer.cursor_x + x;
            int64_t screen_y = (int64_t)framebuffer.cursor_y + y;
            if (screen_x < 0 || screen_y < 0
                || (uint64_t)screen_x >= framebuffer.width
                || (uint64_t)screen_y >= framebuffer.height) continue;
            found_pixel = true;
            if (framebuffer_scanout_row((uint64_t)screen_y)[screen_x] != fill)
                return false;
        }
    }
    return found_pixel;
}

static void cursor_restore_layer(void) {
    if (!framebuffer.cursor_saved) return;
    if (!framebuffer.frame_active && framebuffer.backbuffer != 0
        && !cursor_overlay_intact()) {
        /* A painter changed scanout but left our saved-underlay flag stale.
           Recover the current clean composition rather than restoring pixels
           captured before the desktop changed. */
        for (uint32_t row = 0; row < framebuffer.cursor_saved_height; ++row) {
            volatile uint32_t *destination = framebuffer_scanout_row(
                framebuffer.cursor_saved_y + row) + framebuffer.cursor_saved_x;
            const uint32_t *source = framebuffer.backbuffer
                + (framebuffer.cursor_saved_y + row)
                    * (framebuffer.pitch / sizeof(uint32_t))
                + framebuffer.cursor_saved_x;
            for (uint32_t column = 0; column < framebuffer.cursor_saved_width; ++column)
                destination[column] = source[column];
        }
        framebuffer.cursor_saved = false;
        return;
    }
    cursor_restore();
}

static bool channel_mask_is_valid(uint8_t shift, uint8_t size) {
    return size == 8 && shift <= 24;
}

bool framebuffer_initialize(const struct limine_framebuffer *source) {
    if (source == 0 || source->address == 0 || source->width == 0 || source->height == 0) {
        return false;
    }
    if (source->memory_model != LIMINE_FRAMEBUFFER_RGB || source->bpp != 32) {
        return false;
    }
    if (source->width > UINT64_MAX / sizeof(uint32_t)
        || source->pitch < source->width * sizeof(uint32_t)
        || source->pitch % sizeof(uint32_t) != 0
        || source->height > UINT64_MAX / source->pitch) {
        return false;
    }
    if (!channel_mask_is_valid(source->red_mask_shift, source->red_mask_size)
        || !channel_mask_is_valid(source->green_mask_shift, source->green_mask_size)
        || !channel_mask_is_valid(source->blue_mask_shift, source->blue_mask_size)) {
        return false;
    }
    uint32_t red_mask = 0xffu << source->red_mask_shift;
    uint32_t green_mask = 0xffu << source->green_mask_shift;
    uint32_t blue_mask = 0xffu << source->blue_mask_shift;
    if ((red_mask & green_mask) != 0 || (red_mask & blue_mask) != 0 || (green_mask & blue_mask) != 0) {
        return false;
    }

    framebuffer.address = (volatile uint8_t *)source->address;
    framebuffer.width = source->width;
    framebuffer.height = source->height;
    framebuffer.pitch = source->pitch;
    framebuffer.red_mask_shift = source->red_mask_shift;
    framebuffer.green_mask_shift = source->green_mask_shift;
    framebuffer.blue_mask_shift = source->blue_mask_shift;
    framebuffer.initialized = true;
    return true;
}

bool framebuffer_is_initialized(void) {
    return framebuffer.initialized;
}

bool framebuffer_enable_backbuffer(void) {
    if (!framebuffer.initialized) return false;
    uint64_t bytes = framebuffer.pitch * framebuffer.height;
    if (bytes == 0 || bytes > FRAMEBUFFER_CACHE_MAX_BYTES) return false;
    if (framebuffer.backbuffer != 0 && framebuffer.backbuffer_bytes >= bytes) return true;
    uint32_t *replacement = kmalloc(bytes);
    if (replacement == 0) return false;
    if (framebuffer.backbuffer != 0) (void)kfree(framebuffer.backbuffer);
    framebuffer.backbuffer = replacement;
    framebuffer.backbuffer_bytes = bytes;
    return true;
}

bool framebuffer_backbuffer_available(void) {
    return framebuffer.initialized && framebuffer.backbuffer != 0;
}

bool framebuffer_frame_begin(bool preserve_contents) {
    if (!framebuffer.initialized || framebuffer.frame_active
        || framebuffer.backbuffer == 0) return false;
    cursor_restore_layer();
    if (preserve_contents) {
        for (uint64_t row = 0; row < framebuffer.height; ++row) {
            volatile uint32_t *source = framebuffer_scanout_row(row);
            uint32_t *destination = (uint32_t *)framebuffer.backbuffer
                + row * (framebuffer.pitch / sizeof(uint32_t));
            for (uint64_t column = 0; column < framebuffer.pitch / sizeof(uint32_t); ++column) {
                destination[column] = source[column];
            }
        }
    }
    framebuffer.frame_active = true;
    framebuffer.frame_damage_active = false;
    return true;
}

bool framebuffer_frame_begin_damage(uint64_t x, uint64_t y,
                                    uint64_t width, uint64_t height) {
    if (!framebuffer.initialized || width == 0 || height == 0
        || x >= framebuffer.width || y >= framebuffer.height
        || width > framebuffer.width - x || height > framebuffer.height - y
        || !framebuffer_frame_begin(false)) return false;
    framebuffer.frame_damage_active = true;
    framebuffer.frame_damage_x = x;
    framebuffer.frame_damage_y = y;
    framebuffer.frame_damage_width = width;
    framebuffer.frame_damage_height = height;
    return true;
}

void framebuffer_frame_present(void) {
    if (!framebuffer.initialized || !framebuffer.frame_active
        || framebuffer.backbuffer == 0) return;
    for (uint64_t row = 0; row < framebuffer.height; ++row) {
        volatile uint32_t *destination = framebuffer_scanout_row(row);
        const uint32_t *source = framebuffer.backbuffer
            + row * (framebuffer.pitch / sizeof(uint32_t));
        for (uint64_t column = 0; column < framebuffer.pitch / sizeof(uint32_t); ++column) {
            destination[column] = source[column];
        }
    }
    framebuffer.frame_active = false;
    framebuffer.frame_damage_active = false;
    /* The GUI service re-anchors the independent cursor after presentation. */
    framebuffer.cursor_saved = false;
}

bool framebuffer_frame_present_damage(uint64_t x, uint64_t y,
                                      uint64_t width, uint64_t height) {
    if (!framebuffer.initialized || !framebuffer.frame_active
        || !framebuffer.frame_damage_active || framebuffer.backbuffer == 0
        || x != framebuffer.frame_damage_x || y != framebuffer.frame_damage_y
        || width != framebuffer.frame_damage_width
        || height != framebuffer.frame_damage_height) return false;
    for (uint64_t row = y; row < y + height; ++row) {
        volatile uint32_t *destination = framebuffer_scanout_row(row) + x;
        const uint32_t *source = framebuffer.backbuffer
            + row * (framebuffer.pitch / sizeof(uint32_t)) + x;
        uint64_t column = 0;
        if (((uintptr_t)destination & (sizeof(uint64_t) - 1u)) != 0) {
            destination[column] = source[column];
            ++column;
        }
        volatile uint64_t *wide_destination =
            (volatile uint64_t *)(destination + column);
        for (; width - column >= 2; column += 2) {
            uint64_t pair = source[column]
                | ((uint64_t)source[column + 1] << 32);
            wide_destination[(column - (column & 1u)) / 2u] = pair;
        }
        for (; column < width; ++column)
            destination[column] = source[column];
    }
    framebuffer.frame_active = false;
    framebuffer.frame_damage_active = false;
    framebuffer.cursor_saved = false;
    return true;
}

void framebuffer_frame_abort(void) {
    framebuffer.frame_active = false;
    framebuffer.frame_damage_active = false;
    cursor_draw();
}

void framebuffer_cursor_set(bool visible, int32_t x, int32_t y) {
    if (!framebuffer.initialized) return;
    if (!visible) {
        if (!framebuffer.cursor_visible && !framebuffer.cursor_saved) return;
        cursor_restore_layer();
        framebuffer.cursor_visible = false;
        return;
    }

    if (x < 0) x = 0;
    if (y < 0) y = 0;
    uint64_t max_x = framebuffer.width - 1;
    uint64_t max_y = framebuffer.height - 1;
    int32_t next_x = (uint64_t)(uint32_t)x > max_x
        ? (int32_t)(max_x > INT32_MAX ? INT32_MAX : max_x) : x;
    int32_t next_y = (uint64_t)(uint32_t)y > max_y
        ? (int32_t)(max_y > INT32_MAX ? INT32_MAX : max_y) : y;

    /* Input polling frequently reports the same pointer position. Leaving the
       retained overlay untouched avoids a restore/redraw flash on every poll. */
    bool same_position = framebuffer.cursor_visible
        && framebuffer.cursor_x == next_x && framebuffer.cursor_y == next_y;
    if (same_position && framebuffer.cursor_saved) {
        if (cursor_overlay_intact()) {
            if (framebuffer.cursor_integrity_skip_count != UINT64_MAX)
                ++framebuffer.cursor_integrity_skip_count;
            return;
        }
        if (framebuffer.cursor_integrity_repair_count != UINT64_MAX)
            ++framebuffer.cursor_integrity_repair_count;
    }

    cursor_restore_layer();
    framebuffer.cursor_visible = true;
    framebuffer.cursor_x = next_x;
    framebuffer.cursor_y = next_y;
    cursor_draw();
}

void framebuffer_cursor_suspend(void) {
    cursor_restore_layer();
}

void framebuffer_cursor_resume(void) {
    cursor_draw();
}

void framebuffer_cursor_refresh(void) {
    if (!framebuffer.initialized || !framebuffer.cursor_visible
        || framebuffer.frame_active) return;
    uint64_t now = timer_get_uptime_ms();
    if (framebuffer.cursor_last_refresh_ms != 0
        && now >= framebuffer.cursor_last_refresh_ms
        && now - framebuffer.cursor_last_refresh_ms < 8) return;
    framebuffer.cursor_last_refresh_ms = now;
    if (!framebuffer.cursor_saved) {
        cursor_draw();
        return;
    }
    if (!cursor_overlay_intact()) {
        if (framebuffer.cursor_integrity_repair_count != UINT64_MAX)
            ++framebuffer.cursor_integrity_repair_count;
        cursor_restore_layer();
        cursor_draw();
        return;
    }
    /* Repaint only the tiny cursor shape. This avoids tearing if scanout has
       transiently hidden the overlay, without touching desktop damage. */
    cursor_paint();
    if (framebuffer.cursor_refresh_count != UINT64_MAX)
        ++framebuffer.cursor_refresh_count;
}

void framebuffer_cursor_overlay_stats(uint64_t *draws, uint64_t *repairs,
                                      uint64_t *intact_skips, int32_t *x, int32_t *y,
                                      bool *visible, bool *saved, uint64_t *refreshes) {
    if (draws != 0) *draws = framebuffer.cursor_draw_count;
    if (repairs != 0) *repairs = framebuffer.cursor_integrity_repair_count;
    if (intact_skips != 0) *intact_skips = framebuffer.cursor_integrity_skip_count;
    if (x != 0) *x = framebuffer.cursor_x;
    if (y != 0) *y = framebuffer.cursor_y;
    if (visible != 0) *visible = framebuffer.cursor_visible;
    if (saved != 0) *saved = framebuffer.cursor_saved;
    if (refreshes != 0) *refreshes = framebuffer.cursor_refresh_count;
}

static volatile uint32_t *framebuffer_scanout_row(uint64_t row) {
    return (volatile uint32_t *)(framebuffer.address + row * framebuffer.pitch);
}

bool framebuffer_background_capture(void) {
    if (!framebuffer.initialized || framebuffer.frame_damage_active
        || framebuffer.width > UINT64_MAX / sizeof(uint32_t)) return false;
    uint64_t row_bytes = framebuffer.width * sizeof(uint32_t);
    if (framebuffer.height > UINT64_MAX / row_bytes) return false;
    uint64_t bytes = row_bytes * framebuffer.height;
    if (bytes == 0 || bytes > FRAMEBUFFER_CACHE_MAX_BYTES) return false;
    if (framebuffer.background_cache == 0 || framebuffer.background_cache_bytes < bytes) {
        uint32_t *replacement = kmalloc(bytes);
        if (replacement == 0) return false;
        if (framebuffer.background_cache != 0) (void)kfree(framebuffer.background_cache);
        framebuffer.background_cache = replacement;
        framebuffer.background_cache_bytes = bytes;
    }
    for (uint64_t row = 0; row < framebuffer.height; ++row) {
        volatile uint32_t *source = framebuffer_row(row);
        uint32_t *destination = framebuffer.background_cache + row * framebuffer.width;
        for (uint64_t column = 0; column < framebuffer.width; ++column) {
            destination[column] = source[column];
        }
    }
    framebuffer.background_cache_valid = true;
    return true;
}

bool framebuffer_background_restore(void) {
    if (!framebuffer.initialized || !framebuffer.background_cache_valid
        || framebuffer.background_cache == 0) return false;
    if (framebuffer.frame_damage_active)
        return framebuffer_background_restore_rect(framebuffer.frame_damage_x,
            framebuffer.frame_damage_y, framebuffer.frame_damage_width,
            framebuffer.frame_damage_height);
    for (uint64_t row = 0; row < framebuffer.height; ++row) {
        volatile uint32_t *destination = framebuffer_row(row);
        const uint32_t *source = framebuffer.background_cache + row * framebuffer.width;
        for (uint64_t column = 0; column < framebuffer.width; ++column) {
            destination[column] = source[column];
        }
    }
    return true;
}

bool framebuffer_background_restore_rect(uint64_t x, uint64_t y,
                                         uint64_t width, uint64_t height) {
    if (!framebuffer.initialized || !framebuffer.frame_active
        || !framebuffer.background_cache_valid || framebuffer.background_cache == 0
        || width == 0 || height == 0 || x >= framebuffer.width
        || y >= framebuffer.height || width > framebuffer.width - x
        || height > framebuffer.height - y) return false;
    for (uint64_t row = y; row < y + height; ++row) {
        volatile uint32_t *destination = framebuffer_row(row) + x;
        const uint32_t *source = framebuffer.background_cache
            + row * framebuffer.width + x;
        for (uint64_t column = 0; column < width; ++column)
            destination[column] = source[column];
    }
    return true;
}

bool framebuffer_background_cache_available(void) {
    return framebuffer.background_cache_valid;
}

uint32_t framebuffer_make_color(uint8_t red, uint8_t green, uint8_t blue) {
    if (!framebuffer.initialized) {
        return 0;
    }
    return ((uint32_t)red << framebuffer.red_mask_shift)
        | ((uint32_t)green << framebuffer.green_mask_shift)
        | ((uint32_t)blue << framebuffer.blue_mask_shift);
}

uint64_t framebuffer_width(void) {
    return framebuffer.width;
}

uint64_t framebuffer_height(void) {
    return framebuffer.height;
}

uint64_t framebuffer_pitch(void) {
    return framebuffer.pitch;
}

uint64_t framebuffer_virtual_address(void) {
    return (uint64_t)framebuffer.address;
}

static volatile uint32_t *framebuffer_row(uint64_t row) {
    if (framebuffer.frame_active && framebuffer.backbuffer != 0) {
        return (volatile uint32_t *)((uint8_t *)framebuffer.backbuffer
            + row * framebuffer.pitch);
    }
    return framebuffer_scanout_row(row);
}

static uint8_t pixel_channel(uint32_t pixel, uint8_t shift) {
    return (uint8_t)(pixel >> shift);
}

static uint32_t rgb_pixel(uint8_t red, uint8_t green, uint8_t blue) {
    return framebuffer_make_color(red, green, blue);
}

static uint32_t blend_pixel(uint32_t destination, uint8_t red, uint8_t green,
                            uint8_t blue, uint8_t alpha) {
    if (alpha == 255) return rgb_pixel(red, green, blue);
    if (alpha == 0) return destination;
    uint32_t inverse = 255u - alpha;
    uint8_t out_red = (uint8_t)((red * alpha
        + pixel_channel(destination, framebuffer.red_mask_shift) * inverse + 127u) / 255u);
    uint8_t out_green = (uint8_t)((green * alpha
        + pixel_channel(destination, framebuffer.green_mask_shift) * inverse + 127u) / 255u);
    uint8_t out_blue = (uint8_t)((blue * alpha
        + pixel_channel(destination, framebuffer.blue_mask_shift) * inverse + 127u) / 255u);
    return rgb_pixel(out_red, out_green, out_blue);
}

static uint64_t rounded_row_inset(uint64_t radius, uint64_t row, uint64_t height) {
    if (radius == 0 || row >= height || (row >= radius && row + radius < height)) return 0;
    uint64_t dy = row < radius ? radius * 2 - row * 2 - 1
        : (row - (height - radius)) * 2 + 1;
    uint64_t diameter = radius * 2;
    uint64_t circle = diameter * diameter;
    for (uint64_t inset = 0; inset < radius; ++inset) {
        uint64_t dx = diameter - inset * 2 - 1;
        if (dx * dx + dy * dy <= circle) return inset;
    }
    return radius;
}

static uint32_t channel_sum_value(uint32_t sum, uint32_t count) {
    return count == 0 ? 0 : (sum + count / 2) / count;
}

static void blur_snapshot(const uint32_t *source, uint32_t *horizontal,
                          uint64_t width, uint64_t height, uint64_t radius) {
    for (uint64_t y = 0; y < height; ++y) {
        uint32_t red = 0, green = 0, blue = 0, count = 0;
        uint64_t right = radius < width ? radius : width - 1;
        for (uint64_t x = 0; x <= right; ++x) {
            uint32_t pixel = source[y * width + x];
            red += pixel_channel(pixel, framebuffer.red_mask_shift);
            green += pixel_channel(pixel, framebuffer.green_mask_shift);
            blue += pixel_channel(pixel, framebuffer.blue_mask_shift);
            ++count;
        }
        for (uint64_t x = 0; x < width; ++x) {
            horizontal[y * width + x] = rgb_pixel(
                (uint8_t)channel_sum_value(red, count),
                (uint8_t)channel_sum_value(green, count),
                (uint8_t)channel_sum_value(blue, count));
            if (x + 1 == width) continue;
            if (x >= radius) {
                uint32_t pixel = source[y * width + x - radius];
                red -= pixel_channel(pixel, framebuffer.red_mask_shift);
                green -= pixel_channel(pixel, framebuffer.green_mask_shift);
                blue -= pixel_channel(pixel, framebuffer.blue_mask_shift);
                --count;
            }
            if (x + radius + 1 < width) {
                uint32_t pixel = source[y * width + x + radius + 1];
                red += pixel_channel(pixel, framebuffer.red_mask_shift);
                green += pixel_channel(pixel, framebuffer.green_mask_shift);
                blue += pixel_channel(pixel, framebuffer.blue_mask_shift);
                ++count;
            }
        }
    }
}

static bool ensure_blur_buffers(uint64_t required_bytes) {
    if (required_bytes == 0 || required_bytes > FRAMEBUFFER_BLUR_MAX_BYTES) return false;
    if (framebuffer.blur_source != 0 && framebuffer.blur_horizontal != 0
        && framebuffer.blur_result != 0
        && framebuffer.blur_capacity_bytes >= required_bytes) return true;
    uint64_t capacity = framebuffer.blur_capacity_bytes;
    if (capacity < 1024ull * 1024ull) capacity = 1024ull * 1024ull;
    while (capacity < required_bytes) {
        if (capacity > FRAMEBUFFER_BLUR_MAX_BYTES / 2) {
            capacity = FRAMEBUFFER_BLUR_MAX_BYTES;
            break;
        }
        capacity *= 2;
    }
    if (capacity < required_bytes) return false;
    uint32_t *source = kmalloc(capacity);
    uint32_t *horizontal = kmalloc(capacity);
    uint32_t *result = kmalloc(capacity);
    if (source == 0 || horizontal == 0 || result == 0) {
        if (source != 0) (void)kfree(source);
        if (horizontal != 0) (void)kfree(horizontal);
        if (result != 0) (void)kfree(result);
        return false;
    }
    if (framebuffer.blur_source != 0) (void)kfree(framebuffer.blur_source);
    if (framebuffer.blur_horizontal != 0) (void)kfree(framebuffer.blur_horizontal);
    if (framebuffer.blur_result != 0) (void)kfree(framebuffer.blur_result);
    framebuffer.blur_source = source;
    framebuffer.blur_horizontal = horizontal;
    framebuffer.blur_result = result;
    framebuffer.blur_capacity_bytes = capacity;
    return true;
}

static uint64_t blur_source_hash(uint64_t x, uint64_t y,
                                uint64_t width, uint64_t height) {
    uint64_t hash = 14695981039346656037ull;
    for (uint64_t row = 0; row < height; ++row) {
        volatile uint32_t *pixels = framebuffer_row(y + row) + x;
        for (uint64_t column = 0; column < width; ++column) {
            hash ^= pixels[column];
            hash *= 1099511628211ull;
        }
    }
    return hash;
}

static void blur_cache_evict(uint32_t index) {
    struct framebuffer_blur_cache_entry *entry = &framebuffer.blur_cache[index];
    if (entry->pixels != 0) {
        (void)kfree(entry->pixels);
        if (framebuffer.blur_cache_bytes >= entry->bytes)
            framebuffer.blur_cache_bytes -= entry->bytes;
        else framebuffer.blur_cache_bytes = 0;
    }
    *entry = (struct framebuffer_blur_cache_entry){0};
}

static int32_t blur_cache_find(uint64_t x, uint64_t y, uint64_t width,
                               uint64_t height, uint64_t radius,
                               uint64_t source_hash) {
    for (uint32_t index = 0; index < FRAMEBUFFER_BLUR_CACHE_SLOTS; ++index) {
        struct framebuffer_blur_cache_entry *entry = &framebuffer.blur_cache[index];
        if (entry->valid && entry->x == x && entry->y == y
            && entry->width == width && entry->height == height
            && entry->radius == radius && entry->source_hash == source_hash) {
            if (framebuffer.blur_cache_clock != UINT64_MAX)
                ++framebuffer.blur_cache_clock;
            entry->last_used = framebuffer.blur_cache_clock;
            if (framebuffer.blur_cache_hit_count != UINT64_MAX)
                ++framebuffer.blur_cache_hit_count;
            return (int32_t)index;
        }
    }
    if (framebuffer.blur_cache_miss_count != UINT64_MAX)
        ++framebuffer.blur_cache_miss_count;
    return -1;
}

static uint32_t blur_cache_lru(uint32_t exclude_index) {
    uint32_t oldest = FRAMEBUFFER_BLUR_CACHE_SLOTS;
    uint64_t oldest_time = UINT64_MAX;
    for (uint32_t index = 0; index < FRAMEBUFFER_BLUR_CACHE_SLOTS; ++index) {
        struct framebuffer_blur_cache_entry *entry = &framebuffer.blur_cache[index];
        if (index == exclude_index || !entry->valid) continue;
        if (oldest == FRAMEBUFFER_BLUR_CACHE_SLOTS || entry->last_used < oldest_time) {
            oldest = index;
            oldest_time = entry->last_used;
        }
    }
    return oldest;
}

static uint32_t *blur_cache_store(uint64_t x, uint64_t y, uint64_t width,
                                  uint64_t height, uint64_t radius,
                                  uint64_t source_hash, const uint32_t *pixels,
                                  uint64_t bytes) {
    if (bytes == 0 || bytes > FRAMEBUFFER_BLUR_CACHE_ENTRY_MAX_BYTES
        || bytes > FRAMEBUFFER_BLUR_CACHE_MAX_BYTES) return 0;

    uint32_t slot = FRAMEBUFFER_BLUR_CACHE_SLOTS;
    for (uint32_t index = 0; index < FRAMEBUFFER_BLUR_CACHE_SLOTS; ++index) {
        struct framebuffer_blur_cache_entry *entry = &framebuffer.blur_cache[index];
        if (entry->valid && entry->x == x && entry->y == y
            && entry->width == width && entry->height == height
            && entry->radius == radius) {
            slot = index;
            break;
        }
        if (!entry->valid && slot == FRAMEBUFFER_BLUR_CACHE_SLOTS) slot = index;
    }
    if (slot == FRAMEBUFFER_BLUR_CACHE_SLOTS) {
        slot = blur_cache_lru(FRAMEBUFFER_BLUR_CACHE_SLOTS);
        if (slot == FRAMEBUFFER_BLUR_CACHE_SLOTS) return 0;
    }
    blur_cache_evict(slot);
    while (framebuffer.blur_cache_bytes > FRAMEBUFFER_BLUR_CACHE_MAX_BYTES - bytes) {
        uint32_t victim = blur_cache_lru(slot);
        if (victim == FRAMEBUFFER_BLUR_CACHE_SLOTS) return 0;
        blur_cache_evict(victim);
    }

    uint32_t *cached_pixels = kmalloc(bytes);
    if (cached_pixels == 0) return 0;
    for (uint64_t index = 0; index < width * height; ++index)
        cached_pixels[index] = pixels[index];
    if (framebuffer.blur_cache_clock != UINT64_MAX)
        ++framebuffer.blur_cache_clock;
    framebuffer.blur_cache[slot] = (struct framebuffer_blur_cache_entry){
        .pixels = cached_pixels,
        .bytes = bytes,
        .x = x,
        .y = y,
        .width = width,
        .height = height,
        .radius = radius,
        .source_hash = source_hash,
        .last_used = framebuffer.blur_cache_clock,
        .valid = true,
    };
    framebuffer.blur_cache_bytes += bytes;
    return cached_pixels;
}

void framebuffer_blur_cache_stats(uint64_t *hits, uint64_t *misses,
                                  uint64_t *bytes_used) {
    if (hits != 0) *hits = framebuffer.blur_cache_hit_count;
    if (misses != 0) *misses = framebuffer.blur_cache_miss_count;
    if (bytes_used != 0) *bytes_used = framebuffer.blur_cache_bytes;
}

void framebuffer_glass_rect(uint64_t x, uint64_t y, uint64_t width, uint64_t height,
                            uint64_t radius, uint64_t blur_radius,
                            uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha,
                            uint8_t border_red, uint8_t border_green, uint8_t border_blue,
                            uint8_t border_alpha) {
    if (!framebuffer.initialized || width == 0 || height == 0
        || x >= framebuffer.width || y >= framebuffer.height) return;
    if (width > framebuffer.width - x) width = framebuffer.width - x;
    if (height > framebuffer.height - y) height = framebuffer.height - y;
    uint64_t draw_x = x;
    uint64_t draw_y = y;
    uint64_t draw_end_x = x + width;
    uint64_t draw_end_y = y + height;
    if (framebuffer.frame_damage_active) {
        uint64_t damage_end_x = framebuffer.frame_damage_x
            + framebuffer.frame_damage_width;
        uint64_t damage_end_y = framebuffer.frame_damage_y
            + framebuffer.frame_damage_height;
        if (draw_x < framebuffer.frame_damage_x) draw_x = framebuffer.frame_damage_x;
        if (draw_y < framebuffer.frame_damage_y) draw_y = framebuffer.frame_damage_y;
        if (draw_end_x > damage_end_x) draw_end_x = damage_end_x;
        if (draw_end_y > damage_end_y) draw_end_y = damage_end_y;
        if (draw_x >= draw_end_x || draw_y >= draw_end_y) return;
    }
    if (radius > width / 2) radius = width / 2;
    if (radius > height / 2) radius = height / 2;
    if (blur_radius > 30) blur_radius = 30;

    if (width > UINT64_MAX / height) return;
    uint64_t area = width * height;
    if (area > UINT64_MAX / sizeof(uint32_t)) return;
    uint64_t buffer_bytes = area * sizeof(uint32_t);
    uint32_t *blurred_pixels = 0;
    if (blur_radius != 0 && buffer_bytes <= FRAMEBUFFER_BLUR_MAX_BYTES) {
        uint64_t source_hash = blur_source_hash(x, y, width, height);
        int32_t cached = blur_cache_find(x, y, width, height,
                                         blur_radius, source_hash);
        if (cached >= 0) {
            blurred_pixels = framebuffer.blur_cache[cached].pixels;
        }
        else if (ensure_blur_buffers(buffer_bytes)) {
            uint32_t *source = framebuffer.blur_source;
            uint32_t *horizontal = framebuffer.blur_horizontal;
            uint32_t *result = framebuffer.blur_result;
            for (uint64_t row = 0; row < height; ++row) {
                volatile uint32_t *pixels = framebuffer_row(y + row) + x;
                for (uint64_t column = 0; column < width; ++column) {
                    source[row * width + column] = pixels[column];
                }
            }
            blur_snapshot(source, horizontal, width, height, blur_radius);

            for (uint64_t column = 0; column < width; ++column) {
                uint32_t red_sum = 0, green_sum = 0, blue_sum = 0, count = 0;
                uint64_t bottom = blur_radius < height ? blur_radius : height - 1;
                for (uint64_t row = 0; row <= bottom; ++row) {
                    uint32_t pixel = horizontal[row * width + column];
                    red_sum += pixel_channel(pixel, framebuffer.red_mask_shift);
                    green_sum += pixel_channel(pixel, framebuffer.green_mask_shift);
                    blue_sum += pixel_channel(pixel, framebuffer.blue_mask_shift);
                    ++count;
                }
                for (uint64_t row = 0; row < height; ++row) {
                    result[row * width + column] = rgb_pixel(
                        (uint8_t)channel_sum_value(red_sum, count),
                        (uint8_t)channel_sum_value(green_sum, count),
                        (uint8_t)channel_sum_value(blue_sum, count));
                    if (row + 1 == height) continue;
                    if (row >= blur_radius) {
                        uint32_t pixel = horizontal[(row - blur_radius) * width + column];
                        red_sum -= pixel_channel(pixel, framebuffer.red_mask_shift);
                        green_sum -= pixel_channel(pixel, framebuffer.green_mask_shift);
                        blue_sum -= pixel_channel(pixel, framebuffer.blue_mask_shift);
                        --count;
                    }
                    if (row + blur_radius + 1 < height) {
                        uint32_t pixel = horizontal[(row + blur_radius + 1) * width + column];
                        red_sum += pixel_channel(pixel, framebuffer.red_mask_shift);
                        green_sum += pixel_channel(pixel, framebuffer.green_mask_shift);
                        blue_sum += pixel_channel(pixel, framebuffer.blue_mask_shift);
                        ++count;
                    }
                }
            }
            uint32_t *cached_pixels = blur_cache_store(x, y, width, height,
                blur_radius, source_hash, result, buffer_bytes);
            blurred_pixels = cached_pixels != 0 ? cached_pixels : result;
        }
    }

    uint64_t first_damage_column = draw_x - x;
    uint64_t last_damage_column = draw_end_x - x;
    for (uint64_t row = draw_y - y; row < draw_end_y - y; ++row) {
        /* Corner inset depends only on this scanline; compute it once instead
           of repeating the radius search for every pixel in the row. */
        uint64_t inset = rounded_row_inset(radius, row, height);
        uint64_t first_column = first_damage_column > inset
            ? first_damage_column : inset;
        uint64_t last_column = last_damage_column < width - inset
            ? last_damage_column : width - inset;
        volatile uint32_t *destination = framebuffer_row(y + row) + x + first_column;
        for (uint64_t column = first_column; column < last_column; ++column) {
            if (blurred_pixels != 0) {
                uint32_t blurred = blurred_pixels[row * width + column];
                *destination = blend_pixel(blurred, red, green, blue, alpha);
            }
            else *destination = blend_pixel(*destination, red, green, blue, alpha);
            ++destination;
        }
    }

    if (border_alpha != 0 && width > 1 && height > 1) {
        uint64_t inner_width = width - 2;
        uint64_t inner_height = height - 2;
        uint64_t inner_radius = radius == 0 ? 0 : radius - 1;
        if (inner_radius > inner_width / 2) inner_radius = inner_width / 2;
        if (inner_radius > inner_height / 2) inner_radius = inner_height / 2;
        for (uint64_t row = draw_y - y; row < draw_end_y - y; ++row) {
            uint64_t outer_inset = rounded_row_inset(radius, row, height);
            uint64_t inner_inset = row == 0 || row + 1 == height ? 0
                : rounded_row_inset(inner_radius, row - 1, inner_height);
            uint64_t first_column = outer_inset;
            uint64_t last_column = width - outer_inset;
            if (first_column < draw_x - x) first_column = draw_x - x;
            if (last_column > draw_end_x - x) last_column = draw_end_x - x;
            for (uint64_t column = first_column; column < last_column; ++column) {
                bool inner = row != 0 && row + 1 != height
                    && column >= 1 + inner_inset
                    && column < width - 1 - inner_inset;
                if (!inner) {
                    volatile uint32_t *pixel = framebuffer_row(y + row) + x + column;
                    *pixel = blend_pixel(*pixel, border_red, border_green,
                                         border_blue, border_alpha);
                }
            }
        }
    }
}

void framebuffer_fill_rect(uint64_t x, uint64_t y, uint64_t width, uint64_t height, uint32_t color) {
    if (!framebuffer.initialized || x >= framebuffer.width || y >= framebuffer.height) {
        return;
    }

    uint64_t end_x = x + width;
    uint64_t end_y = y + height;
    if (end_x > framebuffer.width || end_x < x) {
        end_x = framebuffer.width;
    }
    if (end_y > framebuffer.height || end_y < y) {
        end_y = framebuffer.height;
    }
    if (framebuffer.frame_damage_active) {
        uint64_t damage_end_x = framebuffer.frame_damage_x
            + framebuffer.frame_damage_width;
        uint64_t damage_end_y = framebuffer.frame_damage_y
            + framebuffer.frame_damage_height;
        if (x < framebuffer.frame_damage_x) x = framebuffer.frame_damage_x;
        if (y < framebuffer.frame_damage_y) y = framebuffer.frame_damage_y;
        if (end_x > damage_end_x) end_x = damage_end_x;
        if (end_y > damage_end_y) end_y = damage_end_y;
        if (x >= end_x || y >= end_y) return;
    }

    for (uint64_t row = y; row < end_y; ++row) {
        volatile uint32_t *destination = framebuffer_row(row) + x;
        for (uint64_t column = x; column < end_x; ++column) {
            *destination = color;
            ++destination;
        }
    }
}

void framebuffer_blit_rgb24_row(uint64_t x, uint64_t y, uint64_t destination_width,
                                uint32_t source_width, const uint8_t *pixels) {
    if (!framebuffer.initialized || pixels == 0 || source_width == 0
        || destination_width == 0 || x >= framebuffer.width || y >= framebuffer.height
        || destination_width > framebuffer.width - x) return;
    uint64_t first_column = 0;
    uint64_t last_column = destination_width;
    if (framebuffer.frame_damage_active) {
        uint64_t damage_end_x = framebuffer.frame_damage_x
            + framebuffer.frame_damage_width;
        uint64_t damage_end_y = framebuffer.frame_damage_y
            + framebuffer.frame_damage_height;
        if (y < framebuffer.frame_damage_y || y >= damage_end_y
            || x >= damage_end_x
            || x + destination_width <= framebuffer.frame_damage_x) return;
        if (framebuffer.frame_damage_x > x)
            first_column = framebuffer.frame_damage_x - x;
        if (damage_end_x < x + destination_width)
            last_column = damage_end_x - x;
    }
    volatile uint32_t *destination = framebuffer_row(y) + x;
    uint64_t source_column = 0;
    uint64_t source_step = source_width / destination_width;
    uint64_t source_remainder = source_width % destination_width;
    uint64_t source_error = 0;
    for (uint64_t column = 0; column < destination_width; ++column) {
        const uint8_t *pixel = pixels + source_column * 3;
        if (column >= first_column && column < last_column)
            destination[column] = framebuffer_make_color(pixel[0], pixel[1], pixel[2]);
        source_column += source_step;
        source_error += source_remainder;
        if (source_error >= destination_width) {
            ++source_column;
            source_error -= destination_width;
        }
    }
}

void framebuffer_fill(uint32_t color) {
    framebuffer_fill_rect(0, 0, framebuffer.width, framebuffer.height, color);
}

void framebuffer_scroll_up(uint64_t pixel_rows, uint32_t fill_color) {
    if (!framebuffer.initialized || pixel_rows == 0) {
        return;
    }
    if (pixel_rows >= framebuffer.height) {
        framebuffer_fill(fill_color);
        return;
    }

    uint64_t remaining_rows = framebuffer.height - pixel_rows;
    for (uint64_t row = 0; row < remaining_rows; ++row) {
        volatile uint32_t *destination = framebuffer_row(row);
        volatile uint32_t *source = framebuffer_row(row + pixel_rows);
        for (uint64_t column = 0; column < framebuffer.width; ++column) {
            *destination = *source;
            ++destination;
            ++source;
        }
    }
    framebuffer_fill_rect(0, remaining_rows, framebuffer.width, pixel_rows, fill_color);
}
