**한국어** | [English](./04_ESP32S3_PowerMode_TROUBLESHOOTING_en.md) · [← 랩 README](../README_kr.md)

# Lab 04 트러블슈팅 (ESP32-S3 Power Mode) - CONFIG_PM 시행착오 기록

이 문서는 [Lab 04](../README_kr.md)를 실제 하드웨어(ESP32-S3 DevKitC-1)에서 검증하는 과정에서 만난 빌드 오류·런타임 크래시와, 각각의 원인 분석 및 최종적으로 `CONFIG_PM=n`(소프트웨어 정책 방식)으로 안정화하기까지의 전체 과정을 정리한 것입니다.

수업 자료 본문([README_kr.md](../README_kr.md))은 최종 안정화된 구조만 설명하고 있으므로, "왜 Zephyr의 `CONFIG_PM` 기반 실제 Light-Sleep을 쓰지 않고 소프트웨어 정책으로 구현했는지"가 궁금하다면 이 문서를 참고하시면 됩니다. 같은 문제를 만난 분들을 위한 디버깅 참고 자료이기도 합니다.

> 환경: Zephyr `v4.4.0-13070-g6d1d551f6080`, board target `esp32s3_devkitc/esp32s3/procpu` / `.../appcpu`, `west build --sysbuild`

---

## 1. 빌드 오류: `gpio-leds` 바인딩 없이 GPIO 노드를 선언한 경우

### 증상

OLED 전원 게이팅용 GPIO(`oled-pwr`)를 오버레이에 아래처럼 선언하고 빌드하면:

```dts
/ {
    oled_pwr_ctrl {
        oled_power_pin: oled_power_pin {
            gpios = <&gpio0 6 GPIO_ACTIVE_HIGH>;
        };
    };
};
```

DTS 컴파일은 통과하지만, C 코드에서 `GPIO_DT_SPEC_GET(DT_ALIAS(oled_pwr), gpios)`을 쓰는 순간 아래와 같은 링크/빌드 오류가 발생합니다.

```
error: '__device_dts_ord_DT_N_S_oled_pwr_ctrl_P_gpios_IDX_0_PH_ORD' undeclared
```

### 원인

Zephyr의 devicetree 코드 생성기는 노드에 매칭되는 **바인딩(YAML, `compatible` 프로퍼티)** 이 있어야, `GPIO_DT_SPEC_GET()` 같은 매크로가 필요로 하는 프로퍼티 접근자(`DT_GPIO_CTLR`, `DT_GPIO_PIN` 등)를 만들어 줍니다. `compatible` 없이 `gpios` 프로퍼티만 있는 노드는 DTS 파일 자체는 유효하지만, 이 접근자 매크로들이 생성되지 않아 컴파일 시점에 위와 같이 깨집니다.

### 해결

Zephyr in-tree의 `gpio-leds` 바인딩을 "빌려서" 씁니다. 이 바인딩은 부모 노드가 `compatible = "gpio-leds";`를 갖고, 각 자식 노드가 `gpios` 프로퍼티를 갖는 구조를 그대로 지원하며, 자식 노드 자체에는 별도의 `compatible`이 필요 없습니다. LED가 아니라 순수 on/off 제어용 GPIO(OLED 전원 게이트 등)에도 문제없이 쓸 수 있습니다.

```dts
/ {
    oled_pwr_ctrl {
        compatible = "gpio-leds";

        oled_power_pin: oled_power_pin {
            gpios = <&gpio0 6 GPIO_ACTIVE_HIGH>;
        };
    };

    aliases {
        oled-pwr = &oled_power_pin;
    };
};
```

이후 `GPIO_DT_SPEC_GET(DT_ALIAS(oled_pwr), gpios)`이 정상적으로 컴파일됩니다.

---

## 2. 런타임 크래시: `CONFIG_PM_DEVICE=y` + LIS2DH 센서 드라이버 충돌

### 증상

`CONFIG_PM=y` + `CONFIG_PM_DEVICE=y`를 켠 상태로 플래시하면, 부팅은 되지만 얼마 지나지 않아 아래와 같은 로그가 반복되다가 크래시합니다.

```
[00:00:0x.xxx,000] <err> lis2dh: failed to read reg_ctrl1
[00:00:0x.xxx,000] <err> pm_device: Device lis3dh@19 did not enter suspended state (-116)
[00:00:0x.xxx,000] <err> app_procpu: Could not read accel axis data
...
***** BOOTING ZEPHYR OS (FATAL EXCEPTION) *****
E (xxxxx) esp_core_dump_flash: ...
Exception cause: 29 (StoreProhibitedCause)
```

