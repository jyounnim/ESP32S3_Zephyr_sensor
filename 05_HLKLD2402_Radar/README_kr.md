**한국어** | [English](./README.md)

# ESP32-S3 Dual-core AMP: HLK-LD2402 레이더 + SSD1306 디스플레이 (Zephyr)

> 상태: **실기 검증 완료 (2026-10-02)**. host 문법 검사(gcc, Zephyr API stub) + LD2402 parser 단위 테스트를 거쳐, ESP32-S3-DevKitC-1 + HLK-LD2402 + SSD1306 실제 하드웨어에서 7절의 검증 체크리스트까지 확인했습니다.
> 센서 프로토콜 상세는 저장소 루트의 [`HLK-LD2402_sensor_research_kr.md`](../HLK-LD2402_sensor_research_kr.md)와 Hi-Link 공식 사용자 설명서(`HLK-LD2402_User_Manual.pdf`, 저장소 루트)를 참고하십시오.

## 1. 목표

- **Core 0 (PRO_CPU)**: LD2402 UART 수신, 프레임 파싱, engineering mode 설정을 맡는 **센서 전용 코어**
- **Core 1 (APP_CPU)**: SSD1306에 상태·거리·16 gate 에너지 바 그래프를 그리는 **디스플레이 전용 코어**
- 두 코어는 서로 독립된 Zephyr 이미지(AMP)이며, soft IPM으로 56 B 메시지를 주고받습니다.

기존 multi-sensor AMP Lab 01~04(core0 센서 / core1 SSD1306 / IPM / LINK 표시)와 같은 구조를 LD2402로 확장한 예제입니다.

## 2. 아키텍처

```
 LD2402 ──UART1 115200──▶ [PRO_CPU] ld2402.c ─ parser ─▶ sensor_main.c
                                                         │ radar_msg (56B)
                                                         ▼ ipm_send()
                                             ipm0 shared memory (512B/방향)
                                                         │ FROM_CPU IRQ
                                                         ▼
                           [APP_CPU] ISR copy → k_msgq → display_main.c → radar_display_ssd1306.c → ssd1306_display.c (raw I2C) → SSD1306 (I2C1)
```

| 항목 | PRO_CPU (sensor) | APP_CPU (display) |
|---|---|---|
| Board target | `esp32s3_devkitc/esp32s3/procpu` | `esp32s3_devkitc/esp32s3/appcpu` (sysbuild가 자동 빌드) |
| 주변장치 | UART1 (GPIO17 TX / GPIO18 RX) | I2C1 (GPIO4 SDA / GPIO5 SCL) |
| IPC | `radar_link_send()` → `ipm_send()` | `ipm_register_callback()` → `k_msgq` |
| 콘솔 | UART0 (USB) | 없음 (화면이 출력 수단) |

### 송신 정책 (sensor core)

| 조건 | 동작 |
|---|---|
| presence / flags / **direction** 변화 | 즉시 전송 |
| 거리 변화 ≥ 10 cm | 즉시 전송 |
| 그 외 | 500 ms마다 전송 (바 그래프 갱신 겸 heartbeat) |
| 2초간 레이더 프레임 없음 | `RADAR_FLAG_SENSOR_OK` 해제 → 화면 `NO SENSOR` |

display core는 3초간 메시지가 없으면 `LINK: FAIL`을 표시합니다.

### IPM 사용 시 주의점 (`drivers/ipm/ipm_esp32.c` 기준)

- receive callback은 **ISR context**에서 호출되며, payload size가 아니라 shared memory pointer만 넘겨받습니다. 그래서 메시지에 `magic`을 넣어 유효성을 검사합니다.
- 다음 `ipm_send()`가 같은 버퍼를 덮어쓰므로, 수신 측은 callback 안에서 즉시 복사해야 합니다. 이 예제의 송신 주기는 165 ms 이상이라 overwrite race가 생기지 않습니다.
- `CONFIG_IPM=y`를 켜면 PRO_CPU가 APP_CPU 이미지를 로드하고 기동합니다(`SOC_ENABLE_APPCPU`). `CONFIG_ESP32_SOFT_IPM`은 devicetree에서 자동으로 선택됩니다.

