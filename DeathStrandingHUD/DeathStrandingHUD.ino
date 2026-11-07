/*
 * ============================================================================
 *  DEATH STRANDING · ODRADEK TOPO SCANNER   (Terminal Cuff / v1.3)
 * ----------------------------------------------------------------------------
 *  目标板：ESP32-1732S019N  (ESP32-S3-N16R8 + 板载 1.9" ST7789 170x320)
 *  依赖  ：TFT_eSPI（User_Setup.h 已按本板配置：ST7789 / 170x320 / HSPI 13,12,10,11,1,14）
 *          ESP32 Arduino core 自带 BLE 与 WiFi 库
 *
 *  v1.3 相对 v1.2 的改动 —— "Chiral Network"
 *    · 拆掉 BLE 扫描和 WiFi 热点扫描：扫到的设备数没有信息价值，
 *      还占着射频。腾出来的部分换成真正来自外部世界的数据。
 *    · NTP 校时：STRAND TIME 变成真实时钟（UTC+8）。
 *    · 天气（Open-Meteo）：气温 / 风速 / 降水概率 / WMO 天气码。
 *      降水概率 >= 60% 或天气码属于雨雪类时，整机进入 TIMEFALL 状态。
 *    · 空气质量（Open-Meteo Air Quality）：PM2.5 映射成 BT 浓度——
 *      数字是真的，"BT"是死亡搁浅的语义。旁边同时给出真实 AQI。
 *    · 链路质量：实测 HTTPS 往返时延，顶部那根进度条终于反映你家网速。
 *    · 首次使用走配网门户：连 ODRADEK-SETUP 热点，选 WiFi、填城市，
 *      其余全部自动（城市 → 经纬度用 Open-Meteo 地理编码）。
 *
 *  唯一的探测器坐标系还在：1px 等高线地形 + chiral route。
 *  底图开机画一次，之后整块地图一个像素都不再写。
 *
 *  BOOT 键（GPIO0）：短按立刻重新同步一次，长按 1.5s 清除 WiFi 配置重开门户。
 *
 *  性能：整屏只在开机时写一次（约 2600 个 drawPixel），
 *        稳态只有文字行在变，网络请求跑在独立任务里，不卡渲染。
 * ============================================================================
 */

// FS / WebServer 必须排在 TFT_eSPI 前面：TFT_eSPI 会先把 FS_H 这个
// 头文件守卫用掉，之后再 include FS.h 就是空操作，WebServer.h 里
// 的 `FS &fs` 就找不到类型。顺序换回来编译立刻炸。
#include <FS.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>

#include <TFT_eSPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>

TFT_eSPI tft = TFT_eSPI();

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

/* ---------------------------- 死亡搁浅配色 ---------------------------- */
#define C_BLACK  0x0000
#define C_WHITE  RGB565(255, 255, 255)
#define C_CREAM  RGB565(237, 227, 205)   // 标题 / 地形高处
#define C_CY     RGB565(120, 232, 224)   // 开罗尔青
#define C_CY_M   RGB565(60, 190, 190)
#define C_CY_D   RGB565(18, 74, 80)
#define C_CY_F   RGB565(9, 30, 36)
#define C_AMBER  RGB565(245, 166, 35)    // BT 警示橙
#define C_RED    RGB565(196, 57, 43)
#define C_GREY   RGB565(122, 134, 142)
#define C_SLATE  RGB565(38, 52, 64)

/* ------------------------------ 屏幕布局 ------------------------------ */
#define SCR_W 170
#define SCR_H 320

#define MAP_X   3
#define MAP_Y   80
#define MAP_W   164
#define MAP_H   144

#define CELL 2
#define MB_W (MAP_W / CELL)              // 82 格（粗高度场）
#define MB_H (MAP_H / CELL)              // 72 格
#define MB_N (MB_W * MB_H)

#define FW MAP_W                         // 细高度场 = 164x144，一个像素一条线
#define FH MAP_H
#define FN (FW * FH)

#define NBAND   6                        // 调色带数（低 → 高）
#define BAND_H  43                       // 每带高度（255/6）
#define LSTEP   26                       // 等值线间隔：高度每跨 26 就是一整条等高线
#define MAJOR_EVERY 3                    // 每 3 条线加亮一条（计曲线），线宽不变
#define WATER_V 34                       // 低于此值算水面

#define MAXROUTE 192
#define NET_PERIOD_MS 600000UL           // 10 分钟同步一次外界数据
#define NET_TLS_MS    12000UL            // 单次 HTTPS 超时

#define BTN_PIN 0                        // 板载 BOOT 键

/* ------------------------------ 全局状态 ------------------------------ */
static uint8_t  TERR[MB_N];              // 粗高度场 0..255
static uint8_t  HB[FN];                  // 插值后的细高度场（整张底图的唯一真源）
static uint16_t PAL[NBAND][4];           // [带][0 底 1 受光 2 背光 3 计曲线]
static uint8_t  ROUTEB[(FN + 7) / 8];    // chiral route 的 1px 位图

static uint32_t lastFrame = 0, lastText = 0, lastLog = 0;
static uint32_t fpsCount = 0, fpsTimer = 0;
static int      gFps = 0;

/* --------------------------- 外界数据（网络任务写、UI 读） --------------------------- */
enum { WX_NONE, WX_CLEAR, WX_OVERCAST, WX_TIMEFALL };

struct WxData {
  float temp;          // °C
  int   rainP;         // 降水概率 %
  int   code;          // WMO 天气码
  int   wind;          // km/h
  int   pm25;          // μg/m³
  int   aqi;           // 欧洲 AQI
  int   rtt;           // HTTPS 往返 ms
  uint8_t state;       // WX_*
  uint8_t ok;          // 至少成功同步过一次
  uint8_t stale;       // 最近一次同步失败，屏上是旧值
  uint32_t stamp;      // 上次成功同步的 millis
  char  city[16];
};

static WxData   gWx;                     // 受 gWxMux 保护
static portMUX_TYPE gWxMux = portMUX_INITIALIZER_UNLOCKED;

static bool     gHasCreds = false;       // NVS 里有 WiFi 账号
static bool     gPortalUp = false;       // 配网门户在跑
static bool     gTimeOk = false;         // NTP 已对上
static bool     gWifiUp = false;
static uint32_t gWifiDownSince = 0;
static uint32_t gLastReconnect = 0;
static uint32_t gLastNetTry = 0;
static volatile bool gSyncReq = false;   // 按钮请求立刻同步

static float    gLat = 31.2304f, gLon = 121.4737f;   // 默认上海，配网时可改
static char     gCityQ[20] = "Shanghai";             // 用户输入的城市
static bool     gGeoPending = false;                 // 城市还没解析成经纬度

/* ============================== 工具函数 ============================== */

static inline uint16_t lerp565(uint16_t a, uint16_t b, float t) {
  if (t <= 0.0f) return a;
  if (t >= 1.0f) return b;
  int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
  int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
  int r = ar + (int)((br - ar) * t + 0.5f);
  int g = ag + (int)((bg - ag) * t + 0.5f);
  int c = ab + (int)((bb - ab) * t + 0.5f);
  return (uint16_t)((r << 11) | (g << 5) | c);
}

