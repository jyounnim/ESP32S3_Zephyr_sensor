**한국어** | [English](./README.md)

# Lab 03: HW-664 (실제 칩은 LIS3DH) 3축 가속도계 (듀얼코어, IPM, SSD1306)

> `zephyr_sensor` 시리즈 세 번째 랩. ESP32-S3-DevKitC-1 기준, 실기 검증 완료.
> 실기 검증 중 겪은 이슈와 해결 과정: [트러블슈팅 문서](./doc/03_HW664_LIS3DH_TROUBLESHOOTING_kr.md)

## 1. 개요

core0(procpu)이 HW-664 모듈(실제 칩: LIS3DH)에서 3축 가속도를 읽어,
core1(appcpu)이 SSD1306 OLED에 표시합니다. Lab 01/02와 동일하게 두 코어는
완전히 독립된 Zephyr 이미지(AMP, `west build --sysbuild`)이고, IPM으로 값을
전달합니다.

```
core0 (procpu)                              core1 (appcpu)
────────────────                            ────────────────
[lis3dh_thread] 100ms  (가속도 XYZ fetch, 표준 Zephyr 센서 API)
   └─ 가속도 벡터 크기 0.15g 이상 변화 시 즉시 push

[heartbeat_thread] 1000ms ─ 무조건 push (링크 생존 신호)

   └──(위 두 경로 모두)──> push_snapshot_to_core1()
                              3개 값(가속도 X/Y/Z) 전부를
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
                                  SSD1306(I2C1) 갱신: LINK, AX/AY/AZ
```

**전송 트리거 방식**: 가속도 3축의 벡터 크기가 마지막 전송값 대비 0.15g 이상
변하면 즉시 전송(Lab 02 MPU6050 가속도와 동일 임계값). 안 넘으면 1초마다
heartbeat로 무조건 한 번 전송. core1은 마지막 메시지 수신 후 2초 동안
무응답이면 링크가 끊긴 것으로 판단.

**Lab 02와의 구조적 유사성**: 이 랩도 센서가 하나뿐이라 버스 경합 방지용
뮤텍스가 따로 필요 없는 단순한 구조. 주소 자동 감지(0x18/0x19) 방식도
Lab 02의 MPU6050 0x68/0x69 자동 감지와 완전히 동일한 패턴 적용.

**왜 코어를 나누고 I2C 버스도 나눴는가**: (Lab 01/02와 동일한 이유) ESP32-S3에서
Zephyr의 core0/core1은 SMP가 아니라 AMP - 완전히 독립된 두 이미지입니다.
같은 I2C 컨트롤러를 두 코어가 동시에 소유하는 건 위험하므로, 센서는
I2C0(core0 전담), 디스플레이는 I2C1(core1 전담)으로 물리적으로 분리했습니다.

## 2. 센서 소개

### HW-664 (실제 칩: LIS3DH)
- STMicroelectronics 제작 3축 MEMS 가속도계. **판매 페이지/실크스크린
  표기는 LIS3DSH였으나, 실기에서 WHO_AM_I 레지스터를 읽어 `0x33`이 나온
  것으로 실제 칩이 LIS3DH 계열(LIS3DH/LIS2DH/LIS2DH12/LSM303DLHC/
  LSM303AGR - 서로 레지스터 호환)임을 확인함** (LIS3DSH였다면 `0x3F`가
  나왔어야 함). 저가 브레이크아웃 모듈에서 판매 페이지 표기와 실제 실장
  부품이 다른 경우가 종종 있다는 걸 보여준 사례.
- I2C 주소: **실기 확인됨 - SDO/SA0 핀을 미연결 또는 HIGH로 두면 `0x19`,
  LOW로 묶으면 `0x18`**. 오버레이는 두 주소를 모두 devicetree 노드로
  선언해두고, 부팅 시 실제로 준비된(`device_is_ready()`) 쪽을 자동으로
  골라 씀 (`lis3dh_select()`, `main()`이 아니라 스레드 자신의 시작
  부분에서 실행 - `K_THREAD_DEFINE` 스레드가 `main()`보다 먼저 뜰 수
  있어서). 이 모듈은 CS 핀을 연결하지 않아도 I2C가 정상 동작함(온보드
  풀업으로 추정).
- Zephyr 공식 드라이버: `drivers/sensor/st/lis2dh/lis2dh.c`가 LIS2DH/
  LIS3DH/LSM303DLHC/LIS2DH12/LSM303AGR를 레지스터 호환 계열로 함께
  지원. Kconfig 심볼은 `CONFIG_LIS2DH=y`(드라이버 파일명 기준이라 칩
  이름과 다름에 주의).
  **devicetree compatible은 반드시 `"st,lis2dh"`를 쓸 것** - Zephyr 문서에는
  `"st,lis3dh"`라는 별도 바인딩 페이지도 있지만, 실제 드라이버는 그 문자열에
  반응하지 않아 디바이스 인스턴스가 안 생김(Zephyr GitHub 이슈 #31253로
  확인된 알려진 이슈) - 자세한 경위는 [트러블슈팅 문서](./doc/03_HW664_LIS3DH_TROUBLESHOOTING_kr.md) 참고.
- 표준 `sensor_sample_fetch()`/`sensor_channel_get(SENSOR_CHAN_ACCEL_XYZ)`로
  m/s² 단위 가속도를 받아 g로 변환(÷9.80665)해 사용
