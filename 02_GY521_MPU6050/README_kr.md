**한국어** | [English](./README.md)

# Lab 02: GY-521 (MPU-6050) 6축 IMU (듀얼코어, IPM, SSD1306)

> `zephyr_sensor` 시리즈 두 번째 랩. ESP32-S3-DevKitC-1 기준, 실기 검증 완료.
> 실기 검증 중 겪은 이슈와 해결 과정: [트러블슈팅 문서](./doc/02_GY521_MPU6050_TROUBLESHOOTING_kr.md)

## 1. 개요

core0(procpu)이 GY-521 모듈(MPU-6050 칩)에서 3축 가속도 + 3축 자이로 + 온도를
읽어, core1(appcpu)이 SSD1306 OLED에 표시합니다. Lab 01과 동일하게 두 코어는
완전히 독립된 Zephyr 이미지(AMP, `west build --sysbuild`)이고, IPM(inter-processor
mailbox)으로 값을 전달합니다.

```
core0 (procpu)                              core1 (appcpu)
────────────────                            ────────────────
[mpu6050_thread] 100ms  (가속도+자이로+온도 fetch)
   └─ 가속도 벡터 크기 0.15g 이상 변화 시 즉시 push
   └─ 자이로 벡터 크기 15deg/s 이상 변화 시 즉시 push

[heartbeat_thread] 1000ms ─ 무조건 push (링크 생존 신호)

   └──(위 세 경로 모두)──> push_snapshot_to_core1()
                              7개 값(가속도 X/Y/Z, 자이로 X/Y/Z, 온도)
                              전부를 담은 스냅샷을 통째로 전송
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
                                  SSD1306(I2C1) 갱신: LINK, AX/AY/AZ, GX/GY/GZ, TEMP
```

**전송 트리거 방식**: 가속도 3축의 벡터 크기(√(ax²+ay²+az²))가 마지막 전송값
대비 0.15g 이상 변하거나, 자이로 3축의 벡터 크기가 15deg/s 이상 변하면 즉시
전송. 둘 다 조건을 안 넘으면 1초마다 heartbeat로 무조건 한 번 전송(스냅샷
전체를 담아서). core1은 마지막 메시지 수신 후 2초 동안 무응답이면 링크가
끊긴 것으로 판단.

**Lab 01과의 구조적 차이**: Lab 01은 센서 2개(AHT20/BMP280)가 같은 I2C0 버스를
공유해서 `g_i2c0_bus_lock`으로 두 스레드의 버스 접근을 직렬화해야 했습니다.
이 랩은 센서가 MPU6050 하나뿐이라 그런 경합 자체가 없어 별도 버스 락이
필요 없습니다 - 스레드 구조가 더 단순합니다.

**왜 코어를 나누고 I2C 버스도 나눴는가**: (Lab 01과 동일한 이유) ESP32-S3에서
Zephyr의 core0/core1은 SMP가 아니라 AMP - 완전히 독립된 두 이미지입니다.
같은 I2C 컨트롤러를 두 코어가 동시에 소유하는 건 위험하므로, 센서는
I2C0(core0 전담), 디스플레이는 I2C1(core1 전담)으로 물리적으로 분리했습니다.

## 2. 센서 소개

### GY-521 / MPU-6050 (3축 가속도 + 3축 자이로 + 온도)
- InvenSense(현 TDK) 제작, 3축 MEMS 가속도계와 3축 MEMS 자이로스코프, 온도
  센서가 한 칩에 통합된 6축 모션 트래킹 디바이스
- I2C 주소: AD0 핀이 GND(또는 미연결 - GY-521 보드 대부분 온보드 풀다운)면
  `0x68`, VCC면 `0x69` - **실기 스캔으로 두 경우 모두 확인됨**