`EXCCAUSE 29`(store prohibited, 잘못된 주소 쓰기 시도)는 대개 NULL 포인터 역참조나 이미 해제/손상된 메모리에 쓰기를 시도했을 때 발생합니다.

### 원인

`CONFIG_PM_DEVICE=y`를 켜면, Zephyr는 시스템이 Light-Sleep에 진입/복귀할 때마다 PM_DEVICE를 지원하는 모든 드라이버에 suspend/resume 훅을 호출합니다. 이 Zephyr 스냅샷의 LIS2DH 드라이버는 이 suspend/resume 훅 처리 과정에서 I2C0 버스 상태를 온전히 복구하지 못하는 것으로 보이며, 그 결과 이후의 레지스터 읽기(`reg_ctrl1`)가 실패하고 관련 내부 상태가 손상되어 최종적으로 NULL 포인터 접근 크래시로 이어집니다.

이 조합(LIS2DH 드라이버 + `CONFIG_PM_DEVICE` + ESP32-S3 SoC PM 드라이버)은 실기에서 재현성 있게 크래시를 일으켰습니다.

### 시도한 해결 1

`CONFIG_PM_DEVICE=y`를 끄고 `CONFIG_PM=y`만 유지, 그리고 그 사이 실수로 함께 빠졌던 `CONFIG_COUNTER=y` / `CONFIG_COUNTER_RTC_ESP32=y`를 복원 → 크래시는 사라졌으나, 아래 3번 항목의 경고가 계속 발생.

---

## 3. 경고: `Sleep skipped. Make sure RTC counter driver is enabled.`

### 증상

크래시는 없지만, 매번 Light-Sleep 진입을 시도할 때마다 아래 경고가 콘솔에 반복 출력됩니다.

```
[00:00:00.367,000] <wrn> soc_pm: Sleep skipped. Make sure RTC counter driver is enabled.
```

`CONFIG_COUNTER=y` / `CONFIG_COUNTER_RTC_ESP32=y`는 이미 켜져 있는데도 발생했습니다.

### 원인

ESP32-S3 기본 SoC devicetree(dtsi)에는 `rtc_timer` 노드(`compatible = "espressif,esp32-rtc-timer"`)가 **`status = "disabled"`** 상태로 들어 있습니다. Kconfig 심볼(`CONFIG_COUNTER_RTC_ESP32`)을 켜는 것만으로는 드라이버 코드가 빌드에 포함될 뿐, devicetree 노드 자체가 비활성 상태면 실제 디바이스 인스턴스는 생성되지 않습니다.

바인딩 문서의 "별도로 켤 필요 없이 전원 인가 시점부터 자체적으로 카운트한다"는 설명은 **물리적인 RTC 카운터 하드웨어**에 대한 설명이며, devicetree 노드의 `status` 프로퍼티와는 별개입니다. 이 설명 때문에 devicetree에서 노드를 명시적으로 활성화해야 한다는 사실을 놓치기 쉬웠습니다.

### 해결

보드 오버레이에 아래를 추가해서 노드를 명시적으로 활성화합니다.

```dts
&rtc_timer {
    status = "okay";
};
```

이후 이 경고 메시지는 사라지고, 다른 메시지(4번 항목)로 바뀌었습니다.

---

## 4. 경고: `Make sure '&rtc_timer' is enabled as a wakeup source.` (그리고 재크래시)

### 증상

3번을 해결한 뒤 경고 문구가 아래처럼 바뀌었습니다.

```
[00:00:07.286,000] <wrn> soc_pm: Sleep skipped. Make sure '&rtc_timer' is enabled as a wakeup source.
```

### 원인

Zephyr의 디바이스 wakeup-source 메커니즘은 두 단계로 구성됩니다.

1. devicetree에서 `wakeup-source;` 프로퍼티를 노드에 추가하는 것은 "이 디바이스가 wakeup source가 **될 수 있다**"는 능력만 선언할 뿐입니다.
2. 실제로 wakeup source로 **활성화**하려면, 런타임에 `pm_device_wakeup_enable(dev, true)`를 호출해야 하며, 이 API는 Device PM 프레임워크의 일부이므로 `CONFIG_PM_DEVICE=y`가 다시 필요합니다.

즉, 2번 항목에서 크래시 원인으로 지목해 껐던 `CONFIG_PM_DEVICE`가, RTC 타이머를 wakeup source로 쓰려면 다시 필요한 구조적 충돌이 있었습니다.

### 시도한 해결 2 (전체 조합 재시도)

이 시점에서 두 가지 선택지가 있었습니다.

