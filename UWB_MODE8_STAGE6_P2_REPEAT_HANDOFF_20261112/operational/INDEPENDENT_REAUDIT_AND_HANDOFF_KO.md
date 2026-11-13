# Mode 8 운영 통합 — 적대적 재감사와 인수 경계

**기준**: 2026-11-12 검증 실험 브랜치 커밋 \`ce8f986793c5a12be0a020df924f05b516edf70e\`.
**통합 브랜치**: \`codex/mode8-operational-integration-20261113\`.
**판정 범위**: 연결된 GitHub에서 실제로 게시·실행한 소스, Python 적대적 호스트 시험, portable C++ 회귀, Arduino ESP32 Core 3.3.7 두 역할 타깃 컴파일. **RF 실기·전원·Wi-Fi·OTA 재부팅의 물리 시험은 실시하지 않았다.** 이는 동일 구현자가 수행한 적대적 자체 검토이며 독립 감사팀 승인서가 아니다.

## 1. 수정 코드

- 앵커/태그의 \`shared/app.hpp\`와 sketch mirrors: STOP 후에도 직렬 writer·ACK·cache에 남아 있는 기록이 새 session, 설정, 바우드, 보정과 섞이지 않도록 \`opsRecordsDrained\`, \`opsWriterBusy\` 추가. ACK/NACK 명령은 drain gate 앞에서 처리.
- 앵커 \`operational_anchor.hpp\`: HMAC challenge 방식 29310/TCP 상태 조회·측정 예약·GET/ACKFILE/PURGE, Wi-Fi OFF RF 구간, LittleFS 보존, 명시적 OTA 정비 창, 같은 보드에서의 Wi-Fi 재연결/OTA 충돌 방지. 예약 후 외부 USB STOP이 오면 RF 자동 시작을 취소하고 정비 Wi-Fi 재진입.
- 단일 정지 point **최대 32 Mode8 거래**를 RAM의 \`OpsArchiveRow\` 스키마에 보존. 모든 유효 RF 프레임을 실제 FCB8 원본 바이트로 포함; 저장 전 레코드·원본 RF 일치·소프트웨어 CRC 검사. 파일을 \`m8_pending.tmp\`에 기록·읽기 검증한 뒤 \`m8_pending.bin\`으로 rename하고 재검증. 복구 중 미완료 temp나 미회수 파일이 있으면 새 session 및 OTA 차단. 저장 후 RAM 보관 영역 해제.
- 기존 앵커의 큰 정적 \`Record\` 풀을 부팅 시 점검되는 heap 할당으로 이동해, Wi-Fi/OTA 종속성 링크 시 발생한 **DRAM .bss 8,384B overflow** 해결. \`opsPlan\` 실행 시 남은 heap이 32KiB 미만이면 시작 거부. 실제 heap 여유와 fragmentation은 실물 시험에서 확인.
- Pi 호스트 \`ops_archive.py\`, \`ops_client.py\`, \`ops_session.py\`: HMAC 원격 제어, 태그 USB 원본 수집, 앵커 오프라인 아카이브 CRC/프레임/세션/부팅/거래 검증, fsync 성공 뒤 ACKFILE, 명시적 PURGE. GET이 절단되면 임시 .partial만 남기고 확정하지 않음.

## 2. 적대적 시험 및 발견 사항

| ID | 반례 또는 위험 | 방어와 재확인 |
|---|---|---|
| ADV-01 | STOP 직후 ACK 대기·writer 큐가 남은 상태에서 SET/START | records drained 조건이 만족될 때까지 전이 거부; ACK/NACK는 계속 허용 |
| ADV-02 | P1/P2 완결을 주장하나 저장된 RF 프레임이 누락·불일치 | \`r.complete\`일 때 4개 실제 on-air frame encode 성공 강제, host parser에서 Type·session·txn·config·boot 교차 검사 |
| ADV-03 | 올바른 바깥 CRC로 재계산한 중복·변조 행 | row CRC, identity, 순차 거래, 레코드 길이, payload CRC 중첩 검사 |
| ADV-04 | TCP GET 중 연결 단절 후 파일 ACK 전송 | .partial만 보존하고 fsync+CRC·END 확인 전 ACK 금지 |
| ADV-05 | TCP 인증 없음, HMAC 오류 | AUTH_FAIL, 상태 변경 불허; 매 TCP 연결마다 nonce 갱신 |
| ADV-06 | TCP START 승인 후 USB STOP, RF 시작 카운트다운 중 | Armed 상태 취소, Wi-Fi 정비 경로 복귀. 새 RF START 금지 |
| ADV-07 | 미회수 파일·실패한 저장 temp 존재 중 OTA/새 시작 | 저장 파일 자동 삭제·포맷 금지, fail-closed |
| ADV-08 | OTA 정비 창 중 USB 측정 명령/설정 변경 | 유지보수 상태의 RF 변경 거부 |
| ADV-09 | Wi-Fi/OTA 링크 추가로 앵커 정적 DRAM 초과 | 포인터 풀로 수정해 ESP32 실제 타깃 링크 통과; 힙 런타임은 HOLD |
| ADV-10 | 기존 Stage6 P2 repeat 및 200ms 시간 관계 훼손 | 동일 RF \`engine.hpp\`·\`core.hpp\` 기준선 유지; 새로운 운영 계층은 거래 밖에서 동작 |

호스트 검증: \`operational/tests/test_archive.py\`, \`test_integration_gates.py\`; GitHub Actions \`host-adversarial\`의 실제 종료 코드·시험 출력 확인. Portable \`test_core.cpp\`는 AddressSanitizer/UBSan 빌드·실행. 소스와 sketch mirror의 해시 동등성 점검.

타깃 컴파일: GitHub Actions \`esp32-build\`, \`esp32:esp32:esp32doit-devkit-v1\`, ESP32 Core **3.3.7**. 빌드/링크 성공은 RF 수신, OTA flash, RTC/Wi-Fi 충돌, RAM 확보 및 기기간 통신을 입증하지 않는다. \`dist/target_build_receipt.json\`과 artifact 내 BIN/ELF·컴파일 로그를 사용한다.

## 3. 아직 완료되지 않은 요구·명확한 우회

1. **완전한 기존 R03/Modes3–7 운용 동등성은 미완료**. 과거 \`route_v1.hpp\`, \`remote_v3.hpp\`, \`remote_compact.hpp\`는 52B 응답·479B compact record·단일 DW1000 가정. Mode8의 96B FCB8, A/B 독립 두 왕복 교환, P2 반복, RFB8와 호환되지 않는다. **우회**: 검증된 기존 Mode8 RF 코어를 유지하면서 현재 32거래 점 단위 운영 어댑터로 정지 실험을 수행한다. 구형 route 연속 운용은 새 계약·시퀀스/attempt·저장 schema를 설계하여 별도 승인해야 한다.
2. **Wi-Fi Enterprise(EAP) 및 태그 OTA 미통합**. 현재 앵커 WPA 개인망 TCP/OTA와 Pi USB 태그 경로만 구현. **우회**: 시험용 격리 WPA AP로 앵커를 연결하고 태그는 USB 직접 업로드·복구. 과거 EAP provision/transport는 실제 네트워크 인증 조건의 별도 컴파일·실기 계약으로 이식.
3. **장기 1,000거래 Wi-Fi OFF 원격 운영 기능 미구현**. 앵커 RAM 32개 compact row만 보존. **우회**: 32거래 이하 point 단위 저장·확인·다운로드·정리 반복. 정지 없이 1,000거래가 필요하면 RAM이 아니라 외부 비휘발성 저장 또는 저장 chunk·journal 복구 설계와 새로운 전송 예산 검증이 필요.
4. **실물 보드·Pi/AP 및 현장 계측에 접근 불가**. 당장 장치 플래시, PLL 초기화 100회, RF P1/P2, Wi-Fi OFF 여부, TCP 복구, LittleFS 정전, OTA 실패 롤백을 실측할 수 없음. **우회**: \`VALIDATION_CHECKLIST_KO.md\`의 B/C/D 검증을 현장 보드에서 수행하고 원본 UART·RFB8, flash readback SHA, 메모리·컴파일 영수증을 후속 감사 자료로 보존. 업로드 전 기존 image/파티션/키 백업 및 비휘발성 저장 영역 보호.
5. **TLS/서명된 OTA·자동 rollback 미구현**. 현재 HMAC 챌린지로 제어 인증을 하지만 데이터 기밀성/이미지 서명은 없으며 ArduinoOTA는 기존의 관리 창 방식을 따름. **우회**: 실험용 폐쇄 LAN에서만 사용, OTA는 기존 복구 가능한 앱/파티션 정보를 확보한 뒤 한 번에 한 장치씩 시험. 인터넷에 TCP 29310/UDP 3232 노출 금지.

## 4. 최종 인수 계약

- **SOURCE + HOST QA**: GitHub Actions CI 모두 통과한 커밋에 한해 구현·검증됨으로 표시.
- **TARGET BUILD**: 두 ESP32 앱의 빌드와 SHA artifact 확인 가능. 컴파일된 이미지는 물리적으로 업로드·사용 검증되지 않음.
- **HARDWARE FIELD / WIFI TCP OTA**: 미실시. 합격 처리 금지.
- **운용 적합성**: 최초 운영 후보는 *WPA AP + TCP 정비 + Wi-Fi OFF 정지 point≤32 + Pi 태그 USB* 만 명시적으로 허용하는 시험 범위. 연구실의 기존 R03 완전 대체품으로 무조건 사용하지 않는다.

설치 절차·오류 주입 목록은 \`README_KO.md\` 및 \`VALIDATION_CHECKLIST_KO.md\`에 별도로 기록하였다.
