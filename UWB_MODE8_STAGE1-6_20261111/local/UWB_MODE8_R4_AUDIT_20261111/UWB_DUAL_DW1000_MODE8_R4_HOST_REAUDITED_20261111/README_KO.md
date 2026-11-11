# UWB Dual-DW1000 Mode 8 — R4 소스·호스트 적대적 재감사 패키지

**상태: R4_SOURCE_FIXED / HOST_ADVERSARIAL_PASS / R4_TARGET_BUILD_HOLD / HARDWARE_NOT_RUN.**

- R3 타깃 BIN/ELF가 있던 원본 ZIP과 구분하십시오. R3 BIN은 R4 수정 코드를 포함하지 않습니다.
- R4는 RF 송신 버퍼 2바이트 경계 초과와 CIR·수신 모듈 표시 신뢰 경계 문제를 수정했습니다.
- 실제 ESP32 빌드 도구가 이 실행 환경에 없어 **R4 바이너리는 생성하지 않았습니다**. 기존 R3 BIN을 R4 BIN으로 전달하거나 업로드하지 마십시오.
- 새 `tools/build.py`는 컴파일 입력 파일 전체의 SHA-256 계약을 기록하며, `tools/upload.py`는 이를 검사합니다. 두 역할 모두 R4로 빌드돼야 업로드가 허용됩니다.
- R4 수정 소스·호스트 테스트·실제 R3 이미지 정적 검사 기록·하드웨어 수락 절차를 제공합니다.

## 먼저 읽을 문서

`docs/R4_REAUDIT_REPORT_KO.md`, `docs/R4_OFFLINE_UPLOAD_HANDOFF_KO.md`, `docs/BUILD_AND_FLASH_KO.md`.

## Windows (Arduino CLI 및 ESP32 Core 3.3.7 준비된 환경)

```powershell
python tools/verify_package.py
python tools/sync_shared.py --check
python -m unittest discover -s tests -p "test_*.py" -v
python tools/build.py
python tools/offline_preflight.py
# preflight pass인 경우에만 업로드. COM은 실장 보드에 맞게 확인.
python tools/upload.py --role anchor --port COM5
python tools/upload.py --role tag --port COM6
python -m pip install -r requirements.txt
python tools/usb_smoke.py --role anchor --port COM5 --out captures/anchor_usb.json
python tools/usb_smoke.py --role tag --port COM6 --out captures/tag_usb.json
python tools/run_pair.py --anchor COM5 --tag COM6 --stage 2 --count 20 --out captures/stage2
```

Stage 2는 첫 RF 수신 시험입니다. USB smoke는 HELLO/SELFTEST만 수행하고 RF TX는 요청하지 않습니다.

## 하드웨어 경계

- 태그: SPI18/19/23 공유, A CS21/IRQ22/RST17, B CS25/IRQ26/RST27, RF 스위치 없음.
- 앵커: 기존 CS21/IRQ22/RST5. GPIO5는 **앵커 리셋**입니다.
- RSTn은 LOW / High-Z 제어; RF 안테나 지연은 초기 미보정이며 51 ms는 명목 슬롯 일정입니다.
- 기존 Wi-Fi/TCP/OTA/Mode3–7와 통합되지 않은 USB 전용 측정 소스입니다. 운영 장비 백업 없이는 덮어쓰지 마십시오.
