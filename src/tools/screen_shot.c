#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <strings.h>
#include <string.h>
#include <ctype.h>
#include <sys/time.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <pthread.h>

#define PNG_DEBUG 3
#include <png.h>

#ifdef WINDOWS
#include <windows.h>
#else
#include <termios.h>
#endif

#include "m65common.h"
#include "logging.h"

#define SCREEN_POSITION ((800 - 720) / 2)

#ifdef WINDOWS
#define bzero(b, len) (memset((b), '\0', (len)), (void)0)
#define bcopy(b1, b2, len) (memmove((b2), (b1), (len)), (void)0)
#endif

unsigned char bitmap_multi_colour;
unsigned int current_physical_raster;
unsigned int next_raster_interrupt;
int raster_interrupt_enabled;
unsigned int screen_address = 0;
unsigned int charset_address;
unsigned int screen_line_step = 0;
unsigned int colour_address;
unsigned int screen_width = 0;
unsigned int upper_case;
unsigned int screen_rows;
unsigned int sixteenbit_mode;
unsigned int screen_size;
unsigned int charset_size;
unsigned int extended_background_mode;
unsigned int multicolour_mode;
int bitmap_mode;

int border_colour;
int background_colour;

unsigned int y_scale;
unsigned int h640;
unsigned int v400;
unsigned int viciii_attribs;
unsigned int chargen_x;
unsigned int chargen_y;

unsigned int top_border_y;
unsigned int bottom_border_y;
unsigned int side_border_width;
unsigned int left_border;
unsigned int right_border;
unsigned int x_scale_120;
float x_step;

unsigned char vic_regs[0x700]; // we fetch two palettes
#define MAX_SCREEN_SIZE (128 * 1024)
unsigned char screen_data[MAX_SCREEN_SIZE];
unsigned char colour_data[MAX_SCREEN_SIZE];
unsigned char char_data[8192 * 8];

unsigned char mega65_rgb(int colour, int rgb, unsigned char alt)
{
  return ((vic_regs[(alt ? 0x400 : 0x100) + (0x100 * rgb) + colour] & 0xf) << 4)
       + ((vic_regs[(alt ? 0x400 : 0x100) + (0x100 * rgb) + colour] & 0xf0) >> 4);
}

png_structp png_ptr = NULL;
png_bytep png_rows[576];
int is_pal_mode = 0;

int min_y = 0;
int max_y = 999;

int set_pixel(int x, int y, int r, int g, int b)
{
  if (y < min_y || y > max_y)
    return 0;
  if (y < 0 || y > (is_pal_mode ? 575 : 479)) {
    log_debug("set_pixel: y=%d out of bounds", y);
    return 1;
  }
  if (x < 0 || x > 719) {
    log_debug("set_pixel: x=%d out of bounds", x);
    return 1;
  }

  //  log_debug("Setting pixel at %d,%d to #%02x%02x%02x",x,y,b,g,r);
  ((unsigned char *)png_rows[y])[x * 3 + 0] = r;
  ((unsigned char *)png_rows[y])[x * 3 + 1] = g;
  ((unsigned char *)png_rows[y])[x * 3 + 2] = b;

  return 0;
}

typedef struct {
  char mask;       /* char data will be bitwise AND with this */
  char lead;       /* start bytes of current char in utf-8 encoded character */
  uint32_t beg;    /* beginning of codepoint range */
  uint32_t end;    /* end of codepoint range */
  int bits_stored; /* the number of bits from the codepoint that fits in char */
} utf_t;

utf_t *utf[] = {
  /*             mask        lead        beg      end       bits */
  [0] = &(utf_t) { 0b00111111, 0b10000000, 0, 0, 6 },
  [1] = &(utf_t) { 0b01111111, 0b00000000, 0000, 0177, 7 },
  [2] = &(utf_t) { 0b00011111, 0b11000000, 0200, 03777, 5 },
  [3] = &(utf_t) { 0b00001111, 0b11100000, 04000, 0177777, 4 },
  [4] = &(utf_t) { 0b00000111, 0b11110000, 0200000, 04177777, 3 },
  &(utf_t) { 0 },
};

// UTF-8 from https://rosettacode.org/wiki/UTF-8_encode_and_decode#C

/* All lengths are in bytes */
int codepoint_len(const uint32_t cp); /* len of associated utf-8 char */
int utf8_len(const char ch);          /* len of utf-8 encoded char */

char *to_utf8(const uint32_t cp);
uint32_t to_cp(const char chr[4]);