// 死亡搁浅那种"拉开字距"的小字排版
static int trackedWidth(int len, uint8_t track) { return len * 6 + (len - 1) * track; }

static void drawTracked(const char *s, int x, int y, uint16_t col, uint8_t track) {
  char one[2] = {0, 0};
  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(col, C_BLACK);
  for (const char *p = s; *p; ++p) {
    one[0] = *p;
    tft.drawString(one, x, y);
    x += 6 + track;
  }
}

static void drawTrackedRight(const char *s, int right, int y, uint16_t col, uint8_t track) {
  int n = 0;
  for (const char *p = s; *p; ++p) n++;
  if (!n) return;
  drawTracked(s, right - trackedWidth(n, track) + 1, y, col, track);
}

/* ============================== 地形高度场 ============================== */

/*
 * 高度场全部由正弦/高斯叠加而成：
 *   多层正弦  → 破碎的山脊走向
 *   中央高斯  → 一条南北向主山脊（带锯齿，像死亡搁浅的岩石断层）
 *   东北高斯  → 一座高峰
 *   西南负高斯→ 一个湖盆（低于 WATER_V 的部分画成水面横纹）
 */
static float terrainRaw(int x, int y) {
  const float nx = (x - MB_W * 0.5f) / (MB_W * 0.5f);
  const float ny = (y - MB_H * 0.5f) / (MB_H * 0.5f);

  float e = 0.0f;
  e += 0.42f * sinf(nx * 2.2f + 0.35f) * cosf(ny * 1.7f - 0.20f);
  e += 0.24f * sinf(nx * 4.6f - ny * 3.4f + 1.90f);
  e += 0.13f * cosf(nx * 9.1f + ny * 7.3f - 2.00f);
  e += 0.07f * sinf(nx * 17.0f - ny * 13.0f + 0.40f);

  const float rx = nx + 0.10f * sinf(ny * 3.6f + 0.70f);
  e += 0.44f * expf(-rx * rx * 7.0f) * (0.55f + 0.45f * sinf(ny * 4.3f + 1.10f));

  const float mx = nx - 0.42f, my = ny + 0.50f;
  e += 0.55f * expf(-(mx * mx * 3.0f + my * my * 4.2f));

  const float lx = nx + 0.58f, ly = ny - 0.46f;
  e -= 0.95f * expf(-(lx * lx * 10.0f + ly * ly * 12.0f));

  e += 0.16f * (ny * 0.6f - nx * 0.4f);
  return e * (1.0f - 0.10f * (nx * nx + ny * ny));
}

// 两遍归一化：先扫最小/最大值，保证整张图铺满 0..255
void buildTerrain() {
  float lo = 1e9f, hi = -1e9f;
  for (int y = 0; y < MB_H; y++)
    for (int x = 0; x < MB_W; x++) {
      float e = terrainRaw(x, y);
      if (e < lo) lo = e;
      if (e > hi) hi = e;
    }
  float span = hi - lo;
  if (span < 0.001f) span = 1.0f;

  for (int y = 0; y < MB_H; y++)
    for (int x = 0; x < MB_W; x++) {
      float f = (terrainRaw(x, y) - lo) / span;
      if (f < 0.0f) f = 0.0f;
      if (f > 1.0f) f = 1.0f;
      TERR[y * MB_W + x] = (uint8_t)(f * 255.0f + 0.5f);
    }
}

/*
 * 线色：低处深开罗尔青 → 高处奶油白，只走青这一条色相，
 * 所以海拔是靠"线的颜色"读出来的，不靠填充。
 *
 * 每带派生三个色：
 *   [1] 受光面（往上提）  [2] 背光面（往下压）  [3] 计曲线（最亮）
 * 关键：[2] 只压到 38%，不能压到黑 —— 最低那几条带本来就暗，
 * 压狠了线就消失在黑底里，"细"会变成"看不见"。
 *
 * 计曲线是地形图的通行做法：每 3 条等高线加亮一条做分级，
 * 线宽仍然是 1px，但远近疏密一眼就读得出来。
 * 颜色固定，底图开机画一次，中途不换色，所以永远不需要重画。
 */
void buildPalette() {
  static const uint16_t base[NBAND] = {
    RGB565(16, 62, 74), RGB565(22, 96, 104), RGB565(34, 132, 138),
    RGB565(74, 176, 176), RGB565(150, 214, 206), RGB565(237, 227, 205)
  };
  for (int i = 0; i < NBAND; i++) {
    PAL[i][0] = base[i];
    PAL[i][1] = lerp565(base[i], C_WHITE, 0.42f);
    PAL[i][2] = lerp565(base[i], C_BLACK, 0.38f);
    PAL[i][3] = lerp565(base[i], C_WHITE, 0.72f);
  }
}

// 两遍归一化之后再做双线性插值：82x72 的粗场放大到 164x144。
// 放大不是为了让地形更"高分辨率"，而是为了等高线能落在 1 个像素上 ——
// 直接在粗场上取等值线只能得到 2px 的块，做不到细线。
static void buildFineField() {
  const float sx = (float)(MB_W - 1) / (float)(FW - 1);
  const float sy = (float)(MB_H - 1) / (float)(FH - 1);
  for (int y = 0; y < FH; y++) {
    const float fy = (float)y * sy;
    const int y0 = (int)fy;
    const int y1 = (y0 + 1 < MB_H) ? y0 + 1 : y0;
    const float ty = fy - (float)y0;
    for (int x = 0; x < FW; x++) {
      const float fx = (float)x * sx;
      const int x0 = (int)fx;
      const int x1 = (x0 + 1 < MB_W) ? x0 + 1 : x0;
      const float tx = fx - (float)x0;
      const float a = TERR[y0 * MB_W + x0], b = TERR[y0 * MB_W + x1];
      const float c = TERR[y1 * MB_W + x0], d = TERR[y1 * MB_W + x1];
      const float top = a + (b - a) * tx;
      const float bot = c + (d - c) * tx;
      HB[y * FW + x] = (uint8_t)(top + (bot - top) * ty + 0.5f);
    }
  }
}

static inline void routeSet(int x, int y) {
  if (x < 0 || x >= FW || y < 0 || y >= FH) return;
  const int i = y * FW + x;
  ROUTEB[i >> 3] |= (uint8_t)(1u << (i & 7));
}

static inline bool routeGet(int x, int y) {
  if (x < 0 || x >= FW || y < 0 || y >= FH) return false;
  const int i = y * FW + x;
  return (ROUTEB[i >> 3] >> (i & 7)) & 1u;
}

