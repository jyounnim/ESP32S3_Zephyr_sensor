**한국어** | [English](./README.md)

# Lab 04: ESP32-S3 Zephyr RTOS Power Mode 실습 (HW-664 + SSD1306, 듀얼코어 AMP)

> `zephyr_sensor` 시리즈 네 번째 랩. ESP32-S3-DevKitC-1 기준, 실기 검증 완료.
> 실기 검증 중 겪은 이슈와 해결 과정: [트러블슈팅 문서](./doc/04_ESP32S3_PowerMode_TROUBLESHOOTING_kr.md)

이 문서는 ESP32-S3의 듀얼코어(AMP) 환경에서 Zephyr RTOS의 전원 관리 개념을 코어 간 통신(IPM)과 결합하여, 실제로 손에 잡히는 저전력 시나리오로 구현하는 실습 가이드입니다. 이 문서만 보고도 처음부터 끝까지 따라 할 수 있도록, 다른 실습에서 이미 다룬 내용도 필요한 만큼 다시 설명합니다.

---

## 1. 실습 목적

- **ESP32-S3 하드웨어 전력 모드와 Zephyr RTOS PM subsystem의 관계를 정확히 이해**하고, 실습을 통해 "Zephyr에서 실제로 쓸 수 있는 것"과 "칩 데이터시트 상의 이론"의 차이를 체감합니다.
- **AMP 구조에서의 저전력 동기화**: PRO_CPU(core0)와 APP_CPU(core1)가 IPM(Inter-Processor Messaging)으로 전원 상태를 공유하는 방법을 익힙니다.
- **하드웨어 전원 차단(Power Gating)**: GPIO로 외장 페리페럴(OLED)의 전원 자체를 끊는 방법을 구현합니다.
- **센서 이벤트 기반 Wake-up**: 가속도 센서(HW-664 / LIS3DH)를 폴링하며 급격한 변화(Threshold)를 감지해 절전 모드에서 자동으로 복귀하는 시나리오를 구현합니다.

---

## 2. 준비물

### 2.1 하드웨어

| 부품 | 비고 |
| --- | --- |
| ESP32-S3-DevKitC-1 | 이 시리즈의 기본 보드 |
| HW-664 가속도 센서 모듈 | 모듈 표기는 LIS3DSH이지만, 실제 칩은 **LIS3DH**입니다 ([Lab 03](../03_HW664_LIS3DH/README_kr.md) 참고) |
| SSD1306 128x64 OLED (I2C) | 4핀(VCC/GND/SDA/SCL) 모듈 |
| OLED 전원 게이팅용 스위치 | P-MOSFET(하이사이드) 또는 로드스위치 IC 권장 - 5.3절 참고 |
| 브레드보드 / 점퍼선 | |

### 2.2 빌드 환경

- **west**: 이 문서의 모든 빌드 명령은 west 기준입니다.
- **Zephyr IDE (VS Code 확장)**: west 워크스페이스를 그대로 열면 되고, 이 실습만을 위한 별도 설정은 필요 없습니다.
- Zephyr RTOS v4.4+ (west 기반), Zephyr SDK (Xtensa 툴체인), `west blobs fetch hal_espressif`로 받은 Espressif HAL 바이너리

---

## 3. 핀 맵 (Pin Mapping)

| 기능 / 디바이스 | ESP32-S3 핀 | 연결 대상 | 담당 코어 | 비고 |
| --- | --- | --- | --- | --- |
| I2C0 SDA | GPIO8 | HW-664 SDA | core0 | 이 시리즈 I2C0 공통 관례 |
| I2C0 SCL | GPIO9 | HW-664 SCL | core0 | |
| I2C1 SDA | GPIO4 | SSD1306 SDA | core1 | |
| I2C1 SCL | GPIO5 | SSD1306 SCL | core1 | |
| OLED 전원 게이팅 | GPIO6 (`&gpio0` pin 6) | 전원 스위치 제어 입력 | core1 | HIGH = OLED 전원 ON, LOW = OFF |
| Boot 버튼 | GPIO0 (보드 내장) | 온보드 BOOT 스위치 | core0 | Active-LOW, 모드 전환용 |
| VCC / GND | 3.3V / GND | 전체 모듈 공통 | - | |
| HW-664 CS | 미연결 | - | - | 이 모듈은 CS 미연결이 정상 |
| HW-664 SDO/SA0 | GND 또는 미연결/HIGH | - | - | GND→0x18, 미연결/HIGH→0x19 (모듈 기본값) |

