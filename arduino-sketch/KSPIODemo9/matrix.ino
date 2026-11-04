/**
 * @file matrix.ino
 * @brief Drawing on the 12x8 LED matrix of the UNO R4 WiFi.
 *
 * Everything is drawn into the 8x12 byte buffer @c fb and then handed to the matrix library, which refreshes
 * the LEDs from a timer interrupt, so matrixUpdate() costs well under a millisecond and never blocks loop()
 * (a blocking call would trip the watchdog). The 12 column bytes of the last frame are also sent in the
 * DebugPacket, so the plugin writes what the matrix showed to KSP.log and tools/matrix_preview.py can print it.
 *
 * States: splash "KSP" at power on, a scanning dot while waiting for KSP, a blinking X after the link is lost,
 * and, while connected, one of the screens SCR_* (chosen automatically, by the screen button, or by a
 * DisplayCommand from the test tool).
 */

#include "Arduino_LED_Matrix.h"

#define MX_W 12                 ///< Matrix width in pixels
#define MX_H 8                  ///< Matrix height in pixels
#define SPLASHMS 2500           ///< How long the "KSP" splash is shown after power on
#define MX_PERIOD_MS 40         ///< Minimum time between two redraws
#define SCREEN_AUTO SCR_COUNT   ///< manualScreen value meaning "pick the screen from the flight phase"

/// Screens shown while connected
enum Screen { SCR_FUEL, SCR_STAGE, SCR_HORIZON, SCR_VSI, SCR_SAS, SCR_COUNT };

ArduinoLEDMatrix matrix;        ///< The matrix driver
uint8_t fb[MX_H][MX_W];         ///< Frame buffer: fb[row][column], 1 = lit

uint8_t manualScreen = SCREEN_AUTO;  ///< Screen chosen with the button, or SCREEN_AUTO
uint8_t lastStageSeen = 255;         ///< Stage number at the previous redraw, to flash on staging
unsigned long stageFlashUntil = 0;   ///< The stage number is shown until this time after a staging
int screenIdle = HIGH;               ///< Idle level of the screen button, learned while not connected
bool screenPressedBefore = false;    ///< Button state at the previous redraw
unsigned long screenLockUntil = 0;   ///< Presses before this time are ignored (bounce)
unsigned long mxLast = 0;            ///< Time of the previous redraw

/// 3x5 glyphs, one byte per row, bit 2 = left column. Digits 0-9, then K, S, P.
static const uint8_t GLYPH[13][5] = {
  {7,5,5,5,7}, {2,6,2,2,7}, {7,1,7,4,7}, {7,1,7,1,7}, {5,5,7,1,1},
  {7,4,7,1,7}, {7,4,7,5,7}, {7,1,1,2,2}, {7,5,7,5,7}, {7,5,7,1,7},
  {5,5,6,5,5}, {3,4,2,1,6}, {6,5,6,4,4}
};
#define GLYPH_K 10
#define GLYPH_S 11
#define GLYPH_P 12

/// 7x7 icons, one byte per row, bit 6 = left column. Index = autopilot mode (Stability, Prograde, Retrograde,
/// Normal, Antinormal, RadialIn, RadialOut, Target, AntiTarget, Maneuver).
static const uint8_t ICON[10][7] = {
  {0x7F,0x41,0x41,0x41,0x41,0x41,0x7F},   // stability assist: square
  {0x1C,0x22,0x41,0x49,0x41,0x22,0x1C},   // prograde: circle with a dot
  {0x1C,0x22,0x41,0x7F,0x41,0x22,0x1C},   // retrograde: circle with a bar
  {0x08,0x14,0x14,0x22,0x22,0x41,0x7F},   // normal: triangle up
  {0x7F,0x41,0x22,0x22,0x14,0x14,0x08},   // anti-normal: triangle down
  {0x08,0x08,0x08,0x08,0x49,0x2A,0x1C},   // radial in: arrow down
  {0x1C,0x2A,0x49,0x08,0x08,0x08,0x08},   // radial out: arrow up
  {0x08,0x08,0x08,0x7F,0x08,0x08,0x08},   // target: cross
  {0x41,0x22,0x14,0x08,0x14,0x22,0x41},   // anti-target: X
  {0x08,0x14,0x22,0x41,0x22,0x14,0x08}    // maneuver: diamond
};