static void routeLine(int x0, int y0, int x1, int y1) {
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  for (;;) {
    routeSet(x0, y0);
    if (x0 == x1 && y0 == y1) break;
    const int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

// 一条横贯地图的 chiral route：两根低频正弦叠加出自然的绕行感，
// 采样点之间用 Bresenham 连起来，保证线是连续不断的。
void buildRoute() {
  memset(ROUTEB, 0, sizeof(ROUTEB));
  int px = -1, py = 0;
  for (int i = 0; i < MAXROUTE; i++) {
    const float t = (float)i / (float)(MAXROUTE - 1);
    const int x = 4 + (int)(t * (float)(FW - 9));
    int y = (int)(FH * 0.66f
                  - FH * 0.20f * sinf(t * 6.0f + 0.6f)
                  + FH * 0.07f * sinf(t * 15.0f + 2.1f));
    if (y < 3) y = 3;
    if (y > FH - 4) y = FH - 4;
    if (px >= 0) routeLine(px, py, x, y);
    if (i % 24 == 0) {                     // 路标：跨在路线上的一小段竖线
      for (int d = -2; d <= 2; d++) routeSet(x, y + d);
    }
    px = x; py = y;
  }
}

/* ============================== 静态界面 ============================== */

/* 天气图标摆在原来 BB 舱的位置：晴 / 阴 / 时间雨。
 * 整块 29x29 先擦掉再画，所以晴阴雨之间不会残留上一版的像素。 */
static void drawCloud(int cx, int cy, uint16_t col) {
  tft.fillCircle(cx - 4, cy, 4, col);
  tft.fillCircle(cx + 1, cy - 3, 5, col);
  tft.fillCircle(cx + 5, cy, 4, col);
  tft.fillRect(cx - 8, cy, 17, 4, col);
}

static void drawWxGlyph(uint8_t state, bool haveData) {
  const int cx = 152, cy = 17;
  tft.fillRect(139, 3, 29, 29, C_BLACK);

  if (!haveData) {                       // 还没联上开罗尔网络：保留 BB 舱
    tft.drawCircle(cx, cy, 10, C_CY_M);
    tft.drawCircle(cx, cy, 7, C_CY_D);
    tft.fillCircle(cx, cy + 1, 3, C_CREAM);
    return;
  }

  if (state == WX_CLEAR) {
    tft.fillCircle(cx, cy, 5, C_AMBER);
    for (int k = 0; k < 8; k++) {
      const float a = (float)k * 0.7854f;
      tft.drawLine(cx + (int)(cosf(a) * 8), cy + (int)(sinf(a) * 8),
                   cx + (int)(cosf(a) * 11), cy + (int)(sinf(a) * 11), C_AMBER);
    }
    return;
  }

  const bool rain = (state == WX_TIMEFALL);
  drawCloud(cx, cy - 3, rain ? C_CY_M : C_GREY);
  if (rain) {
    for (int k = 0; k < 3; k++) {
      const int x = cx - 5 + k * 6;
      tft.drawLine(x + 1, cy + 5, x - 1, cy + 10, C_AMBER);
    }
  }
}

void drawTopBar() {
  tft.setTextDatum(TL_DATUM);

  tft.setTextFont(1);
  tft.setTextSize(2);
  tft.setTextColor(C_CREAM, C_BLACK);
  tft.drawString("DEATH", 6, 3);
  tft.drawString("STRANDING", 6, 19);

  drawWxGlyph(WX_NONE, false);            // 联网成功后再换成真实天气
  tft.drawFastHLine(0, 44, SCR_W, C_CY_D);

  // 地区行：左边城市名由网络数据填，右边固定
  drawTrackedRight("FREE ROAM", 164, 48, C_GREY, 1);

  // 开罗尔带宽行（数值在 updateReadouts 里刷新）
  drawTracked("CHIRAL", 6, 58, C_CY_D, 1);

  // 带宽进度槽
  tft.drawRect(6, 68, 158, 3, C_SLATE);
  tft.fillRect(6, 68, 158, 3, C_BLACK);
  tft.drawRect(6, 68, 158, 3, C_SLATE);
}

// 地图外框 + 四角短刻度：像终端戒里那块被框住的地形显示区
void drawMapFrame() {
  tft.drawRect(MAP_X - 1, MAP_Y - 1, MAP_W + 2, MAP_H + 2, C_CY_F);
  const int x0 = MAP_X - 2, x1 = MAP_X + MAP_W + 1;
  const int y0 = MAP_Y - 2, y1 = MAP_Y + MAP_H + 1;
  tft.drawFastHLine(x0, y0, 6, C_CY_D);
  tft.drawFastVLine(x0 + 2, y0, 5, C_CY_D);
  tft.drawFastHLine(x1 - 5, y0, 6, C_CY_D);
  tft.drawFastVLine(x1 - 2, y0, 5, C_CY_D);
  tft.drawFastHLine(x0, y1, 6, C_CY_D);
  tft.drawFastVLine(x0 + 2, y1 - 4, 5, C_CY_D);
  tft.drawFastHLine(x1 - 5, y1, 6, C_CY_D);
  tft.drawFastVLine(x1 - 2, y1 - 4, 5, C_CY_D);
}

void drawBottomBar() {
  tft.drawFastHLine(0, 228, SCR_W, C_CY_D);

  drawTracked("ODRADEK SCAN", 6, 233, C_CY_D, 1);

  drawTracked("BT", 6, 244, C_CY_D, 1);
  drawTracked("AQI", 86, 244, C_CY_D, 1);

  drawTracked("RAIN", 6, 255, C_CY_D, 1);

  tft.drawFastHLine(0, 266, SCR_W, C_SLATE);

  drawTracked("TEMP", 6, 271, C_CY_D, 1);
  drawTracked("WIND", 86, 271, C_CY_D, 1);

  tft.drawFastHLine(0, 282, SCR_W, C_SLATE);

  drawTracked("LINK", 6, 287, C_CY_D, 1);
  drawTracked("SYNC", 96, 287, C_CY_D, 1);

  drawTracked("STRAND TIME", 6, 298, C_CY_D, 1);
  drawTracked("ODOradek v1.3", 6, 309, C_SLATE, 0);
}

/* ============================ 等高线渲染 ============================ */

/*
 * 常驻 HUD：底图是"烧"在屏幕上的。
 *
 * 开机时把 82x72 的粗高度场双线性插值成 164x144（HB），
 * 然后逐像素判断一件事：它和右邻居、下邻居的等值线层号是否相同。
 *   · 三个层号都一样 → 这是同一层等高线的内部，留黑
 *   · 有任何一处不同 → 这里正好是两条等值线之间，点亮 1 个像素
 * 于是每条等高线都是单像素宽、彼此连通、随地形自然弯曲的闭合曲线。
 *
 * 亮/暗按高度梯度分：光来自左上，受光坡用提亮色、背光坡用压暗色，
 * 所以 1px 的线也能看出山体朝向。
 *
 * 这张底图此后永不擦除 —— 屏幕上唯一会变的是信号标记。
 */

static inline uint8_t hbAt(int x, int y) {
  if (x < 0) x = 0; else if (x >= FW) x = FW - 1;
  if (y < 0) y = 0; else if (y >= FH) y = FH - 1;
  return HB[y * FW + x];
}

// 这个像素该是什么颜色；返回 0 = 黑底，不用落笔
static uint16_t fineColor(int x, int y) {
  if (routeGet(x, y)) return C_CY_M;          // chiral route 压在等高线之上

  const uint8_t v = HB[y * FW + x];

  if (v < WATER_V)                             // 水面：稀疏点划
    return (((x + y * 2) & 7) < 2) ? C_CY_D : 0;

  const uint8_t lv = (uint8_t)(v / LSTEP);
  if (lv == (uint8_t)(hbAt(x + 1, y) / LSTEP) &&
      lv == (uint8_t)(hbAt(x, y + 1) / LSTEP)) return 0;

  const uint8_t band = (uint8_t)(v / BAND_H);
  const uint16_t *pal = PAL[band < NBAND ? band : NBAND - 1];
  if (lv % MAJOR_EVERY == 0) return pal[3];   // 计曲线：不分受光背光，一律提亮

  const int gx = (int)hbAt(x + 1, y) - (int)hbAt(x - 1, y);
  const int gy = (int)hbAt(x, y + 1) - (int)hbAt(x, y - 1);
  return (gx + gy >= 0) ? pal[1] : pal[2];
}

// 整张底图画一遍：只在开机时调用，约 2600 个像素
void drawStaticMap() {
  int px = 0;
  tft.startWrite();
  for (int y = 0; y < FH; y++)
    for (int x = 0; x < FW; x++) {
      const uint16_t c = fineColor(x, y);
      if (!c) continue;
      tft.drawPixel(MAP_X + x, MAP_Y + y, c);
      px++;
    }
  tft.endWrite();
  Serial.printf("[MAP] %d contour px = %.1f%% of map\n", px, 100.0f * (float)px / (float)FN);
}

/* ============================== 数值刷新 ============================== */

// 握手时延从国内到 open-meteo 基本是四位数，按 ms 显示会把格子撑破
static void fmtRtt(int ms, char *out, size_t n) {
  if (ms <= 0)        snprintf(out, n, "--");
  else if (ms < 1000) snprintf(out, n, "%dms", ms);
  else                snprintf(out, n, "%d.%ds", ms / 1000, (ms % 1000) / 100);
}

static char lastVal[12][24];        // 每个数值区上一次写过什么

// 值没变就一个字节都不发 —— 文本区的周期性闪烁就是这么消掉的
static bool valChanged(int slot, const char *s) {
  if (!strcmp(lastVal[slot], s)) return false;
  strncpy(lastVal[slot], s, 23);
  lastVal[slot][23] = 0;
  return true;
}

static void updateReadouts(uint32_t now) {
  if (now - lastText < 100) return;
  lastText = now;

  WxData w;
  portENTER_CRITICAL(&gWxMux);
  w = gWx;
  portEXIT_CRITICAL(&gWxMux);

  char b[24];

  // 天气图标只在状态变化时重画
  static uint8_t lastState = 0xFF;
  static bool    lastOk = false;
  if (w.state != lastState || w.ok != lastOk) {
    lastState = w.state;
    lastOk = w.ok;
    drawWxGlyph(w.state, w.ok);
  }

  // 地区：城市名来自地理编码，没联上就直说
  const char *city = w.city[0] ? w.city : (gPortalUp ? "SETUP MODE" : "NO LINK");
  if (valChanged(0, city)) {
    tft.fillRect(6, 48, 92, 9, C_BLACK);
    drawTracked(city, 6, 48, w.city[0] ? C_CREAM : C_CY_D, 2);
  }

  // 开罗尔带宽：真实 HTTPS 握手时延，越短条越长
  fmtRtt(w.rtt, b, sizeof(b));
  if (valChanged(1, b)) {
    tft.fillRect(72, 56, 92, 9, C_BLACK);
    drawTrackedRight(b, 164, 58, w.rtt > 0 ? C_CREAM : C_CY_D, 1);
  }

  static int lastBarW = -1;
  int bw = 0;
  if (w.rtt > 0) {
    int q = 3000 - w.rtt;                // 0ms → 3000，国内实测多在 2000 上下
    if (q < 0) q = 0;
    if (q > 3000) q = 3000;
    bw = 4 + (q * 152) / 3000;
  }
  if (bw != lastBarW) {
    lastBarW = bw;
    tft.fillRect(7, 69, 156, 1, C_BLACK);
    if (bw > 0) tft.fillRect(7, 69, bw, 1, w.rtt > 0 && w.rtt < 900 ? C_CY_M : C_AMBER);
  }

  // 天气状态（右对齐，最长 8 个字符）
  const char *st;
  uint16_t stCol;
  if (!w.ok)            { st = "NO DATA";  stCol = C_CY_D; }
  else if (w.state == WX_TIMEFALL) { st = "TIMEFALL"; stCol = C_AMBER; }
  else if (w.state == WX_OVERCAST) { st = "OVERCAST"; stCol = C_GREY; }
  else                  { st = "CLEAR";    stCol = C_CY; }
  if (valChanged(2, st)) {
    tft.fillRect(98, 233, 66, 9, C_BLACK);
    drawTrackedRight(st, 164, 233, stCol, 1);
  }

  // BT 浓度 = PM2.5 / 4：数字是真的，语义是游戏的
  if (w.pm25 >= 0) snprintf(b, sizeof(b), "%02d", w.pm25 / 4 > 99 ? 99 : w.pm25 / 4);
  else             snprintf(b, sizeof(b), "--");
  if (valChanged(3, b)) {
    tft.fillRect(22, 244, 34, 9, C_BLACK);
    drawTracked(b, 24, 244, w.pm25 >= 0 ? C_AMBER : C_CY_D, 1);
  }
  if (w.aqi >= 0) snprintf(b, sizeof(b), "%d", w.aqi);
  else            snprintf(b, sizeof(b), "--");
  if (valChanged(4, b)) {
    tft.fillRect(114, 244, 50, 9, C_BLACK);
    drawTrackedRight(b, 164, 244, w.aqi >= 0 ? C_CREAM : C_CY_D, 1);
  }

  // 降水概率：数字 + 8 格条
  if (w.rainP >= 0) snprintf(b, sizeof(b), "%d%%", w.rainP);
  else              snprintf(b, sizeof(b), "--");
  if (valChanged(5, b)) {
    tft.fillRect(38, 255, 82, 9, C_BLACK);
    drawTrackedRight(b, 118, 255, w.rainP >= 0 ? C_CREAM : C_CY_D, 1);
  }

  static int lastRainBars = -1;
  int rb = (w.rainP >= 0) ? (w.rainP * 8 + 99) / 100 : 0;
  if (rb != lastRainBars) {
    lastRainBars = rb;
    for (int i = 0; i < 8; i++) {
      uint16_t c = (i < rb) ? (rb >= 7 ? C_AMBER : C_CY_M) : C_SLATE;
      tft.fillRect(126 + i * 5, 256, 3, 6, c);
    }
  }

  // 气温 / 风速
  if (w.temp > -99.0f) snprintf(b, sizeof(b), "%.1fC", (double)w.temp);
  else                 snprintf(b, sizeof(b), "--");
  if (valChanged(6, b)) {
    tft.fillRect(38, 271, 46, 9, C_BLACK);
    drawTracked(b, 40, 271, w.temp > -99.0f ? C_CREAM : C_CY_D, 1);
  }
  if (w.wind >= 0) snprintf(b, sizeof(b), "%d", w.wind);
  else             snprintf(b, sizeof(b), "--");
  if (valChanged(7, b)) {
    tft.fillRect(118, 271, 46, 9, C_BLACK);
    drawTrackedRight(b, 164, 271, w.wind >= 0 ? C_CREAM : C_CY_D, 1);
  }

  // 链路时延 + 距上次同步多久
  fmtRtt(w.rtt, b, sizeof(b));
  if (valChanged(8, b)) {
    tft.fillRect(38, 287, 46, 9, C_BLACK);
    drawTracked(b, 40, 287, w.rtt > 0 ? C_CY_M : C_CY_D, 1);
  }
  if (!w.ok) snprintf(b, sizeof(b), "--");
  else {
    const uint32_t age = now - w.stamp;
    if (age < 60000UL) snprintf(b, sizeof(b), "now");
    else               snprintf(b, sizeof(b), "%lum", (unsigned long)(age / 60000UL));
  }
  if (valChanged(9, b)) {
    tft.fillRect(126, 287, 38, 9, C_BLACK);
    drawTrackedRight(b, 164, 287, w.stale ? C_AMBER : C_CY_D, 1);
  }

  // 真实时钟（NTP 未对上前显示横杠）
  if (gTimeOk) {
    const time_t t = time(nullptr);
    struct tm lt;
    localtime_r(&t, &lt);
    snprintf(b, sizeof(b), "%02d:%02d:%02d", lt.tm_hour, lt.tm_min, lt.tm_sec);
  } else {
    snprintf(b, sizeof(b), "--:--:--");
  }
  if (valChanged(10, b)) {
    tft.fillRect(88, 298, 76, 9, C_BLACK);
    drawTrackedRight(b, 164, 298, C_CREAM, 2);
  }

  // FPS
  snprintf(b, sizeof(b), "%d", gFps);
  if (valChanged(11, b)) {
    tft.fillRect(146, 309, 20, 9, C_BLACK);
    drawTrackedRight(b, 164, 309, C_SLATE, 0);
  }
}

/* ============================== 开罗尔网络 ============================== */

/*
 * 全部数据走 HTTPS，三件事按顺序做完算一次同步：
 *   ① 城市名 → 经纬度（只在首次或改城市时做一次）
 *   ② Open-Meteo 天气：当前气温 / 天气码 / 风速 + 未来一小时降水概率
 *   ③ Open-Meteo 空气质量：PM2.5 / 欧洲 AQI
 * 同步跑在独立任务里，渲染不会被 TLS 握手卡住。
 * 证书不校验（只读公共天气数据），省掉 30KB 的 CA 和每次的校验开销。
 */

static WebServer   gSrv(80);
static DNSServer   gDns;
static Preferences gPrefs;

static const char *jsonVal(const char *from, const char *key) {
  char pat[28];
  snprintf(pat, sizeof(pat), "\"%s\":", key);
  const char *p = from ? strstr(from, pat) : nullptr;
  if (!p) return nullptr;
  p += strlen(pat);
  while (*p == ' ') p++;
  return p;
}

static int jsonInt(const char *from, const char *key, int dflt) {
  const char *p = jsonVal(from, key);
  return p ? atoi(p) : dflt;
}

static float jsonFloat(const char *from, const char *key, float dflt) {
  const char *p = jsonVal(from, key);
  return p ? (float)atof(p) : dflt;
}

static void jsonStr(const char *from, const char *key, char *out, size_t n) {
  out[0] = 0;
  const char *p = jsonVal(from, key);
  if (!p || *p != '"') return;
  p++;
  size_t i = 0;
  while (*p && *p != '"' && i < n - 1) out[i++] = *p++;
  out[i] = 0;
}

// 返回本次请求总耗时（ms），失败返回 -1。
// connMs 单独回传 TCP+TLS 握手耗时 —— 那才是链路时延，
// 总耗时里还混着服务器出数据的几百毫秒。
static int httpsGet(const char *host, const char *path, String &out, int *connMs = nullptr) {
  WiFiClientSecure cli;
  cli.setInsecure();
  cli.setConnectionTimeout(NET_TLS_MS);
  const uint32_t t0 = millis();
  if (!cli.connect(host, 443)) { cli.stop(); return -1; }
  if (connMs) *connMs = (int)(millis() - t0);

  HTTPClient http;
  http.setTimeout(NET_TLS_MS);
  http.setConnectTimeout(NET_TLS_MS);
  // 已经连上的 socket HTTPClient 会直接复用，不会再握一次手
  if (!http.begin(cli, String("https://") + host + path)) { cli.stop(); return -1; }
  http.addHeader("Connection", "close");
  const int code = http.GET();
  const int dt = (int)(millis() - t0);
  if (code == 200) out = http.getString();
  http.end();
  return (code == 200) ? dt : -1;
}

static bool isWet(int c) {
  return (c >= 51 && c <= 67) || (c >= 71 && c <= 77) ||
         (c >= 80 && c <= 82) || (c >= 85 && c <= 86) || (c >= 95);
}

static uint8_t wxState(int code, int rainP) {
  if (isWet(code) || rainP >= 60) return WX_TIMEFALL;
  if (rainP >= 30 || code >= 2) return WX_OVERCAST;
  return WX_CLEAR;
}

static void saveLoc() {
  gPrefs.begin("odradek", false);
  gPrefs.putFloat("lat", gLat);
  gPrefs.putFloat("lon", gLon);
  gPrefs.putBool("geo", false);
  gPrefs.putString("place", gWx.city);   // 重启后顶栏直接显示它，不用再解析
  gPrefs.end();
}

// 屏上的内置字库只有 ASCII，中文地名会画成乱码，宁可不显示
static void asciiName(char *s, size_t n) {
  for (size_t i = 0; i < n && s[i]; i++)
    if ((uint8_t)s[i] < 32 || (uint8_t)s[i] > 126) { snprintf(s, n, "SITE"); return; }
}

static bool geocode() {
  String q = gCityQ;
  q.replace(" ", "%20");
  char path[128];
  snprintf(path, sizeof(path), "/v1/search?name=%s&count=1&language=en&format=json", q.c_str());
  String js;
  if (httpsGet("geocoding-api.open-meteo.com", path, js) < 0) return false;
  const char *r = jsonVal(js.c_str(), "results");
  if (!r) return false;
  gLat = jsonFloat(r, "latitude", gLat);
  gLon = jsonFloat(r, "longitude", gLon);
  char nm[16];
  jsonStr(r, "name", nm, sizeof(nm));
  if (!nm[0]) { strncpy(nm, gCityQ, sizeof(nm) - 1); nm[sizeof(nm) - 1] = 0; }
  asciiName(nm, sizeof(nm));
  portENTER_CRITICAL(&gWxMux);
  if (nm[0]) { strncpy(gWx.city, nm, sizeof(gWx.city) - 1); gWx.city[sizeof(gWx.city) - 1] = 0; }
  portEXIT_CRITICAL(&gWxMux);
  Serial.printf("[GEO] %s -> %.4f, %.4f (%s)\n", gCityQ, (double)gLat, (double)gLon, nm);
  return true;
}

static bool syncOnce() {
  if (gGeoPending) {
    if (!geocode()) { Serial.println("[NET] geocode failed"); return false; }
    gGeoPending = false;
    saveLoc();
  }

  char path[220];
  String js;

  snprintf(path, sizeof(path),
           "/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,weather_code,wind_speed_10m"
           "&hourly=precipitation_probability&forecast_hours=1&timezone=auto",
           (double)gLat, (double)gLon);
  int hs = -1;
  const int fetchMs = httpsGet("api.open-meteo.com", path, js, &hs);
  if (fetchMs < 0) { Serial.println("[NET] weather failed"); return false; }
  const int rtt = (hs > 0) ? hs : fetchMs;

  const char *cur = jsonVal(js.c_str(), "current");
  const float temp = jsonFloat(cur, "temperature_2m", -1.0f);
  const int code   = jsonInt(cur, "weather_code", -1);
  const float wf   = jsonFloat(cur, "wind_speed_10m", -1.0f);
  const int wind   = (wf < 0.0f) ? -1 : (int)(wf + 0.5f);
  const int rainP  = jsonInt(jsonVal(js.c_str(), "hourly"), "precipitation_probability", -1);

  // 空气质量单独一个域名，失败也不影响天气已经拿到的部分
  int pm25 = -1, aqi = -1;
  snprintf(path, sizeof(path),
           "/v1/air-quality?latitude=%.4f&longitude=%.4f&current=pm2_5,european_aqi",
           (double)gLat, (double)gLon);
  String aj;
  if (httpsGet("air-quality-api.open-meteo.com", path, aj) >= 0) {
    const char *c = jsonVal(aj.c_str(), "current");
    const float pf = jsonFloat(c, "pm2_5", -1.0f);
    pm25 = (pf < 0.0f) ? -1 : (int)(pf + 0.5f);
    aqi  = jsonInt(c, "european_aqi", -1);
  }

  portENTER_CRITICAL(&gWxMux);
  gWx.temp  = temp;
  gWx.code  = code;
  gWx.wind  = wind;
  gWx.rainP = rainP;
  if (pm25 >= 0) gWx.pm25 = pm25;
  if (aqi >= 0)  gWx.aqi  = aqi;
  gWx.rtt   = rtt;
  gWx.state = wxState(code, rainP);
  gWx.ok    = 1;
  gWx.stale = 0;
  gWx.stamp = millis();
  portEXIT_CRITICAL(&gWxMux);

  Serial.printf("[NET] %.1fC code=%d rain=%d%% pm2.5=%d aqi=%d hs=%dms fetch=%dms city=%s\n",
                (double)temp, code, rainP, pm25, aqi, rtt, fetchMs, gWx.city);
  return true;
}

static void netTask(void *arg) {
  (void)arg;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(500));
    const uint32_t now = millis();
    if (!gWifiUp) continue;              // NTP 没对上也能取数据，只是时钟先空着

    const bool due = gSyncReq ||
                     (gWx.ok ? (now - gWx.stamp > NET_PERIOD_MS)
                             : (now - gLastNetTry > 20000));
    if (!due) continue;

    gSyncReq = false;
    gLastNetTry = now;
    if (!syncOnce()) {
      portENTER_CRITICAL(&gWxMux);
      if (gWx.ok) gWx.stale = 1;      // 屏上留着旧值，打上 STALE
      portEXIT_CRITICAL(&gWxMux);
    }
    Serial.printf("[NET] heap=%u\n", (unsigned)ESP.getFreeHeap());
  }
}