int codepoint_len(const uint32_t cp)
{
  int len = 0;
  for (utf_t **u = utf; *u; ++u) {
    if ((cp >= (*u)->beg) && (cp <= (*u)->end)) {
      break;
    }
    ++len;
  }
  if (len > 4) /* Out of bounds */
    exit(1);

  return len;
}

int utf8_len(const char ch)
{
  int len = 0;
  for (utf_t **u = utf; *u; ++u) {
    if ((ch & ~(*u)->mask) == (*u)->lead) {
      break;
    }
    ++len;
  }
  if (len > 4) { /* Malformed leading byte */
    exit(1);
  }
  return len;
}

char *to_utf8(const uint32_t cp)
{
  static char ret[5];
  const int bytes = codepoint_len(cp);

  int shift = utf[0]->bits_stored * (bytes - 1);
  ret[0] = (cp >> shift & utf[bytes]->mask) | utf[bytes]->lead;
  shift -= utf[0]->bits_stored;
  for (int i = 1; i < bytes; ++i) {
    ret[i] = (cp >> shift & utf[0]->mask) | utf[0]->lead;
    shift -= utf[0]->bits_stored;
  }
  ret[bytes] = '\0';
  return ret;
}

void print_screencode(unsigned char c, int upper_case)
{
  // A nice reference for these mappings can be found here:
  // https://style64.org/petscii/
  // clang-format off
  static int map_screencode_to_utf8[][2] = {
    { 0x40, 0x2501 }, // box drawings heavy horizontal
    { 0x43, 0x2501 }, // box drawings heavy horizontal
    { 0x60, 0xa0 },   // no-break space?
    { 0x61, 0x258c }, // left half block
    { 0x62, 0x2584 }, // lower half block
    { 0x63, 0x2594 }, // upper one eigth block
    { 0x64, 0x2581 }, // lower one eigth block
    { 0x65, 0x258e }, // left one quarter block
    { 0x66, 0x2592 }, // medium shade
    { 0x67, 0x258a }, // left three quarter block (but it really should be right one quarter block)
    { 0x68, 0x25db }, // no equivalant to lower half medium shade, so got an approximation
    { 0x69, 0x25e4 }, // black upper left triangle
    { 0x6a, 0x258a }, // left three quarter block (but it really should be right one quarter block)
    { 0x6b, 0x2523 }, // box drawings heavy vertical and right
    { 0x6c, 0x2597 }, // quadrant lower right
    { 0x6d, 0x2517 }, // box drawings heavy up and right
    { 0x6e, 0x2513 }, // box drawings heavy down and left
    { 0x6f, 0x2582 }, // lower one quarter block
    { -1, -1 } };
  // clang-format on
  int rev = 0;
  if (c & 0x80) {
    rev = 1;
    c &= 0x7f;
    // Now swap foreground/background
    printf("%c[7m", 27);
  }
  if (c >= '0' && c <= '9')
    printf("%c", c);
  else if (c >= 0x00 && c <= 0x1f) {
    if (upper_case)
      printf("%c", c + 0x40);
    else
      printf("%c", c + 0x60);
  }
  else if (c >= 0x20 && c < 0x40)
    printf("%c", c);
  else if ((c >= 0x40 && c <= 0x5f) && (!upper_case))
    printf("%c", c);
  else {
    int k = 0;
    while (map_screencode_to_utf8[k][0] != -1) {
      if (c == map_screencode_to_utf8[k][0]) {
        printf("%s", to_utf8(map_screencode_to_utf8[k][1]));
        break;
      }
      k++;
    }
    if (map_screencode_to_utf8[k][0] == -1) {
      printf("?");
    }
  }

  if (rev) {
    // Reverse off again
    printf("%c[0m", 27);
  }
}