- **주소 자동 감지**: 오버레이에 `0x68`/`0x69` 두 노드를 모두 선언해두고,
  부팅 시 실제로 `device_is_ready()`가 true인 쪽을 골라 씀. Zephyr의 mpu6050
  드라이버는 초기화 시 WHO_AM_I 레지스터를 읽어 확인하므로, 실제 배선 안 된
  주소는 초기화가 자동으로 실패함 - 이 성질을 이용해 raw I2C 우회 없이 표준
  드라이버 프레임워크만으로 "AD0을 GND에 연결하든 VCC에 연결하든 코드 수정
  없이 동작"하게 만듦(`main.c`의 `mpu6050_select()` 참고). 이 선택 로직은
  `main()`이 아니라 `mpu6050_thread` 스레드 자신의 시작 부분에서 수행함 -
  `K_THREAD_DEFINE`로 등록된 스레드는 `main()`보다 먼저 스케줄될 수도 있어서,
  `main()`에서 고른 결과를 스레드가 나중에 참조하는 구조는 경쟁 상태가 될
  수 있기 때문. 이 프로젝트는 0x68(AD0=GND)로 사용 중이며 실기로 정상 동작
  확인됨(부팅 로그 `MPU6050 found at 0x68`).
- Zephyr 공식 드라이버: devicetree compatible `"invensense,mpu6050"`
  (`drivers/sensor/tdk/mpu6050/mpu6050.c`), Kconfig `CONFIG_MPU6050=y`
- 드라이버가 반환하는 단위: 가속도는 `SENSOR_CHAN_ACCEL_XYZ`로 m/s², 자이로는
  `SENSOR_CHAN_GYRO_XYZ`로 rad/s, 온도는 `SENSOR_CHAN_DIE_TEMP`로 °C - 이 랩은
  가속도를 g(÷9.80665), 자이로를 deg/s(×180/π)로 변환해 더 직관적으로 표시함
- 이 랩은 인터럽트 핀을 쓰지 않는 폴링 모드(`CONFIG_MPU6050_TRIGGER_NONE=y`) -
  100ms마다 스레드에서 직접 `sensor_sample_fetch()`를 호출