OLED 전원 게이팅 핀과 Boot 버튼 둘 다 물리적으로는 `gpio0` 컨트롤러에 속하지만, 서로 다른 코어(core1 / core0)의 오버레이에서 각자 독립적으로 선언합니다 - 이 시리즈의 다른 실습들과 마찬가지로 한 핀은 항상 한쪽 코어의 이미지에서만 소유합니다.

---

## 4. 전력 모드 개념 정리

### 4.1 ESP32-S3 칩 레벨 전력 모드 (Espressif 기준)

| 모드 | CPU | 유지되는 것 | 비고 |
| --- | --- | --- | --- |
| Active | ON | 전체 | 기본 동작 상태 |
| Modem-Sleep | ON | CPU/RAM 전체, Wi-Fi/BT PHY만 주기적 OFF | Wi-Fi/BT를 쓸 때만 의미가 있음 - **이 실습은 무선을 쓰지 않으므로 해당 없음** |
| Light-Sleep | 일시정지 | RAM/레지스터 내용 보존 | 빠른 복귀, 다양한 wake source |
| Deep-Sleep | OFF | RTC 영역만 | 복귀 시 사실상 재부팅 |

### 4.2 Zephyr RTOS PM subsystem이 실제로 지원하는 범위 (ESP32-S3)

칩 데이터시트에는 4가지 모드가 있지만, **Zephyr의 ESP32-S3 SoC PM 드라이버가 표준 `pm_state_set()` 경로로 실제 지원하는 것은 Light-Sleep(`PM_STATE_SUSPEND_TO_IDLE`)까지**입니다. `CONFIG_PM=y`를 켜면 idle 상태일 때 자동으로 Light-Sleep에 진입하는데, Zephyr 공식 샘플(`samples/boards/espressif/light_sleep`) 문서에 "wake-up from light sleep is supported only via an RTC timer"라고 명시돼 있을 만큼, **wake source가 RTC 타이머로 제한**됩니다. GPIO 인터럽트로 Light-Sleep에서 깨우는 표준 경로는 없습니다.