int do_screen_shot_ascii(void)
{
  //  dump_bytes(0,"screen data",screen_data,screen_size);

  // printf("Got screen line @ $%x. %d to go.\n",screen_address,screen_rows_remaining);

#ifndef WINDOWS
  // Display a thin border
  printf("%c[48;2;%d;%d;%dm", 27, mega65_rgb(border_colour, 0, 0), mega65_rgb(border_colour, 1, 0),
      mega65_rgb(border_colour, 2, 0));
  for (int x = 0; x < (1 + screen_width + 1); x++)
    printf(" ");
  printf("%c[0m\n", 27);

  for (int y = 0; y < screen_rows; y++) {
    //    dump_bytes(0,"row data",&screen_data[y*screen_line_step],screen_width*(1+sixteenbit_mode));

    printf("%c[48;2;%d;%d;%dm ", 27, mega65_rgb(border_colour, 0, 0), mega65_rgb(border_colour, 1, 0),
        mega65_rgb(border_colour, 2, 0));

    for (int x = 0; x < screen_width; x++) {

      int char_background_colour;
      int char_id = 0;
      int char_value = screen_data[y * screen_line_step + x * (1 + sixteenbit_mode)];
      if (sixteenbit_mode)
        char_value |= (screen_data[y * screen_line_step + x * (1 + sixteenbit_mode) + 1] << 8);
      int colour_value = colour_data[y * screen_line_step + x * (1 + sixteenbit_mode)];
      if (sixteenbit_mode)
        colour_value |= (colour_data[y * screen_line_step + x * (1 + sixteenbit_mode) + 1] << 8);
      if (extended_background_mode) {
        char_id = char_value &= 0x3f;
        char_background_colour = vic_regs[0x21 + ((char_value >> 6) & 3)];
      }
      else {
        char_id = char_value & 0x1fff;
        char_background_colour = background_colour;
      }
      int glyph_width_deduct = char_value >> 13;

      // Set foreground and background colours
      int foreground_colour = colour_value & 0xff;
      //      int glyph_flip_vertical=colour_value&0x8000;
      //      int glyph_flip_horizontal=colour_value&0x4000;
      //      int glyph_with_alpha=colour_value&0x2000;
      //      int glyph_goto=colour_value&0x1000;
      int glyph_full_colour = 0;
      //      int glyph_blink=0;
      //      int glyph_underline=0;
      int glyph_bold = 0;
      int glyph_reverse = 0;
      if (viciii_attribs && (!multicolour_mode)) {
        //	glyph_blink=colour_value&0x0010;
        glyph_reverse = colour_value & 0x0020;
        glyph_bold = colour_value & 0x0040;
        //	glyph_underline=colour_value&0x0080;
        if (glyph_bold && !glyph_reverse)
          foreground_colour |= 0x10;
      }
      unsigned char glyph_altpalette = glyph_bold && glyph_reverse;
      if (vic_regs[0x54] & 2)
        if (char_id < 0x100)
          glyph_full_colour = 1;
      if (vic_regs[0x54] & 4)
        if (char_id > 0x0FF)
          glyph_full_colour = 1;
      int glyph_4bit = colour_value & 0x0800;
      if (glyph_4bit)
        glyph_full_colour = 1;
      if (colour_value & 0x0400)
        glyph_width_deduct += 8;

      int fg = foreground_colour;
      int bg = char_background_colour;
      if (glyph_reverse && !glyph_bold) {
        bg = foreground_colour;
        fg = char_background_colour;
      }
      printf("%c[48;2;%d;%d;%dm%c[38;2;%d;%d;%dm", 27, mega65_rgb(bg, 0, glyph_altpalette),
          mega65_rgb(bg, 1, glyph_altpalette), mega65_rgb(bg, 2, glyph_altpalette), 27, mega65_rgb(fg, 0, glyph_altpalette),
          mega65_rgb(fg, 1, glyph_altpalette), mega65_rgb(fg, 2, glyph_altpalette));

      // Xterm can't display arbitrary graphics, so just mark full-colour chars
      if (glyph_full_colour || glyph_4bit) {
        printf("?");
      }
      else
        print_screencode(char_id & 0xff, upper_case);
    }

    printf("%c[48;2;%d;%d;%dm ", 27, mega65_rgb(border_colour, 0, 0), mega65_rgb(border_colour, 1, 0),
        mega65_rgb(border_colour, 2, 0));

    // Set foreground and background colours back to normal at end of each line, before newline
    printf("%c[0m\n", 27);
  }
  printf("%c[48;2;%d;%d;%dm", 27, mega65_rgb(border_colour, 0, 0), mega65_rgb(border_colour, 1, 0),
      mega65_rgb(border_colour, 2, 0));
  for (int x = 0; x < (1 + screen_width + 1); x++)
    printf(" ");
  printf("%c[0m", 27);

#endif

  printf("\n");

  return 0;
}

