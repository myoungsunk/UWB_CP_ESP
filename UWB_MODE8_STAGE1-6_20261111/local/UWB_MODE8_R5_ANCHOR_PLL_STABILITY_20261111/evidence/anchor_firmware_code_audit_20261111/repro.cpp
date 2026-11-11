// Reproduce anchor application decisions with simulated register reads and USB.
// The production app, adapter, engine and serializer are compiled unmodified.
#define DUAL8_ANCHOR 1
#include "app.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>

using namespace dual8;

namespace fake {
uint64_t now_us = 0;
uint64_t sys_status = 0x00800002;
uint64_t scheduled = 0;
uint64_t actual = 0;
uint8_t rf_status = 1;
uint8_t ec_ctrl = 4;
uint16_t tx_antd = 16436;
uint16_t rx_antd = 16436;
unsigned tx_starts = 0;
unsigned channel_reads = 0;
unsigned requested_channel = 0;
unsigned channel_readback = 2;
unsigned fast_off_with_pll_loss = 0;
uint8_t inject_rf = 1;
bool inject_event = false;
SPIClock speed = SPIClock::FAST;
std::deque<unsigned char> input;
struct Queue {
    explicit Queue(unsigned count, unsigned size) : capacity(count), element_size(size) {}
    unsigned capacity;
    unsigned element_size;
    std::deque<std::vector<uint8_t>> entries;
};
std::vector<Queue*> queues;
uint64_t system_time() {
    return (usTicks(++now_us) + 0x40000000) & MASK40;
}
void send_usb(const char* bytes, size_t count) {
    input.insert(input.end(), bytes, bytes + count);
    dual8app::service();
}
void expect_event(const char* text) {
    auto& queue = *static_cast<Queue*>(dual8app::outQ);
    assert(!queue.entries.empty());
    const auto& bytes = queue.entries.back();
    dual8app::Out event;
    std::memcpy(&event, bytes.data(), sizeof event);
    assert(std::strstr(event.text, text));
}
}

HardwareSerial Serial;
SPIClass SPI;
void pinMode(uint8_t, int) {}
void digitalWrite(uint8_t, int) {}
void delay(unsigned long ms) { fake::now_us += ms * 1000; }
void delayMicroseconds(unsigned int us) { fake::now_us += us; }
int64_t esp_timer_get_time() { return int64_t(++fake::now_us); }
uint32_t esp_random() { return 4; }
int HardwareSerial::available() { return int(fake::input.size()); }
int HardwareSerial::read() {
    auto value = fake::input.front();
    fake::input.pop_front();
    return value;
}
size_t HardwareSerial::write(const uint8_t*, size_t size) { return size; }
void HardwareSerial::flush() {}
void HardwareSerial::end() {}
void HardwareSerial::begin(unsigned long) {}
bool HardwareSerial::setRxBufferSize(size_t) { return true; }
bool HardwareSerial::setTxBufferSize(size_t) { return true; }
void HardwareSerial::println(const char*) {}
void SPIClass::begin(int8_t, int8_t, int8_t, int8_t) {}
void SPIClass::beginTransaction(const SPISettings&) {}
void SPIClass::endTransaction() {}
uint8_t SPIClass::transfer(uint8_t) { return 0; }
void SPIClass::transferBytes(const uint8_t*, uint8_t* out, uint32_t len) { std::memset(out, 0, len); }
void vTaskDelay(TickType_t ticks) { delay(ticks); }
int xTaskCreatePinnedToCore(void(*)(void*),const char*,uint32_t,void*,unsigned,TaskHandle_t*,int) {
    return pdPASS; // Host assertion code reads queue entries directly.
}
QueueHandle_t xQueueCreate(unsigned capacity, unsigned bytes) {
    auto* q = new fake::Queue(capacity, bytes);
    fake::queues.push_back(q);
    return q;
}
int xQueueSend(QueueHandle_t q, const void* ptr, TickType_t) {
    auto& queue = *static_cast<fake::Queue*>(q);
    if (queue.entries.size() == queue.capacity) return 0;
    const auto* bytes = static_cast<const uint8_t*>(ptr);
    queue.entries.emplace_back(bytes, bytes + queue.element_size);
    return pdTRUE;
}
int xQueueReceive(QueueHandle_t q, void* ptr, TickType_t) {
    auto& queue = *static_cast<fake::Queue*>(q);
    if (queue.entries.empty()) return 0;
    std::memcpy(ptr, queue.entries.front().data(), queue.element_size);
    queue.entries.pop_front();
    return pdTRUE;
}

namespace SPIporting {
void setSPIspeed(SPIClock speed) { fake::speed = speed; }
void setSlowReadShiftFix(bool) {}
void readFromSPI(uint8_t, uint8_t, byte header[], uint16_t len, byte data[]) {
    uint64_t value = 0;
    switch (header[0] & 63) {
        case 0: value = 0xdeca0130; break;
        case 0x06: value = fake::system_time(); break;
        case 0x0f: value = fake::sys_status; break;
        case 0x18: value = fake::tx_antd; break;
        case 0x1f:
            ++fake::channel_reads;
            value = fake::channel_readback | (fake::channel_readback << 4);
            break;
        case 0x24: value = fake::ec_ctrl; break;
        case 0x28: value = fake::rf_status; break;
        case 0x2e: value = fake::rx_antd; break;
    }
    assert(len <= 8);
    putLE(data, value, len);
}
void writeToSPI(uint8_t, uint8_t, byte header[], uint16_t len, byte data[]) {
    if ((header[0] & 63) == 0x0f) fake::sys_status &= ~getLE(data, len);
}
}