- (A) `CONFIG_PM=n`으로 완전히 끄고 Ultra-Sleep을 소프트웨어 정책으로만 문서화
- (B) `CONFIG_PM_DEVICE=y` + `wakeup-source;` + `pm_device_wakeup_enable()`을 모두 갖춰서 정공법으로 재시도

우선 (B)를 시도했습니다: `CONFIG_PM_DEVICE=y`를 다시 켜고, `&rtc_timer`에 `wakeup-source;`를 추가하고, `main()`에서 `pm_device_wakeup_enable(rtc_timer_dev, true)`를 호출하도록 구현했습니다.

### 결과: 크래시 재발

이 조합으로 플래시하자 2번 항목과 동일한 크래시(`lis2dh` 레지스터 읽기 실패 → `pm_device` suspend 실패 → `EXCCAUSE 29`)가 다시 발생했습니다. 즉 `wakeup-source` 설정 여부와 무관하게, **`CONFIG_PM_DEVICE=y`가 켜져 있는 것 자체가 LIS2DH 드라이버와 충돌**하는 근본 원인이라는 것이 재확인되었습니다.

---

## 5. 최종 결정: `CONFIG_PM=n` + 소프트웨어 정책

`CONFIG_PM_DEVICE`를 다시 끄지 않는 한 이 크래시를 피할 수 없고, `CONFIG_PM_DEVICE` 없이는 RTC 타이머를 wakeup source로 활성화할 방법이 없어 실제 Light-Sleep 진입/기상 자체가 의미 있게 동작하지 않는 구조적 충돌이었습니다. 이에 따라 아래와 같이 최종 정리했습니다.

- `CONFIG_PM=n`으로 Zephyr System Power Management 자체를 끕니다 (`CONFIG_PM_DEVICE`, `CONFIG_PM_DEVICE_RUNTIME`, `CONFIG_POWEROFF`, RTC counter 관련 설정 전부 제거).
- 오버레이에서 `&rtc_timer { status = "okay"; wakeup-source; };` 블록을 제거합니다.
- `main.c`에서 `pm_device_wakeup_enable()` 호출과 관련 include(`<zephyr/pm/device.h>`)를 제거합니다.
- **Ultra-Sleep 모드는 실제 SoC Light-Sleep 진입이 아니라, "1초 주기로 느리게 폴링하고, 움직임이 감지되면 `sys_reboot(SYS_REBOOT_WARM)`으로 Normal 모드로 복귀하는" 순수 소프트웨어 정책**으로 재정의합니다. 평균 소비 전류를 낮추는 효과(짧은 처리 후 대부분 시간을 `k_msleep()`으로 대기)는 여전히 있지만, ESP32-S3 칩 자체를 실제 Light-Sleep 상태로 전이시키는 것은 아닙니다.

이 조합으로 재검증한 결과, `lis2dh` 오류·`pm_device` 오류·`Sleep skipped` 경고·크래시가 모두 사라지고 전체 10단계 시나리오가 안정적으로 동작했습니다.

### 참고: Deep-Sleep을 애초에 시도하지 않은 이유

ESP-IDF의 네이티브 Deep-Sleep API(`sys_poweroff()` + `esp_sleep_enable_timer_wakeup()`)를 Zephyr에서 사용하는 경로도 검토했으나, 이 조합은 워치독 리셋 루프나 부팅 실패를 유발하는 것으로 보고된 업스트림 Zephyr 이슈가 있습니다 (zephyr-rtos/zephyr#86193, ESP32/ESP32-S3 모두 영향, "not planned"로 닫힘). 이런 이유로 이 실습에서는 Deep-Sleep 경로 자체를 처음부터 시도하지 않았습니다.

---

## 요약 표

| 단계 | 설정 | 결과 |
|---|---|---|
| 초기 WIP | `oled-pwr` GPIO에 `compatible` 없음 | 빌드 오류 (`__device_dts_ord_...` undeclared) |
| 수정 1 | `gpio-leds` 바인딩 적용 | 빌드 성공 |
| 수정 2 | `CONFIG_PM=y` + `CONFIG_PM_DEVICE=y` | 런타임 크래시 (`EXCCAUSE 29`) |
| 수정 3 | `CONFIG_PM_DEVICE` 제거, `CONFIG_PM=y`만 유지 | 크래시 없음, `Sleep skipped` 경고 (RTC counter) |
| 수정 4 | `&rtc_timer { status = "okay"; }` 추가 | 경고 문구 변경 (wakeup-source 요구) |
| 수정 5 | `CONFIG_PM_DEVICE=y` + `wakeup-source;` + `pm_device_wakeup_enable()` 재도입 | 크래시 재발 |
| **최종** | **`CONFIG_PM=n`, Ultra-Sleep은 소프트웨어 정책** | **안정 동작 확인** |
