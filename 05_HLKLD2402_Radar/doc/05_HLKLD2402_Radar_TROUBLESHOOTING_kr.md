**한국어** | [English](./05_HLKLD2402_Radar_TROUBLESHOOTING_en.md)

# [Troubleshooting] Lab 05: HLK-LD2402 레이더 — 증상별 점검 가이드

이 문서는 Lab 05 (`../README_kr.md`)를 실제 하드웨어(ESP32-S3-DevKitC-1 + HLK-LD2402 + SSD1306)에서 검증하는 과정에서 마주칠 수 있는 증상과 그 점검 방법을 정리한 것입니다. 수업 자료 본문은 최종 동작 구조만 설명하므로, 배선/통신 문제로 막혔을 때는 이 문서를 먼저 참고하십시오.

> 센서 프로토콜 자체(UART 포맷, 명령/ACK, 파라미터 ID)에 대한 배경은 저장소 루트의 [`HLK-LD2402_sensor_research_kr.md`](../../HLK-LD2402_sensor_research_kr.md)와 Hi-Link 공식 사용자 설명서(`HLK-LD2402_User_Manual.pdf`, 저장소 루트)를 참고하십시오.

## 증상별 점검

| 증상 | 점검 |
|---|---|
| `engineering mode attempt` 계속 실패, `txt`만 증가 | TX/RX 교차 배선 확인 (`R` ← GPIO17). 수신은 되는데 송신이 안 되는 전형적인 경우입니다 |
| `bin`·`txt` 모두 0 | GPIO18 ↔ LD2402 `T` 배선, 전원, 115200 bps 확인 |
| `bad` 증가 | 전원 노이즈 또는 GND 공유 확인. `ovf` 증가 시 `RX_RING_SIZE`를 늘립니다 |
| 화면이 전혀 안 나옴 | I2C1 배선(GPIO4/5)과 SSD1306 전원 확인. raw driver가 0x3C/0x3D 모두 응답이 없으면 init 실패 |
| `CMake configure failed for Zephyr project: remote` | sysbuild 출력의 **위쪽**에 remote 이미지의 실제 에러(DTS 또는 Kconfig)가 찍혀 있습니다. tail만 보면 원인이 보이지 않습니다 |
| 사람이 없어도/어디 있어도 항상 비슷한 거리 (예: 150 cm) | 그 거리 gate의 배경 반사가 threshold를 넘음. 책상 위에 눕혀 두었다면 천장, 모니터, 벽이 원인 → 부팅 로그의 threshold와 빈 방의 바 그래프 비교 → 센서를 세워서 사람 쪽을 향하게 설치 → Boot 버튼 2초 길게 눌러 자동 threshold 실행, 또는 아래 "PC용 HLK-LD2402 Tool" 참고 |
| 손을 10 cm 이내로 대면 0 cm | 정상 동작 (gate 0 = 0~70 cm, 근거리 강한 반사) |
| 화살표가 깜빡이거나 반대로 나옴 | 콘솔 speed 값 확인. 노이즈가 크면 `RADAR_TREND_ENTER_CMS`/`CONFIRM` 상향, 반응이 느리면 `RADAR_TREND_WINDOW` 축소 |
| 항상 `LINK: FAIL` | `ipm0` 활성화 여부(procpu overlay)와 sysbuild로 APP_CPU 이미지가 함께 빌드됐는지 확인 |

## PC용 HLK-LD2402 Tool로 튜닝하기 (보드 코드 수정 없이)

위 표의 "항상 비슷한 거리로 잡힘"처럼 threshold 자체를 조정해야 하는 증상은, 이 Lab의 코드(Boot 버튼 2초 → 자동 threshold)로도 해결되지만, Hi-Link에서 제공하는 **PC용 설정 도구("HLK-LD2402 Tool")**를 쓰면 더 편합니다.

- 다운로드: <https://h.hlktech.com/Mobile/download/fdetail/318.html>
- USB-UART 어댑터로 LD2402를 PC에 직접 연결해서 사용합니다 (ESP32-S3를 거치지 않고, 센서의 UART T/R 핀에 직결).
- 실시간으로 16-gate 에너지 그래프를 보면서 gate별 motion/micro threshold를 눈으로 보고 조정할 수 있고, 조정한 값은 레이더 자체 flash에 저장됩니다 — 이후 ESP32-S3에 다시 연결해도 그대로 적용됩니다.
- 이 Lab의 자동 threshold 루틴(`0x0009`/`0x000A`/`0x00FD`, 상단 "5.5.2" 참고)은 이 PC 툴과 **같은 하드웨어 설정을 다른 경로로 쓰는 것**뿐이라, 둘 중 편한 쪽을 쓰면 됩니다 — 둘 다 레이더 자체 flash에 저장하므로 서로 충돌하지 않습니다.
- PC 툴로 작업할 때는 당연히 ESP32-S3 쪽 UART1이 LD2402를 점유하고 있지 않아야 합니다 (즉 ESP32-S3를 끄거나 LD2402의 T/R을 PC 어댑터로 바꿔 물려야 함).
