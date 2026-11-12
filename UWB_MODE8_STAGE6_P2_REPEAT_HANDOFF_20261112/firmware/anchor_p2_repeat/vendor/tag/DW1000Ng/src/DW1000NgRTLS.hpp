/*
 * MIT License
 * 
 * Copyright (c) 2018 Michele Biondi, Andrea Salvatori
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
*/

#pragma once

#include <Arduino.h>
#include "DW1000Ng.hpp"

/* Frame control */
constexpr byte BLINK = 0xC5;
constexpr byte DATA = 0x41;
constexpr byte SHORT_SRC_AND_DEST = 0x88;
constexpr byte LONG_SRC_AND_DEST = 0xCC;
constexpr byte SHORT_SRC_LONG_DEST = 0x8C;
constexpr byte LONG_SRC_SHORT_DEST = 0xC8;

/* Application ID */
constexpr byte RTLS_APP_ID_LOW = 0x9A;
constexpr byte RTLS_APP_ID_HIGH = 0x60;
constexpr uint16_t RTLS_APP_ID = RTLS_APP_ID_LOW | ((uint16_t) (RTLS_APP_ID_HIGH << 8));

/* Function code */
constexpr byte ACTIVITY_CONTROL = 0x10;
constexpr byte RANGING_INITIATION = 0x20;
constexpr byte RANGING_TAG_POLL = 0x21;
constexpr byte RANGING_TAG_FINAL_RESPONSE_EMBEDDED = 0x23;
constexpr byte RANGING_TAG_FINAL_RESPONSE_NO_EMBEDDED = 0x25;
constexpr byte RANGING_TAG_FINAL_SEND_TIME = 0x27;

/* Activity code */
constexpr byte ACTIVITY_FINISHED = 0x00;
constexpr byte RANGING_CONFIRM = 0x01;
constexpr byte RANGING_CONTINUE = 0x02;

/* BLINK Encoding Header */
constexpr byte BATTERY_GOOD = 0x00;
constexpr byte BATTERY_10_30_PERCENT = 0x02;
constexpr byte BATTERY_0_10_PERCENT = 0x01;
constexpr byte NO_BATTERY_STATUS = 0x03;

constexpr byte TEMPERATURE_DATA = 0x20;

constexpr byte EX_ID = 0x80;
constexpr byte NO_EX_ID = 0x40;

/* BLINK Ext Header */
constexpr byte BLINK_RATE_AND_LISTENING = 0x01;
constexpr byte TAG_LISTENING_NOW = 0x02;

enum class NextActivity {
    ACTIVITY_FINISHED,
    RANGING_CONFIRM
};

typedef struct RangeRequestResult {
    boolean success;
    uint16_t target_anchor;
} RangeRequestResult;

typedef struct RangeResult {
    boolean success;
    boolean next;
    uint16_t next_anchor;
    uint32_t new_blink_rate;
} RangeResult;

typedef struct RangeInfrastructureResult {
    boolean success;
    uint16_t new_blink_rate;
} RangeInfrastructureResult;

typedef struct RangeAcceptResult {
    boolean success;
    double range;
} RangeAcceptResult;

namespace DW1000NgRTLS {
// =========================
// [DUAL] Device-aware overloads (Stage-2)
// These overloads select the provided device and call the legacy (active-device) implementation.
// Requires Stage-1 (DW1000Ng::select/active/service).
// =========================
byte increaseSequenceNumber(const DW1000Ng::Device& dev);
void transmitTwrShortBlink(const DW1000Ng::Device& dev);
void transmitRangingInitiation(const DW1000Ng::Device& dev, byte tag_eui[], byte tag_short_address[]);
void transmitPoll(const DW1000Ng::Device& dev, byte anchor_address[]);
void transmitResponseToPoll(const DW1000Ng::Device& dev, byte tag_short_address[]);
void transmitFinalMessage(const DW1000Ng::Device& dev, byte anchor_address[], uint16_t reply_delay, uint64_t timePollSent, uint64_t timeResponseToPollReceived);
void transmitRangingConfirm(const DW1000Ng::Device& dev, byte tag_short_address[], byte next_anchor[]);
void transmitActivityFinished(const DW1000Ng::Device& dev, byte tag_short_address[], byte blink_rate[]);

boolean receiveFrame(const DW1000Ng::Device& dev);
void waitForTransmission(const DW1000Ng::Device& dev);

RangeRequestResult tagRangeRequest(const DW1000Ng::Device& dev);
RangeAcceptResult anchorRangeAccept(const DW1000Ng::Device& dev, NextActivity next, uint16_t value);
RangeInfrastructureResult tagRangeInfrastructure(const DW1000Ng::Device& dev, uint16_t target_anchor, uint16_t finalMessageDelay);
RangeInfrastructureResult tagTwrLocalize(const DW1000Ng::Device& dev, uint16_t finalMessageDelay);

void transmitResponseToPoll_SS(const DW1000Ng::Device& dev, byte tag_short_address[], uint32_t reply_delay_us, uint64_t timePollReceived);
RangeAcceptResult tagRange_SS(const DW1000Ng::Device& dev, uint16_t target_anchor);
float getClockOffsetRatio(const DW1000Ng::Device& dev);

