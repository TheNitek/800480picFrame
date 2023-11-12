#include <Esp.h>
#include <Arduino_GFX_Library.h>
#include <SdFat.h>
#include "sdios.h"
#include <bb_captouch.h>
#include "JPEGDEC.h"

#define BL_PIN 2
#define BUILTIN_SDCARD 10

#define SCREEN_W 800
#define SCREEN_H 480

#define TOUCH_GT911_SCL 20
#define TOUCH_GT911_SDA 19
#define TOUCH_GT911_INT 18
#define TOUCH_GT911_RST 38

#define TOUCH_MAX_X 480
#define TOUCH_MAX_Y 272

// Static instance of the JPEGDEC structure. It requires about
// 17.5K of RAM. You can allocate it dynamically too. Internally it
// does not allocate or free any memory; all memory management decisions
// are left to you
JPEGDEC jpeg;

Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
    40 /* DE */, 41 /* VSYNC */, 39 /* HSYNC */, 42 /* PCLK */,
    45 /* R0 */, 48 /* R1 */, 47 /* R2 */, 21 /* R3 */, 14 /* R4 */,
    5 /* G0 */, 6 /* G1 */, 7 /* G2 */, 15 /* G3 */, 16 /* G4 */, 4 /* G5 */,
    8 /* B0 */, 3 /* B1 */, 46 /* B2 */, 9 /* B3 */, 1 /* B4 */,
		0 /* hsync_polarity */, 8 /* hsync_front_porch */, 4 /* hsync_pulse_width */, 8 /* hsync_back_porch */,
		0 /* vsync_polarity */, 8 /* vsync_front_porch */, 4 /* vsync_pulse_width */, 8 /* vsync_back_porch */,
		1 /* pclk_active_neg */
);

Arduino_RGB_Display *gfx = new Arduino_RGB_Display(SCREEN_W, SCREEN_H, rgbpanel, 0, true);

BBCapTouch bbct;

TOUCHINFO ti;

// Functions to access a file on the SD card
SdFs sd;
FsFile root;
int16_t currentIndex;
uint16_t fileCount;

#define FB_SIZE SCREEN_W * SCREEN_H
uint16_t* fb;
uint8_t fb_count = 0;

#define CACHE_SIZE 1024*1024 * sizeof(uint8_t)
uint8_t* file_cache;

uint32_t timer;
int16_t y_offset;
int16_t lastY;

bool bitmap_to_framebuffer(uint16_t *buffer, uint16_t *from_bitmap, int16_t bitmap_w, int16_t bitmap_h, int16_t x, int16_t y) {
  int16_t max_X = SCREEN_W - 1;
  int16_t max_Y = SCREEN_H - 1;

  // Skip copying if area is out of display region anyway
  if (((x + bitmap_w - 1) < 0) || ((y + bitmap_h - 1) < 0) || (x > max_X) || (y > max_Y)) {
    return false;
  }

  // If image overlaps with screen edges, only copy what's only the screen
  int16_t xskip = 0;
  if ((y + bitmap_h - 1) > max_Y) {
    bitmap_h -= (y + bitmap_h - 1) - max_Y;
  }
  if (y < 0) {
    from_bitmap -= y * bitmap_w;
    bitmap_h += y;
    y = 0;
  }
  if ((x + bitmap_w - 1) > max_X) {
    xskip = (x + bitmap_w - 1) - max_X;
    bitmap_w -= xskip;
  }
  if (x < 0) {
    from_bitmap -= x;
    xskip -= x;
    bitmap_w += x;
    x = 0;
  }

  uint16_t *row = buffer;
  row += y * SCREEN_W;
  row += x;
  if (((SCREEN_W & 1) == 0) && ((xskip & 1) == 0) && ((bitmap_w & 1) == 0)) {
    uint32_t *row2 = (uint32_t *)row;
    uint32_t *from_bitmap2 = (uint32_t *)from_bitmap;
    int16_t framebuffer_w2 = SCREEN_W >> 1;
    int16_t xskip2 = xskip >> 1;
    int16_t w2 = bitmap_w >> 1;

    int16_t j = bitmap_h;
    while (j--) {
      for (int16_t i = 0; i < w2; ++i)
      {
        row2[i] = *from_bitmap2++;
      }
      from_bitmap2 += xskip2;
      row2 += framebuffer_w2;
    }
  } else {
    int16_t j = bitmap_h;
    while (j--) {
      for (int i = 0; i < bitmap_w; ++i) {
        row[i] = *from_bitmap++;
      }
      from_bitmap += xskip;
      row += SCREEN_W;
    }
  }
  return true;
}

int JPEGDraw(JPEGDRAW *pDraw) {
  bitmap_to_framebuffer((uint16_t*)pDraw->pUser, pDraw->pPixels, pDraw->iWidth, pDraw->iHeight, pDraw->x, pDraw->y);
  return 1;
}

