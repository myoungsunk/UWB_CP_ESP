# Mode 8 운영 통합 — 96d8430 기준 적대적 재감사 및 수정 인수 보고

## 1. 대상과 판정

- **입력 SHA**: `96d84306b1517adf4ecd19053d29e7dd4a67cdad`
- **저장소/브랜치**: `myoungsunk/UWB_CP_ESP` / `codex/mode8-operational-integration-20261113`
- **기준 패키지**: `UWB_MODE8_STAGE6_P2_REPEAT_HANDOFF_20261112`
- **코드 변경 커밋**: `03f5c7583fcba861ad0dd277cd9be482b06f48f3` (Pi/호스트), `66b764d12c7eb6686188ca36e4aac1c9e076a31e` (앵커 TCP·RAM).
- **재현 가능한 CI**: https://github.com/myoungsunk/UWB_CP_ESP/actions/runs/43373641774 (host-adversarial, esp32-build 모두 success).
- **감사자 경계**: 동일 대화 안에서 수행한 적대적 자체 감사이며 별도 제3자 감사 또는 실제 보드 시험이 아님.

**정적 소스·호스트 적대적 시험·ESP32 타깃 컴파일: PASS / 실물 Wi-Fi·RF·OTA 운영 인수: HOLD.**
빌드 결과는 수정된 해당 커밋의 CI artifacts만 사용한다. 이전 `dist/` 이미지로 신규 코드를 대표해서는 안 된다.

## 2. 확인하고 수정한 결함

| ID | 심각도 / 공격·오류 입력 | 96d8430에서의 확인 | 수정과 방어 |
|---|---|---|---|
| ADV-H01 | 높음 — 단독 `ops_client get --ack` | RF 양단 검산 없이 앵커 파일 ACK 요청 가능. 앵커는 인증된 GET 세션·CRC만 확인하며 태그 RF 검증은 알 수 없음 | 독립 GET에서 `--ack` 제거, `get_archive(ack=True)`는 TCP 접근 전 거부. 검증·ACK 자동화는 `ops_session.py`만 담당. `test_standalone_get_ack_cannot_bypass_paired_audit` |
| ADV-H02 | 높음 — 이전 부팅·세션의 `HELLO(active=true)` | START의 태그 응답이 active 비트만 확인 | `tag_start_matches`: role·boot·session·config·firmware·stage·period·baud 모두 대조. 이전 세션·다른 설정 주입 시험 |
| ADV-F01 | 높음 — TCP `START -1 1`, `START 4294967296 1` | ESP32 `strtoul`이 부호·포화 값을 허용할 수 있으며 텍스트의 숫자와 내부 32비트 값이 달라짐 | portable `parseDecimalU32`로 부호·범위초과·공백·지수 표기 거부. START 세션·count, ACKFILE session·CRC, OTA seconds에 적용 |
| ADV-F02 | 중간 — 추가 인자를 붙인 `GET`/`PURGE`/`STATUS`/`START`/`OTA`/`AUTH` | 일부 명령이 뒤따르는 토큰을 무시하고 실행 | 인증·상태·파일·시작·OTA 명령의 초과 토큰 거부, 기존 명령 결과 유지 |
| ADV-F03 | 중간 — Wi-Fi OFF 실패 또는 TCP 예약 START 취소를 반복 | RF 시작 전 확보한 32행 보관 RAM이 예약 취소·Fault 이후 남을 수 있음 | 측정이 아직 시작되지 않은 Armed 경로에서만 `delete[] opsArchive`, 실제 측정/저장 데이터는 해제하지 않음 |

실제 펌웨어가 해당 문자열을 수신한 실험이 아니라, C++ 펌웨어 mock·portable parser 및 Python 적대적 테스트로 검증했다. 승인 경계에 관한 조건 검사는 양단 실제 측정 없이도 재현 가능한 논리 시험이다.

## 3. 실행 검증과 기존 기능 보존