    /*** TWR functions used in ISO/IEC 24730-62:2013, refer to the standard or the decawave manual for details about TWR ***/
    byte increaseSequenceNumber();
    void transmitTwrShortBlink();
    void transmitRangingInitiation(byte tag_eui[], byte tag_short_address[]);
    void transmitPoll(byte anchor_address[]);
    void transmitResponseToPoll(byte tag_short_address[]);
    void transmitFinalMessage(byte anchor_address[], uint16_t reply_delay, uint64_t timePollSent, uint64_t timeResponseToPollReceived);
    void transmitRangingConfirm(byte tag_short_address[], byte next_anchor[]);
    void transmitActivityFinished(byte tag_short_address[], byte blink_rate[]);
    
    boolean receiveFrame();
    void waitForTransmission();
    /*** End of TWR functions ***/
    
    /* Send a request range from tag to the rtls infrastructure */
    RangeRequestResult tagRangeRequest();

    /* Used by an anchor to accept an incoming tagRangeRequest by means of the infrastructure
       NextActivity is used to indicate the tag what to do next after the ranging process (Activity finished is to return to blink (range request), 
        Continue range is to tell the tag to range a new anchor)
       value is the value relative to the next activity (Activity finished = new blink rante, continue range = new anchor address)
    */
    RangeAcceptResult anchorRangeAccept(NextActivity next, uint16_t value);

    /* Used by tag to range after range request accept of the infrastructure 
       Target anchor is given after a range request success
       Finalmessagedelay is used in the process of TWR, a value of 1500 works on 8mhz-80mhz range devices,
        you could try to decrease it to improve system performance.
    */
    RangeInfrastructureResult tagRangeInfrastructure(uint16_t target_anchor, uint16_t finalMessageDelay);

    /* Can be used as a single function start the localization process from the tag.
        Finalmessagedelay is the same as in function tagRangeInfrastructure
    */
    RangeInfrastructureResult tagTwrLocalize(uint16_t finalMessageDelay);



void transmitResponseToPoll_SS(byte tag_short_address[], uint32_t reply_delay_us, uint64_t timePollReceived);


RangeAcceptResult tagRange_SS(uint16_t target_anchor);
float getClockOffsetRatio();

// =========================
// [Stage-3] Non-blocking helpers for Dual-Module Sniffing
// =========================

// --- SS-TWR (tag side) non-blocking FSM ---
enum class SsTagFsmState : uint8_t {
    IDLE = 0,
    POLL_TX_WAIT,
    RESP_RX_WAIT,
    DONE,
    FAIL_TIMEOUT,
    FAIL_RX,
    FAIL_BAD_FRAME
};

typedef struct SsTagFsm {
    SsTagFsmState state;
    uint16_t target_anchor;

    // Timing
    uint32_t start_ms;
    uint32_t overall_timeout_ms;
    uint16_t rx_wait_timeout_us;

    // Measurements
    uint64_t timePollSent;
    uint64_t timeRespReceived;
    uint32_t anchorDelay32;
    float clockOffsetRatio;
    double range_m;
} SsTagFsm;

/** Reset FSM to IDLE. */
void ssTagReset(SsTagFsm &fsm);

/**
 * Begin non-blocking SS-TWR ranging (tag side).
 *
 * Notes:
 * - Uses WAIT4RESP so the DW1000 automatically enables RX after TX.
 * - Does not busy-wait; advance by calling ssTagStep() repeatedly.
 */
void ssTagBegin(const DW1000Ng::Device& dev, SsTagFsm &fsm, uint16_t target_anchor,
                uint16_t rx_wait_timeout_us = 4000,
                uint32_t overall_timeout_ms = 20);

/** Advance the FSM; returns current state. */
SsTagFsmState ssTagStep(const DW1000Ng::Device& dev, SsTagFsm &fsm);

// --- RX sniff helper (RSSI + minimal frame parsing) ---
typedef struct SniffReport {
    bool valid;
    uint16_t dst;
    uint16_t src;
    uint8_t function_code;
    uint8_t activity_code;
    uint8_t seq;
    float rssi_dbm;
    uint64_t rx_ts;
} SniffReport;

/** Arm the device for continuous sniffing (start RX). */
void sniffArm(const DW1000Ng::Device& dev);

/**
 * Non-blocking sniff step:
 * - If a frame is available: fills report (valid=true) and re-arms RX
 * - If timeout/fail: clears status and re-arms RX
 * - Otherwise: report.valid=false
 */
SniffReport sniffStep(const DW1000Ng::Device& dev);
}