void get_video_state(void)
{

  fetch_ram_invalidate();
  // log_debug("Calling fetch_ram");
  fetch_ram(0xffd3000, 0x0100, vic_regs);
  // log_debug("Got video regs, pal = $%02X", vic_regs[0x70]);
  unsigned char palreg = vic_regs[0x70];
  unsigned char altpalsel = vic_regs[0x70] & 0x3;
  unsigned char btpalsel = (vic_regs[0x70] & 0x30) >> 4;
  unsigned char mapedpal = vic_regs[0x70] >> 6;
  unsigned char mapbtpal = (palreg & 0x3f) | (btpalsel << 6);
  unsigned char mapaltpal = (palreg & 0x3f) | (altpalsel << 6);
  // log_debug("palreg = $%02X, btpalsel = %d(%02X), altpalsel = %d(%02X)", palreg, btpalsel, mapbtpal, altpalsel,
  // mapaltpal);
  if (mapedpal != btpalsel)
    push_ram(0xffd3070, 1, &mapbtpal);
  fetch_ram(0xffd3100, 0x0300, vic_regs + 0x100);
  if (btpalsel != altpalsel) {
    // also fetch ALTernate palette
    push_ram(0xffd3070, 1, &mapaltpal);
    fetch_ram_invalidate();
    fetch_ram(0xffd3100, 0x0300, vic_regs + 0x400);
  }
  else
    // BTPAL == ALTPAL
    memcpy(vic_regs + 0x400, vic_regs + 0x100, 0x300);
  // restore MAPEDPAL if we switched it
  if (mapedpal != btpalsel || mapedpal != altpalsel)
    push_ram(0xffd3070, 1, &palreg);

  screen_address = vic_regs[0x60] + (vic_regs[0x61] << 8) + (vic_regs[0x62] << 16);
  charset_address = vic_regs[0x68] + (vic_regs[0x69] << 8) + (vic_regs[0x6A] << 16);
  if (charset_address == 0x1000)
    charset_address = 0x2D000;
  if (charset_address == 0x9000)
    charset_address = 0x29000;
  if (charset_address == 0x1800)
    charset_address = 0x2D800;
  if (charset_address == 0x9800)
    charset_address = 0x29800;

  is_pal_mode = (vic_regs[0x6f] & 0x80) ^ 0x80;
  screen_line_step = vic_regs[0x58] + (vic_regs[0x59] << 8);
  colour_address = vic_regs[0x64] + (vic_regs[0x65] << 8);
  screen_width = vic_regs[0x5e];
  upper_case = 2 - (vic_regs[0x18] & 2);
  screen_rows = 1 + vic_regs[0x7B];
  sixteenbit_mode = vic_regs[0x54] & 1;
  screen_size = screen_line_step * screen_rows * (1 + sixteenbit_mode);
  charset_size = 2048;
  extended_background_mode = vic_regs[0x11] & 0x40;
  multicolour_mode = vic_regs[0x16] & 0x10;
  bitmap_mode = vic_regs[0x11] & 0x20;

  //  log_debug("bitmap_mode=%d, multicolour_mode=%d, extended_background_mode=%d", bitmap_mode, multicolour_mode,
  //      extended_background_mode);

  border_colour = vic_regs[0x20];
  background_colour = vic_regs[0x21];

  current_physical_raster = vic_regs[0x52] + ((vic_regs[0x53] & 0x3) << 8);
  next_raster_interrupt = vic_regs[0x79] + ((vic_regs[0x7A] & 0x3) << 8);
  if (!(vic_regs[0x53] & 0x80)) {
    // Raster compare is VIC-II raster, not physical, so double it
    current_physical_raster *= 2;
  }
  if (!(vic_regs[0x7A] & 0x80)) {
    // Raster compare is VIC-II raster, not physical, so double it
    next_raster_interrupt *= 2;
  }
  raster_interrupt_enabled = vic_regs[0x1a] & 1;

  y_scale = vic_regs[0x5B];
  h640 = vic_regs[0x31] & 0x80;
  v400 = vic_regs[0x31] & 0x08;
  viciii_attribs = vic_regs[0x31] & 0x20;
  chargen_x = (vic_regs[0x4c] + (vic_regs[0x4d] << 8)) & 0xfff;
  chargen_x -= SCREEN_POSITION; // adjust for pipeline delay
  chargen_y = (vic_regs[0x4e] + (vic_regs[0x4f] << 8)) & 0xfff;

  top_border_y = (vic_regs[0x48] + (vic_regs[0x49] << 8)) & 0xfff;
  bottom_border_y = (vic_regs[0x4A] + (vic_regs[0x4B] << 8)) & 0xfff;
  // side border width is measured in pixelclock ticks, so divide by 3
  side_border_width = ((vic_regs[0x5C] + (vic_regs[0x5D] << 8)) & 0xfff);
  left_border = side_border_width - SCREEN_POSITION; // Adjust for screen position
  right_border = 800 - side_border_width - SCREEN_POSITION;
  x_scale_120 = vic_regs[0x5A];
  // x_scale is actually in 120ths of a pixel.
  // so 120 = 1 pixel wide
  // 60 = 2 pixels wide
  x_step = x_scale_120 / 120.0;
  if (!h640)
    x_step /= 2;
  //  log_debug("x_scale_120=$%02x\n", x_scale_120);

  // Check if we are in 16-bit text mode, without full-colour chars for char IDs > 255
  if (sixteenbit_mode && (!(vic_regs[0x54] & 4))) {
    charset_size = 8192 * 8;
  }

  if (screen_size > MAX_SCREEN_SIZE) {
    log_crit("implausibly large screen size of %d bytes: %d rows, %d columns", screen_size, screen_line_step, screen_rows);
    exit(-1);
  }

  log_debug("screen is at $%07x, width= %d chars, height= %d rows, size=%d bytes", screen_address, screen_width, screen_rows,
      screen_size);
  log_debug("  uppercase=%d, line_step= %d charset_address=$%x", upper_case, screen_line_step, charset_address);

  log_debug("fetching screen data");
  fetch_ram(screen_address, screen_size, screen_data);
  log_debug("fetching colour data");
  fetch_ram(0xff80000 + colour_address, screen_size, colour_data);

  log_debug("fetching charset");
  fetch_ram(charset_address, charset_size, char_data);

  log_debug("fetching done");

  return;
}

