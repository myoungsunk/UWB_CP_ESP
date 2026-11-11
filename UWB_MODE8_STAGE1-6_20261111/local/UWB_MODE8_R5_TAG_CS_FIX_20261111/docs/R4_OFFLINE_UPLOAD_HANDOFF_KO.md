# R4 — 오프라인 업로드·현장 검증 가능성 판정

**현재 결론:** 제공된 R3 BIN/ELF는 형식과 내부 배치가 정상인 플래시 **후보**였으나, R4 수정 전 코드이므로 최종 시험용으로 추천하지 않습니다. R4 수정 소스는 호스트 시험에 통과했지만 R4 타깃 `.bin/.elf`가 아직 없습니다. 따라서 `FLASH_HOLD`입니다.

## 무엇까지 오프라인으로 확인했나

| 검사 | R3 패키지 | R4 수정본 |
|---|---|---|
| 실제 ESP32 컴파일/링크 receipt | 2역할 success=0 exit code 기록 | 재빌드 미실시 |
| BIN/ELF/bootloader/partition SHA 검산 | 일치 | 없음 |
| ESP32 앱 이미지 magic·Xtensa ELF 헤더 | 일치 | 없음 |
| merged BIN 내 0x1000/0x8000/0x10000 위치 | 일치 | 없음 |
| 소스와 빌드 이미지가 같은 수정본인지 | R3 이전 빌드 receipt에는 결합 SHA 없음 | 빌드 도구에서 강제 구현; 실제 재빌드 후 검사 필요 |
| 실제 COM 포트·USB 부팅·RF 수신 | 보드 없음 · 미실시 | 보드 없음 · 미실시 |

## 올바른 배선

- 태그 A: CS21 IRQ22 RST17, B: CS25 IRQ26 RST27, 공통 SCK18/MISO19/MOSI23.
- 앵커는 원본과 동일하게 CS21 IRQ22 RST5. 태그 GPIO4·5는 스위치 제거 후 사용하지 않습니다.

## 단계별 실행 (Windows, 테스트 보드 권장)

1. 운영 보드 전체 플래시 백업 및 기존 부트·등록키·복구 이미지 확보. 운영 모드를 재생성할 키가 패키지에 없습니다.
2. `arduino-cli`와 `esp32:esp32` Core 3.3.7을 사용해 `python tools/build.py` 실행. 앵커/태그 모두 빌드 성공 필요.
3. `python tools/offline_preflight.py --out dist/offline_preflight.json` 가 **exit 0 / flash_preflight_pass=true**인지 확인.
4. 배선 확인 후 `python tools/upload.py --role anchor --port COM5`, `python tools/upload.py --role tag --port COM6`. 명령 실패/다른 포트 사용 시 중단.
5. `python tools/usb_smoke.py --role anchor --port COM5`, `python tools/usb_smoke.py --role tag --port COM6`에서 HELLO·SELFTEST·DEV_ID PASS 확인. **USB-only: RF 송신을 요청하지 않음.**
6. 동일 컴퓨터에서 `python tools/run_pair.py --anchor COM5 --tag COM6 --stage 2 --count 20 --out captures/stage2` 수행. 정상 동시 RX + 4 CIR 통과 후 Stage 3, Stage 4, Stage 5를 순차 실행.
7. `capture_receipt.json`의 audit + shutdown + pass를 확인하고, `tools/analyze.py`의 DS-TWR·공통 물리 지연축·FP·CIR 로그를 검산.

## 중단 조건

- R4 BIN·ELF가 없거나 소스 해시 불일치: 플래시 금지.
- 두 보드 중 하나라도 SELFTEST 실패 또는 DEV_ID!=0xDECA0130: RF 시험 금지.
- SPI 비트 시프트, RXENAB 이후 ID 소실, 전원 불안정, CS 동시 LOW: RF 시험 중단.
- stage2에서 동일 Probe의 A/B AIR 불일치, CIR CRC/반복 읽기 실패: Stage3 이상 진입 금지.
- Tx 슬롯 시간/약한 신호에서 누락/50ms 간섭 문제, 기본 안테나 지연 미보정: 거리 정확도 승인 금지.

**현재 수락 판정은 `R4_SOURCE_HOST_PASS / R4_TARGET_BUILD_HOLD / HW_NOT_RUN`입니다.** 타깃 컴파일/업로드/실측 완료를 주장하지 않습니다.
