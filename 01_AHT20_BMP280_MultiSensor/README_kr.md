**한국어** | [English](./README.md)

# Lab 01: AHT20 + BMP280 멀티센서 (듀얼코어, IPM, SSD1306)

> `zephyr_sensor` 시리즈 첫 번째 랩. ESP32-S3-DevKitC-1 기준, 실기 검증 완료.
> 실기 검증 중 겪은 이슈와 해결 과정: [트러블슈팅 문서](./doc/01_AHT20_BMP280_MultiSensor_TROUBLESHOOTING_kr.md)

## 1. 개요

core0(procpu)이 AHT20(온도+습도), BMP280(온도+기압) 두 센서를 각자의 스레드에서
읽고, core1(appcpu)이 그 값을 받아 SSD1306 OLED에 표시합니다. 두 코어는 완전히
독립된 Zephyr 이미지(AMP, `west build --sysbuild`)이고, 값 전달은 ESP32의 IPM
(inter-processor mailbox)을 통해 이뤄집니다.

```
core0 (procpu)                              core1 (appcpu)
────────────────                            ────────────────
[aht20_thread]  100ms  (온도+습도)
   └─ 온도 1°C 이상 변화 시 즉시 push
[bmp280_thread] 100ms  (온도+기압)
   └─ 기압 10hPa 이상 변화 시 즉시 push

[heartbeat_thread] 1000ms ─ 무조건 push (링크 생존 신호)

   └──(위 세 경로 모두)──> push_snapshot_to_core1()
                              4개 값(AHT20 T/H, BMP280 T/P) 전부를
                              담은 스냅샷을 통째로 전송
                                  │
                          ipm_send(channel=2)
                                  │
                                  v  (물리 mailbox)
                                          [ipm_callback] (ISR 컨텍스트)
                                                  │
                                          k_msgq_put (K_NO_WAIT)
                                                  │
                                                  v
                                        [display_thread] (k_msgq_get, 2초 타임아웃)
                                                  │
                                  SSD1306(I2C1) 갱신: LINK, AHT20 T/H, BMP280 T/P
```

**전송 트리거 방식**:
- AHT20 온도: 마지막 전송값 대비 1.0°C 이상 변화 시 즉시 전송
- BMP280 기압: 마지막 전송값 대비 10hPa 이상 변화 시 즉시 전송
- AHT20 습도 / BMP280 온도: 자체 임계값 없는 "동승자" 값 - 위 두 조건 중 하나가
  트리거되거나 heartbeat(1초)가 돌 때 스냅샷에 같이 실려 전송됨
- core1은 마지막 메시지 수신 후 2초 동안 무응답이면 링크가 끊긴 것으로 판단

**왜 코어를 나누고 I2C 버스도 나눴는가**: ESP32-S3에서 Zephyr의 core0/core1은
SMP가 아니라 AMP - 완전히 독립된 두 이미지로 각각 빌드/플래시됩니다. 같은 I2C
컨트롤러를 두 코어가 동시에 소유하는 건 위험하므로, 센서는 I2C0(core0 전담),
디스플레이는 I2C1(core1 전담)으로 물리적으로 분리했습니다.

## 2. 센서 소개

### AHT20 (온도 + 습도)
- Aosong 제작, I2C 주소 고정 `0x38`
- 정전용량식 습도 센서 + 온도 센서 복합 칩. `0xAC` 트리거 명령 → 약 80ms
  변환 시간 → 6바이트 데이터 + CRC 리드 순서로 동작
- Zephyr 공식 드라이버: devicetree compatible `"aosong,aht20"`
  (`drivers/sensor/aosong/dht20/dht20.c` - AHT20/DHT20/AM2301B가 레지스터
  호환이라 하나의 드라이버 파일을 공유)