void paint_screen_shot(void)
{
  log_debug("Painting rasters %d -- %d", min_y, max_y);

  // Now render the text display
  int y_position = chargen_y;
  for (int cy = 0; cy < screen_rows; cy++) {
    if (y_position >= (is_pal_mode ? 576 : 480))
      break;

    int x_position = chargen_x;

    int xc = 0;

    int is_foreground = 0;
    int transparent_background = 0;

    for (int cx = 0; cx < screen_width; cx++) {

      // printf("Rendering char (%d,%d) at (%d,%d)\n",cx,cy,x_position,y_position);
      //      int char_background_colour;
      int char_id = 0;
      int char_value = screen_data[cy * screen_line_step + cx * (1 + sixteenbit_mode)];
      if (sixteenbit_mode)
        char_value |= (screen_data[cy * screen_line_step + cx * (1 + sixteenbit_mode) + 1] << 8);
      int colour_value = colour_data[cy * screen_line_step + cx * (1 + sixteenbit_mode)];
      if (sixteenbit_mode) {
        colour_value = colour_value << 8;
        colour_value |= (colour_data[cy * screen_line_step + cx * (1 + sixteenbit_mode) + 1]);
      }
      if (extended_background_mode) {
        char_id = char_value &= 0x3f;
        //	char_background_colour=vic_regs[0x21+((char_value>>6)&3)];
      }
      else {
        char_id = char_value & 0x1fff;
        //	char_background_colour=background_colour;
      }
      int glyph_width_deduct = char_value >> 13;

      // Set foreground and background colours
      int foreground_colour = colour_value & 0x0f;
      int glyph_flip_vertical = colour_value & 0x8000;
      int glyph_flip_horizontal = colour_value & 0x4000;
      int glyph_with_alpha = colour_value & 0x2000;
      int glyph_goto = colour_value & 0x1000;
      int glyph_full_colour = 0;
      // int glyph_blink=0;
      int glyph_underline = 0;
      int glyph_bold = 0;
      int glyph_reverse = 0;
      unsigned char glyph_altpalette = 0;
      if (viciii_attribs && (!multicolour_mode)) {
        // glyph_blink=colour_value&0x0010;
        glyph_reverse = colour_value & 0x0020;
        glyph_bold = colour_value & 0x0040;
        glyph_underline = colour_value & 0x0080;
        glyph_altpalette = glyph_bold && glyph_reverse;
        // if (glyph_altpalette) printf("alt %d %d\n", cx, cy);
        if (glyph_bold && !glyph_reverse)
          foreground_colour |= 0x10;
      }
      if (multicolour_mode)
        foreground_colour = colour_value & 0xff;

      if (bitmap_mode) {
        char_value = screen_data[cy * screen_line_step + cx * (1 + sixteenbit_mode)];
        foreground_colour = char_value & 0xf;
        background_colour = char_value >> 4;
        bitmap_multi_colour = colour_data[cy * screen_line_step + cx * (1 + sixteenbit_mode)];
        // log_debug("bitmap fore/background colours are $%x/$%x", foreground_colour, background_colour);
      }

      if (vic_regs[0x54] & 2)
        if (char_id < 0x100)
          glyph_full_colour = 1;
      if (vic_regs[0x54] & 4)
        if (char_id > 0x0FF)
          glyph_full_colour = 1;
      int glyph_4bit = colour_value & 0x0800;
      if (colour_value & 0x0400)
        glyph_width_deduct += 8;

      // Lookup the char data, and work out how many pixels we need to paint
      int glyph_width = 8;
      if (glyph_4bit)
        glyph_width = 16;
      glyph_width -= glyph_width_deduct;

      // For each row of the glyph
      for (int yy = 0; yy < 8; yy++) {
        int glyph_row = yy;
        if (glyph_flip_vertical)
          glyph_row = 7 - glyph_row;

        unsigned char glyph_data[8];

        if (glyph_full_colour) {
          // Get 8 bytes of data
          fetch_ram_cacheable(char_id * 64 + glyph_row * 8, 8, glyph_data);
        }
        else {
          // Use existing char data we have already fetched
          // log_Debug("Chardata for char $%03x = $%02x",char_id,char_data[char_id*8+glyph_row]);
          if (!bitmap_mode) {
            for (int i = 0; i < 8; i++)
              if ((char_data[char_id * 8 + glyph_row] >> i) & 1)
                glyph_data[i] = 0xff;
              else
                glyph_data[i] = 0;
          }
          else {
            int addr = charset_address & 0xfe000;
            addr += cx * 8 + cy * 320 + glyph_row;
            if (h640) {
              addr = charset_address & 0xfc000;
              addr += cx * 8 + cy * 640 + glyph_row;
            }
            unsigned char pixels;
            fetch_ram_cacheable(addr, 1, &pixels);
            // log_debug("reading bitmap data from $%x = $%02x, charset_address=$%x", addr, pixels, charset_address);
            for (int i = 0; i < 8; i++)
              if ((pixels >> i) & 1)
                glyph_data[i] = 0xff;
              else
                glyph_data[i] = 0;
          }
        }

        if (glyph_flip_horizontal) {
          unsigned char b[8];
          for (int i = 0; i < 8; i++)
            b[i] = glyph_data[i];
          for (int i = 0; i < 8; i++)
            glyph_data[i] = b[7 - i];
        }

        if (glyph_reverse && !glyph_bold) {
          for (int i = 0; i < 8; i++)
            glyph_data[i] = 0xff - glyph_data[i];
        }

        // XXX Do blink with PNG animation?

        if (glyph_underline && (yy == 7)) {
          for (int i = 0; i < 8; i++)
            glyph_data[i] = 0xff;
        }

        xc = 0;
        if (glyph_goto) {
          x_position = chargen_x + (char_value & 0x3ff);
          transparent_background = colour_value & 0x8000;
        }
        else
          for (float xx = 0; xx < glyph_width; xx += x_step) {
            int r = mega65_rgb(background_colour, 0, glyph_altpalette);
            int g = mega65_rgb(background_colour, 1, glyph_altpalette);
            int b = mega65_rgb(background_colour, 2, glyph_altpalette);

            is_foreground = 0;

            if (glyph_4bit) {
              // 16-colour 4 bits per pixel
              int c = glyph_data[((int)xx) / 2];
              if (((int)xx) & 1)
                c = c >> 4;
              else
                c = c & 0xf;
              if (glyph_with_alpha) {
                // Alpha blended pixels:
                // Here we blend the foreground and background colours we already know
                // according to the alpha value
                int a = c;
                r = (mega65_rgb(foreground_colour, 0, glyph_altpalette) * a + r * (15 - a)) >> 8;
                g = (mega65_rgb(foreground_colour, 1, glyph_altpalette) * a + g * (15 - a)) >> 8;
                b = (mega65_rgb(foreground_colour, 2, glyph_altpalette) * a + b * (15 - a)) >> 8;
              }
              else {
                r = mega65_rgb(c, 0, glyph_altpalette);
                g = mega65_rgb(c, 1, glyph_altpalette);
                b = mega65_rgb(c, 2, glyph_altpalette);
              }
              if (c)
                is_foreground = 1;
            }
            else if (glyph_full_colour) {
              // 256-colour 8 bits per pixel
              if (glyph_with_alpha) {
                // Alpha blended pixels:
                // Here we blend the foreground and background colours we already know
                // according to the alpha value
                int a = glyph_data[(int)xx];
                r = (mega65_rgb(foreground_colour, 0, glyph_altpalette) * a + r * (255 - a)) >> 8;
                g = (mega65_rgb(foreground_colour, 1, glyph_altpalette) * a + g * (255 - a)) >> 8;
                b = (mega65_rgb(foreground_colour, 2, glyph_altpalette) * a + b * (255 - a)) >> 8;
                if (foreground_colour)
                  is_foreground = 1;
              }
              else {
                r = mega65_rgb(glyph_data[(int)xx], 0, glyph_altpalette);
                g = mega65_rgb(glyph_data[(int)xx], 1, glyph_altpalette);
                b = mega65_rgb(glyph_data[(int)xx], 2, glyph_altpalette);
              }
            }
            else if (multicolour_mode && ((foreground_colour & 8) || bitmap_mode)) {
              // Multi-colour normal char
              int bits = 0;
              if (glyph_data[6 - (((int)xx) & 0x6)])
                bits |= 1;
              if (glyph_data[7 - (((int)xx) & 0x6)])
                bits |= 2;
              int colour;
              if (!bitmap_mode) {
                switch (bits) {
                case 0:
                  colour = vic_regs[0x21];
                  break; // background colour
                case 1:
                  is_foreground = 1;
                  colour = vic_regs[0x22];
                  break; // multi colour 1
                case 2:
                  is_foreground = 1;
                  colour = vic_regs[0x23];
                  break; // multi colour 2
                case 3:
                  is_foreground = 1;
                  colour = foreground_colour & 7;
                  break; // foreground colour
                }
              }
              else {
                switch (bits) {
                case 0:
                  is_foreground = 1;
                  colour = vic_regs[0x21];
                  break;
                case 1:
                  colour = background_colour;
                  break;
                case 2:
                  is_foreground = 1;
                  colour = foreground_colour;
                  break;
                case 3:
                  is_foreground = 1;
                  colour = bitmap_multi_colour & 0xf;
                  break;
                }
              }
              r = mega65_rgb(colour, 0, glyph_altpalette);
              g = mega65_rgb(colour, 1, glyph_altpalette);
              b = mega65_rgb(colour, 2, glyph_altpalette);
            }
            else {
              // Mono normal char
              if (glyph_data[7 - (int)xx]) {
                r = mega65_rgb(foreground_colour, 0, glyph_altpalette);
                g = mega65_rgb(foreground_colour, 1, glyph_altpalette);
                b = mega65_rgb(foreground_colour, 2, glyph_altpalette);
                //	      log_debug("Foreground pixel. colour = $%02x = #%02x%02x%02x",
                //		     foreground_colour,b,g,r);
                is_foreground = 1;
              }
            }

            // Actually draw the pixels
            for (int yc = 0; yc <= y_scale; yc++) {
              if (((y_position + yc) < bottom_border_y) && ((y_position + yc) >= top_border_y)
                  && ((x_position + xc) < right_border) && ((x_position + xc) >= left_border))
                if (is_foreground || (!transparent_background)) {
                  set_pixel(x_position + xc, y_position + yc + yy * (1 + y_scale), r, g, b);
                }
            }

            xc++;
          }
      }

      // Advance for width of the glyph
      //      log_debug("Char was %d pixels wide",xc);
      x_position += xc;
    }
    y_position += 8 * (1 + y_scale);
  }

  return;
}