- 실기 부팅 로그로 ±2g 풀스케일(`fs=2`) 기본 설정 확인됨
- **Datasheet**: [ST 공식 LIS3DH 데이터시트 PDF](https://www.st.com/resource/en/datasheet/lis3dh.pdf)
- 참고 구매처: [알리익스프레스 HW-664 상품페이지](https://ko.aliexpress.com/item/4000116105737.html?gatewayAdapt=glo2kor)
  (위에서 설명한 대로, 판매 페이지 표기와 실제 실장 칩이 다른 사례이니 참고)

### SSD1306 (128x64 OLED, core1 전담)
- Lab 01/02와 동일한 커스텀 I2C 드라이버(`ssd1306_display.c`) 및 폰트
  (`font5x7.h`, Lab 02 것 재사용) 재사용 - ESP32 Zephyr I2C 드라이버가
  멀티 세그먼트 write 트랜잭션을 안정적으로 처리하지 못하는 확인된 플랫폼
  이슈를 피하기 위함
- **Datasheet**: [Solomon Systech SSD1306 데이터시트 PDF (Adafruit 미러)](https://cdn-shop.adafruit.com/datasheets/SSD1306.pdf)

## 3. 배선

| 신호 | ESP32-S3 핀 | 연결 대상 | 담당 코어 |
| --- | --- | --- | --- |
| I2C0 SDA | GPIO8 | HW-664 SDA | core0 |
| I2C0 SCL | GPIO9 | HW-664 SCL | core0 |
| I2C1 SDA | GPIO4 | SSD1306 SDA | core1 |
| I2C1 SCL | GPIO5 | SSD1306 SCL | core1 |
| VCC / GND | 3.3V / GND | 전부 | - |
| CS (선택, 이 모듈은 미연결도 정상) | - | HW-664 CS | - |
| SDO/SA0 (선택, 미연결 시 이 모듈은 0x19) | GND(0x18) 또는 미연결/HIGH(0x19) | HW-664 SDO | - |

**Lab 01/02에서 얻은 관례 그대로 적용**: 새 I2C 디바이스는 배선 직후
`lab_tools/i2c0_scanner`로 실제 응답 주소를 먼저 확인할 것 - 이번 랩에서는
그렇게 확인한 값(0x19)이 애초에 칩 정체를 다시 파악하는 단서가 되기도 했음.

## 4. IPM 통신 규격 (`ipm_protocol.h`)

```c
#define IPM_SENSOR_CHANNEL 2   /* Lab 01/02와 동일 채널 - 동시에 같은 보드에 안 올리므로 충돌 없음 */

struct ipm_sensor_payload {
    float accel_x_g;
    float accel_y_g;
    float accel_z_g;
    uint8_t valid;
};
```
`lab/src/ipm_protocol.h`와 `lab/remote/src/ipm_protocol.h`는 손으로 관리하는
별도 사본 - 필드를 바꾸면 양쪽 다 갱신할 것.

## 5. 빌드 방법

메인 랩(듀얼코어):
```
west build -p always --sysbuild -b esp32s3_devkitc/esp32s3/procpu \
    03_HW664_LIS3DH/lab
west flash
```
- `lab/sysbuild.conf`에 `SB_CONFIG_BOOTLOADER_NONE=y` 필요 (Lab 01/02와
  동일한 이유 - Espressif 보드는 `--sysbuild` 시 기본적으로 MCUboot를 같이
  빌드하려 시도함).

I2C 버스 스캐너(진단용, sysbuild 불필요):
```
west build -p always -b esp32s3_devkitc/esp32s3/procpu \
    03_HW664_LIS3DH/lab_tools/i2c0_scanner
west flash
```

## 6. 예상 결과 (실기 확인됨)

### core0 부팅 로그
```
[00:00:00.229,000] <err> lis2dh: Failed to read chip id.
[00:00:00.235,000] <inf> lis2dh: fs=2, odr=0x4 lp_en=0x0 scale=9576
*** Booting Zephyr OS build v4.4.0 ***
[00:00:00.237,000] <inf> app_procpu: 03_HW664_LIS3DH (core0/procpu) starting
[00:00:00.242,000] <inf> app_procpu: LIS3DH found at 0x19 (SDO/SA0 high/unconnected)
```
첫 줄의 `Failed to read chip id.`는 배선 안 된 0x18 노드의 초기화가 커널
부팅 단계에서 자동 실행되며 실패한 것으로, **정상적으로 예상되는 동작**
(Lab 02의 MPU6050 0x68/0x69 자동 감지 때와 동일한 패턴). 그 다음 줄에서
0x19가 `fs=2`(±2g, 예상대로)로 정상 초기화되는 것을 확인.

### core1 화면
```
LINK: OK

AX:-0.02
AY:0.01
AZ:0.98
```
- 정지 상태에서는 세 축 중 보드를 놓은 방향에 해당하는 한 축이 약 ±1.00
  근처(1g)로, 나머지 두 축은 0 근처로 나오는 것이 정상
- 보드를 움직이면 다음 100ms 사이클 내로 화면에 반영됨
- 부팅 직후 첫 메시지 전까지 `WAITING FOR CORE0...`
- core0→core1 전달이 2초 이상 끊기면 첫 줄이 `LINK: FAIL`로 바뀜

## 7. 확장 아이디어

- LIS3DH의 내장 FIFO/인터럽트/클릭 감지(탭/더블탭) 기능을 활용해 이벤트
  기반으로 확장 (`CONFIG_LIS2DH_TRIGGER_*`)
- ±2g 대신 더 넓은 풀스케일(±4g/8g/16g)로 전환
- LIS3DH의 보조 ADC 채널(온도 측정에도 활용 가능하나 Zephyr 드라이버가
  센서 채널로 노출하진 않음)을 raw 레지스터 접근으로 별도 읽어와 보너스
  온도 채널로 추가
