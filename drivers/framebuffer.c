#include <stdbool.h>
#include <stdint.h>

#include <kos/framebuffer.h>
#include <kos/heap.h>

struct kos_framebuffer {
    volatile uint8_t *address;
    uint32_t *backbuffer;
    uint32_t *background_cache;
    uint32_t *blur_source;
    uint32_t *blur_horizontal;
    uint64_t width;
    uint64_t height;
    uint64_t pitch;
    uint64_t backbuffer_bytes;
    uint64_t background_cache_bytes;
    uint64_t blur_capacity_bytes;
    uint8_t red_mask_shift;
    uint8_t green_mask_shift;
    uint8_t blue_mask_shift;
    bool initialized;
    bool frame_active;
    bool background_cache_valid;
};

static struct kos_framebuffer framebuffer;
static volatile uint32_t *framebuffer_row(uint64_t row);

#define FRAMEBUFFER_CACHE_MAX_BYTES (64ull * 1024ull * 1024ull)
#define FRAMEBUFFER_BLUR_MAX_BYTES (16ull * 1024ull * 1024ull)

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
    if (preserve_contents) {
        for (uint64_t row = 0; row < framebuffer.height; ++row) {
            volatile uint32_t *source = (volatile uint32_t *)
                (framebuffer.address + row * framebuffer.pitch);
            uint32_t *destination = (uint32_t *)((uint8_t *)framebuffer.backbuffer
                + row * framebuffer.pitch);
            for (uint64_t column = 0;
                 column < framebuffer.pitch / sizeof(uint32_t); ++column) {
                destination[column] = source[column];
            }
        }
    }
    framebuffer.frame_active = true;
    return true;
}

void framebuffer_frame_present(void) {
    if (!framebuffer.initialized || !framebuffer.frame_active
        || framebuffer.backbuffer == 0) return;
    for (uint64_t row = 0; row < framebuffer.height; ++row) {
        volatile uint32_t *destination = (volatile uint32_t *)
            (framebuffer.address + row * framebuffer.pitch);
        const uint32_t *source = (const uint32_t *)((const uint8_t *)framebuffer.backbuffer
            + row * framebuffer.pitch);
        for (uint64_t column = 0; column < framebuffer.pitch / sizeof(uint32_t); ++column) {
            destination[column] = source[column];
        }
    }
    framebuffer.frame_active = false;
}

void framebuffer_frame_abort(void) {
    framebuffer.frame_active = false;
}

static volatile uint32_t *framebuffer_scanout_row(uint64_t row) {
    return (volatile uint32_t *)(framebuffer.address + row * framebuffer.pitch);
}

bool framebuffer_background_capture(void) {
    if (!framebuffer.initialized
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
        uint32_t *destination = (uint32_t *)((uint8_t *)framebuffer.background_cache
            + row * row_bytes);
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
    uint64_t row_bytes = framebuffer.width * sizeof(uint32_t);
    for (uint64_t row = 0; row < framebuffer.height; ++row) {
        volatile uint32_t *destination = framebuffer_row(row);
        const uint32_t *source = (const uint32_t *)((const uint8_t *)framebuffer.background_cache
            + row * row_bytes);
        for (uint64_t column = 0; column < framebuffer.width; ++column) {
            destination[column] = source[column];
        }
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
        && framebuffer.blur_capacity_bytes >= required_bytes) return true;
    uint32_t *source = kmalloc(required_bytes);
    uint32_t *horizontal = kmalloc(required_bytes);
    if (source == 0 || horizontal == 0) {
        if (source != 0) (void)kfree(source);
        if (horizontal != 0) (void)kfree(horizontal);
        return false;
    }
    if (framebuffer.blur_source != 0) (void)kfree(framebuffer.blur_source);
    if (framebuffer.blur_horizontal != 0) (void)kfree(framebuffer.blur_horizontal);
    framebuffer.blur_source = source;
    framebuffer.blur_horizontal = horizontal;
    framebuffer.blur_capacity_bytes = required_bytes;
    return true;
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
    if (radius > width / 2) radius = width / 2;
    if (radius > height / 2) radius = height / 2;
    if (blur_radius > 30) blur_radius = 30;

    if (width > UINT64_MAX / height) return;
    uint64_t area = width * height;
    if (area > UINT64_MAX / sizeof(uint32_t)) return;
    uint64_t buffer_bytes = area * sizeof(uint32_t);
    bool blurred = blur_radius != 0 && ensure_blur_buffers(buffer_bytes);
    uint32_t *source = blurred ? framebuffer.blur_source : 0;
    uint32_t *horizontal = blurred ? framebuffer.blur_horizontal : 0;
    if (blurred) {
        for (uint64_t row = 0; row < height; ++row) {
            volatile uint32_t *pixels = framebuffer_row(y + row) + x;
            for (uint64_t column = 0; column < width; ++column) {
                source[row * width + column] = pixels[column];
            }
        }
        blur_snapshot(source, horizontal, width, height, blur_radius);
    }

    for (uint64_t column = 0; column < width; ++column) {
        uint32_t red_sum = 0, green_sum = 0, blue_sum = 0, count = 0;
        uint64_t bottom = blur_radius < height ? blur_radius : height - 1;
        if (horizontal != 0) {
            for (uint64_t row = 0; row <= bottom; ++row) {
                uint32_t pixel = horizontal[row * width + column];
                red_sum += pixel_channel(pixel, framebuffer.red_mask_shift);
                green_sum += pixel_channel(pixel, framebuffer.green_mask_shift);
                blue_sum += pixel_channel(pixel, framebuffer.blue_mask_shift);
                ++count;
            }
        }
        for (uint64_t row = 0; row < height; ++row) {
            uint64_t inset = rounded_row_inset(radius, row, height);
            if (column >= inset && column < width - inset) {
                volatile uint32_t *destination = framebuffer_row(y + row) + x + column;
                if (horizontal != 0) {
                    uint32_t blurred = rgb_pixel(
                        (uint8_t)channel_sum_value(red_sum, count),
                        (uint8_t)channel_sum_value(green_sum, count),
                        (uint8_t)channel_sum_value(blue_sum, count));
                    *destination = blend_pixel(blurred, red, green, blue, alpha);
                }
                else {
                    *destination = blend_pixel(*destination, red, green, blue, alpha);
                }
            }
            if (horizontal != 0 && row + 1 < height) {
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
    }

    if (border_alpha != 0 && width > 1 && height > 1) {
        uint64_t inner_width = width - 2;
        uint64_t inner_height = height - 2;
        uint64_t inner_radius = radius == 0 ? 0 : radius - 1;
        if (inner_radius > inner_width / 2) inner_radius = inner_width / 2;
        if (inner_radius > inner_height / 2) inner_radius = inner_height / 2;
        for (uint64_t row = 0; row < height; ++row) {
            uint64_t outer_inset = rounded_row_inset(radius, row, height);
            uint64_t inner_inset = row == 0 || row + 1 == height ? 0
                : rounded_row_inset(inner_radius, row - 1, inner_height);
            for (uint64_t column = outer_inset; column < width - outer_inset; ++column) {
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

    for (uint64_t row = y; row < end_y; ++row) {
        volatile uint32_t *destination = framebuffer_row(row) + x;
        for (uint64_t column = x; column < end_x; ++column) {
            *destination = color;
            ++destination;
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