- **Datasheet**: [Aosong 공식 AHT20 데이터시트 PDF](https://www.aosong.com/userfiles/files/media/Data%20Sheet%20AHT20.pdf)
- **자가복구(self-heal)**: 실기에서 이 유닛이 가끔 CRC는 통과하지만 물리적으로
  불가능한 값(~145°C, 습도 정확히 ~50% - 20비트 ADC의 절반값 패턴)을 계속
  반환하는 상태에 빠지는 것을 확인함. MCU 리셋으로는 안 풀리고 센서 자체
  전원 재인가로만 풀리는 것도 확인했으나, 현장에서 매번 전원을 뽑을 순 없으므로
  AHT20의 공식 소프트 리셋 명령(`0xBA`, 파라미터 없음, ~20ms)을 Zephyr 센서
  API 밖에서 직접 I2C로 보내 대응함 - 읽은 온도가 AHT20 동작 범위(-40~85°C)를
  벗어나면 자동으로 트리거됨 (`main.c`의 `aht20_soft_reset()` 참고).

### BMP280 (온도 + 기압)
- Bosch 제작, I2C 주소는 SDO 핀에 따라 `0x76`(GND) 또는 `0x77`(VDD) - **이
  프로젝트의 모듈은 실기 스캔 결과 0x77로 확인됨**
- 압저항식 기압 센서 + 온도 센서 복합 칩. 온도 채널은 사용자에게 온도를
  보여주기 위해서라기보다, 기압 원시값을 보정하는 데 필요한 `t_fine` 값을
  구하기 위해 존재함
- Zephyr 공식 드라이버: devicetree compatible `"bosch,bme280"` - 칩 ID
  레지스터(0xD0)로 BME280(습도 O, ID 0x60)과 BMP280(습도 X, ID 0x58)을 자동
  판별하므로 BMP280 전용 compatible 문자열은 따로 없음
- 기압 채널은 kPa 단위로 나오므로 x10 하여 hPa로 변환해 사용
- **Datasheet**: [Bosch Sensortec 공식 BMP280 데이터시트 PDF](https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bmp280-ds001.pdf)
- **알려진 제약**: 드물게 기압이 비현실적인 값(예: 600hPa대)으로 튀는 현상이
  관측됨 - AHT20 self-heal과 달리 이 랩에서는 별도 자동 복구를 넣지 않음
  (아래 "알려진 제약사항" 참고)

### SSD1306 (128x64 OLED, core1 전담)
- Solomon Systech 제작 OLED 컨트롤러
- **Datasheet**: [Solomon Systech SSD1306 데이터시트 PDF (Adafruit 미러)](https://cdn-shop.adafruit.com/datasheets/SSD1306.pdf)
- 이 랩에서는 Zephyr 공식 디스플레이 서브시스템 대신, 이전 디스플레이 시리즈에서
  검증된 "컨트롤 바이트+페이로드를 하나의 버퍼로 합쳐 단일 `i2c_write()`로
  전송" 방식의 커스텀 드라이버(`ssd1306_display.c`)를 재사용 - ESP32 Zephyr
  I2C 드라이버가 멀티 세그먼트 write 트랜잭션을 안정적으로 처리하지 못하는
  확인된 플랫폼 이슈를 피하기 위함
- 주소(0x3C/0x3D)는 부팅 시 자동 프로빙
- 텍스트는 이 랩을 위해 새로 만든 5x7 도트 폰트(`font5x7.h`, 라벨/숫자/기호
  최소 세트) 사용

## 3. IPM 통신 규격 (`ipm_protocol.h`)

```c
#define IPM_SENSOR_CHANNEL 2   /* 채널 0/1은 플랫폼 예약, 2/3이 애플리케이션 여유분 */

struct ipm_sensor_payload {
    float aht20_temp_c;         /* 임계값 있음 (1.0°C) */
    float aht20_humidity_pct;   /* 동승자 - 임계값 없음 */
    uint8_t aht20_valid;

    float bmp280_temp_c;        /* 동승자 - 임계값 없음 */
    float bmp280_pressure_hpa;  /* 임계값 있음 (10hPa) */
    uint8_t bmp280_valid;
};
```
`lab/src/ipm_protocol.h`와 `lab/remote/src/ipm_protocol.h`는 손으로 관리하는
별도 사본 - 필드를 바꾸면 양쪽 다 갱신할 것.

## 4. 배선표

| 신호 | ESP32-S3 핀 | 연결 대상 | 담당 코어 |
| --- | --- | --- | --- |
| I2C0 SDA | GPIO8 | AHT20 SDA, BMP280 SDA | core0 |
| I2C0 SCL | GPIO9 | AHT20 SCL, BMP280 SCL | core0 |
| I2C1 SDA | GPIO4 | SSD1306 SDA | core1 |
| I2C1 SCL | GPIO5 | SSD1306 SCL | core1 |
| VCC / GND | 3.3V / GND | 전부 | - |

I2C0과 I2C1은 물리적으로 완전히 분리된 버스입니다. **새 I2C 디바이스를 배선한
직후에는 오버레이에 주소를 넣기 전에 `lab_tools/i2c0_scanner`로 실제 응답
주소를 먼저 확인**하는 것을 권장합니다 (datasheet 기본 주소와 실제 모듈의
SDO 결선이 다른 경우가 흔함).
<img width="418" height="400" alt="image" src="https://github.com/user-attachments/assets/6303848d-a7bd-4242-b047-df01ef3f1e87" />


## 5. 빌드 방법

메인 랩(듀얼코어):
```
west build -p always --sysbuild -b esp32s3_devkitc/esp32s3/procpu \
    01_AHT20_BMP280_MultiSensor/lab
west flash
```
- `lab/sysbuild.conf`에 `SB_CONFIG_BOOTLOADER_NONE=y`가 필요함 - Espressif
  보드는 `--sysbuild` 사용 시 기본적으로 MCUboot까지 같이 빌드하려 시도하는데,
  이 랩은 OTA와 무관한 순수 2-이미지 AMP 구성이라 불필요함.

I2C 버스 스캐너(진단용, sysbuild 불필요):
```
west build -p always -b esp32s3_devkitc/esp32s3/procpu \
    01_AHT20_BMP280_MultiSensor/lab_tools/i2c0_scanner
west flash
```

## 6. 예상 결과

### core0 부팅 로그 (정상 시)
```
[00:00:00.xxx,xxx] <inf> app_procpu: 01_AHT20_BMP280_MultiSensor (core0/procpu) starting
```

### core1 화면
```
LINK: OK

AHT20 T:24.3C
AHT20 H:55.2%

BMP280 T:28.3C
BMP280 P:998 hPa
```
- 부팅 직후 첫 메시지 전까지 `WAITING FOR CORE0...`
- core0→core1 전달이 2초 이상 끊기면 첫 줄이 `LINK: FAIL`로 바뀜 (직전 값은
  화면에 그대로 유지)

## 7. 알려진 제약사항

- **BMP280 기압 이상치**: 드물게 기압이 비현실적인 값으로 튀는 현상이 있음.
  두 센서(AHT20/BMP280)가 동시에 이상 동작하는 패턴이 관측된 적이 있어, 개별
  칩 불량보다는 전원/배선 품질(브레드보드 접촉, 디커플링 커패시터 부재 등)이
  근본 원인일 가능성이 있음 - 이 랩에서는 AHT20처럼 자동 복구 로직을 넣지
  않았으므로, 필요하다면 BMP280의 하드웨어 리셋 레지스터(0xE0에 0xB6 write)를
  이용해 유사한 자가복구를 추가하는 것을 다음 확장으로 고려할 수 있음.
- **AHT20 100ms 주기의 실측 정확도**: AHT20 자체 변환 시간이 ~80ms라 100ms
  주기 대비 여유가 크지 않음. 또한 core0의 두 센서 스레드가 하나의 I2C0 버스
  뮤텍스(`g_i2c0_bus_lock`)를 공유하므로, BMP280 스레드는 AHT20 스레드의 변환
  대기 시간에 밀려 정확히 100ms 간격으로 읽히지 않을 수 있음 - 두 센서가 같은
  버스를 겹치지 않고 쓰도록 하는 정확성을 타이밍 정밀도보다 우선한 설계 선택.

## 8. 확장 아이디어

- BMP280에도 AHT20과 유사한 값 타당성 검사 + 하드웨어 리셋 기반 자가복구 추가
- 습도/BMP280 온도에도 각각 독립 임계값을 추가해 4채널 모두 개별 트리거를
  갖도록 확장
- IPM 채널 3을 이용한 core1→core0 역방향 메시지(예: 임계값을 core1에서 실시간
  조정) 추가
