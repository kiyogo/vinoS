#ifndef KOS_FRAMEBUFFER_H
#define KOS_FRAMEBUFFER_H

#include <stdbool.h>
#include <stdint.h>

#include <limine.h>

bool framebuffer_initialize(const struct limine_framebuffer *framebuffer);
bool framebuffer_is_initialized(void);
bool framebuffer_enable_backbuffer(void);
bool framebuffer_backbuffer_available(void);
bool framebuffer_frame_begin(bool preserve_contents);
void framebuffer_frame_present(void);
void framebuffer_frame_abort(void);
void framebuffer_cursor_set(bool visible, int32_t x, int32_t y);
void framebuffer_cursor_suspend(void);
void framebuffer_cursor_resume(void);
bool framebuffer_background_capture(void);
bool framebuffer_background_restore(void);
bool framebuffer_background_cache_available(void);
uint32_t framebuffer_make_color(uint8_t red, uint8_t green, uint8_t blue);
uint64_t framebuffer_width(void);
uint64_t framebuffer_height(void);
uint64_t framebuffer_pitch(void);
uint64_t framebuffer_virtual_address(void);
void framebuffer_fill(uint32_t color);
void framebuffer_fill_rect(uint64_t x, uint64_t y, uint64_t width, uint64_t height, uint32_t color);
void framebuffer_blit_rgb24_row(uint64_t x, uint64_t y, uint64_t destination_width,
                                uint32_t source_width, const uint8_t *pixels);
void framebuffer_glass_rect(uint64_t x, uint64_t y, uint64_t width, uint64_t height,
                            uint64_t radius, uint64_t blur_radius,
                            uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha,
                            uint8_t border_red, uint8_t border_green, uint8_t border_blue,
                            uint8_t border_alpha);
void framebuffer_scroll_up(uint64_t pixel_rows, uint32_t fill_color);

#endif