/// @brief Lights or clears one pixel; coordinates outside the matrix are ignored.
static void setPxV(int x, int y, uint8_t v) {
  if (x >= 0 && x < MX_W && y >= 0 && y < MX_H) fb[y][x] = v;
}

/// @brief Lights one pixel.
static void setPx(int x, int y) {
  setPxV(x, y, 1);
}

/// @brief Draws a 3x5 glyph with its top-left corner at (x, y).
static void drawGlyph(uint8_t g, int x, int y) {
  for (int r = 0; r < 5; r++)
    for (int c = 0; c < 3; c++)
      if (GLYPH[g][r] & (4 >> c)) setPx(x + c, y + r);
}

/// @brief Draws a 7x7 icon with its top-left corner at (x, y).
static void drawIcon(const uint8_t *rows, int x, int y) {
  for (int r = 0; r < 7; r++)
    for (int c = 0; c < 7; c++)
      if (rows[r] & (0x40 >> c)) setPx(x + c, y + r);
}

/// @brief Bar height 0..8 for a resource, or 255 if the vessel has no such tank.
static uint8_t barLevel(float current, float total) {
  if (total <= 0.0f) return 255;
  int n = (int)(current / total * 8.0f + 0.5f);
  if (current > 0.0f && n == 0) n = 1;
  return n > 8 ? 8 : (n < 0 ? 0 : n);
}

/// @brief Draws a number 0-99 with 3x5 digits, horizontally centered, top row at y.
static void drawNumber(int value, int y) {
  if (value > 99) value = 99;
  if (value < 10) {
    drawGlyph(value, 4, y);                 // one digit at columns 4-6
  } else {
    drawGlyph(value / 10, 2, y);            // two digits at columns 2-4 and 6-8
    drawGlyph(value % 10, 6, y);
  }
}

/// @brief Splash: the letters K S P and a bar that fills while the splash lasts.
static void drawSplash(unsigned long ms) {
  drawGlyph(GLYPH_K, 1, 1);
  drawGlyph(GLYPH_S, 5, 1);
  drawGlyph(GLYPH_P, 9, 1);
  int fill = (int)(ms * MX_W / SPLASHMS);
  for (int x = 0; x < fill && x < MX_W; x++) setPx(x, 7);
}

/// @brief Waiting for KSP: a dot with a short tail bouncing along the middle.
static void drawWaiting(unsigned long ms) {
  int p = (int)((ms / 90) % 22);
  int x = p < MX_W ? p : 21 - p;
  setPx(x, 3);
  setPx(x, 4);
  setPx(p < MX_W ? x - 1 : x + 1, 3);
}

/// @brief Link lost: a blinking X.
static void drawLost(unsigned long ms) {
  if ((ms / 400) & 1) drawIcon(ICON[8], 2, 0);
}

/// @brief Fuel screen: four 2-pixel bars (liquid fuel, oxidizer, monopropellant, electric charge).
static void drawFuel() {
  uint8_t lv[4] = {
    barLevel(VData.LiquidFuel, VData.LiquidFuelTot), barLevel(VData.Oxidizer, VData.OxidizerTot),
    barLevel(VData.MonoProp, VData.MonoPropTot), barLevel(VData.ECharge, VData.EChargeTot)
  };
  for (int i = 0; i < 4; i++) {
    int x0 = i * 3;
    if (lv[i] == 255) { setPx(x0, 7); continue; }   // no such tank: a single dot at the bottom
    for (int h = 0; h < lv[i]; h++) { setPx(x0, 7 - h); setPx(x0 + 1, 7 - h); }
  }
}

/// @brief Stage screen: the current stage as a number and one dot per stage below it.
static void drawStage() {
  drawNumber(VData.CurrentStage, 0);
  int dots = VData.CurrentStage > MX_W ? MX_W : VData.CurrentStage;
  for (int i = 0; i < dots; i++) setPx(i, 7);
}

/// @brief Artificial horizon: the horizon line tilts with roll and moves with pitch, the ground is dotted.
static void drawHorizon() {
  float r = VData.Roll * 0.0174533f;
  float s = sin(r), c = cos(r);
  float shift = VData.Pitch * (4.0f / 45.0f);      // pitch up moves the horizon down
  for (int y = 0; y < MX_H; y++) {
    for (int x = 0; x < MX_W; x++) {
      float d = -(x - 5.5f) * s + (y - 3.5f) * c - shift;
      if (d > -0.6f && d < 0.6f) setPx(x, y);
      else if (d >= 0.6f && ((x + y) & 1) == 0) setPx(x, y);
    }
  }
}