/* --------------------------- 配网门户 --------------------------- */

static void portalSave() {
  String s = gSrv.arg("s2");
  if (s.length() == 0) s = gSrv.arg("s");
  String p = gSrv.arg("p");
  String c = gSrv.arg("c");
  if (c.length() == 0) c = "Shanghai";
  gPrefs.begin("odradek", false);
  gPrefs.putString("ssid", s);
  gPrefs.putString("pass", p);
  gPrefs.putString("city", c);
  gPrefs.putBool("geo", true);
  gPrefs.putString("place", "");       // 换了城市，旧的解析结果作废
  gPrefs.end();
  Serial.printf("[CFG] saved ssid=%s city=%s\n", s.c_str(), c.c_str());
  gSrv.send(200, "text/html",
            "<body style='background:#08131a;color:#7ae8e0;font-family:monospace;padding:16px'>"
            "<h3>CHIRAL LINK SAVED</h3><p>Rebooting...</p></body>");
  delay(1200);
  ESP.restart();
}

static void portalRoot() {
  String h = "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
             "<body style='background:#08131a;color:#7ae8e0;font-family:monospace;padding:16px'>"
             "<h2 style='letter-spacing:4px'>ODRADEK</h2>";
  h += "<form method=post action=/save>";
  h += "<p>NETWORK (2.4GHz only)<br><select name=s style='width:100%;padding:8px'>";
  const int n = WiFi.scanNetworks();
  for (int i = 0; i < n; i++) {
    h += "<option value=\"" + WiFi.SSID(i) + "\">" + WiFi.SSID(i) +
         " (" + String(WiFi.RSSI(i)) + " dBm)</option>";
  }
  WiFi.scanDelete();
  h += "</select></p>";
  h += "<p>PASSWORD<br><input name=p type=password style='width:100%;padding:8px'></p>";
  h += "<p>OR TYPE SSID MANUALLY<br><input name=s2 style='width:100%;padding:8px'></p>";
  h += "<p>CITY (english)<br><input name=c value=\"" + String(gCityQ) +
       "\" style='width:100%;padding:8px'></p>";
  h += "<p><button style='padding:10px 28px;background:#7ae8e0;border:0;color:#000'>LINK</button></p>";
  h += "</form></body>";
  gSrv.send(200, "text/html", h);
}