Deep-Sleep은 `sys_poweroff()` + `esp_sleep_enable_timer_wakeup()` 조합으로 하드웨어 수준까지는 접근할 수 있지만, 이 조합은 ESP32/ESP32-S3에서 watchdog reset 루프나 부팅 실패를 일으키는 것으로 보고된 알려진 상위(upstream) 이슈가 있습니다(zephyr-rtos/zephyr#86193, "not planned"으로 closed). 게다가 이 실습의 초절전 모드는 **1초 주기로 센서를 계속 읽어야 하는데**, 진짜 Deep-Sleep은 CPU/RAM 전원이 완전히 꺼지므로 ULP 코프로세서 없이는 그 사이에 아무 코드도 실행할 수 없습니다.

**이 실습은 `CONFIG_PM`(Zephyr System Power Management) 자체를 사용하지 않습니다.** 실기에서 `CONFIG_PM=y`로 Light-Sleep을 실제로 동작시켜 봤지만, 이 Zephyr 스냅샷(`v4.4.0-13070-g6d1d551f6080`)에서 ESP32-S3 SoC PM 드라이버가 이 보드/드라이버 조합과 아직 안정적으로 맞물리지 않는 것을 확인했습니다 - 자세한 시도/원인/실기 로그는 별도 문서 [`doc/04_ESP32S3_PowerMode_TROUBLESHOOTING_kr.md`](./doc/04_ESP32S3_PowerMode_TROUBLESHOOTING_kr.md)에 정리해 두었으니, Light-Sleep을 직접 살려보고 싶다면 그 문서부터 읽어보시길 권합니다. Modem-Sleep도 무선을 전혀 쓰지 않는 이 실습에서는 애초에 의미가 없어 다루지 않습니다.

즉 이 실습의 3단계 절전 "모드"는 Zephyr PM subsystem이 아니라, **앱 레벨에서 직접 구현한 정책**입니다 - 전원 게이팅(OLED VCC 차단), 샘플링 주기 변경, 트리거 시 소프트웨어 리셋을 조합한 것으로, "칩이 실제로 얼마나 절전 상태에 들어가는가"보다는 "애플리케이션이 상황에 맞게 주변장치/동작 빈도를 얼마나 잘 조절하는가"를 보여주는 실습입니다. 아래 5~6절에서 이 정책을 정의합니다.

---

## 5. 시스템 구조

### 5.1 듀얼코어 역할 분담

```text
core0 (procpu)                         core1 (appcpu)
-----------------------------          -----------------------------
I2C0 -- HW-664 (LIS3DH)                I2C1 -- SSD1306 OLED
Boot 버튼 (모드 전환)                    OLED 전원 게이팅 GPIO
모드 상태 머신 소유                       화면 렌더링 / 연결 끊김 감지
        |                                       ^
        +--------------- IPM (ipm0) ------------+
```

core0가 센서를 읽고 모든 모드 전환을 결정하는 "두뇌" 역할을, core1은 core0의 지시를 받아 화면과 OLED 전원만 다루는 "출력 담당" 역할을 맡습니다. core1은 센서를 직접 읽지 않고, core0는 화면을 직접 그리지 않습니다.

### 5.2 IPM 메시지 종류 (`ipm_protocol.h`)

| 메시지 | 방향 | 의미 |
| --- | --- | --- |
| `IPM_CMD_ACCEL_AVG` | core0 → core1 | 1000ms마다, 최근 10개(100ms 간격) 샘플의 평균값. Normal 모드에서만 전송 |
| `IPM_CMD_ACCEL_ALERT` | core0 → core1 | Normal 모드 중 0.15g 이상 변화가 감지된 즉시 전송 (주기와 무관) |
| `IPM_CMD_DISPLAY_SLEEP` | core0 → core1 | OLED 전원을 끄라는 지시 |
| `IPM_CMD_DISPLAY_WAKE` | core0 → core1 | OLED 전원을 다시 켜고 처음부터 재초기화하라는 지시 |

이 헤더 파일은 `lab/src/ipm_protocol.h`와 `lab/remote/src/ipm_protocol.h` **양쪽에 동일하게 존재**합니다 (두 이미지가 서로 다른 빌드 트리라 공유 include 경로가 없어서 내용을 그대로 복제합니다). 프로토콜을 수정할 때는 반드시 두 파일을 함께 고쳐야 합니다.

### 5.3 OLED 전원 게이팅 배선 및 devicetree 표현에 대한 주의

`oled-pwr` GPIO는 SSD1306 모듈의 VCC 라인 자체를 끊는 용도입니다. SSD1306 모듈은 보통 활성 시 20~30mA 내외를 소비하는데, ESP32-S3 GPIO 핀의 절대 최대 소싱 전류(약 40mA)에 여유가 크지 않으므로, **GPIO로 VCC를 직접 구동하지 말고 P-MOSFET 하이사이드 스위치나 소형 로드스위치 IC를 통해 게이팅하는 것을 권장**합니다. 빠른 데모 목적으로 GPIO에 VCC를 직결해도 당장 동작은 할 수 있지만, 정식 실습/블로그용 배선으로는 권장하지 않습니다.

devicetree 쪽에서는 이 핀을 **반드시 `gpio-leds` 바인딩의 자식 노드로 선언**해야 합니다 - "compatible"이 없는 임의 노드에 `gpios` 프로퍼티만 넣으면 오버레이 자체는 빌드되지만, `GPIO_DT_SPEC_GET()`이 참조하는 C 매크로가 생성되지 않아 애플리케이션 빌드 단계에서 실패합니다.

---

## 6. 상태 머신 상세

```text
                    Boot 버튼                Boot 버튼
   [NORMAL] -----------------> [SLEEP] -----------------> [ULTRA_SLEEP]
      ^                           |                             |
      |                           |                             |
      +--- delta >= 0.15g --------+                             |
      |    (core1: WAKE, 재초기화)                                |
      |                                                          |
      +---------------------- sys_reboot() <--- delta >= 0.15g --+
           (재부팅 후 자동으로 NORMAL 상태에서 시작)
```

### 6.1 NORMAL (정상 동작)

- core0: 100ms마다 HW-664를 읽습니다. 10개 샘플(=1000ms)마다 평균을 계산해 `IPM_CMD_ACCEL_AVG`로 core1에 전달합니다.
- core0: 매 샘플마다, 마지막으로 전송했던 값 대비 가속도 벡터 크기 변화가 0.15g 이상이면 주기와 무관하게 즉시 `IPM_CMD_ACCEL_ALERT`를 보냅니다.
- core1: 수신한 값을 화면에 표시하고, 화면 첫 줄에 링크 상태(`LINK: OK` / `LINK: FAIL`)를 표시합니다. 1000ms 주기 데이터가 3회(=3000ms) 이상 오지 않으면 `LINK: FAIL`로 바뀝니다.
- Boot 버튼을 누르면 SLEEP으로 전환하고, core1에 `IPM_CMD_DISPLAY_SLEEP`을 보냅니다.

### 6.2 SLEEP (절전 모드)

- core0: 여전히 100ms 주기로 센서를 읽습니다 (그래야 깨어나는 반응이 빠릅니다). 다만 `IPM_CMD_ACCEL_AVG`/`ALERT`는 보내지 않습니다 - 화면이 꺼져 있으니 보낼 필요가 없습니다.
- core1: `IPM_CMD_DISPLAY_SLEEP`을 받으면 `oled-pwr` GPIO를 LOW로 내려 OLED 전원을 물리적으로 끊습니다.
- 이 상태에서 가속도 변화가 0.15g 이상이면, core0는 즉시 NORMAL로 복귀하며 core1에 `IPM_CMD_DISPLAY_WAKE`를 보냅니다. core1은 `oled-pwr`를 HIGH로 올리고, **OLED를 처음부터 재초기화**합니다 (전원이 완전히 끊겼던 터라 패널 내부 상태가 전혀 남아있지 않기 때문입니다).
- 이 상태에서 Boot 버튼을 한 번 더 누르면 ULTRA_SLEEP으로 전환합니다 (core1에는 별도로 알릴 필요가 없습니다 - 화면은 이미 꺼져 있는 상태 그대로 유지됩니다).

### 6.3 ULTRA_SLEEP (초절전 모드)

- core0: 센서 읽기 주기를 1000ms로 늦춥니다. core1과는 통신하지 않습니다.
- 이 모드는 **Zephyr Light-Sleep을 사용하지 않는, 소프트웨어 전용 저전력 정책**입니다 (4.2절 참고). CPU는 계속 Active 상태에서 `k_msleep(1000)`을 반복할 뿐이므로, 센서/디스플레이를 쉬게 해서 얻는 절전 효과는 있지만 SoC 레벨의 절전 효과는 크지 않습니다.
- 가속도 변화가 0.15g 이상이면, `sys_reboot(SYS_REBOOT_WARM)`으로 **소프트웨어 재부팅**을 실행합니다. 재부팅된 펌웨어는 항상 NORMAL 상태에서 시작하므로, 결과적으로 "리부트하여 Normal 모드로 복귀"가 됩니다.

---

## 7. 파일 구성

```text
04_ESP32S3_PowerMode_Lab/
├── README_kr.md                                    (한국어, 이 문서)
├── README.md                                       (영어)
├── doc/
│   ├── 04_ESP32S3_PowerMode_TROUBLESHOOTING_kr.md  (한국어 트러블슈팅)
│   └── 04_ESP32S3_PowerMode_TROUBLESHOOTING_en.md  (영어 트러블슈팅)
└── lab/
    ├── CMakeLists.txt                (core0 / procpu)
    ├── prj.conf
    ├── sysbuild.conf
    ├── sysbuild.cmake
    ├── sample.yaml
    ├── boards/
    │   └── esp32s3_devkitc_esp32s3_procpu.overlay
    ├── src/
    │   ├── main.c
    │   ├── sensor_hub.c
    │   ├── sensor_hub.h
    │   └── ipm_protocol.h
    └── remote/                       (core1 / appcpu)
        ├── CMakeLists.txt
        ├── prj.conf
        ├── boards/
        │   └── esp32s3_devkitc_esp32s3_appcpu.overlay
        └── src/
            ├── main.c
            ├── ipm_protocol.h
            ├── ssd1306_display.c
            ├── ssd1306_display.h
            └── font5x7.h
```

---

## 8. 빌드 및 실행

west 워크스페이스 루트에서:

```bash
west build -p always --sysbuild -b esp32s3_devkitc/esp32s3/procpu 04_ESP32S3_PowerMode_Lab/lab
west flash
west espressif monitor
```

Zephyr IDE를 쓴다면, 위 west 명령과 동일한 빌드 설정(보드: `esp32s3_devkitc/esp32s3/procpu`, sysbuild 활성화, 애플리케이션 경로: `04_ESP32S3_PowerMode_Lab/lab`)으로 빌드 타겟을 하나 만들어 쓰시면 됩니다.

---

## 9. 동작 확인 체크리스트

1. 부팅 로그에서 `HW-664 (LIS3DH) found at I2C0 address 0x1x`와 `SSD1306 found at 0x3x` 로그가 각각 core0/core1 콘솔에 찍히는지 확인합니다 (주소 자동 스캔).
2. 가만히 두면 화면에 `LINK: OK`와 함께 AX/AY/AZ 값이 1초마다 갱신되는지 확인합니다.
3. 보드를 빠르게 흔들어 0.15g 이상 변화를 주고, 1초 주기와 무관하게 화면이 `IMPACT!`로 즉시 바뀌는지 확인합니다.
4. Boot 버튼을 한 번 눌러 OLED가 완전히 꺼지는지(화면 백라이트까지 꺼짐) 확인합니다.
5. 그 상태에서 보드를 흔들어, OLED가 재초기화되며 다시 켜지고 `NORMAL MODE` 문구가 잠깐 표시되는지 확인합니다.
6. 다시 Boot 버튼을 눌러 SLEEP으로 간 뒤, 한 번 더 눌러 ULTRA_SLEEP으로 전환합니다 (화면은 계속 꺼져 있어 core0 콘솔 로그로만 상태를 확인할 수 있습니다).
7. ULTRA_SLEEP 상태에서 보드를 흔들면 core0 콘솔에 리부트 로그가 찍히고, 보드가 재시작하며 NORMAL 상태(화면 켜짐)로 돌아오는지 확인합니다.
8. (선택) core0만 리셋하거나 IPM 라인을 일부러 끊어, core1 화면에 `LINK: FAIL`이 뜨는지 확인합니다.

---

## 10. 주의사항

- **OLED 전원 게이팅 배선**: 5.3절 참고 - GPIO 직결보다 MOSFET/로드스위치 사용을 권장합니다.
- **ISR 컨텍스트 주의**: Boot 버튼의 GPIO 콜백, IPM 콜백 모두 인터럽트 컨텍스트에서 실행되므로, 코드를 수정할 때 그 안에서 `k_sleep()`이나 블로킹 API를 직접 호출하지 않도록 주의합니다 (이 실습 코드는 콜백에서 플래그/메시지큐만 세팅하고, 실제 처리는 메인 루프/전용 스레드에서 하도록 구성했습니다).
- **devicetree에 순수 GPIO 출력 핀을 추가할 때**: `oled-pwr`처럼 표준 드라이버가 없는 제어용 핀은 반드시 `gpio-leds` 바인딩의 자식 노드로 선언해야 합니다 (5.3절 참고) - `compatible` 없이 `gpios` 프로퍼티만 넣으면 오버레이는 빌드되지만 C 코드에서 `GPIO_DT_SPEC_GET()`을 쓰는 순간 빌드가 깨집니다.
- 이 실습을 실기에서 검증하며 마주친 빌드/런타임 이슈(특히 `CONFIG_PM` 관련 시행착오)와 그 원인/해결 과정은 별도 문서 [`doc/04_ESP32S3_PowerMode_TROUBLESHOOTING_kr.md`](./doc/04_ESP32S3_PowerMode_TROUBLESHOOTING_kr.md)에 정리했습니다.