## 3. 준비물 및 배선

| LD2402 | ESP32-S3-DevKitC-1 |
|---|---|
| V | 3V3 |
| G | GND |
| T (TX) | GPIO18 (UART1 RX) |
| R (RX) | GPIO17 (UART1 TX) |
| IO | 미사용 |

Boot 버튼(GPIO0)은 보드 내장 버튼을 그대로 사용합니다(자동 threshold 실행용).

| SSD1306 128×64 (I2C) | ESP32-S3-DevKitC-1 |
|---|---|
| VCC / GND | 3V3 / GND |
| SDA | GPIO4 (I2C1) |
| SCL | GPIO5 (I2C1) |

<img width="426" height="403" alt="image" src="https://github.com/user-attachments/assets/76a3621d-6bd8-421b-a7dd-c9e0946a1250" />

- 두 장치 모두 3.3 V IO이므로 레벨 시프터가 필요 없습니다.
- LD2402는 평균 50 mA를 소모합니다. DevKit 3V3 레일로 충분하지만, 가능하면 100 µF 디커플링 커패시터를 추가하십시오.
- SSD1306 주소는 raw driver가 부팅 시 0x3C → 0x3D 순서로 자동 probe하므로 overlay에 따로 적지 않습니다.

## 4. 폴더 구조

```
05_HLKLD2402_Radar/
├── README_kr.md                          (한국어)
├── README.md                             (영어)
└── lab/                                  ← PRO_CPU (sensor) 앱 + sysbuild 진입점
    ├── CMakeLists.txt  Kconfig  prj.conf
    ├── sysbuild.cmake  sysbuild.conf     ← APP_CPU 이미지 추가, MCUboot 끔
    ├── boards/esp32s3_devkitc_esp32s3_procpu.overlay
    ├── common/   radar_msg.h  radar_link.h  radar_link_ipm.c
    ├── src/      sensor_main.c  ld2402.c/h  radar_trend.c/h  radar_filter.c/h
    └── remote/                           ← APP_CPU (display) 앱
        ├── CMakeLists.txt  prj.conf
        ├── boards/esp32s3_devkitc_esp32s3_appcpu.overlay
        └── src/  display_main.c  radar_display.h  radar_display_ssd1306.c
                  ssd1306_display.c/h  font5x7.h   ← multi-sensor AMP Lab 01 검증 드라이버
```

`sensor_main.c`, `display_main.c`, `ld2402.*`, `radar_trend.*`, `radar_filter.*`, `radar_display.h`, `radar_msg.h`, `radar_link.h`는 **SR110 프로젝트와 파일 내용이 완전히 같습니다.** 플랫폼별로 다른 파일은 IPC(`radar_link_ipm.c`), 디스플레이 backend(`radar_display_ssd1306.c`), overlay, conf입니다.

## 5. 코드 핵심

### 5.1 `ld2402.c`: 3가지 포맷을 처리하는 단일 state machine

```
ST_SYNC ─(window==F4F3F2F1)─▶ LEN ─▶ PAYLOAD ─▶ FOOTER(F8F7F6F5) ─▶ EVT_REPORT
        ─(window==FDFCFBFA)─▶ LEN ─▶ PAYLOAD ─▶ FOOTER(04030201) ─▶ EVT_ACK
        ─('O' or 'd')──────▶ ST_ASCII ─('\r'/'\n')─▶ "OFF" / "distance:N" ─▶ EVT_REPORT
```

- 수신은 UART RX IRQ → `ring_buf` → `k_sem` 경로로 처리하며, ISR은 생산만 하고 파싱은 sensor thread에서 수행합니다.
- `CONFIG_LD2402_RX_POLLING=y`를 켜면 `uart_poll_in()` 기반 polling으로 바뀝니다(디버그용 fallback).
- `ld2402_enter_engineering_mode()`는 enable(0x00FF) → version(0x0000) → mode(0x0012) → end(0x00FE) 순서로 진행합니다. 중간에 실패해도 **end config는 항상 보냅니다.** 그렇지 않으면 레이더가 보고를 멈춥니다.

### 5.2 `radar_msg.h`: 코어 간 계약

