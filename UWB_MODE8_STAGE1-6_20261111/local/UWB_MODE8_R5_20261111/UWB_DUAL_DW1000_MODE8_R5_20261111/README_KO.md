# UWB Dual-DW1000 Mode 8 — R5 소스·호스트 적대적 재감사 패키지

**호스트 적대적 재감사 통과 · ESP32 타깃 빌드 대기.**

- R5는 한쪽 POLL 누락 시 거리 소실, 잘못된 앵커 TX의 P2 승인, 빌드 입력 해시 누락과 Windows CP949 시험 오류를 수정합니다.
- 펌웨어 C++ 소스와 장치 식별자 `D8_R4_SOURCE_20261111`는 R4와 동일합니다. R5는 호스트 도구와 소스 배포본의 버전입니다.
- 두 역할 모두 현재 빌드 입력과 일치하는 버전 2 빌드 기록이 있어야 업로드 사전 검사를 통과합니다. 이 배포본의 `target_build`는 `false`입니다.

## 먼저 읽을 문서

[`docs/R5_REAUDIT_KO.md`](docs/R5_REAUDIT_KO.md) · [`docs/BUILD_AND_FLASH_KO.md`](docs/BUILD_AND_FLASH_KO.md)

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
