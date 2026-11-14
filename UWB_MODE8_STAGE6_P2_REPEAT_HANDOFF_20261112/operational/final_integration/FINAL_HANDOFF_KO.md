# Mode 8 최종 통합 감사 인계

이 패키지의 펌웨어·운영 코드 빌드 입력 기준은 `17e496ccb584b4f73cfe75198db7cc08ca33f25b`이다. 후속 커밋은 인수 문서와 증거, 과거 원본 재생 스크립트를 추가한다. 빌드 바이트 대조는 [build-binding.json](build-binding.json)을 참조한다.

**판정** `SOURCE_PASS / HOST_QA_PASS / TARGET_BUILD_PASS / HARDWARE_FIELD_HOLD`

- [패키지 인벤토리](PACKAGE_INVENTORY.csv) · [출처](SOURCE_PROVENANCE.json) · [소스 차이](INTEGRATION_DIFF_REPORT_KO.md)
- [기능별 검증](FEATURE_IMPLEMENTATION_MATRIX.csv) · [결함 추적](DEFECT_FIX_TRACEABILITY.csv) · [시험 실행표](TEST_EXECUTION_MATRIX.csv)
- [원시 RF 재검산](RF_TRANSACTION_ANALYSIS_KO.md) · [적대적 감사](ADVERSARIAL_AUDIT_FINAL_KO.md)
- [순차 인수 사양](SEQUENTIAL_ACCEPTANCE_SPEC_KO.md) · [미완료 조건](REMAINING_BLOCKERS_KO.md)

GitHub Draft PR #1에는 코드와 검증 자료가 들어 있다. 최종 SHA의 Actions에서 현재 배포 산출물을 확인한다. 설치 전 플래시 백업·역할·원래 이미지·시리얼 포트·Core·감사 SHA를 확인한다. 시험에서 사용하는 저장 데이터는 별도 폴더에 남기고 기존 승인 원본은 보존한다.
