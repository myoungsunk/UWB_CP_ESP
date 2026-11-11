// Exercise anchor firmware with injected register, TX and USB failures.
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
unsigned channel_readback = 5;
uint32_t sys_cfg = 0x00041000, tx_fctrl = 0x00252000, pll_cfg = 0x0800041d;
uint8_t pll_tune = 0xbe;
uint64_t fail_at_us = 0;
unsigned init_calls = 0, fast_selections = 0, pll_clears = 0, off_calls = 0;
bool driver_init_ok = true;
Type inject_frame = Type::Poll;
Type staged_frame = Type::Poll;
bool fault_in_timestamp = false;
bool tx_mismatch = false;
uint32_t dev_id = 0xdeca0130;
unsigned fast_off_with_pll_loss = 0;
uint8_t inject_rf = 1;
bool inject_event = false;
unsigned bad_rx_lengths = 0, bad_rx_reads = 0, rx_reads = 0, pll_on_rx_read = 0;
uint8_t rx_air[AIR_BYTES]{};
SPIClock speed = SPIClock::FAST;
std::deque<unsigned char> input;
std::vector<uint8_t> uart_bytes;
size_t largest_uart_write = 0;
unsigned uart_flushes = 0, serial_available_calls = 0;
bool short_uart_write = false;
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
void delay(unsigned long ms) {
    fake::now_us += ms * 1000;
    if (fake::fail_at_us && fake::now_us >= fake::fail_at_us) fake::rf_status = 3;
}
void delayMicroseconds(unsigned int us) { fake::now_us += us; }
int64_t esp_timer_get_time() { return int64_t(++fake::now_us); }
uint32_t esp_random() { return 4; }
int HardwareSerial::available() { ++fake::serial_available_calls; return int(fake::input.size()); }
int uart_get_buffered_data_len(uart_port_t, size_t* size) {
    *size = fake::input.size();
    return ESP_OK;
}
int HardwareSerial::read() {
    auto value = fake::input.front();
    fake::input.pop_front();
    return value;
}
size_t HardwareSerial::write(const uint8_t* bytes, size_t size) {
    if (size > fake::largest_uart_write) fake::largest_uart_write = size;
    const size_t written = fake::short_uart_write ? size / 2 : size;
    fake::short_uart_write = false;
    fake::uart_bytes.insert(fake::uart_bytes.end(), bytes, bytes + written);
    return written;
}
void HardwareSerial::flush() { ++fake::uart_flushes; }
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
void setSPIspeed(SPIClock speed) {
    fake::speed = speed;
    if (speed == SPIClock::FAST) ++fake::fast_selections;
}
void setSlowReadShiftFix(bool) {}
void readFromSPI(uint8_t, uint8_t header_len, byte header[], uint16_t len, byte data[]) {
    uint64_t value = 0;
    switch (header[0] & 63) {
        case 0: value = fake::dev_id; break;
        case 4: value = fake::sys_cfg; break;
        case 8: value = fake::tx_fctrl; break;
        case 0x2b:
            assert(header_len == 2 && (header[1] == 7 || header[1] == 0x0b));
            value = header[1] == 7 ? fake::pll_cfg : fake::pll_tune;
            break;
        case 0x06: value = fake::system_time(); break;
        case 0x0f: value = fake::sys_status; break;
        case 0x15:
            assert(len == 14);
            std::memset(data, 0, len);
            return;
        case 0x18: value = fake::tx_antd; break;
        case 0x1f:
            ++fake::channel_reads;
            value = 0x18c40000 | fake::channel_readback | (fake::channel_readback << 4);
            break;
        case 0x24: value = fake::ec_ctrl; break;
        case 0x28: value = fake::rf_status; break;
        case 0x2e: value = fake::rx_antd; break;
    }
    assert(len <= 8);
    putLE(data, value, len);
}
void writeToSPI(uint8_t, uint8_t, byte header[], uint16_t len, byte data[]) {
    if ((header[0] & 63) == 0x0f) {
        const auto mask = getLE(data, len);
        if (mask & rxguard::CLOCK_PLL_LOSS) ++fake::pll_clears;
        fake::sys_status &= ~mask;
    }
}
}