- **선행 기준**: 96d8430의 https://github.com/myoungsunk/UWB_CP_ESP/actions/runs/43371783608 = success.
- **호스트 수정**: 03f5c758의 https://github.com/myoungsunk/UWB_CP_ESP/actions/runs/43373243778 = host-adversarial 및 esp32-build success.
- **앵커/공통 수정**: 66b764d1의 https://github.com/myoungsunk/UWB_CP_ESP/actions/runs/43373641774 = 두 job success.
- **Python 적대적 및 회귀**: 마지막 코드 실행의 21 tests PASS (`test_archive`, `test_integration_gates`, `test_ops_session_ack`).
- **C++ 적대적 검증**: `test_transport_guard.cpp`는 GCC + ASan/UBSan에서 음수·범위초과·부적절한 수치 표현, 기존 UART/ACK/드레인 시험을 수행. `test_operational_anchor.cpp`는 검증 가능한 아카이브에서의 ACKFILE 오류 입력, Wi-Fi OFF 실패, 취소 시 RAM 해제, 복구 저널 등을 시험.
- **ESP32 타깃**: Arduino ESP32 Core 3.3.7, FQBN `esp32:esp32:esp32doit-devkit-v1`의 앵커/태그 스케치 compile/link 성공. CI artifact의 BIN/ELF/receipt만 해당 커밋의 타깃 산출물로 취급.
- **RF 물리 변경 없음**: 기존 dual-DW1000, P1/P2 재수신, CIR 추출·스냅샷·TWR 타이밍 구조 변경 없음. 단, 새로운 하드웨어 실험에서 절대 거리·지터를 재검증한 것은 아님.

### 감사 범위 제외 / 잔여 위험

1. **실제 장치 검증 미완료**: 전원·PLL, Wi-Fi OFF 상태에서 RF 32거래, Pi 수집, 재연결, 저장 원본, 전원 차단, OTA 이미지 오류/롤백, 실거리 보정은 별도 실험 필요. `VALIDATION_CHECKLIST_KO.md`를 사용한다.
2. **최대 32거래 단일 정지 point**: 기존 R03 모드 3–7 전체 운용 동등성 또는 연속 1,000거래를 제공하지 않는다.
3. **권한 있는 호스트에 대한 신뢰**: HMAC 인증된 직접 TCP 클라이언트는 여전히 자체적으로 GET/ACKFILE 명령을 구성할 수 있다. 앵커가 단독으로 태그 측 RF 검산을 증명하는 것은 아니므로 승인 정책은 신뢰된 `ops_session.py` 실행 주체·키 보호에 의존한다. ACL·서명된 검증 영수증이나 태그 서명은 별도 설계 과제.
4. **통신 및 OTA 보안**: TCP는 TLS가 아니며, OTA 이미지 역할 서명·자동 롤백은 이번에 실물 승인되지 않았다. 인터넷 직접 노출 금지.
5. **장치/부트 시의 메모리·저장 상태**: 32개 RAM 기록·NVS·LittleFS·OTA 동작의 장기간 오류 주입은 아직 실기 데이터가 없음.

## 4. 현장 인수 순서 및 중단 조건

1. 이전 운영 이미지 및 파티션·키 복구본을 확보하고 SHA를 대조한다. 새로운 CI BIN만 대상 보드에 기록한다.
2. 앵커/태그 DEV_ID 0xDECA0130, PLL, RXENAB, ANT 지연 및 RF 스위치 비사용 핀맵을 확인한다.
3. Pi·앵커의 Wi-Fi/TCP/HMAC 인증 시험. START 직전 Wi-Fi가 반드시 OFF인지 확인한다.
4. 정지 20~32거래 P1/P2 CIR·DS-TWR·공통 그리드를 양단 실제 시각으로 검산한다.
5. GET 중 단절·재시도, tag RFB8 손상, ACK 지연, STOP→START/SET 전이, OTA 중 파일 보존을 주입한다.
6. 운영형 32거래 × 반복 실행, 전원·플래시 오류 주입 및 RF/캡처 손실률을 확인한다.

**즉시 중단:** DEV_ID/PLL 무결성 실패, 원시 프레임 불일치, 승인 없이 PURGE 성공, 원본 보관 덮어쓰기, 설정·세션 혼합, 누락된 P2를 잘못 유효 처리, Wi-Fi ON 상태에서 RF 시작, 잘못된 OTA 이미지가 적용되는 경우.

## 5. 최종 인수 범위

**SOURCE: PASS / HOST QA: PASS / TARGET ESP32 BUILD: PASS / INDEPENDENT THIRD-PARTY AUDIT: NOT PERFORMED / HARDWARE & OPERATIONAL FIELD: HOLD.**

검증 전 Draft PR #1을 병합하거나 기존 운영 장치를 복구본 없이 덮어쓰지 않는다. 최종 운영 승인은 실물 증거와 실제 설치 바이너리 해시가 확보된 후 별도 판정한다.