// 全零的结构体会显示成 0.0C / 0%，所以显式标成"没有数据"
static void initWx() {
  gWx.temp  = -999.0f;
  gWx.rainP = -1;
  gWx.code  = -1;
  gWx.wind  = -1;
  gWx.pm25  = -1;
  gWx.aqi   = -1;
  gWx.rtt   = -1;
  gWx.state = WX_NONE;
  gWx.ok    = 0;
  gWx.stale = 0;
  gWx.stamp = 0;
  gWx.city[0] = 0;
}

// 没配置过网络时盖在地图中央的提示。只在配网模式出现，
// 配好重启后底图会被重画一次，这块提示自然消失。
static void drawPortalHint() {
  tft.fillRect(MAP_X, 130, MAP_W, 54, C_BLACK);
  tft.drawRect(MAP_X, 130, MAP_W, 54, C_CY_D);
  drawCentered("CHIRAL LINK NOT SET", 137, C_AMBER, 1);
  drawCentered("JOIN WIFI", 152, C_GREY, 1);
  drawCentered("ODRADEK-SETUP", 162, C_CY, 1);
  drawCentered("192.168.4.1", 174, C_CREAM, 1);
}

static void startPortal() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("ODRADEK-SETUP");
  gDns.start(53, "*", WiFi.softAPIP());
  gSrv.on("/", portalRoot);
  gSrv.on("/save", portalSave);
  gSrv.onNotFound(portalRoot);
  gSrv.begin();
  gPortalUp = true;
  drawPortalHint();
  Serial.println("[CFG] portal: join wifi ODRADEK-SETUP, open http://192.168.4.1");
}

