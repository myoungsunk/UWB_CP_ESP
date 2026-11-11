// Run the delivered state machine against the delivered simulated radio.
#define main delivered_host_test_main
#include "test_core.cpp"
#undef main

struct CirGuardFailure : SimTag {
    bool latchedA = false;
    unsigned peerReads = 0;
    unsigned peerArms = 0;

    bool arm(uint8_t radio) override {
        if (latchedA && radio == 0) return false;
        if (latchedA && radio == 1) ++peerArms;
        return SimTag::arm(radio);
    }

    int receive(uint8_t radio, Frame& frame, Rx& rx) override {
        if (latchedA && radio == 0) return -2;
        return SimTag::receive(radio, frame, rx);
    }

    bool readCir(uint8_t radio, uint8_t* data, Cir& cir) override {
        if (latchedA && radio == 1) ++peerReads;
        const bool valid = SimTag::readCir(radio, data, cir);
        if (radio == 0 && last[0] == 1) {
            latchedA = true;
            cir.valid = false;
            return false;
        }
        return valid;
    }
};

int main(int argc, char** argv) {
    CHECK(argc == 2);
    SimTag stage2;
    stage2.cfg.stage = 2;
    stage2.drop[1][0] = true;
    const auto missingPoll = stage2.execute();
    CHECK(!missingPoll.poll[1].received && (missingPoll.errors & ErrPoll));
    CHECK(!missingPoll.complete);
    std::cout << "M8-04: B_poll_received=0 ErrPoll=1 complete=0\n";

    CirGuardFailure faulty;
    const auto faultRecord = faulty.execute();
    CHECK(faulty.latchedA && (faultRecord.errors & ErrCir));
    CHECK(!faultRecord.range[0].valid && !faultRecord.range[1].valid);
    CHECK(faulty.peerReads == 1 && faulty.peerArms == 1);
    std::cout << "M8-03: A_latched=1 B_CIR_reads_after_fault=" << faulty.peerReads
              << " B_P2_arms_after_fault=" << faulty.peerArms << "\n";

    SimAnchor anchor;
    anchor.cfg.stage = 2;
    const auto record = anchor.execute();
    CHECK(record.complete && record.errors == 0 && record.peer_boot == 0);
    CHECK(record.probe_frame[0].peer_boot == 0 && record.probe_frame[1].peer_boot == 0);
    char json[JSON_MAX + 1];
    size_t size = 0;
    CHECK(serializeRecord(record, json, sizeof json, size));
    std::ofstream out(argv[1]);
    CHECK(bool(out.write(json, size)));
    std::cout << "Stage 2 anchor: complete=1 peer_boot=0 P1_peer_boot=0 P2_peer_boot=0\n";
}