namespace DW1000Ng {
void initializeNoInterrupt(uint8_t, uint8_t) {}
void applyConfiguration(device_configuration_t config) {
    fake::requested_channel = unsigned(config.channel); // Inject a lost CHAN_CTRL write.
}
void disableFrameFiltering() {}
void setNetworkId(uint16_t) {}
void setDeviceAddress(uint16_t) {}
void setTxAntennaDelay(uint16_t v) { fake::tx_antd = v; }
void setRxAntennaDelay(uint16_t v) { fake::rx_antd = v; }
void setWait4Response(uint32_t) {}
void setDoubleBuffering(boolean) {}
void setReceiveFrameWaitTimeoutPeriod(uint16_t) {}
uint64_t getSystemTimestamp() { return fake::system_time(); }
void forceTRxOff() {
    if ((fake::sys_status & rxguard::CLOCK_PLL_LOSS) && fake::speed == SPIClock::FAST)
        ++fake::fast_off_with_pll_loss;
}
void startReceive(ReceiveMode) {}
uint16_t getReceivedDataLength() { return 0; }
uint64_t getReceiveTimestamp() { return 0; }
uint64_t getReceiveTimestampRaw() { return 0; }
void getReceivedData(byte*, uint16_t) {}
void setDelayedTRX(byte bytes[]) { fake::scheduled = getLE(bytes, 5); }
void setTransmitData(byte data[], uint16_t len) {
    Frame f;
    assert(decode(data, len, f));
    if (f.type == Type::Poll) {
        fake::rf_status = fake::inject_rf;
        if (fake::inject_event) fake::sys_status |= rxguard::CLOCK_PLL_LOSS;
    }
}
void startTransmit(TransmitMode mode) {
    assert(mode == TransmitMode::DELAYED);
    ++fake::tx_starts;
    fake::actual = (fake::scheduled + fake::tx_antd) & MASK40;
    fake::sys_status |= 1ULL << 7;
}
uint64_t getTransmitTimestamp() { return fake::actual; }
}

int main() {
    auto& anchor = dual8app::port;
    dual8app::config.stage = 2;
    dual8app::setup();
    assert(dual8app::hwOk && anchor.selftest() && fake::requested_channel == 5);
    assert(fake::channel_readback == 2 && fake::channel_reads == 0);
    std::puts("PHY: init and SELFTEST passed with CHAN_CTRL ch2, requested ch5, CHAN_CTRL reads=0");
    fake::channel_readback = 5;

    assert(anchor.verifyIdleClock());
    fake::inject_rf = 3;
    Record low_without_event;
    runAnchor(anchor, dual8app::config, 7, 0, 4, 0, low_without_event);
    assert(low_without_event.complete && !anchor.faulted() && fake::tx_starts == 3);
    std::printf("TX_GATE: RF_STATUS=0x%02x SYS_STATUS=0x%08llx after buffer; "
                "TXSTRT=%u and complete=true\n", fake::rf_status,
                static_cast<unsigned long long>(anchor.pll_startup.first_poll_after_buffer),
                fake::tx_starts);

    fake::rf_status = 1;
    assert(anchor.verifyIdleClock());
    fake::inject_event = true;
    fake::tx_starts = 0;
    Record loss_event;
    runAnchor(anchor, dual8app::config, 8, 0, 4, 0, loss_event);
    assert(anchor.faulted() && fake::tx_starts == 0 && loss_event.errors == ErrPoll);
    assert(loss_event.tx[0].status == 0x02800002 && fake::fast_off_with_pll_loss == 1);
    std::printf("TX_FAULT: SYS_STATUS=0x%08llx TXSTRT=%u errors=%u "
                "expected hardware bit=%u; forced-off at SPI FAST=%u\n",
                static_cast<unsigned long long>(loss_event.tx[0].status),
                fake::tx_starts, loss_event.errors, unsigned(ErrHardware),
                fake::fast_off_with_pll_loss);

    const char pll[] = "PLLSTATUS\n";
    fake::send_usb(pll, sizeof pll - 1);
    fake::expect_event("\"ready\":true");
    fake::expect_event("\"live_rf_slow\":3");
    std::puts("PLLSTATUS: ready=true is emitted with fault latched and live RF_STATUS=0x03");

    fake::inject_event = false;
    fake::inject_rf = 1;
    fake::sys_status = 0x00800002;
    fake::rf_status = 1;
    const char reset[] = "RESET\n";
    fake::send_usb(reset, sizeof reset - 1);
    assert(dual8app::hwOk && !anchor.faulted());

    auto& events = static_cast<fake::Queue*>(dual8app::outQ)->entries;
    const auto prior = events.size();
    const char invalid_stage[] = "SET STAGE 7\n";
    fake::send_usb(invalid_stage, sizeof invalid_stage - 1);
    assert(events.size() == prior && dual8app::config.stage == 2);
    std::puts("USB: invalid SET STAGE 7 produces no response");

    const char truncated_start[] = "START 12 1\0ignored\n";
    fake::send_usb(truncated_start, sizeof truncated_start - 1);
    assert(dual8app::active && dual8app::session == 12);
    std::puts("USB: START 12 1 + NUL + ignored activates the session");

    for (auto* queue : fake::queues) delete queue;
}