56 B 고정 크기이고 padding이 없습니다(`_Static_assert`로 검사). magic `"LD24"`, seq, uptime, distance, presence, flags, motion_db[16], micro_db[16]로 구성됩니다.

### 5.3 `radar_display_ssd1306.c`: 화면 (raw I2C SSD1306 driver)

ESP32-S3에서는 Zephyr in-tree `solomon,ssd1306` + CFB를 **사용하지 않습니다.** in-tree driver는 control byte와 pixel payload를 별도 I2C message로 보내는데, 이것이 이미 확인된 ESP32 I2C driver 문제를 그대로 재현해 화면 노이즈를 만듭니다. 그래서 multi-sensor AMP Lab 01에서 하드웨어 검증된 `ssd1306_display.c`(control byte와 framebuffer를 한 번의 `i2c_write()`로 전송)를 재사용했습니다. 여기에 이 Lab용으로 다음을 추가했습니다.

- `ssd1306_set_pixel()`, `ssd1306_draw_hline()`, `ssd1306_draw_vline()`: 바 그래프와 화살표용
- `font5x7.h` glyph 5개: `+`, `/`, `c`, `m`, `s`

화면 레이아웃 (5×7 font, page = 8 px):

```
┌────────────────────────────┐
│LINK: OK                    │  page0  (항상 최상단)
│                            │
│MOVE   123cm           ◀──  │  page2  상태+거리, x112~127 방향 화살표
│APPROACH -96cm/s            │  page3  방향+속도 (판정 전에는 비움)
│    ▔▔                      │  y=32   target gate marker
│▁▁ ▃▃ ▇▇ ▅▅ ▂▂ ...          │  y=34~63 16 gate motion dB
└────────────────────────────┘
```

SR110 버전은 같은 `radar_display.h` API를 CFB(10×16 font)로 구현합니다.

### 5.4 `radar_trend.c`: 접근 / 이탈 판정 (sensor core)

LD2402는 1T1R이라 각도도 속도도 보고하지 않습니다. 그래서 **거리의 시간 변화(radial speed)**로 접근과 이탈을 추정합니다.

1. 최근 8개 샘플(약 1.3초)의 (시각, 거리)로 **최소제곱 기울기**를 구합니다. 단위는 cm/s이고, 음수면 접근입니다.
2. hysteresis를 적용해 분류합니다.

| 현재 상태 | 진입 조건 | 유지 조건 |
|---|---|---|
| APPROACH | speed ≤ −20 cm/s | speed < −10 cm/s |
| LEAVE | speed ≥ +20 cm/s | speed > +10 cm/s |
| STEADY | 그 외 | |

3. 새 방향이 **연속 3회** 나와야 표시를 바꿉니다. ±15 cm 거리 노이즈 때문에 화살표가 깜빡이는 것을 막는 debounce입니다.
4. 다음 경우에는 window를 리셋하고 `UNKNOWN`으로 돌아갑니다. 대상이 사라졌을 때, 600 ms 이상 프레임이 끊겼을 때, 한 스텝에 100 cm 넘게 거리가 튀었을 때(다른 사람으로 대상이 바뀐 것으로 간주)입니다.
5. 샘플이 5개(약 0.8초) 모이기 전에는 판정하지 않습니다.

방향이 바뀌면 즉시 전송 조건에 해당하므로, 화면 반응 지연은 대략 0.5~1.3초입니다.

**host 시뮬레이션 결과** (165 ms 주기, 거리에 ±15 cm 균등 노이즈를 넣고 seed 4종으로 실행)

| 시나리오 | 결과 |
|---|---|
| 걸어서 접근 −100 cm/s | APPROACH, 추정 −103 cm/s |
| 걸어서 이탈 +80 cm/s | LEAVE, 추정 +79 cm/s |
| 천천히 접근 −30 cm/s | APPROACH, 추정 −20 ~ −48 cm/s |
| 앉아 있음 20초 | STEADY 유지, 오판정 0회 |
| 거리 점프 / 대상 소멸 | UNKNOWN으로 리셋 |

