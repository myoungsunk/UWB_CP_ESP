# Mode 8 앵커 펌웨어 조사

- 질문 ID `UWB-MODE8-SOURCE-HOST-QA-20261111`. 조사 시각 2026-11-11 KST. AUDITOR 자기 검토.
- 정책 `RESEARCH-GOVERNANCE-v1`. COMMON_CONTRACT.md SHA-256 `a5cca7d8a8d5f20f4a89ea9b6509da0b8d7506626aac4a47fa1d27891ff5ebdb`. ROLE_AUDITOR.md SHA-256 `e1bdc2f231c71cdc27201c688e041b078576a6adf6c0290acc16ce4d8218291a`.
- 배포본은 ZIP에서 풀어 빌드한 소스 패키지다. 앵커의 실제 소스 매니페스트 SHA-256 `b22502a268b1b5d3d660a45c3669ef8b81e0e4d8682bcd8029b0c3d5c1bc9340`을 빌드 영수증과 비교했다. 디스크의 BIN과 COM4 MAC `28:56:2f:72:e7:18` 플래시 검증 영수증의 SHA-256이 `6ba6517b3504e1349f3c668636ec1d94b09f37ed47982edce66b54c492a73336`으로 일치한다. HELLO의 버전 문자열은 소스의 고정값 `D8_R4_SOURCE_20261111`이다.
- **판정 REDIRECT.** 첫 POLL 이전에 오류가 발생한다는 재현 결과는 유효하다. 초기화 순서와 해당 오류의 인과관계 및 Stage 2 준비 판정은 보류한다.

## 실기록

- 이전 V3 대기 시험에서 RESET 응답 후 207.53 ms에 `SYS_STATUS=0x00800002`, `RF_STATUS=0x01`을 읽었다. 357.72 ms에는 `SYS_STATUS=0x02800002`, `RF_STATUS=0x03`으로 바뀌었다. 이 구간에 START/POLL 송신 없이 조회했고 2 MHz와 20 MHz 결과가 일치했다. [대기 시험 기록](../../UWB_MODE8_R5_ANCHOR_PLL_TRACE_20261111/evidence/idle_pll_timeline.json)
- 이번 COM4 조회는 `py -3 -B -X utf8 tools/read_anchor_pll.py --port COM4 --allow-fault --out evidence/anchor_firmware_investigation_20261111_163516.json`으로 수행했다. 조회 종료 코드 0, USB CRC·시퀀스 오류 0. HELLO와 PLLSTATUS에서 boot `83345513` 일치. `active=false`, `sent=0`, `first_poll_checked=false`, `hw_ok=false`, `fault_latched=true`. 2 MHz와 20 MHz 모두 `SYS_STATUS=0x02800002`, `RF_STATUS=0x03`. `EC_CTRL=0x04`로 검출 활성화. [조회 원본](anchor_firmware_investigation_20261111_163516.json) SHA-256 `7f0d65ad11a56144f601750761c6e51eb510edad2b96c45d70bd9c0489d8608b`.
- `CLKPLL_LL`은 DW1000의 래치 이벤트, `CPLLLOW`는 DW1000의 상태 표시다. 이 기록으로 실제 발진 주파수나 전원 변동은 판정할 수 없다. SYSSTAT-1에 따르면 PLL이 잠긴 경우에도 일부 잠금 상태 비트가 오보할 수 있다. [Decawave DW1000 Errata §3.6](https://www.qorvo.com/products/r/ra006714)
- 동일 ESP32 MAC에서 보존한 R5 이전 4 MiB 백업의 SHA-256 `4443326863b71589103e966d3c9f6589133809c9b646af6fbc97c4e6aeb52340`을 영수증과 대조했다. 앱 영역에서 `RF_PLATFORM_V1`, `R1J`, `RADIOCHECK`, `R1LINFO`를 확인했다. 이전 Mode 6의 같은 계열 펌웨어 기록에는 앵커 SELFTEST 및 RADIOCHECK 통과와 XTAL trim readback `0x70`이 있다. 이력으로 현재 DW1000 모듈의 일련번호 일치나 현 상태를 판정할 수 없다. [백업 영수증](../../UWB_MODE8_R5_DEPLOY_20261111/anchor_backup/backup_receipt.json) · [이전 Mode 6 진단](../../MODE6_LINK_CHECK_20261111T023224Z_hfDuXP/rf_preflight.json)

## 소스에서 확인한 항목

| ID | 위치와 관찰 | 영향과 재검증 |
|---|---|---|
| FW-01 | `vendor/anchor/DW1000Ng/src/DW1000Ng.cpp:1412`에서 자동 클록 복귀와 5 ms 지연 후 20 MHz로 전환한다. `shared/esp32_port.hpp:164`부터 고속으로 무선 설정을 쓰고 172행에서야 잠금을 검사한다. | Qorvo 데이터시트 표 2의 미잠금 최대 3 MHz를 보장하지 못한다. 잠금 확인까지 저속을 유지해 같은 보드의 대기 중 상태 변화와 고속 전환 직후 상태를 비교한다. |
| FW-02 | `DW1000Ng.cpp:351–373`의 `_enableClock()`은 `PMSC_CTRL0` 하위 2바이트를 한 SPI 트랜잭션에 쓴다. Decawave 참조 드라이버 `_dwt_enableclocks()`는 하위 바이트를 먼저 별도 트랜잭션에 쓴다. | 클록 전환 때 SPI 비트 오류가 날 수 있다. XTI/자동 전환을 참조 드라이버와 맞춰 레지스터 상태 및 대기 재현을 비교한다. 이번 관찰은 실제 해당 쓰기가 깨졌는지 판정하지 못한다. |
| FW-03 | `DW1000Ng.cpp:1394–1402`에서 XTAL trim과 SYSSTAT-1 보정을 쓰고, 그 뒤 `_manageLDE()` 안에서 OTP 값이 있을 때 LDO 보정을 적용한다. Decawave 참조 초기화는 LDO를 XTAL trim보다 먼저 적용한다. | LDO 보정이 실제 실행됐는지 현 Mode 8 USB 출력으로는 확인할 수 없다. OTP 보정 유무와 적용 여부를 기록하고 순서의 영향을 조건부로 확인한다. |

앵커 소스의 SPI 핀은 18/19/23, CS/IRQ/RSTn은 21/22/5다. 이번 조사는 소스와 이전 실기록 대조 및 USB 상태 조회까지 수행했다. 수정본 빌드, 배포, 새로운 RF 시험과 독립 검토는 미실행 상태다.

참조 [Qorvo DW1000 데이터시트 표 2](https://store.qorvo.com/datasheets/qorvo/dwm1000datasheet.pdf) · [Decawave 초기화 코드](https://github.com/Decawave/dwm1001-examples/blob/b0142ced056296169c7a3f68461b67202aabbbba/deca_driver/deca_device.c#L165-L199) · [Decawave 클록 레지스터 쓰기](https://github.com/Decawave/dwm1001-examples/blob/b0142ced056296169c7a3f68461b67202aabbbba/deca_driver/deca_device.c#L2447-L2514)
