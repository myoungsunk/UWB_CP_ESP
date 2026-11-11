// Reuse the delivered simulated radio without changing the delivered source.
#define main delivered_host_test_main
#include "test_core.cpp"
#undef main

struct CirGuardFailure : SimTag {
    bool latchedA = false;
    unsigned peerCirReadsAfterFault = 0;
    unsigned peerArmsAfterFault = 0;

    bool arm(uint8_t radio) override {
        if (latchedA && radio == 0) return false;
        if (latchedA && radio == 1) ++peerArmsAfterFault;
        return SimTag::arm(radio);
    }

    int receive(uint8_t radio, Frame& frame, Rx& rx) override {
        if (latchedA && radio == 0) return -2;
        return SimTag::receive(radio, frame, rx);
    }

    bool readCir(uint8_t radio, uint8_t* data, Cir& cir) override {
        if (latchedA && radio == 1) ++peerCirReadsAfterFault;
        const bool valid = SimTag::readCir(radio, data, cir);
        if (radio == 0 && last[0] == 1) {
            latchedA = true;
            cir.valid = false;
            return false;
        }
        return valid;
    }
};

int main() {
    SimTag stage2;
    stage2.cfg.stage = 2;
    stage2.drop[1][0] = true;
    const Record missingPoll = stage2.execute();
    CHECK(!missingPoll.poll[1].received && (missingPoll.errors & ErrPoll));
    CHECK(missingPoll.complete);
    std::cout << "stage2: B_poll_received=0 ErrPoll=1 complete=1\n";

    CirGuardFailure faulty;
    const Record faultRecord = faulty.execute();
    CHECK(faulty.latchedA && (faultRecord.errors & ErrCir));
    CHECK(faulty.peerCirReadsAfterFault == 1 && faulty.peerArmsAfterFault == 1);
    std::cout << "P1 CIR integrity fault: A_latched=1 B_CIR_reads_after_fault="
              << faulty.peerCirReadsAfterFault << " B_P2_arms_after_fault="
              << faulty.peerArmsAfterFault << '\n';
}
