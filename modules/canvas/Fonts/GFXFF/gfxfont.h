// Adopted by Bodmer to support TFT_eSPI library.

// Font structures for newer Adafruit_GFX (1.1 and later).
// Example fonts are included in 'Fonts' directory.
// To use a font in your Arduino sketch, #include the corresponding .h
// file and pass address of GFXfont struct to setFont().  Pass NULL to
// revert to 'classic' fixed-space bitmap font.

#ifndef _GFXFONT_H_
#define _GFXFONT_H_

typedef struct
{                          // Data stored PER GLYPH
  uint32_t bitmapOffset;   // Pointer into GFXfont->bitmap
  uint8_t width, height;   // Bitmap dimensions in pixels
  uint8_t xAdvance;        // Distance to advance cursor (x axis)
  int8_t xOffset, yOffset; // Dist from cursor pos to UL corner
} GFXglyph;

typedef struct
{                       // Data stored for FONT AS A WHOLE:
  uint8_t *bitmap;      // Glyph bitmaps, concatenated
  GFXglyph *glyph;      // Glyph array
  uint16_t first, last; // ASCII extents
  uint8_t yAdvance;     // Newline distance (y axis)
} GFXfont;

#ifndef PROGMEM
#define PROGMEM
#endif

#define pgm_read_byte(addr) (*(const unsigned char *)(addr))
#define pgm_read_word(addr) (*(const unsigned short *)(addr))
#define pgm_read_dword(addr) (*(const unsigned long *)(addr))

extern const GFXfont Dogica8px;
extern const GFXfont DogicaBold8px;


#endif // _GFXFONT_H_