- **Datasheet**: [InvenSense 공식 MPU-6000/6050 레지스터 맵 PDF](https://www.invensense.com/wp-content/uploads/2015/02/MPU-6000-Register-Map1.pdf)
  (제품 스펙 문서는 InvenSense가 TDK에 인수되며 공식 링크가 자주 바뀌므로,
  "MPU-6000 and MPU-6050 Product Specification Revision 3.4" 문서명으로
  검색해 최신 미러를 찾는 것을 권장)
- 참고 구매처(국내): [디바이스마트 GY-521 상품페이지](https://www.devicemart.co.kr/goods/view?no=1247052)

### SSD1306 (128x64 OLED, core1 전담)
- Lab 01과 동일한 커스텀 I2C 드라이버(`ssd1306_display.c`) 재사용 - ESP32
  Zephyr I2C 드라이버가 멀티 세그먼트 write 트랜잭션을 안정적으로 처리하지
  못하는 확인된 플랫폼 이슈를 피하기 위함
- **Datasheet**: [Solomon Systech SSD1306 데이터시트 PDF (Adafruit 미러)](https://cdn-shop.adafruit.com/datasheets/SSD1306.pdf)
- 폰트(`font5x7.h`)는 Lab 01 것을 기반으로, 이 랩의 라벨(AX/AY/AZ/GX/GY/GZ)에
  필요한 대문자 'X', 'Z' 글리프를 추가하고, 이 랩에서 쓰지 않는 소문자
  h/a는 제거해 이 랩 전용 최소 세트로 정리함

## 3. IPM 통신 규격 (`ipm_protocol.h`)

```c
#define IPM_SENSOR_CHANNEL 2   /* Lab 01과 동일 채널 - 두 랩을 동시에 같은 보드에 올리지 않으므로 충돌 없음 */

struct ipm_sensor_payload {
    float accel_x_g;
    float accel_y_g;
    float accel_z_g;
    float gyro_x_dps;
    float gyro_y_dps;
    float gyro_z_dps;
    float temp_c;
    uint8_t valid;
};
```
`lab/src/ipm_protocol.h`와 `lab/remote/src/ipm_protocol.h`는 손으로 관리하는
별도 사본 - 필드를 바꾸면 양쪽 다 갱신할 것 (Lab 01과 동일한 주의사항).

## 4. 배선표

| 신호 | ESP32-S3 핀 | 연결 대상 | 담당 코어 |
| --- | --- | --- | --- |
| I2C0 SDA | GPIO8 | GY-521 SDA | core0 |
| I2C0 SCL | GPIO9 | GY-521 SCL | core0 |
| I2C1 SDA | GPIO4 | SSD1306 SDA | core1 |
| I2C1 SCL | GPIO5 | SSD1306 SCL | core1 |
| VCC / GND | 3.3V / GND | 전부 | - |
| AD0 | GND (또는 미연결) | GY-521 AD0 | - (0x68 고정용) |

**Lab 01에서 얻은 관례**: 배선 직후, 오버레이에 주소를 넣기 전에
`lab_tools/i2c0_scanner`로 먼저 실제 응답 주소를 확인할 것. 이 랩은 AD0을
GND/VCC 어느 쪽에 연결해도(0x68/0x69) 코드 수정 없이 자동으로 인식되므로,
이 사례에 한해서는 스캐너 확인이 필수는 아니지만 새로 배선할 때는 여전히
권장.

## 5. 빌드 방법

메인 랩(듀얼코어):
```
west build -p always --sysbuild -b esp32s3_devkitc/esp32s3/procpu \
    02_GY521_MPU6050/lab
west flash
```
- `lab/sysbuild.conf`에 `SB_CONFIG_BOOTLOADER_NONE=y` 필요 (Lab 01과 동일한
  이유 - Espressif 보드는 `--sysbuild` 시 기본적으로 MCUboot를 같이 빌드하려
  시도함).

I2C 버스 스캐너(진단용, sysbuild 불필요):
```
west build -p always -b esp32s3_devkitc/esp32s3/procpu \
    02_GY521_MPU6050/lab_tools/i2c0_scanner
west flash
```

## 6. 예상 결과

### core0 부팅 로그 (정상 시)
```
[00:00:00.225,000] <err> MPU6050: Failed to read chip ID.
*** Booting Zephyr OS build v4.4.0 ***
[00:00:00.226,000] <inf> app_procpu: 02_GY521_MPU6050 (core0/procpu) starting
[00:00:00.231,000] <inf> app_procpu: MPU6050 found at 0x68 (AD0 low/floating)
```
첫 줄의 `Failed to read chip ID.` 에러는 배선 안 된 0x69 노드의 초기화가
커널 부팅 단계에서 자동 실행되며 실패한 것으로, **정상적으로 예상되는
동작**입니다 (실기로 확인됨 - [트러블슈팅 문서](./doc/02_GY521_MPU6050_TROUBLESHOOTING_kr.md) 참고). 거슬리면 MPU6050
모듈 로그 레벨을 낮춰 조용히 만들 수 있습니다.

### core1 화면 (실기 확인됨)
```
LINK: OK
AX:-0.02
AY:0.01
AZ:0.98
GX:-0.3
GY:0.1
GZ:0.0
TEMP:24.3C
```
- 정지 상태에서는 중력만 받아 세 축 중 보드를 놓은 방향에 해당하는 한 축이
  약 ±1.00 근처(1g)로, 나머지 두 축은 0 근처로 나오는 것이 정상
- 자이로는 정지 상태에서 세 축 모두 0 근처(약간의 노이즈/바이어스는 정상)
- 보드를 움직이거나 회전시키면 해당 값이 다음 100ms 사이클 내로 화면에
  반영됨 - 임계값 기반 즉시 전송이 실제로 동작함을 실기로 확인
- 부팅 직후 첫 메시지 전까지 `WAITING FOR CORE0...`
- core0→core1 전달이 2초 이상 끊기면 첫 줄이 `LINK: FAIL`로 바뀜

## 7. 확장 아이디어

- 가속도 벡터 크기가 특정 값(예: 낙하/충격 감지용 2g 이상) 넘으면 별도 이벤트로
  구분해 표시
- 자이로 값을 적분해 대략적인 자세(pitch/roll) 추정치를 core1에서 계산해
  표시 (드리프트가 있어 정밀하지 않다는 점을 실습 포인트로 삼기 좋음)
- MPU6050의 내장 FIFO/인터럽트 모드로 전환해 폴링 대신 이벤트 기반 읽기로
  바꿔보기 (`CONFIG_MPU6050_TRIGGER_GLOBAL_THREAD` 등)
- 화면에 단위(g, deg/s) 표시 추가 - 폰트에 필요한 소문자 글리프 추가가 선행
  되어야 함(검토했으나 이번 버전에서는 보류)