한계도 있습니다. 좌/우 이동처럼 거리가 변하지 않는 움직임은 STEADY로 나옵니다. 또 여러 사람이 있으면 레이더가 보고하는 대상이 바뀔 때마다 리셋됩니다.

화살표 방향은 바 그래프와 맞췄습니다. gate 0(센서 쪽)이 왼쪽에 있으므로 접근은 `◀──`, 이탈은 `──▶`, 정지는 짧은 `─`로 표시합니다.

### 5.5 레이더 튜닝: 파라미터 출력 · 자동 threshold · median filter

보드 Boot 버튼으로 하는 자동 threshold(5.5.2) 외에, Hi-Link 공식 PC용 "HLK-LD2402 Tool"로도 같은 설정을 튜닝할 수 있습니다 — 자세한 사용법/다운로드 링크는 [트러블슈팅 문서](./doc/05_HLKLD2402_Radar_TROUBLESHOOTING_kr.md#pc용-hlk-ld2402-tool로-튜닝하기-보드-코드-수정-없이) 참고.

### 5.5.1 부팅 시 레이더 파라미터 출력

startup config 세션(enable → version → **파라미터 읽기** → engineering mode → end)에서 현재 레이더 설정을 콘솔에 출력합니다. threshold는 선형값을 dB로 바꿔 보여주므로, 화면 바 그래프(gate 에너지 dB)와 직접 비교할 수 있습니다.

```
[SENSOR] LD2402 firmware v3.5.5
[PARAM] max distance      : 5.0 m (raw 50)
[PARAM] disappear delay   : 30 s
[PARAM] power interference: none
[PARAM] gate     :  0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15
[PARAM] motion dB: 50 53 54 56 57 57 58 59 59 60 60 60 61 61 61 62
[PARAM] micro dB : 30 33 34 36 37 37 38 39 39 40 40 40 41 41 41 42
[PARAM] gate n = 70*n .. 70*(n+1) cm; threshold dB = 10*log10(raw)
[SENSOR] engineering mode ON
```
(값은 예시입니다)

**읽는 법:** 빈 방에서 바 그래프의 특정 gate 에너지가 그 gate의 motion threshold보다 높으면, 그 gate 거리에 사람이 있는 것으로 판정됩니다. 예를 들어 사람이 없는데 gate 2(140~210 cm)가 threshold를 넘으면 거리가 **항상 150 cm 근처**로 나옵니다.

### 5.5.2 자동 threshold 생성 (Boot 버튼 2초 길게 누르기)

| 단계 | sensor core | 화면 |
|---|---|---|
| 1 | Boot 버튼(GPIO0) 2초 이상 누름 감지 (main loop에서 polling) | |
| 2 | 10초 카운트다운: 이 동안 감지 영역에서 **벗어나야** 합니다 | `AUTO THRESHOLD` / `LEAVE THE AREA` / `START IN 10s` |
| 3 | config 진입 → `0x0009` (계수 trigger/hold/micro = 3.0) | |
| 4 | 1초마다 `0x000A`로 진행률 조회 (timeout 120 s) | `CALIBRATING 40%` + progress bar |
| 5 | `0x00FD`로 레이더 flash에 저장. 응답이 없으면 0x003F 재기록 방식으로 fallback | |
| 6 | 새 파라미터를 다시 읽어 콘솔 출력 → engineering mode 재설정 → config 종료 | `CALIB DONE` / `SAVED TO RADAR` (3초) |

- 저장은 **레이더 모듈 자체의 flash**에 됩니다. ESP32를 다시 flash하거나 전원을 껐다 켜도 유지됩니다.
- 계수 3.0은 HLK 공식 Tool의 기본값입니다(범위 1.0~20.0). 계수를 키우면 threshold가 배경 대비 더 높게 잡혀 오검출은 줄지만 감도가 떨어집니다. `sensor_main.c`의 `CALIB_COEFF_X10`으로 조정합니다.
- 진행 중에는 레이더가 config mode라 거리 보고가 멈춥니다. 대신 상태 메시지를 계속 보내므로 화면이 `LINK: FAIL`로 바뀌지 않습니다.
- 버튼 기능은 devicetree alias `calib-button`이 있을 때만 컴파일됩니다.

### 5.5.3 거리 median filter

`radar_filter.c`는 최근 5개 샘플(약 0.8초)의 median을 냅니다. 평균과 달리 한 프레임만 튀는 값(벽 반사로 잡힌 프레임 등)은 결과에 섞이지 않고 버려집니다. 지연은 약 2프레임(330 ms)입니다.

- 화면, 접근/이탈 판정, IPC 메시지에는 filter를 거친 거리를 씁니다. 콘솔에는 비교용으로 `dist=152cm (raw 420)`처럼 raw 값을 함께 출력합니다.
- 대상이 사라지거나 센서가 끊기면 filter를 리셋합니다.

**host 테스트**: 입력 `150 152 420 149 151 153 0 150` → 출력 `150 152 152 152 151 152 151 150`. 420과 0 spike가 제거됩니다.

## 6. 빌드 및 실행

```powershell
cd D:\work\ESP32-S3\zephyrproject
west build -p always -b esp32s3_devkitc/esp32s3/procpu --sysbuild <path>\05_HLKLD2402_Radar\lab
west flash
west espressif monitor
```

`sysbuild.conf`의 `SB_CONFIG_BOOTLOADER_NONE=y` 덕분에 MCUboot 없이 두 이미지가 함께 flash됩니다. `sysbuild.cmake`는 multi-sensor AMP Lab과 같은 형식(remote board target을 명시)을 따릅니다.

### 예상 콘솔 출력 (PRO_CPU)

```
=== LD2402 AMP: sensor core (esp32s3_devkitc/esp32s3/procpu) ===
[SENSOR] LD2402 firmware v3.5.5, engineering mode ON
[SENSOR] NONE  dist=  0cm -          +0cm/s peak=24dB@g0  | sent=3 bin=6 txt=0 bad=0 ovf=0
[SENSOR] MOVE  dist=142cm APPROACH  -96cm/s peak=58dB@g2  | sent=9 bin=12 txt=0 bad=0 ovf=0
[SENSOR] STILL dist=138cm STEADY     +3cm/s peak=41dB@g1  | sent=11 bin=18 txt=0 bad=0 ovf=0
```

## 7. 실보드 검증 체크리스트

| # | 확인 항목 | 기대 결과 |
|---|---|---|
| 1 | 부팅 직후 화면 | `LINK: WAIT` → 1초 이내 `LINK: OK` |
| 2 | 콘솔 firmware 로그 | `engineering mode ON` (실패 시 5회 재시도 후 ASCII fallback) |
| 3 | `[PARAM]` 로그 | max distance, delay, 16 gate threshold 출력 |
| 3-1 | `bin` 카운터 | 약 6/s 증가, `bad`·`ovf`는 0 유지 |
| 4 | 센서 앞에서 걷기 | `MOVE` + 거리 변화, 해당 gate 바 상승 |
| 5 | 가만히 앉기 | 수 초 후 `STILL` |
| 6 | 센서 쪽으로 걸어오기 / 멀어지기 | 약 1초 내 `◀──` / `──▶`, 콘솔 speed 부호 일치 |
| 7 | 걷다가 멈추기 | 약 1초 후 `─` (STEADY) |
| 8 | 자리 이탈 | 소멸 지연시간(파라미터 0x0004) 후 `NONE`, 화살표 사라짐 |
| 9 | Boot 버튼 2초 → 10초 안에 영역 이탈 | `CALIBRATING n%` → `CALIB DONE`, 콘솔에 새 threshold 출력, 재부팅 후에도 유지 |
| 10 | LD2402 TX선 분리 | 2초 후 `NO SENSOR` |
| 11 | PRO_CPU 정지(디버거) | 3초 후 `LINK: FAIL` |

## 8. 트러블슈팅

이 실습을 실기에서 검증하며 마주칠 수 있는 배선/통신 증상과 점검 방법은 별도 문서 [`doc/05_HLKLD2402_Radar_TROUBLESHOOTING_kr.md`](./doc/05_HLKLD2402_Radar_TROUBLESHOOTING_kr.md)에 정리했습니다. threshold를 PC에서 직접 튜닝하는 "HLK-LD2402 Tool" 사용법도 그 문서에 포함되어 있습니다.
