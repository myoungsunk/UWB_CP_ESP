# MODE8-DEV-REGISTER-001 — 미반영 기능 전체 관리표

**기준**: \`ce8f986\` Stage6 P2-repeat, \`85c42fe8\` 운영 통합 및 후속 Mode8 전용 개발.
**범위 확정**: RF는 Mode 8 전용. 과거 Mode 3–7 및 R03 이전 패킷/479B 저장 형식 호환은 요구하지 않는다. 안테나 지연의 절대 보정/거리 정확도 검증은 후속 연구로 분리하되 실제 TX/RX 보정값과 raw CIR/FP, cal_id는 지금부터 누락 없이 보존한다.

**판정 분류**: IMPLEMENTED(실제 소스와 자동 시험 존재), PARTIAL(작동 범위 제한), HOLD(하드웨어 또는 추가 구조가 필요), NOT_STARTED. GitHub Actions의 성공은 RF·전원·OTA 실물 시험을 대신하지 않는다.

| ID | 미반영/확장 요구 | 현재 개발 판정 | 사용자가 수용할 기능 계약 / 차단 요소 |
|---|---|---|---|
| M01 | 재부팅 뒤 Mode8 Stage6 200ms·2Mbps 운영 설정 복원 | IMPLEMENTED / FIELD HOLD | 역할·CRC·버전 검사 후 선택적 복원; 부패하면 Stage5/921600 진단. 자동 RF 시작 금지. 초기 \`PROFILE SAVE\` 필요 |
| M02 | 32거래 제한을 넘는 장시간 **운영 세션** | PARTIAL | Pi가 ≤32거래 point로 나누어 저장·회수·재시작. 각 point 사이 Wi-Fi/저장 공백 있음 |
| M03 | 다중 route/point 관리, crash resume 및 유일 식별 | IMPLEMENTED / FIELD HOLD | \`ops_route.py\` 의 영속 journal·고유 세션·완료 검증·재개. ROS 로봇 주행은 구현하지 않음 |
| M04 | 무중단 1000거래(5 Hz) Wi-Fi OFF RF와 동시에 앵커 원본 보존 | HOLD — 기존 ESP32 내부 RAM 제한 | RF 미간섭 외부/분리 저장 또는 별도 데이터 회수 하드웨어 필요. USB 실험용 1000거래와 운영형은 별개 |
| M05 | 측정 중 전원 차단 시 미커밋 RAM 원본 무손실 | HOLD — 물리적으로 RAM 보존 불가 | 전원 차단 뒤 진짜 raw 원본을 복원하려면 UPS/비휘발성 로깅 필요. 현재는 fail-closed와 ≤32거래 손실 범위 분명히 표시 |
| M06 | 커밋 저널의 중단/CRC 복구, 자동 덮어쓰기 방지 | IMPLEMENTED / FIELD HOLD | tmp 검증·원자 rename, 손상 tmp 보존/격리; 실제 플래시 정전 주입 미실시 |
| M07 | 파일 ACK/삭제 권한의 구분, 승인 없는 삭제 금지 | IMPLEMENTED / FIELD HOLD | 별도 USB DELETEKEY, 세션·boot·CRC 및 연결 nonce와 ACK/PURGE 용도별 HMAC. 실물 보안 시험 필요 |
| M08 | OTA 이미지의 앵커/태그 역할·해시 확인 | PARTIAL | 기존 ArduinoOTA 정비 창·파티션 검사만 존재. 실제 image role 검증·서명 확인 미인수 |
| M09 | 잘못된 OTA의 안전 자동 롤백/부팅 확인 | HOLD | 부트로더 rollback/OTA 파티션 설정, 이미지 검증 서명/실물 전원 차단 증거 필요 |
| M10 | ESP32 태그 OTA·원격 복구 | NOT_STARTED / OPTIONAL | 현재 태그는 Pi USB로 업로드. Wi-Fi 추가 시 RF/전원/메모리 동시성 새 감사 필요 |
| M11 | WPA Enterprise(EAP) 사내/학교 AP 지원 | NOT_STARTED / OPTIONAL | 현재 개인망 WPA. AP 인증서/계정·실물 환경 확보 전 WPA 개인망 격리 운용 |
| M12 | TCP TLS, NVS 자격증명 암호화/secure boot | HOLD / DEPLOYMENT OPTION | HMAC 인증은 기밀성 없음. 보드별 secure boot·flash encryption 설정 및 키 관리가 필요. 외부 공개 인터넷 노출 금지 |
| M13 | RF 시간 주기 추가 단축(195/190/150/100ms) | NOT APPROVED | 현재 200ms 안정 기준선 유지. 190ms는 실기 검증 후에만. 150/100ms는 RF+UART 재설계 필요 |
| M14 | Wi-Fi OFF/ON, PLL, RXENAB, 바운더리, 32거래 RAM 실기 검증 | FIELD HOLD | 반복 전원/수신, heap 최저치, brownout/WDT, 임시 파일 손실, 200ms 지터, raw CIR |
| M15 | 이동 경로에서 yaw/odom/IMU 시간 정렬, P2 repeat 의미 보존 | FIELD HOLD | 반복 P2의 실제 물리 시각 사용; teach-repeat/ROS 연동 별도 |
| M16 | 호스트 USB/ACK/FEC 스트레스, 조각 오류·큐 역압력·중복 세션 | PARTIAL | 호스트 결함 주입 테스트 존재; 실제 USB·Pi·TCP 24h 장기 시험 미완료 |
| M17 | OTA/저장 플래시 wear, 부족 공간, 파일 조각화/실패 복구 | PARTIAL | LittleFS autoformat 금지, 파일 CRC 존재. 1000회 이상 반복/수명·전원 차단 현장 시험 필요 |
| M18 | 비밀정보 배포와 소스/이미지 SHA·검증 영수증·PR 인수 체계 | PARTIAL | CI 아티팩트, 소스/앱 분리. 개인 인증키와 전체 플래시 원본 공개 업로드 금지. 실기 플래시 readback SHA 별도 |
| M19 | 안테나 A/B TX/RX 지연값과 보정 출처의 데이터 보존 | IMPLEMENTED(원본/메타 유지) | 실험 임시값은 calibrated=false를 유지. 정확도 시험·보정값 확정은 범위 제외 |
| EX-01 | 기존 Mode3–7/Legacy R03 프로토콜 | OUT OF SCOPE | 구현·호환 강요 금지 |
| EX-02 | DS-TWR 절대 정확도 승인 | DEFERRED | LoS 다중 거리·편파·안테나 지연 보정 이후 수행 |

## 구현 우선 순서 및 검증 문턱

- **G0**: 기준 SHA, 패킷/기록 스키마 고정 및 숨은 코드 복사본 검사
- **G1**: M01, M07 호스트/펌웨어 변경과 음성 입력·전원 취소 Mock/적대적 시험
- **G2**: M02/M03 point journal·다운로드·ACK·삭제 순서 검증(수집 공백이 있음을 명시)
- **G3**: M06, M08/M09 OTA/파일 파티션 오류 경로를 코드에 추가하고 안전한 차단 조건을 확인
- **G4**: ESP32 Arduino Core 3.3.7 앵커/태그 별도 컴파일/링크·BIN/ELF·메모리 확인
- **G5**: 실제 플래시/부팅/PLL/Wi-Fi RF 32거래, Pi/RFB8·저장 양단 감사
- **G6**: 정전·OTA·통신 장애 주입. 미검증 오류를 pass 처리 금지.
- **G7**: 연속 1000거래/무중단 문제는 M04/M05 하드웨어 계약 충족 후 새 승인.

**운영 금지**: G4 통과만으로 통합형 연구실 운영 펌웨어 승인을 표시하지 않는다. MID-MEASUREMENT 파워컷의 데이터 유실을 숨기는 "복구 완료" 표현을 금지한다.
