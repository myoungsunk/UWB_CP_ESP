// Compile the production anchor DW1000Ng driver against a simulated register bus.
#include "DW1000Ng.hpp"
#include "SPIporting.hpp"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <vector>

namespace {
struct Write {
    uint8_t reg;
    uint16_t offset;
    std::vector<byte> data;
};
std::vector<Write> writes;
byte registers[64][128]{};
SPIClock speed = SPIClock::FAST;
bool shifted, correction, missing_device;
uint16_t otp_address;
uint8_t trim;
enum class Drop { None, DisableSequencing, Trim, LdeClock, Detector, RestoreSequencing, Auto };
Drop drop;

void prepare(bool slip, Drop lost = Drop::None, bool missing = false) {
    std::memset(registers, 0, sizeof registers);
    registers[0x36][1] = 0x02;
    registers[0x36][4] = 0xe7;
    registers[0x36][5] = 0x02;
    const byte id[] = {0x30, 0x01, 0xca, 0xde};
    std::copy(id, id + sizeof id, registers[0]);
    speed = SPIClock::FAST; // A prior mode may have left the bus at FAST.
    shifted = slip;
    correction = false;
    missing_device = missing;
    otp_address = 0;
    trim = slip ? 0x13 : 0x80;
    drop = lost;
    writes.clear();
}

size_t find_write(size_t start, uint8_t reg, uint16_t off, std::initializer_list<byte> bytes) {
    while (start < writes.size()) {
        const auto& w = writes[start];
        if (w.reg == reg && w.offset == off && w.data == std::vector<byte>(bytes)) return start;
        ++start;
    }
    assert(false && "missing write or incorrect initialization order");
    return start;
}

void check_sequence(bool slip) {
    prepare(slip);
    assert(DW1000Ng::initializeNoInterruptChecked(21, 5));
    assert(correction == slip && speed == SPIClock::SLOW);
    const auto xti = find_write(0, 0x36, 0, {1});
    assert(find_write(xti + 1, 0x36, 1, {2}) == xti + 1);
    const auto disable = find_write(xti + 2, 0x36, 4, {0, 0});
    const auto ldo = find_write(disable + 1, 0x2d, 0x12, {2});
    const auto trimmed = find_write(ldo + 1, 0x2b, 0x0e, {byte(slip ? 0x73 : 0x70)});
    const auto pll = find_write(trimmed + 1, 0x2b, 0, {0x48, 0x04, 0x95, 0xe7});
    const auto lde = find_write(pll + 1, 0x36, 1, {3});
    const auto load = find_write(lde + 1, 0x2d, 0x06, {0, 0x80});
    const auto restore_xti = find_write(load + 1, 0x36, 0, {1});
    assert(find_write(restore_xti + 1, 0x36, 1, {2}) == restore_xti + 1);
    const auto detector = find_write(restore_xti + 2, 0x24, 0, {4});
    const auto restore = find_write(detector + 1, 0x36, 4, {0xe7, 2});
    const auto automatic = find_write(restore + 1, 0x36, 0, {0});
    assert(find_write(automatic + 1, 0x36, 1, {2}) == automatic + 1);

    // Exercise the pinned driver's real profile writes. The app-level test
    // compares the same values using its own independent readback fixture.
    device_configuration_t profile = {false,false,false,true,false,SFDMode::STANDARD_SFD,
        Channel::CHANNEL_5,DataRate::RATE_850KBPS,PulseFrequency::FREQ_16MHZ,
        PreambleLength::LEN_256,PreambleCode::CODE_3};
    DW1000Ng::applyConfiguration(profile);
    DW1000Ng::disableFrameFiltering();
    DW1000Ng::setDoubleBuffering(false);
    DW1000Ng::setReceiveFrameWaitTimeoutPeriod(0);
    assert(registers[0x04][1] == 0x10 && registers[0x04][2] == 0x04);
    assert(registers[0x1f][0] == 0x55 && registers[0x1f][2] == 0xc4 && registers[0x1f][3] == 0x18);
    assert(registers[0x08][1] == 0x20 && registers[0x08][2] == 0x25);
    assert(registers[0x2b][0x07] == 0x1d && registers[0x2b][0x0a] == 0x08);
    assert(registers[0x2b][0x0b] == 0xbe);
}
}

