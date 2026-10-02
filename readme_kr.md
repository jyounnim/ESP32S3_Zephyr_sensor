**한국어** | [English](./README.md)

# Zephyr Multi-Sensor 예제 시리즈 (ESP32-S3, 듀얼코어 + IPM)

ESP32-S3-DevKitC-1 + Zephyr RTOS 기반으로, I2C 센서를 하나씩 새로 붙여가며
학습하는 예제 모음입니다. 다루는 센서는 매번 바뀌지만, **핵심 아키텍처는
모든 랩에서 동일**합니다 - 센서 읽기와 디스플레이 출력을 ESP32-S3의 두
코어에 각각 나눠서 맡기는 구조입니다.

## 핵심 아키텍처: 코어별 역할 분담

ESP32-S3는 물리적으로 코어가 두 개(procpu, appcpu)입니다. Zephyr는 이 둘을
SMP(하나의 커널이 코어 두 개를 함께 스케줄링)가 아니라 **AMP** - 완전히
독립된 두 개의 Zephyr 이미지로 다룹니다. 이 시리즈는 이 구조를 그대로
활용해 역할을 명확히 나눕니다.

```
        core0 (procpu)                         core1 (appcpu)
   ─────────────────────                  ─────────────────────
   센서 읽기 전담                          디스플레이 전담
   I2C0 (SDA=GPIO8, SCL=GPIO9)             I2C1 (SDA=GPIO4, SCL=GPIO5)
        │                                        │
   센서 스레드가 주기적으로 값을 읽고               SSD1306 OLED에 값을 표시
   변화가 크면 즉시, 아니면 주기적으로               (event-driven display thread)
   IPM으로 core1에 전송                            │
        └──────────── IPM (inter-processor mailbox) ────────────┘
                        (channel 2, ipm_send / ipm_callback)
```

- **core0 (procpu) — 센서 읽기 전담**: 센서마다 자기 스레드에서 주기적으로
  값을 읽습니다. 값이 임계값 이상 변하면 그 즉시 `ipm_send()`로 core1에
  전송하고, 값이 안정적이어도 1초마다 heartbeat로 한 번씩 전송해 core1이
  "링크가 살아있다"고 판단할 근거를 유지합니다.
- **core1 (appcpu) — 디스플레이 전담**: IPM 콜백(인터럽트 컨텍스트)이
  수신한 값을 메시지 큐에 넣기만 하고, 별도 디스플레이 스레드가 그 큐를
  소비해 SSD1306 OLED를 갱신합니다. 일정 시간(보통 2초) 응답이 없으면
  화면에 링크 끊김을 표시합니다.

**왜 코어를 나누고 I2C 버스까지 물리적으로 분리하는가**: Zephyr에서
core0/core1은 완전히 독립된 두 이미지(AMP)이기 때문에, 같은 I2C 컨트롤러를
두 이미지가 동시에 건드리는 건 위험합니다. 그래서 센서는 항상 I2C0(core0
전담), 디스플레이는 항상 I2C1(core1 전담)에 물려 이 문제를 구조적으로
피합니다.

## 공통 관례

- **디렉토리 구조**: 각 랩은 `NN_모듈명_칩명/` 형태로 독립된 폴더를 가지며,
  내부는 다음과 같이 통일돼 있습니다.
  ```
  NN_모듈명_칩명/
  ├── README_kr.md / README.md             랩 설명 문서 (한국어 / 영어)
  ├── doc/                                  트러블슈팅 문서 (_kr.md / _en.md)
  ├── lab/                                  메인 애플리케이션 (west build --sysbuild)
  │   ├── src/                              core0(procpu) 소스
  │   ├── remote/src/                       core1(appcpu) 소스
  │   ├── boards/*.overlay                  core0 오버레이
  │   ├── remote/boards/*.overlay           core1 오버레이
  │   ├── sysbuild.cmake / sysbuild.conf
  │   └── prj.conf / remote/prj.conf
  └── lab_tools/i2c0_scanner/               새 I2C 디바이스 주소 확인용 진단 도구 (Lab 01~03)
  ```
- **빌드**:
  ```
  west build -p always --sysbuild -b esp32s3_devkitc/esp32s3/procpu <랩경로>/lab
  west flash
  ```