// STA 后来连上了（密码其实是错的、或者路由器只是刚才重启），
// 就把门户收掉、地图补回来，不然提示框会一直盖在中间。
static void stopPortal() {
  gSrv.stop();
  gDns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  gPortalUp = false;
  tft.fillRect(MAP_X, MAP_Y, MAP_W, MAP_H, C_BLACK);
  drawStaticMap();
  Serial.println("[CFG] portal closed, link up");
}

/* --------------------------- 网络状态机 --------------------------- */

static void loadNet() {
  gPrefs.begin("odradek", true);
  const String ssid = gPrefs.getString("ssid", "");
  const String pass = gPrefs.getString("pass", "");
  const String city = gPrefs.getString("city", "Shanghai");
  String place = gPrefs.getString("place", "");
  gLat = gPrefs.getFloat("lat", 31.2304f);
  gLon = gPrefs.getFloat("lon", 121.4737f);
  gGeoPending = gPrefs.getBool("geo", true);
  gPrefs.end();

  strncpy(gCityQ, city.c_str(), sizeof(gCityQ) - 1);
  gCityQ[sizeof(gCityQ) - 1] = 0;

  if (place.length() == 0) place = city;      // 还没解析过就先用用户填的
  strncpy(gWx.city, place.c_str(), sizeof(gWx.city) - 1);
  gWx.city[sizeof(gWx.city) - 1] = 0;
  asciiName(gWx.city, sizeof(gWx.city));
  gHasCreds = (ssid.length() > 0);

  if (!gHasCreds) { startPortal(); return; }

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid.c_str(), pass.c_str());
  Serial.printf("[WIFI] linking to %s\n", ssid.c_str());
}

