// Exercise the delivered firmware model and record serializer for each POLL loss.
#define main delivered_test_main
#include "test_core.cpp"
#undef main

int main(int argc, char** argv) {
    CHECK(argc == 2);
    for (unsigned lost = 0; lost < 2; ++lost) {
        SimTag sim;
        sim.drop[lost][0] = true;
        const auto record = sim.execute();
        CHECK(!record.complete && record.p1_tx_confirmed);
        CHECK(!record.poll[lost].received && record.poll[lost ^ 1U].received);
        CHECK(!record.range[lost].valid && record.range[lost ^ 1U].valid);
        char json[JSON_MAX + 1]{};
        size_t size = 0;
        CHECK(serializeRecord(record, json, sizeof json, size));
        const auto prefix = std::string(argv[1]) + "/partial_poll_lost_" + std::to_string(lost);
        std::ofstream(prefix + ".json").write(json, size);
        std::ofstream(prefix + ".raw", std::ios::binary)
            .write(reinterpret_cast<const char*>(record.cir), sizeof record.cir);
    }
}