void decodeJpeg(uint16_t *target) {
  memset(target, 0, FB_SIZE * sizeof(uint16_t));
  jpeg.openRAM(file_cache, CACHE_SIZE, JPEGDraw);
  if(jpeg.getWidth() < gfx->width() || jpeg.getHeight() < gfx->height()) {
      memset(target, 0, FB_SIZE * sizeof(uint16_t));
  }
  jpeg.setUserPointer(target);
  jpeg.decode((gfx->width()-jpeg.getWidth())/2, (gfx->height()-jpeg.getHeight())/2, 0);
  jpeg.close();
}

void cacheFile(FsFile &file) {
  uint8_t* fCache = file_cache;
  while(file.available()) {
    size_t r = file.read(fCache, CACHE_SIZE);
    fCache += r;
  }
}

void loadImage(int16_t targetIndex, uint16_t* targetFb) {
  if(targetIndex < 0) {
    // Going backwards
    targetIndex += fileCount;
  } else if(targetIndex >= fileCount) {
    // Starting from the beginning again
    targetIndex -= fileCount;
  }

  Serial.print("Loading index "); Serial.println(targetIndex);

  root.rewind();

  uint16_t index = 0;

  FsFile entry;
  char name[100];
  while (entry.openNext(&root)) {
    if (entry.isDirectory()) {
      entry.close();
      continue;
    }
    entry.getName(name, sizeof(name));
    const int len = strlen(name);
    if (len > 3 && strcasecmp(name + len - 3, "JPG") != 0) {
      entry.close();
      continue;
    }
    if(index < targetIndex) {
      index++;
      entry.close();
      continue;
    }
    Serial.print("File: "); Serial.println(name);
    Serial.print("Size: "); Serial.println(entry.size());

    uint32_t start = millis();
    cacheFile(entry);
    Serial.print("Cached: "); Serial.println(millis()-start);

    start = millis();
    decodeJpeg(targetFb);
    Serial.print("Decoded: "); Serial.println(millis()-start);

    entry.close();
    return;
  }

  Serial.print("Could not load index "); Serial.println(targetIndex);
  initSD();
}

void initSD() {
  fileCount = 0;
  currentIndex = 0;

  while(fileCount == 0) {
    while(!sd.begin(SdSpiConfig(BUILTIN_SDCARD, DEDICATED_SPI))) {
      sd.initErrorPrint(&Serial);
      gfx->setCursor(0, 0);
      sd.initErrorPrint(gfx);
      delay(1000);
    }
  
    while(!root.open("/")) {
      Serial.println("Could not open /");
      gfx->setCursor(0, 0);
      gfx->println("Could not open /");
      delay(1000);
    }

    FsFile file;
    char filenameChar[13];

    char name[100];
    while (file.openNext(&root))
    {
      file.getName(name, sizeof(name));
      const int len = strlen(name);
      if (len > 3 && strcasecmp(name + len - 3, "JPG") == 0) {
        fileCount++;
      }
      file.close();
    }

    if(fileCount == 0) {
      Serial.println("No .JPG files found");
      gfx->setCursor(0, 0);
      gfx->println("No .JPG files found");
      root.close();
      sd.end();
      delay(2000);
    } else {
      Serial.print("JPGs found: "); Serial.println(fileCount);
    }
  }
}

void setup() {
  Serial.begin(115200);

	pinMode(BL_PIN, OUTPUT);
  pinMode(BUILTIN_SDCARD, OUTPUT);
  pinMode(BUILTIN_SDCARD, HIGH);
	pinMode(0, INPUT);

	Serial.print("Cores:		"); Serial.println(ESP.getChipCores());
	Serial.print("Chip:		"); Serial.println(ESP.getChipModel());
	Serial.print("ChipRev:	"); Serial.println(ESP.getChipRevision());
	Serial.print("CpuFreq:	"); Serial.println(ESP.getCpuFreqMHz());
	Serial.print("FlashSize:	"); Serial.println(ESP.getFlashChipSize());
	Serial.print("FlashSpd:	"); Serial.println(ESP.getFlashChipSpeed());
	Serial.print("FreeHeap:	"); Serial.println(ESP.getFreeHeap());
	Serial.print("FreePSRam:	"); Serial.println(ESP.getFreePsram());
  Serial.print("FrameBuffer Size:   "); Serial.println(FB_SIZE);

  while(!psramInit()){
    Serial.println("PSRAM not available");
    delay(1000);
  }

  // alloc 3*FB_SIZE for scrolling left+right
  fb = (uint16_t*) ps_malloc(3*FB_SIZE * sizeof(uint16_t));
  file_cache = (uint8_t*) ps_malloc(CACHE_SIZE);

  bbct.init(TOUCH_GT911_SDA, TOUCH_GT911_SCL, TOUCH_GT911_RST, TOUCH_GT911_INT);

	gfx->begin();
  gfx->fillScreen(BLACK);
  gfx->setTextSize(4);

  ledcSetup(0, 2000, 8);
  ledcAttachPin(BL_PIN, 0);
  ledcWrite(0, 64);

  initSD();

  // Draw first image ASAP
  loadImage(0, fb+FB_SIZE);
  gfx->draw16bitRGBBitmap(0, 0, fb+FB_SIZE, gfx->width(), gfx->height());

  // Fill cache for previous/next image
  loadImage(-1, fb);
  loadImage(1, fb+2*FB_SIZE);

  Serial.print("FAT type:   "); sd.printFatType(&Serial);
	Serial.print("\nFreePSRam after setup:	"); Serial.println(ESP.getFreePsram());

  timer = millis();
  y_offset = 0;
  lastY = -1;
}

