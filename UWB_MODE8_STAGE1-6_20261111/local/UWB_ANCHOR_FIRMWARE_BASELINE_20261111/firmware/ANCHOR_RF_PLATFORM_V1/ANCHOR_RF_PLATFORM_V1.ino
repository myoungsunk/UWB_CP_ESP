#include <new>
// RF_PLATFORM_V1 complete measurement firmware.
// Runtime-controlled: modes 3/4/5/6/7, full CIR, timing, diagnostics, power plan, persistence, self-test.
// R1G2: Arduino loopTask default stack is 8 KB. GETPOINT runs command()(~3.4 KB frame) ->
// r03DumpPointTcp(~1.5 KB) -> remoteWriteWireOnly(~1.1 KB) -> WiFiClient/lwIP, which leaves
// only a few hundred bytes. R1G1 Anchor panicked at the first GETPOINT chunk 10/10 times.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);
void benchSetup();
void benchLoop();
void setup(){ benchSetup(); }
void loop(){ benchLoop(); }