int do_screen_shot(char *userfilename)
{
  log_note("fetching screenshot");
  log_debug("syncing to monitor");
  monitor_sync();
  log_debug("synced to monitor");

  get_video_state();

  log_debug("got video state");

  log_note("got ASCII screenshot");
  do_screen_shot_ascii();

  FILE *f = NULL;
  char filename[1024];
  if (userfilename != NULL)
    strncpy(filename, userfilename, 1023);
  else
    for (int n = 0; n < 1000000; n++) {
      snprintf(filename, 1024, "mega65-screen-%06d.png", n);
      f = fopen(filename, "rb");
      if (!f)
        break;
    }
  f = fopen(filename, "wb");
  if (!f) {
    log_error("could not open '%s' for writing.", filename);
    return -1;
  }
  log_debug("rendering pixel-exact version to %s...", filename);

  png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  if (!png_ptr) {
    log_error("could not create PNG structure");
    return -1;
  }

  png_infop info_ptr = png_create_info_struct(png_ptr);
  if (!info_ptr) {
    log_error("Could not create PNG info structure");
    return -1;
  }

  png_init_io(png_ptr, f);

  // Set image size based on PAL or NTSC video mode
  png_set_IHDR(png_ptr, info_ptr, 720, is_pal_mode ? 576 : 480, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
      PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);

  png_write_info(png_ptr, info_ptr);

  // Allocate frame buffer for image, and set all pixels to the border colour by default
  log_debug("allocating PNG frame buffer...");
  for (int y = 0; y < (is_pal_mode ? 576 : 480); y++) {
    png_rows[y] = (png_bytep)malloc(3 * 720 * sizeof(png_byte));
    if (!png_rows[y]) {
      perror("malloc()");
      return -1;
    }
    // Set all pixels to border colour
    for (int x = 0; x < 720; x++) {
      ((unsigned char *)png_rows[y])[x * 3 + 0] = mega65_rgb(border_colour, 0, 0);
      ((unsigned char *)png_rows[y])[x * 3 + 1] = mega65_rgb(border_colour, 1, 0);
      ((unsigned char *)png_rows[y])[x * 3 + 2] = mega65_rgb(border_colour, 2, 0);
    }
  }

  log_note("rendering screen...");

  // Start by drawing the non-border area
  for (int y = top_border_y; y < bottom_border_y && (y < (is_pal_mode ? 576 : 480)); y++) {
    for (int x = left_border; x < right_border; x++) {
      ((unsigned char *)png_rows[y])[x * 3 + 0] = mega65_rgb(background_colour, 0, 0);
      ((unsigned char *)png_rows[y])[x * 3 + 1] = mega65_rgb(background_colour, 1, 0);
      ((unsigned char *)png_rows[y])[x * 3 + 2] = mega65_rgb(background_colour, 2, 0);
    }
  }

  /*
    Get list of raster interrupts by allowing CPU to run intermittently with long enough pauses
    so that an interrupt is caused each time.
   */
  //  raster_interrupt_count=0;

  // log_debug("Finding raster splits...");
  // log_debug("next_raster_interrupt=%d", next_raster_interrupt);
  raster_interrupt_enabled = 0;
  if (raster_interrupt_enabled) {
    progress_to_RTI();
    get_video_state();
    // log_debug("Current raster line is $%x, next raster interrupt at $%x", current_physical_raster, next_raster_interrupt);

    // At this point, the screen is (presumably) set up for raster #next_raster_interrupt
    // But we don't yet know which raster we can render from, because CPU was stopped before
    // the interrupt was triggered.
    // So just remember the raster, so we can stop when we hit it again
    unsigned int start_raster = next_raster_interrupt;
    unsigned int last_raster = next_raster_interrupt;
    // Advance to the end of the next interrupt.
    progress_to_RTI();
    get_video_state();
    // So now we can assume that the current video mode applies from rasters
    // #start_raster to #next_raster_interrupt

    while (next_raster_interrupt != start_raster) {
      // log_debug("Current raster line is $%x, next raster interrupt at $%x", current_physical_raster,
      // next_raster_interrupt); log_debug("Rendering from raster %d -- %d", last_raster, next_raster_interrupt);
      if (last_raster < next_raster_interrupt) {
        min_y = last_raster;
        max_y = next_raster_interrupt;
        paint_screen_shot();
      }
      else {
        // Raster wraps around end of frame
        min_y = last_raster;
        max_y = 999;
        paint_screen_shot();
        min_y = 0;
        max_y = next_raster_interrupt;
        paint_screen_shot();
      }
      last_raster = next_raster_interrupt;
      progress_to_RTI();
      get_video_state();
    }
  }
  else {
    //     log_debug("Video mode does not use raster splits. Drawing normally.");
    min_y = 0;
    max_y = is_pal_mode ? 576 : 480;
    paint_screen_shot();
  }

  //  log_debug("Writing out PNG frame buffer...");
  // Write out each row of the PNG
  for (int y = 0; y < (is_pal_mode ? 576 : 480); y++)
    png_write_row(png_ptr, png_rows[y]);

  png_write_end(png_ptr, NULL);

  fclose(f);

  log_note("Wrote screen capture to %s", filename);
  // start_cpu();
  // exit(0);

  return 0;
}