namespace DW1000Ng {
bool initializeNoInterruptChecked(uint8_t cs, uint8_t rst, InitFailure& failure) {
    assert(cs == 21 && rst == 5 && fake::speed == SPIClock::SLOW);
    ++fake::init_calls;
    return fake::driver_init_ok ||
        failure.check("PMSC_RETURN_AUTO",0x36,0,3,0,1);
}
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
    ++fake::off_calls;
    if (fake::speed == SPIClock::FAST)
        ++fake::fast_off_with_pll_loss;
}
void startReceive(ReceiveMode) {}
uint16_t getReceivedDataLength() {
    if (fake::bad_rx_lengths) {
        --fake::bad_rx_lengths;
        return 48;
    }
    return AIR_BYTES;
}
uint64_t getReceiveTimestamp() { return 1234; }
uint64_t getReceiveTimestampRaw() { return 1200; }
void getReceivedData(byte* dest, uint16_t len) {
    assert(len == AIR_BYTES);
    std::memcpy(dest, fake::rx_air, len);
    if (fake::bad_rx_reads) {
        --fake::bad_rx_reads;
        dest[40] ^= 1;
    }
    if (++fake::rx_reads == fake::pll_on_rx_read) fake::sys_status |= rxguard::CLOCK_PLL_LOSS;
}
void setDelayedTRX(byte bytes[]) { fake::scheduled = getLE(bytes, 5); }
void setTransmitData(byte data[], uint16_t len) {
    Frame f;
    assert(decode(data, len, f));
    fake::staged_frame = f.type;
    assert(data[AIR_BYTES] == 0 && data[AIR_BYTES + 1] == 0);
    if (f.type == fake::inject_frame) {
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
uint64_t getTransmitTimestamp() {
    if (fake::fault_in_timestamp && fake::staged_frame == fake::inject_frame) fake::rf_status = 5;
    return fake::actual + (fake::tx_mismatch && fake::staged_frame == fake::inject_frame);
}
}


namespace {
auto& anchor = dual8app::port;
std::deque<std::vector<uint8_t>>& events() {
    return static_cast<fake::Queue*>(dual8app::outQ)->entries;
}

template<size_t N>
void usb(const char (&input)[N]) { fake::send_usb(input, N - 1); }

void reset_hardware() {
    fake::rf_status = fake::inject_rf = 1;
    fake::inject_frame = Type::Poll;
    fake::sys_status = 0x00800002;
    fake::dev_id = 0xdeca0130;
    fake::ec_ctrl = 4;
    fake::fail_at_us = 0;
    fake::inject_event = fake::fault_in_timestamp = fake::tx_mismatch = false;
    fake::tx_starts = fake::init_calls = fake::off_calls = fake::pll_clears = 0;
    fake::channel_readback = 5;
    fake::sys_cfg = 0x00041000;
    fake::tx_fctrl = 0x00252000;
    fake::pll_cfg = 0x0800041d;
    fake::pll_tune = 0xbe;
    fake::driver_init_ok = true;
    fake::bad_rx_lengths = fake::bad_rx_reads = fake::rx_reads = fake::pll_on_rx_read = 0;
    dual8app::active = false;
    events().clear();
}

void restart() {
    reset_hardware();
    usb("RESET\n");
    assert(dual8app::hwOk && !anchor.faulted() && fake::init_calls == 1);
}

void run(Record& record) {
    runAnchor(anchor, dual8app::config, 7, 0, 4, 0, record);
}

void startup_and_phy() {
    reset_hardware();
    fake::channel_readback = 2;
    assert(!anchor.initialize() && !anchor.selftest() && anchor.faulted());
    assert(fake::init_calls == 1 && fake::requested_channel == 5 && fake::channel_reads > 0);

    // A later read and a second SELFTEST cannot replace the first snapshot.
    fake::channel_readback = 5;
    usb("SELFTEST\n");
    fake::expect_event("\"pass\":false");
    fake::expect_event("\"step\":\"CHAN_CTRL\"");
    fake::expect_event("\"reg\":\"0x1f\",\"offset\":null");
    fake::expect_event("\"expected\":\"0x18c40055\",\"actual\":\"0x18c40022\"");
    usb("SELFTEST\n");
    fake::expect_event("\"expected\":\"0x18c40055\",\"actual\":\"0x18c40022\"");
    assert(anchor.faulted());

    restart();
    usb("SELFTEST\n");
    fake::expect_event("\"init_failure\":null");

    reset_hardware();
    fake::tx_fctrl = 0x0025a00c; // Actual COM4 readback, including TR and frame length.
    usb("RESET\n");
    usb("SELFTEST\n");
    fake::expect_event("\"pass\":true");
    fake::expect_event("\"init_failure\":null");
    assert(!anchor.faulted());

    reset_hardware();
    fake::tx_fctrl = 0x0025800c; // Same readback with bit 13 cleared (110 instead of 850 kbps).
    usb("RESET\n");
    usb("SELFTEST\n");
    fake::expect_event("\"pass\":false");
    fake::expect_event("\"step\":\"TX_FCTRL\",\"reg\":\"0x08\"");
    fake::expect_event("\"mask\":\"0xffff6000\",\"expected\":\"0x252000\",\"actual\":\"0x25800c\"");
    assert(anchor.faulted());
    std::puts("PASS TX_FCTRL: COM4 readback accepted, changed data rate rejected");

    restart();
    fake::channel_readback = 2;
    usb("SELFTEST\n");
    fake::expect_event("\"pass\":false");
    fake::expect_event("\"init_failure\":null");
    assert(anchor.faulted());

    restart();
    fake::channel_readback = 2;
    usb("START 12 1\n");
    fake::expect_event("ANCHOR_PHY_MISMATCH");
    assert(!dual8app::active && fake::tx_starts == 0);
    for (int i = 0; i < 4; ++i) {
        restart();
        if (i == 0) fake::sys_cfg |= 1U << 29;
        if (i == 1) fake::tx_fctrl ^= 1U << 13;
        if (i == 2) fake::pll_cfg ^= 1;
        if (i == 3) fake::pll_tune ^= 1;
        assert(!anchor.selftest() && anchor.faulted());
    }
    std::puts("PASS A2: lost channel write and four other PHY mismatches rejected");

    reset_hardware();
    fake::sys_status |= rxguard::CLOCK_PLL_LOSS;
    assert(anchor.initialize() && anchor.pll_startup.ready && anchor.pll_startup.acknowledged);
    assert(fake::pll_clears == 1 && fake::init_calls == 1);

    reset_hardware();
    fake::fail_at_us = fake::now_us + 2300000;
    assert(!anchor.initialize() && anchor.faulted() && fake::init_calls == 2);
    assert(fake::pll_clears == 0 && anchor.pll_startup.retry_rf == 3);
    assert(std::strcmp(anchor.startupFailure().step,"PLL_RF_STATUS")==0 &&
           anchor.startupFailure().actual==3);

    reset_hardware();
    fake::driver_init_ok = false;
    assert(!anchor.initialize() && fake::init_calls == 1 && anchor.faulted());
    usb("SELFTEST\n");
    fake::expect_event("\"step\":\"PMSC_RETURN_AUTO\",\"reg\":\"0x36\",\"offset\":\"0x0\"");
    fake::expect_event("\"mask\":\"0x3\",\"expected\":\"0x0\",\"actual\":\"0x1\"");
    reset_hardware();
    fake::sys_status = MASK40;
    assert(!anchor.initialize() && fake::init_calls == 1 && fake::pll_clears == 0);
    reset_hardware();
    fake::ec_ctrl = 0;
    assert(!anchor.initialize() && fake::init_calls == 1 && fake::pll_clears == 0);
    std::puts("PASS PLL startup: single sticky acknowledgement, delayed loss, bounded retry, untrusted reads rejected");
}

void tx_and_diagnostics() {
    restart();
    assert(anchor.selftest());
    Record healthy;
    run(healthy);
    assert(healthy.complete && healthy.errors == 0 && fake::tx_starts == 3);

    for (uint8_t rf : {uint8_t(0), uint8_t(3), uint8_t(5), uint8_t(255)}) {
        restart();
        fake::inject_rf = rf;
        Record failed;
        run(failed);
        assert(!failed.complete && fake::tx_starts == 0 && anchor.faulted());
        assert(failed.errors == (ErrPoll|ErrHardware) && failed.tx[0].status == 0x00800002);
        assert(failed.tx[0].hardware_fault && failed.tx[0].end_us >= failed.tx[0].begin_us);
        assert(!failed.tx[1].attempted && !failed.tx[2].attempted);
    }
    std::puts("PASS A1: unlatched CPLLLOW, CPLLHIGH, unlocked and invalid RF_STATUS block POLL");

    restart();
    fake::inject_rf = 3;
    fake::inject_event = true;
    Record sticky;
    run(sticky);
    assert(!sticky.complete && fake::tx_starts == 0 && fake::pll_clears == 0);
    assert(sticky.errors == (ErrPoll|ErrHardware) && sticky.tx[0].status == 0x02800002);
    assert(fake::off_calls == 2 && fake::speed == SPIClock::SLOW);
    usb("PLLSTATUS\n");
    fake::expect_event("\"startup_ready\":true");
    fake::expect_event("\"ready\":false");
    fake::expect_event("\"live_rf_slow\":3");
    fake::expect_event("\"fast_sampled\":false");
    fake::expect_event("\"clock_fault_recorded\":true");
    fake::expect_event("\"fault_rf\":3");
    assert(fake::fast_selections == 0 && fake::fast_off_with_pll_loss == 0);
    fake::rf_status = 1;
    fake::sys_status = 0x00800002;
    usb("PLLSTATUS\n");
    fake::expect_event("\"ready\":false");
    fake::expect_event("\"live_rf_slow\":1");
    fake::expect_event("\"fault_rf\":3");
    std::puts("PASS A3/A4: shutdown at 2 MHz, no diagnostic FAST read, live fault remains latched");

    restart();
    fake::rf_status = 3;
    usb("PLLSTATUS\n");
    fake::expect_event("\"ready\":false");
    usb("START 12 1\n");
    fake::expect_event("ANCHOR_CLOCK_NOT_READY");
    assert(!dual8app::active && anchor.faulted());

    restart();
    fake::dev_id = 0;
    Record device;
    run(device);
    assert(device.errors == (ErrPoll|ErrHardware) && fake::tx_starts == 0);

    restart();
    fake::inject_frame = Type::Probe1;
    fake::inject_rf = 3;
    Record p1;
    run(p1);
    assert(p1.errors == (ErrProbe1|ErrHardware) && fake::tx_starts == 1);
    assert(!p1.tx[2].attempted && !p1.p1_tx_confirmed);

    restart();
    fake::inject_frame = Type::Probe2;
    fake::fault_in_timestamp = true;
    Record p2;
    run(p2);
    assert(p2.errors == (ErrProbe2|ErrHardware) && fake::tx_starts == 3);
    assert(p2.tx[1].ok && p2.p1_tx_confirmed && !p2.tx[2].ok);

    restart();
    fake::inject_frame = Type::Probe1;
    fake::tx_mismatch = true;
    Record mismatch;
    run(mismatch);
    assert(mismatch.errors == ErrProbe1 && mismatch.tx[2].ok && !anchor.faulted());
    restart();
    usb("START 12 1\n");
    fake::now_us += 21000;
    fake::inject_rf = 3;
    dual8app::loop();
    fake::expect_event("\"event\":\"HARDWARE_FAULT_LATCHED\"");
    fake::expect_event("\"fault_rf\":3");
    assert(!dual8app::active && !dual8app::hwOk && dual8app::pool[0].errors == (ErrPoll|ErrHardware));
    char json[JSON_MAX+1];
    size_t size=0;
    assert(serializeRecord(dual8app::pool[0],json,sizeof json,size));
    assert(std::strstr(json,"\"errors\":3") && std::strstr(json,"\"firmware\":\"D8_R6_ANCHOR_STAGE6_RX_FEC_20261111\""));
    std::puts("PASS A4: entry and probe hardware faults are recorded; P2 stays available for a non-hardware P1 failure");
}

void usb_parser() {
    restart();
    usb("SET STAGE 7\nSET VERIFY_CIR 2\n");
    assert(events().size() == 3 && dual8app::config.stage == 2);
    fake::expect_event("BAD_VERIFY_CIR");
    usb("SET STAGE 0\n");
    fake::expect_event("BAD_STAGE");
    events().clear();
    usb("START 12 1\0ignored\n");
    fake::expect_event("BAD_COMMAND_BYTE");
    usb("STA\rRT 12 1\n");
    fake::expect_event("BAD_COMMAND_BYTE");
    usb("START 12 1\xff\n");
    fake::expect_event("BAD_COMMAND_BYTE");
    assert(!dual8app::active && events().size() == 3);
    std::vector<char> long_command(192, 'x');
    fake::send_usb(long_command.data(), long_command.size());
    assert(events().size() == 3);
    usb("\r\n");
    fake::expect_event("COMMAND_TOO_LONG");
    usb("SET\tSTAGE 2\r");
    assert(events().size() == 4);
    usb("\n");
    fake::expect_event("\"event\":\"HELLO\"");
    fake::expect_event("\"firmware\":\"D8_R6_ANCHOR_STAGE6_RX_FEC_20261111\"");
    usb("START 12 1\r\n");
    assert(dual8app::active && dual8app::session == 12);
    assert(events().size() == 6);
    std::puts("PASS A5: invalid SET, NUL, embedded CR, non-ASCII, overlong and split CRLF");
}

void stage5_rx_reread() {
    Config cfg;
    cfg.stage = 5;
    Frame response;
    response.type = Type::RespA;
    response.radio = 0;
    response.stage = 5;
    response.session = 7;
    response.txn = 15;
    response.boot = 6;
    response.peer_boot = 4;
    assert(encode(response, fake::rx_air));

    auto armWithFrame = [&] {
        restart();
        anchor.configure(cfg);
        assert(anchor.arm(0));
        fake::sys_status = 0x806f02;
    };
    Frame received;
    Rx rx;
    armWithFrame();
    fake::bad_rx_lengths = 1;
    assert(anchor.receive(0, received, rx) == 1);
    assert(fake::rx_reads == 1 && rx.first_read_issue == RxReadIssue::Length && rx.first_read_length == 48);
    assert(rx.received && rx.lde && rx.hw == 1234 && sameFrame(received, response));

    armWithFrame();
    fake::bad_rx_reads = 1;
    assert(anchor.receive(0, received, rx) == 1);
    assert(fake::rx_reads == 2 && rx.first_read_issue == RxReadIssue::Decode);
    assert(rx.received && rx.lde && rx.hw == 1234 && sameFrame(received, response));

    armWithFrame();
    fake::bad_rx_reads = 2;
    assert(anchor.receive(0, received, rx) == -1);
    assert(fake::rx_reads == 2 && !rx.received && !anchor.faulted());

    armWithFrame();
    fake::bad_rx_reads = 1;
    fake::pll_on_rx_read = 2;
    assert(anchor.receive(0, received, rx) == -2);
    assert(fake::rx_reads == 2 && !rx.received && anchor.faulted());
    std::puts("PASS Stage 5: latched RX retry, persistent corruption rejection, PLL fault guard");
}
}

int main() {
    std::vector<uint8_t> raw(3968);
    for (size_t i = 0; i < raw.size(); ++i) raw[i] = uint8_t(i);
    fake::short_uart_write = true;
    dual8app::writeAll(raw.data(), raw.size(), true);
    assert(fake::largest_uart_write == 128 && fake::uart_bytes == raw);
    fake::uart_bytes.clear();
    fake::largest_uart_write = 0;
    dual8app::writeAll(raw.data(), raw.size(), false);
    assert(fake::largest_uart_write == raw.size() && fake::uart_bytes == raw);
    fake::uart_bytes.clear();
    fake::largest_uart_write = 0;
    fake::short_uart_write = true;
    const char control[]="{\"event\":\"HELLO\"}";
    assert(dual8app::wire(1, control, sizeof control-1, nullptr, 0, false, true));
    const auto& encoded = fake::uart_bytes;
    const auto* frag = encoded.data();
    constexpr size_t fragmentBytes=UART_FRAGMENT_HEADER+UART_FRAGMENT_BYTES;
    assert(encoded.size()==(1+uartParityCount(1))*fragmentBytes+3*UART_FRAGMENT_HEADER);
    assert(std::memcmp(frag,"R8F6",4)==0&&frag[8]==0&&frag[9]==1);
    assert(getLE(frag+10,2)==24+sizeof control-1);
    assert(std::memcmp(frag+UART_FRAGMENT_HEADER,"RFB8",4)==0);
    const uint32_t fragmentCRC=crcStep(crcStep(0xffffffff,frag,12),
                                      frag+UART_FRAGMENT_HEADER,UART_FRAGMENT_BYTES)^0xffffffff;
    assert(fragmentCRC==getLE(frag+12,4));
    assert(frag[fragmentBytes+8]==1);
    // Row 1 uses coefficient inverse(0 XOR 1), so its parity is identical.
    assert(std::memcmp(frag+UART_FRAGMENT_HEADER,frag+fragmentBytes+UART_FRAGMENT_HEADER,UART_FRAGMENT_BYTES)==0);
    std::puts("PASS Stage 6: RFB8 fragment size, CRC, parity, and complete short writes");
    dual8app::config.stage = 2;
    dual8app::setup();
    startup_and_phy();
    tx_and_diagnostics();
    usb_parser();
    const auto old_config = dual8app::config;
    dual8app::config.stage = 6;
    dual8app::active = true;
    fake::serial_available_calls = 0;
    dual8app::service();
    assert(fake::serial_available_calls == 0);
    fake::send_usb("HELLO\n", 6);
    assert(fake::serial_available_calls > 0);
    fake::expect_event("\"stage\":6");
    dual8app::active = false;
    dual8app::config = old_config;
    std::puts("PASS Stage 6: active empty RX ring skips UART mutex; buffered command is served");
    stage5_rx_reread();
    for (auto* queue : fake::queues) delete queue;
}