static void startNtp() {
  configTime(8 * 3600, 0, "ntp.aliyun.com", "cn.pool.ntp.org", "pool.ntp.org");
}

static void netPoll(uint32_t now) {
  if (gPortalUp) {
    gDns.processNextRequest();
    gSrv.handleClient();
    if (WiFi.status() == WL_CONNECTED) stopPortal();
    return;
  }

  if (!gTimeOk && time(nullptr) > 1700000000L) {
    gTimeOk = true;
    Serial.println("[NTP] time synced");
  }

  const bool up = (WiFi.status() == WL_CONNECTED);
  if (up != gWifiUp) {
    gWifiUp = up;
    if (up) {
      Serial.printf("[WIFI] linked %s rssi=%d ip=%s\n", WiFi.SSID().c_str(),
                    WiFi.RSSI(), WiFi.localIP().toString().c_str());
      startNtp();
    } else {
      gWifiDownSince = now;
      Serial.println("[WIFI] link lost");
    }
  }

  // 连不上就别死等：45 秒后重新开门户，省得用户去长按 BOOT
  if (!up && gHasCreds) {
    if (now - gWifiDownSince > 45000UL) {
      Serial.println("[WIFI] no link 45s -> reopen portal");
      startPortal();
      return;
    }
    if (now - gLastReconnect > 15000UL) {
      gLastReconnect = now;
      WiFi.reconnect();
    }
  }
}

/* ============================ 物理按键 ============================ */

/*
 * 只有 BOOT（GPIO0）可以当普通按键用，RST 是硬件复位。
 *   短按 → 立刻同步一次外界数据
 *   长按（>1.5s）→ 清除 WiFi 配置并重启，重新进配网门户
 * GPIO0 上电时是烧录模式选择脚，所以只能在 setup 之后再当输入用。
 */
static uint8_t  gBtnRaw = 1;
static uint32_t gBtnEdge = 0, gBtnStart = 0;
static bool     gBtnFired = false;

void initButton() {
  pinMode(BTN_PIN, INPUT_PULLUP);
  gBtnRaw = (uint8_t)digitalRead(BTN_PIN);
}

void pollButton(uint32_t now) {
  const uint8_t raw = (uint8_t)(digitalRead(BTN_PIN) ? 1 : 0);
  if (raw != gBtnRaw) {
    gBtnRaw = raw;
    gBtnEdge = now;
    return;                                  // 20ms 内不认，防抖
  }
  if (now - gBtnEdge < 20) return;

  if (raw == 0) {
    if (gBtnStart == 0) gBtnStart = now;
    else if (!gBtnFired && now - gBtnStart > 1500) {
      gBtnFired = true;
      Serial.println("[BTN] long press -> clear wifi config, restart");
      gPrefs.begin("odradek", false);
      gPrefs.clear();
      gPrefs.end();
      tft.fillScreen(C_BLACK);
      drawTracked("CHIRAL LINK RESET", 20, 150, C_AMBER, 1);
      delay(900);
      ESP.restart();
    }
  } else {
    if (gBtnStart && !gBtnFired && now - gBtnStart > 40) {
      gSyncReq = true;
      Serial.println("[BTN] short press -> sync now");
    }
    gBtnStart = 0;
    gBtnFired = false;
  }
}

/* ================================= 主流程 ================================= */

/* ------------------------------ 开机动画 ------------------------------ */

static int textW(const char *s, uint8_t track) {
  int n = 0;
  for (const char *p = s; *p; ++p) n++;
  return n ? trackedWidth(n, track) : 0;
}

static void drawCentered(const char *s, int y, uint16_t col, uint8_t track) {
  drawTracked(s, (SCR_W - textW(s, track)) / 2, y, col, track);
}

// size 2 的标题逐字敲出来。必须固定左边界，逐字居中会左右乱跳。
static void drawTitleRun(const char *s, int n, int x, int y, uint16_t col) {
  tft.setTextFont(1);
  tft.setTextSize(2);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(col, C_BLACK);
  char one[2] = {0, 0};
  for (int i = 0; i < n && s[i]; i++) {
    one[0] = s[i];
    tft.drawString(one, x + i * 12, y);
  }
}

/*
 * 开罗尔脉冲：三圈环从中心往外推。
 * 每帧画三圈、擦上一帧的三圈，所以屏幕上永远只有三条线在走。
 * 这是整段动画里唯一会动的东西，也是死亡搁浅最标志性的扫描语汇。
 */
static void bootPulse(int cx, int cy) {
  int pr[3] = {-1, -1, -1};
  for (int r = 3; r <= 84; r += 2) {
    for (int k = 0; k < 3; k++)
      if (pr[k] > 0) tft.drawCircle(cx, cy, pr[k], C_BLACK);

    tft.drawCircle(cx, cy, r, C_CY_M);
    if (r > 12) tft.drawCircle(cx, cy, r - 12, C_CY_D);
    if (r > 24) tft.drawCircle(cx, cy, r - 24, C_CY_F);

    pr[0] = r;
    pr[1] = (r > 12) ? r - 12 : -1;
    pr[2] = (r > 24) ? r - 24 : -1;
    delay(5);
  }
}