- **sysbuild + MCUboot 주의사항**: Espressif 보드는 `--sysbuild` 사용 시
  기본적으로 MCUboot를 같이 빌드하려 시도합니다. 이 시리즈는 OTA와 무관한
  순수 2-이미지(procpu+appcpu) 구성이므로, 각 랩의 `lab/sysbuild.conf`에
  `SB_CONFIG_BOOTLOADER_NONE=y`를 명시해 끕니다.
- **IPM 채널**: Lab 01~03은 모든 스냅샷을 ID 2로 전송 (0/1은 플랫폼 예약,
  2/3이 애플리케이션 여유분). Lab 04는 메시지 종류가 여러 개라 payload 안의
  명령 코드(`ipm_msg_t.cmd`)로 구분합니다.
- **새 센서 배선 시**: 오버레이에 주소를 하드코딩하기 전에
  `lab_tools/i2c0_scanner`로 실제 응답 주소부터 확인합니다 - datasheet
  기본 주소와 실제 모듈의 결선이 다른 경우가 흔합니다.
- **문서 작성 흐름**: 실기 검증 전에는 한글 문서만 작성하고, 실기 검증이
  끝나면 문서를 정리하면서 영문 버전을 함께 만듭니다. 모든 문서 상단에
  한/영 전환 링크가 있습니다.

## 진행 중인 랩 목록

**이 시리즈는 계속 확장됩니다** - 센서가 하나씩 늘어날 때마다 새 랩이
추가될 예정입니다. 아래 표는 지금까지 진행한 랩 현황입니다.

| # | 폴더 | 모듈 / 칩 | 센서 종류 | 상태 |
| --- | --- | --- | --- | --- |
| 01 | [`01_AHT20_BMP280_MultiSensor`](./01_AHT20_BMP280_MultiSensor/README_kr.md) | AHT20 + BMP280 | 온습도 + 기압 (센서 2개가 I2C0 버스 하나를 공유) | ✅ 실기 검증 완료 |
| 02 | [`02_GY521_MPU6050`](./02_GY521_MPU6050/README_kr.md) | GY-521 (MPU-6050) | 6축 IMU (3축 가속도 + 3축 자이로 + 온도) | ✅ 실기 검증 완료 |
| 03 | [`03_HW664_LIS3DH`](./03_HW664_LIS3DH/README_kr.md) | HW-664 (실제 칩: LIS3DH — 판매 페이지엔 LIS3DSH로 표기돼 있었으나 WHO_AM_I로 정정 확인) | 3축 가속도계 | ✅ 실기 검증 완료 |
| 04 | [`04_ESP32S3_PowerMode_Lab`](./04_ESP32S3_PowerMode_Lab/README_kr.md) | HW-664 (LIS3DH) + SSD1306 전원 게이팅 | 전력 모드: core0 = 상시 센싱(AON), core1 = 디스플레이 전원 제어 (Normal / Sleep / Ultra-Sleep) | ✅ 실기 검증 완료 |
| 05 | [`05_HLKLD2402_Radar`](./05_HLKLD2402_Radar/README_kr.md) | HLK-LD2402 (UART 레이더) + SSD1306 | 거리/presence/방향 감지, 16-gate 에너지 바 그래프 디스플레이 (core0 = AON 레이더 파싱, core1 = 디스플레이) | ✅ 실기 검증 완료 |

각 랩 폴더의 `README_kr.md`에 설계/배선/빌드/예상 결과가 있고, 트러블슈팅
문서는 각 랩의 `doc/` 폴더에 있습니다.

## 새 랩을 시작할 때 체크리스트

1. 새 센서를 배선한다.
2. `lab_tools/i2c0_scanner`로 실제 응답 주소를 먼저 확인한다 (오버레이
   작성 전).
3. Zephyr 공식 드라이버 지원 여부를 확인한다 - 있으면 표준
   `sensor_sample_fetch()`/`sensor_channel_get()` API, 없으면 SSD1306처럼
   raw I2C 레지스터 접근 기반 커스텀 드라이버.
4. Lab 01~03에서 확립된 패턴(듀얼코어 + IPM + 임계값 즉시전송 + heartbeat)을
   그대로 적용한다.
5. KR 문서 먼저 작성 → 실기 검증 → 문서 정리 + EN 번역.

## 환경

- 보드: ESP32-S3-DevKitC-1 (N16R8, 16MB Flash / 8MB PSRAM)
- Zephyr: v4.4.0
- 빌드 시스템: west + sysbuild
