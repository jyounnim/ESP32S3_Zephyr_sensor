**한국어** | [English](./03_HW664_LIS3DH_TROUBLESHOOTING_en.md) · [← 랩 README](../README_kr.md)

# Lab 03 트러블슈팅 (HW-664, 듀얼코어/IPM)

실기 검증 과정에서 실제로 겪은 이슈와 해결책만 정리했습니다.

## 1. 칩 정체가 판매 페이지 표기(LIS3DSH)와 달리 실제로는 LIS3DH였음

**증상**: 처음에 LIS3DSH로 가정하고 커스텀 raw I2C 드라이버를 작성해
빌드/플래시했더니 부팅 로그에:
```
<wrn> lis3dsh: Device at 0x19 answered but WHO_AM_I=0x33 (expected 0x3F)
<err> lis3dsh: LIS3DSH not found at 0x18 or 0x19 - check wiring
```

**원인**: `0x33`은 LIS3DSH의 고정값(`0x3F`)이 아니라 **LIS3DH 계열**(LIS3DH/
LIS2DH/LIS2DH12/LSM303DLHC/LSM303AGR - 서로 레지스터 호환)의 WHO_AM_I 값.
HW-664 모듈의 실크스크린/판매 페이지 표기와 실제 실장 칩이 달랐던 것 -
저가 브레이크아웃 모듈에서 종종 있는 일.

**의미**: "Zephyr에 LIS3DSH 드라이버가 없다"(Zephyr GitHub 디스커션 #73810로
확인된 사실)는 전제로 SSD1306과 같은 커스텀 raw I2C 드라이버를 처음에
작성했었으나, 실제 칩이 LIS3DH로 밝혀지면서 이 전제 자체가 적용되지 않게
됨 - **Zephyr는 LIS3DH를 공식 지원**하므로 커스텀 드라이버를 전부 제거하고
표준 센서 API로 재작성함(아래 2번 항목의 devicetree compatible 이슈만
해결하면 그대로 동작).

**교훈**: 판매 페이지/실크스크린에 적힌 모델명을 100% 신뢰하지 말고,
WHO_AM_I 같은 자체 식별 레지스터가 있는 칩이면 실기로 먼저 확인할 것.
이번 경우 그 덕분에 오히려 더 간단하고 표준적인 구현으로 갈 수 있었음.
(참고: 검토했던 [SparkFun_LIS3DH_Arduino_Library](https://github.com/sparkfun/SparkFun_LIS3DH_Arduino_Library)
에서 이미 "LIS3DH의 WHO_AM_I는 0x33"이라는 걸 확인해뒀던 것이 이번 실기
결과와 정확히 일치해 칩 정체를 빠르게 특정하는 데 도움이 됨.)

## 2. devicetree compatible을 `st,lis3dh`로 썼다가 링크 에러

**증상**: 칩이 LIS3DH임을 확인하고 표준 API로 전환한 뒤, devicetree
compatible을 `"st,lis3dh"`로 썼더니 컴파일은 되는데 링크 단계에서:
```
undefined reference to `__device_dts_ord_102'
undefined reference to `__device_dts_ord_103'
```
로 실패. 두 오디널 번호는 정확히 우리가 선언한 두 LIS3DH devicetree
노드에 해당함 - 노드는 만들어졌는데 그 노드에 대응하는 실제 디바이스
드라이버 인스턴스가 컴파일에 안 들어간 상태였다는 뜻.

**원인**: Zephyr GitHub 이슈 #31253("lis3dh driver support is confusing")로
확인됨 - Zephyr는 `"st,lis3dh"`라는 devicetree 바인딩 문서(YAML)를 별도로
갖고 있어서 마치 전용 드라이버가 있는 것처럼 보이지만, **실제로 디바이스를
인스턴스화하는 `lis2dh.c` 드라이버는 `"st,lis2dh"` compatible에만 반응**함
(`#define DT_DRV_COMPAT st_lis2dh`). `"st,lis3dh"`만 써서 노드를 선언하면
드라이버 자체는 `CONFIG_LIS2DH=y`로 켜져 있어도 그 노드에 대해서는 아무
디바이스도 안 만들어짐(`#warning "LIS2DH driver enabled without any
devices"`).

**해결**: 오버레이의 두 노드 모두 `compatible = "st,lis3dh"` → `compatible =
"st,lis2dh"`로 변경. 칩 자체는 실제로 LIS3DH가 맞지만(WHO_AM_I=0x33으로
확인됨), Zephyr 드라이버 쪽 요구사항이 이 compatible 문자열을 쓰라는 것.

**교훈**: Zephyr 공식 문서에 특정 compatible의 바인딩 페이지가 존재한다고
해서 그 compatible로 실제 디바이스가 만들어진다는 보장은 아님 - 레지스터
호환 칩 계열을 하나의 드라이버로 묶은 경우, 문서화된 별칭(alias) compatible과
드라이버가 실제로 반응하는 `DT_DRV_COMPAT` compatible이 다를 수 있음. 링크
타임에 `undefined reference to __device_dts_ord_N`이 뜨면 "devicetree 노드는
있는데 디바이스 인스턴스가 없다"는 뜻이므로 compatible 문자열 자체를
의심해볼 것.

## ✅ 최종 실기 검증 완료

```
[00:00:00.229,000] <err> lis2dh: Failed to read chip id.
[00:00:00.235,000] <inf> lis2dh: fs=2, odr=0x4 lp_en=0x0 scale=9576
*** Booting Zephyr OS build v4.4.0 ***
[00:00:00.237,000] <inf> app_procpu: 03_HW664_LIS3DH (core0/procpu) starting
[00:00:00.242,000] <inf> app_procpu: LIS3DH found at 0x19 (SDO/SA0 high/unconnected)
```
첫 줄의 `Failed to read chip id.`는 배선 안 된 0x18 노드 초기화 실패로,
Lab 02의 MPU6050 0x68/0x69 자동 감지 때와 동일한 정상 패턴 - 그 다음 줄에서
0x19가 `fs=2`(±2g, 예상대로)로 정상 초기화되는 것까지 확인. core1 화면도
정상 출력 확인됨.

## 참고: SparkFun LIS3DH 라이브러리와의 레지스터 맵 크로스체크

칩 정체를 LIS3DSH로 오인했던 시점에 참고한
[SparkFun_LIS3DH_Arduino_Library](https://github.com/sparkfun/SparkFun_LIS3DH_Arduino_Library)
를 검토하며 확인한 내용 (결과적으로 실제 칩과 정확히 일치하는 레퍼런스였음):

| 항목 | LIS3DH (SparkFun 라이브러리) | 참고: LIS3DSH (당시 오인했던 칩) |
| --- | --- | --- |
| WHO_AM_I 값 | `0x33` | `0x3F` |
| ODR+축 활성화 레지스터 | `CTRL_REG1`(0x20) | `CTRL_REG4`(0x20) |
| 풀스케일(FS) 레지스터 | `CTRL_REG4`(0x23) | `CTRL_REG5`(0x24) |
| 출력 레지스터(OUT_X_L~OUT_Z_H) | 0x28~0x2D | 0x28~0x2D (동일) |