SPIClass SPI;
void pinMode(uint8_t, int) {}
void digitalWrite(uint8_t, int) {}
void delay(unsigned long) {}
void delayMicroseconds(unsigned int) {}
unsigned long micros() { return 0; }
void attachInterruptArg(uint8_t, void(*)(void*), void*, int) {}

namespace SPIporting {
void SPIinit(SPIClass&) { assert(speed == SPIClock::SLOW); }
void SPIselect(uint8_t cs, uint8_t irq) { assert(cs == 21 && irq == 0xff); }
void setSPIspeed(SPIClock value) { speed = value; }
void setSlowReadShiftFix(bool enabled) { correction = enabled; }

void readFromSPI(uint8_t, uint8_t n, byte header[], uint16_t length, byte data[]) {
    assert(speed == SPIClock::SLOW);
    const byte reg = header[0] & 63;
    const auto off = n == 1 ? 0 : (header[1] & 127) + (n == 3 ? header[2] * 128 : 0);
    if (reg == 0 && (missing_device || correction != shifted)) {
        std::memset(data, 0xff, length);
    } else if (reg == 0x2d && off == 0x0a) {
        std::memset(data, 0, length);
        if (otp_address == 4) data[0] = 0x34;
        if (otp_address == 0x1e) data[0] = trim;
    } else if (off >= 128) {
        std::memset(data, 0, length);
    } else {
        assert(off + length <= 128);
        std::memcpy(data, &registers[reg][off], length);
    }
}

void writeToSPI(uint8_t, uint8_t n, byte header[], uint16_t length, byte data[]) {
    assert(speed == SPIClock::SLOW);
    const byte reg = header[0] & 63;
    const auto off = n == 1 ? 0 : (header[1] & 127) + (n == 3 ? header[2] * 128 : 0);
    writes.push_back({reg, uint16_t(off), {data, data + length}});
    if (reg == 0x36 && off <= 1) assert(length == 1);
    if (reg == 0x2b && off == 0) {
        assert((registers[0x36][0] & 3) == 1);
        assert(registers[0x36][4] == 0 && registers[0x36][5] == 0);
    }
    if (reg == 0x2d && off == 4) otp_address = data[0] | (uint16_t(data[1]) << 8);
    if ((drop == Drop::DisableSequencing && reg == 0x36 && off == 4 && data[0] == 0) ||
        (drop == Drop::RestoreSequencing && reg == 0x36 && off == 4 && data[0] != 0) ||
        (drop == Drop::Trim && reg == 0x2b && off == 0x0e) ||
        (drop == Drop::LdeClock && reg == 0x36 && off == 1 && data[0] == 3) ||
        (drop == Drop::Auto && reg == 0x36 && off == 0 && data[0] == 0) ||
        (drop == Drop::Detector && reg == 0x24 && off == 0)) return;
    if (off < 128) {
        assert(off + length <= 128);
        std::memcpy(&registers[reg][off], data, length);
    }
}
}

int main() {
    check_sequence(false);
    check_sequence(true);
    for (Drop fault : {Drop::DisableSequencing, Drop::Trim, Drop::LdeClock, Drop::Detector,
                       Drop::RestoreSequencing, Drop::Auto}) {
        prepare(false, fault);
        assert(!DW1000Ng::initializeNoInterruptChecked(21, 5) && speed == SPIClock::SLOW);
    }
    prepare(false, Drop::None, true);
    assert(!DW1000Ng::initializeNoInterruptChecked(21, 5) && writes.empty());
    std::puts("PASS PLL driver: INIT/OTP/trim/calibration/LDE/AUTO order, split writes, both SPI read modes and lost writes");
}