/// @brief Vertical speed: a bar growing up from the middle when climbing and down when descending.
static void drawVsi() {
  float v = VData.VVI;
  float a = v < 0 ? -v : v;
  int n = (a >= 2.0f) + (a >= 10.0f) + (a >= 50.0f) + (a >= 200.0f);
  setPx(0, 3); setPx(0, 4); setPx(11, 3); setPx(11, 4);       // zero markers
  for (int k = 0; k < n; k++) {
    int row = v >= 0 ? 3 - k : 4 + k;
    for (int x = 2; x < 10; x++) setPx(x, row);
  }
  if (n == 0) for (int x = 2; x < 10; x += 2) setPx(x, 3);    // level flight: dotted line
}

/// @brief SAS mode icon, or a dotted line when SAS is off.
static void drawSas() {
  if (!ControlStatus(AGSAS)) { for (int x = 1; x < 11; x += 2) setPx(x, 3); return; }
  int mode = (VData.NavballSASMode & 0x0F) - 1;
  if (mode < 0 || mode > 9) mode = 0;
  drawIcon(ICON[mode], 2, 0);
}

/// @brief Reads the screen button (learned idle level, 200 ms lockout) and cycles manualScreen.
static void screenButton(unsigned long ms) {
  int level = digitalRead(SCREENPIN);
  if (!Connected) { screenIdle = level; screenPressedBefore = false; return; }
  bool pressed = (level != screenIdle);
  if (pressed && !screenPressedBefore && ms > screenLockUntil) {
    manualScreen = (manualScreen + 1) % (SCR_COUNT + 1);
    screenLockUntil = ms + 200;
  }
  screenPressedBefore = pressed;
}

/// @brief Chooses the screen for the connected state.
static uint8_t chooseScreen(unsigned long ms) {
  if (forcedScreen < SCR_COUNT && ms < forcedUntil) return forcedScreen;
  if (manualScreen < SCR_COUNT) return manualScreen;
  if (ms < stageFlashUntil) return SCR_STAGE;                       // a staging just happened
  if (VData.RAlt < 50.0f && VData.Vsurf < 2.0f && VData.CurrentStage > 0) return SCR_STAGE;   // on the pad
  return SCR_FUEL;
}

/// @brief Configures the matrix and the screen button; call once from setup().
void matrixInit() {
  matrix.begin();
  pinMode(SCREENPIN, INPUT_PULLUP);
}

/**
 * @brief Redraws the matrix (at most every MX_PERIOD_MS): splash, waiting/lost state, or the selected screen,
 *        then a flashing overlay on warnings and a heartbeat pixel proving that loop() is still running.
 */
void matrixUpdate() {
  unsigned long ms = millis();
  if (ms - mxLast < MX_PERIOD_MS) return;
  mxLast = ms;

  screenButton(ms);
  memset(fb, 0, sizeof(fb));

  if (ms < SPLASHMS) {
    drawSplash(ms);
  } else if (!Connected) {
    if (everConnected) drawLost(ms); else drawWaiting(ms);
  } else {
    everConnected = true;
    if (lastStageSeen != 255 && VData.CurrentStage != lastStageSeen) stageFlashUntil = ms + 1500;
    lastStageSeen = VData.CurrentStage;
    switch (chooseScreen(ms)) {
      case SCR_FUEL:    drawFuel();    break;
      case SCR_STAGE:   drawStage();   break;
      case SCR_HORIZON: drawHorizon(); break;
      case SCR_VSI:     drawVsi();     break;
      case SCR_SAS:     drawSas();     break;
    }
    if (warning != 0 && ((ms / 250) & 1))                            // warning: invert the whole picture
      for (int y = 0; y < MX_H; y++) for (int x = 0; x < MX_W; x++) fb[y][x] ^= 1;
  }

  setPxV(11, 0, (ms / 250) & 1);                                      // heartbeat: freezes if loop() hangs

  for (int x = 0; x < MX_W; x++) {
    uint8_t col = 0;
    for (int y = 0; y < MX_H; y++) if (fb[y][x]) col |= (1 << y);
    mxCols[x] = col;
  }
  matrix.renderBitmap(fb, MX_H, MX_W);
}
