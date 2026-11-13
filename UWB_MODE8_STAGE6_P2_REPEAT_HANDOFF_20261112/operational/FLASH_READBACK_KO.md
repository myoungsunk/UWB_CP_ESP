# Mode 8 — 실제 보드 애플리케이션 플래시 READ-ONLY SHA 검증

이 도구는 USB-시리얼을 통해 **읽기만** 수행합니다. 펌웨어를 플래시에 쓰거나 erase하지 않습니다. 설치 이미지가 `ops_image_preflight.verify()`의 역할·ESP32 세그먼트·빌드 영수증 기준을 통과해야 실행됩니다.

```bash
# 예상 명령만 출력 (보드에 접근하지 않음)
python operational/host/ops_flash_readback.py --role anchor --serial COM4 \
  --image firmware/anchor_p2_repeat/dist/anchor/ANCHOR_DUAL_DS_CIR.ino.bin \
  --build-receipt firmware/anchor_p2_repeat/dist/target_build_receipt.json \
  --running-app-offset 0x10000 --out captures/anchor_readback_receipt.json
# 해당 보드의 실제 실행 app partition offset을 장치 상태 및 파티션 맵으로 확인한 뒤에만:
# 위 명령에 --execute-readback 을 추가
```

**중요**: 0x10000은 예시일 뿐입니다. OTA 슬롯으로 부팅했으면 실제 실행 영역의 오프셋이 다를 수 있습니다. readback 바이트가 일치하더라도 *현재 부팅해서 실행한 이미지가 이 파티션임을 이 툴이 증명하지 않습니다*. 또한 flash encryption 보드에서 readback은 암호문일 수 있습니다. 부팅 상태·image role·efuse secure boot 상태·전력/PLL/RF 실측·실제 4MiB 전체 백업은 별도 확인해야 합니다.
