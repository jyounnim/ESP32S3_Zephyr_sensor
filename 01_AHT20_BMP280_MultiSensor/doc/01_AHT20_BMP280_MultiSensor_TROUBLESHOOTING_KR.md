**한국어** | [English](./01_AHT20_BMP280_MultiSensor_TROUBLESHOOTING_en.md) · [← 랩 README](../README_kr.md)

# Lab 01 트러블슈팅 (AHT20 + BMP280 MultiSensor, 듀얼코어/IPM)

실기 검증 과정에서 실제로 겪은 이슈와 해결책만 정리했습니다.

## 1. sysbuild가 MCUboot를 같이 빌드하려다 실패

**증상**: `west build --sysbuild ...` 시 `CMake configure failed for Zephyr
project: mcuboot`로 configure 자체가 실패 (build 폴더를 완전히 지워도 재현됨).

**원인**: Espressif 보드는 `--sysbuild` 사용 시 기본적으로 MCUboot까지 같이
빌드하려 시도합니다 (Zephyr 공식 문서: "By default, the ESP32 sysbuild creates
bootloader (MCUboot) and application images"). `SB_CONFIG_BOOTLOADER`는
Kconfig의 **choice(단일 선택)** 항목이라, 원치 않는 멤버를 `SB_CONFIG_
BOOTLOADER_MCUBOOT=n`으로 꺼봐야 소용없습니다 - **대안 멤버를 명시적으로
`=y`로 선택**해야 choice가 바뀝니다.

**해결**: `lab/sysbuild.conf`에 다음을 추가:
```
SB_CONFIG_BOOTLOADER_NONE=y
```

## 2. IPM Kconfig 심볼명이 문서와 다름

**증상**: `prj.conf`에 `CONFIG_IPM_ESP32=y`를 넣었더니 `undefined symbol
IPM_ESP32`로 Kconfig abort.

**원인**: Zephyr `drivers/ipm/Kconfig` 원본을 확인한 결과, ESP32 IPM 드라이버의
실제 심볼명은 `CONFIG_ESP32_SOFT_IPM`이었음 (`IPM_ESP32`라는 심볼 자체가
존재하지 않음).

**해결**: `prj.conf`에 `CONFIG_ESP32_SOFT_IPM=y` 사용. `&ipm0` devicetree
노드가 `okay`면 사실 `default y`라 자동으로 켜지지만, 명시적으로 남겨두는 걸
권장.

## 3. BMP280 I2C 주소 불일치 (0x76 vs 0x77)

**증상**: 부팅 로그에 `BMP280 device not ready`, 이후 모든
`sensor_sample_fetch`가 `-14`(EFAULT)로 실패. 드라이버 디버그 로그에는
`bme280_chip_init: ID read failed`만 뜨고 "bad chip id" 같은 메시지는 없었음 -
즉 응답이 아예 없었다는 뜻.

**원인**: 오버레이 기본값은 0x76(SDO→GND 가정)이었는데, 실제 모듈은 SDO가
VDD에 묶여 있어 0x77이 정답이었음. 저가형 AHT20+BMP280 콤보 보드에서 흔한
패턴.

**해결**: `lab_tools/i2c0_scanner`(1바이트 write 기반 프로빙)로 실제 버스를
스캔해 `0x38`(AHT20)과 `0x77`(BMP280)을 직접 확인 후, 오버레이의
`bmp280@76`/`reg = <0x76>`를 `bmp280@77`/`reg = <0x77>`로 수정.

**교훈**: 새 I2C 디바이스를 배선했으면, 오버레이에 주소를 넣기 전에 먼저
스캐너로 실제 응답 주소를 확인할 것 - datasheet 기본 주소와 실제 모듈의 SDO
결선이 다른 경우가 흔함.

## 4. AHT20이 CRC는 통과하는데 물리적으로 불가능한 값을 반환

**증상**: 온도 ~145°C, 습도 정확히 ~50%가 계속 나옴. CRC 검증(`CONFIG_
DHT20_CRC=y`)을 켜도 에러 없이 통과 - 즉 전송 손상이 아니라 센서가 그 값을
그대로 보내고 있다는 뜻.

**진단**: 습도 ~50%는 20비트 ADC 필드의 정확히 절반값(raw ≈ 0x80000, MSB
하나만 켜진 패턴)과 일치 - "실제 측정값"보다는 "ADC 리셋/미변환 기본값"의
전형적인 패턴. 실기에서 AHT20 모듈의 VCC를 뽑았다 다시 꽂으니 정상 값이
들어오는 것을 확인함 - MCU 리셋만으로는 안 풀리는, 센서 자체 전원 재인가가
필요한 멈춤 상태였음.

**해결**: 현장에서 매번 전원을 뽑을 순 없으므로, AHT20 공식 소프트 리셋
명령(`0xBA`, 파라미터 없음, ~20ms 소요)을 Zephyr 센서 API 밖에서 직접 I2C로
전송하는 자가복구 로직을 추가함 - 읽은 온도가 AHT20 동작 범위(-40~85°C)를
벗어나면 자동 트리거. `main.c`의 `aht20_soft_reset()` 참고.

**한계**: 이 소프트 리셋이 물리적 전원 재인가와 완전히 동일한 효과를 내는지는
datasheet상으로는 그렇다고 되어 있으나 모든 상황에서 100% 보장되진 않음.
드물게 BMP280 기압도 같이 이상해지는 경우가 관측되어(600hPa대), 개별 칩
불량보다 전원/배선 품질(브레드보드 접촉, 디커플링 커패시터 부재) 쪽 공통 원인
가능성도 열어두고 있음 - 설계 문서 "알려진 제약사항" 참고.

## 5. OLED에 'K' 글자가 안 보임 ("LINK"가 "LIN : O"처럼 표시됨)

**증상**: 화면에 `LINK: OK`를 그렸는데 실제로는 `LIN : O`(공백)처럼 보임 - O가
숫자 0처럼 오인되기도 함.

**원인**: 이 랩을 위해 새로 만든 5x7 폰트(`font5x7.h`)에 대문자 'K'가 애초에
없었음 (AHT20/BMP280 라벨에는 K가 필요 없어서 누락). `font5x7_lookup('K')`가
NULL을 반환하면 해당 칸은 빈 칸으로 렌더링됨.

**해결**: 폰트에 'K'와 (기압 단위 `hPa` 표기에 필요한) 소문자 'h', 'a'를
추가.

**교훈**: 라벨 문자열을 바꾸거나 늘릴 때는, 그 문자열에 쓰인 모든 문자가
폰트 테이블에 있는지 확인할 것 - 누락된 문자는 컴파일/런타임 에러 없이
조용히 빈 칸으로 렌더링되어 원인 파악이 헷갈리기 쉬움.
