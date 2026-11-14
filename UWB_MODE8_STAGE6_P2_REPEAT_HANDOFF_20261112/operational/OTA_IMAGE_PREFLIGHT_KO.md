# OTA / USB 업데이트 이미지 사전 검사 — 안전한 인수 경계

\`ops_image_preflight.py\`는 지정 BIN이 실제로 Core3.3.7의 해당 역할 빌드에서 생성됐는지 (SHA-256, 이미지 크기, FQBN, 서명 없는 ESP32 앱 헤더 및 내부 SHA256) 검사합니다.

\`\`\`bash
python operational/host/ops_image_preflight.py --role anchor --image firmware/anchor_p2_repeat/dist/anchor/ANCHOR_DUAL_DS_CIR.ino.bin --receipt firmware/anchor_p2_repeat/dist/target_build_receipt.json --out anchor_image_intent.json
\`\`\`

**이 검사는 호스트 측 사전 검사일 뿐입니다.** ArduinoOTA의 업로드 프로토콜에 이미지를 실제로 적용하는 동작이나 ESP32 부트로더 서명·실제 자동 rollback은 포함하지 않습니다. 승인되지 않은 임의 TCP/OTA 프로그램이 검사를 우회할 수 있으므로 OTA 포트 3232를 인터넷에 노출하지 말고 격리망에서만 사용해야 합니다.

보드의 **실제 OTA 파티션 크기, App rollback Kconfig/bootloader, Secure Boot/Flash Encryption, 잘못된 역할 이미지와 전원 차단 복구**는 반드시 실기에서 확인해야 합니다. 복구 가능한 전체 4MiB 플래시 백업 없이 기존 운영 이미지를 덮어쓰지 마십시오.