void loop() {

  if (bbct.getSamples(&ti)) {
    for (int i=0; i<ti.count; i++){
      Serial.print("Touch ");Serial.print(i+1);Serial.print(": ");
      Serial.print("  x: ");Serial.print(ti.x[i]);
      Serial.print("  y: ");Serial.println(ti.y[i]);
      if(lastY >= 0) {
        y_offset -= (ti.y[i] - lastY) * (gfx->height() / (float) TOUCH_MAX_Y);
        y_offset = y_offset % gfx->height();
        Serial.print("Offset: "); Serial.println(y_offset);
        gfx->draw16bitRGBBitmap(0, 0, fb+FB_SIZE+y_offset*gfx->width(), gfx->width(), gfx->height());
      }
      lastY = ti.y[i];
      timer = millis();
    }
  } else {
    // Touch ended so scroll to the nearest image
    if(y_offset != 0) {
      if(y_offset < -gfx->height()/2) {
          Serial.println("Going Back");
          for(int16_t i = y_offset - (y_offset % 16); i >= -gfx->height(); i-=16) {
            gfx->draw16bitRGBBitmap(0, 0, fb+FB_SIZE+i*gfx->width(), gfx->width(), gfx->height());
          }
          memcpy(fb+2*FB_SIZE, fb+FB_SIZE, FB_SIZE * sizeof(uint16_t));
          memcpy(fb+FB_SIZE, fb, FB_SIZE * sizeof(uint16_t));
          currentIndex--;
          loadImage(currentIndex-1, fb);
      } else if(y_offset < 0) {
          Serial.println("Stay forward");
          for(int16_t i = y_offset + (16 - (y_offset % 16)); i <= 0; i+=16) {
            gfx->draw16bitRGBBitmap(0, 0, fb+FB_SIZE+i*gfx->width(), gfx->width(), gfx->height());
          }

      } else if (y_offset < gfx->height()/2) {
          Serial.println("Stay back");
          for(int16_t i = y_offset - (y_offset % 16); i >= 0; i-=16) {
            gfx->draw16bitRGBBitmap(0, 0, fb+FB_SIZE+i*gfx->width(), gfx->width(), gfx->height());
          }
      } else {
          Serial.println("Going forward");
          for(int16_t i = y_offset + (16 - (y_offset % 16)); i <= gfx->height(); i+=16) {
            gfx->draw16bitRGBBitmap(0, 0, fb+FB_SIZE+i*gfx->width(), gfx->width(), gfx->height());
          }
          memcpy(fb, fb+FB_SIZE, FB_SIZE * sizeof(uint16_t));
          memcpy(fb+FB_SIZE, fb+2*FB_SIZE, FB_SIZE * sizeof(uint16_t));
          currentIndex++;
          loadImage(currentIndex+1, fb+2*FB_SIZE);
      }
      timer = millis();
    }
    y_offset = 0;
    lastY = -1;
  }

  // Next image after 10 seconds or if button is pressed
  if((millis() - timer > 10*1000) || (digitalRead(0) == LOW)) {
    for(int16_t i = 0; i <= gfx->height(); i+=16) {
      gfx->draw16bitRGBBitmap(0, 0, fb+FB_SIZE+i*gfx->width(), gfx->width(), gfx->height());
    }
    memcpy(fb, fb+FB_SIZE, FB_SIZE * sizeof(uint16_t));
    memcpy(fb+FB_SIZE, fb+2*FB_SIZE, FB_SIZE * sizeof(uint16_t));
    currentIndex++;
    loadImage(currentIndex+1, fb+2*FB_SIZE);
    timer = millis();
  } else {
    delay(50);
  }

  if(currentIndex < 0) {
    // Going backwards
    currentIndex += fileCount;
  } else if(currentIndex >= fileCount) {
    // Starting from the beginning again
    currentIndex -= fileCount;
  }

}