/*
 * 开机序列（约 3.9 秒）：
 *   ① 开罗尔脉冲环      ② 两条导轨线对向扫出
 *   ③ BRIDGES 从中间长出来，再把字距拉开、压暗淡出
 *   ④ CHIRAL NETWORK 打字机 + 链路进度条
 *   ⑤ DEATH / STRANDING 逐字敲出 + 中线展开
 *   ⑥ 扫描线自上而下把画面擦干净，交给主界面
 */
void bootSplash() {
  tft.fillScreen(C_BLACK);

  // ① 脉冲
  bootPulse(SCR_W / 2, 158);
  tft.fillScreen(C_BLACK);

  // ② 导轨线：上一条从左扫出，下一条从右扫回
  for (int i = 0; i <= 85; i += 3) {
    tft.drawFastHLine(0, 112, i * 2, C_CY_D);
    tft.drawFastHLine(SCR_W - i * 2, 208, i * 2, C_SLATE);
    delay(5);
  }

  // ③ BRIDGES
  const char *brand = "BRIDGES";
  for (int k = 1; k <= 7; k++) {
    char tmp[8];
    memcpy(tmp, brand, k);
    tmp[k] = 0;
    tft.fillRect(0, 130, SCR_W, 12, C_BLACK);
    drawCentered(tmp, 134, C_CREAM, 4);
    delay(38);
  }
  for (int tr = 4; tr <= 9; tr++) {
    tft.fillRect(0, 130, SCR_W, 12, C_BLACK);
    drawCentered("BRIDGES", 134, C_CREAM, (uint8_t)tr);
    delay(34);
  }
  drawCentered("BRIDGES", 134, C_GREY, 9);
  delay(70);
  drawCentered("BRIDGES", 134, C_SLATE, 9);
  delay(70);
  tft.fillRect(0, 130, SCR_W, 12, C_BLACK);

  // ④ 链路
  const char *net = "CHIRAL NETWORK";
  const int nx = (SCR_W - textW(net, 1)) / 2;
  for (int k = 1; k <= 14; k++) {
    char tmp[20];
    memcpy(tmp, net, k);
    tmp[k] = 0;
    drawTracked(tmp, nx, 134, C_CY_M, 1);
    delay(24);
  }

  const int bx = 22, by = 164, bw = 126;
  tft.drawRect(bx, by, bw, 5, C_CY_D);
  int lastW = 0;
  for (int p = 0; p <= 100; p += 2) {
    const int w = (p * (bw - 2)) / 100;
    if (w > lastW) {
      tft.fillRect(bx + 1 + lastW, by + 1, w - lastW, 3, C_CY_M);
      lastW = w;
    }
    tft.fillRect(0, 174, SCR_W, 9, C_BLACK);
    char pct[8];
    snprintf(pct, sizeof(pct), "%03d%%", p);
    drawCentered(pct, 174, C_CREAM, 1);
    delay(9);
  }
  drawCentered("LINK ESTABLISHED", 174, C_CREAM, 1);
  delay(220);

  // ⑤ 标题
  tft.fillRect(0, 114, SCR_W, 86, C_BLACK);       // 114..199，两根导轨线保留

  const int dx = (SCR_W - 5 * 12) / 2;
  const int sx = (SCR_W - 9 * 12) / 2;
  for (int k = 1; k <= 5; k++) {
    drawTitleRun("DEATH", k, dx, 116, C_CREAM);
    delay(45);
  }
  delay(50);
  for (int k = 1; k <= 9; k++) {
    drawTitleRun("STRANDING", k, sx, 136, C_CREAM);
    delay(30);
  }

  for (int w = 2; w <= 108; w += 6) {
    tft.drawFastHLine((SCR_W - w) / 2, 158, w, C_CY_M);
    delay(12);
  }

  const char *sub = "ODRADEK SYSTEM";
  const int sx2 = (SCR_W - textW(sub, 1)) / 2;
  for (int k = 1; k <= 14; k++) {
    char tmp[20];
    memcpy(tmp, sub, k);
    tmp[k] = 0;
    drawTracked(tmp, sx2, 170, C_GREY, 1);
    delay(18);
  }
  drawCentered("TERMINAL CUFF v1.3", 184, C_CY_D, 0);
  delay(180);

  // ⑥ 扫描线擦除
  for (int y = 0; y < SCR_H + 14; y += 14) {
    if (y >= 14) tft.fillRect(0, y - 14, SCR_W, 14, C_BLACK);
    if (y < SCR_H) tft.drawFastHLine(0, y, SCR_W, C_CY_D);
    delay(10);
  }
  tft.fillScreen(C_BLACK);
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("[BOOT] Death Stranding Odradek Topo Scanner v1.3");

  tft.init();
  tft.setRotation(0);
  tft.fillScreen(C_BLACK);
  Serial.printf("[TFT] %d x %d\n", tft.width(), tft.height());

  buildTerrain();
  buildFineField();
  buildPalette();
  buildRoute();
  Serial.printf("[MAP] field %dx%d -> %dx%d, bands=%d, lines every %d\n",
                MB_W, MB_H, FW, FH, NBAND, LSTEP);

  bootSplash();
  Serial.printf("[BOOT] splash done at %lu ms\n", (unsigned long)millis());
  tft.fillScreen(C_BLACK);

  drawTopBar();
  drawMapFrame();
  drawBottomBar();
  drawStaticMap();          // 底图只在这里画一次，之后永不重建

  initButton();
  initWx();
  loadNet();                // 有配置就联网，没有就开配网门户
  xTaskCreatePinnedToCore(netTask, "net", 10240, nullptr, 1, nullptr, 0);
}

void loop() {
  const uint32_t now = millis();
  if (now - lastFrame < 33) return;          // 30 FPS 节流
  lastFrame = now;

  netPoll(now);              // 配网门户 / 断线重连 / NTP 对时
  pollButton(now);
  updateReadouts(now);

  fpsCount++;
  if (now - fpsTimer >= 1000) {
    gFps = (int)fpsCount;
    fpsCount = 0;
    fpsTimer = now;
  }

  if (now - lastLog >= 1000) {
    lastLog = now;
    WxData w;
    portENTER_CRITICAL(&gWxMux);
    w = gWx;
    portEXIT_CRITICAL(&gWxMux);

    char clock[16] = "--:--:--";
    if (gTimeOk) {
      const time_t t = time(nullptr);
      struct tm lt;
      localtime_r(&t, <);
      snprintf(clock, sizeof(clock), "%02d:%02d:%02d", lt.tm_hour, lt.tm_min, lt.tm_sec);
    }

    Serial.printf("[HUD] fps=%d wifi=%s %s t=%.1fC rain=%d%% code=%d pm25=%d aqi=%d rtt=%dms%s heap=%u\n",
                  gFps, gPortalUp ? "PORTAL" : (gWifiUp ? "UP" : "DOWN"), clock,
                  (double)w.temp, w.rainP, w.code, w.pm25, w.aqi, w.rtt,
                  w.stale ? " STALE" : "", (unsigned)ESP.getFreeHeap());
  }
}
