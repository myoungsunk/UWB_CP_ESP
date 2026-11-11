#pragma once
static void wifiKeepReserve(){}
// Bounded RAM trace; low-rate read-only SPI snapshots. No automatic RF retry.
struct CtrlDiagRow { const char* why; uint64_t us,status,hw; uint32_t session,peer,config,point; uint16_t bytes; uint8_t type; uint8_t raw[52]; };
static CtrlDiagRow ctrlRows[64]{};static uint16_t ctrlN=0;static uint32_t ctrlDropped=0;static bool ctrlRecording=false;
// Counts RAM-only; snapshots add three bounded read-only SPI transactions at 250ms intervals.
static uint64_t ctrlLoopCount=0,ctrlListenCount=0,ctrlPollCount=0,ctrlPollDone=0,ctrlLastLoop=0,ctrlLoopMaxGap=0,ctrlLastPoll=0,ctrlPollMaxGap=0;
static bool ctrlWindow=false,ctrlFault=false;
struct CtrlRxRow{uint64_t begin,end,state,status,loops,listens,polls,done,last_poll,max_poll_gap,max_loop_gap;uint32_t dev;uint8_t valid,flags;};
static CtrlRxRow* ctrlRxRows=nullptr;static unsigned ctrlRxN=0;static uint64_t ctrlNextSample=0;
static void ctrlLoopTick(){if(!ctrlRecording)return;auto t=nowUs();if(ctrlLastLoop&&t-ctrlLastLoop>ctrlLoopMaxGap)ctrlLoopMaxGap=t-ctrlLastLoop;ctrlLastLoop=t;++ctrlLoopCount;}
static void ctrlPollEnter(){if(!ctrlRecording)return;auto t=nowUs();if(ctrlLastPoll&&t-ctrlLastPoll>ctrlPollMaxGap)ctrlPollMaxGap=t-ctrlLastPoll;ctrlLastPoll=t;++ctrlPollCount;}
struct CtrlPollScope{CtrlPollScope(){ctrlPollEnter();}~CtrlPollScope(){if(ctrlRecording)++ctrlPollDone;}};
static bool ctrlWatch=false;static uint64_t ctrlWatchStart=0;static unsigned ctrlWatchIndex=0;
static bool ctrlCheckFault(const char* why);
static void ctrlRxSample(bool session,bool waiting,bool measuring){
 if(!ctrlRecording||!ctrlWindow||!ctrlWatch||ctrlFault||!session||!waiting||measuring)return;
 static const uint32_t offsets[]={0,100000,500000,1500000,2400000};
 if(ctrlWatchIndex>=5||nowUs()-ctrlWatchStart<offsets[ctrlWatchIndex])return;
 if(!ctrlRxRows||ctrlRxN>=24){ctrlCheckFault("RX_TRACE_STORAGE_INVALID");return;}
 ++ctrlWatchIndex;auto& x=ctrlRxRows[ctrlRxN++];x.begin=nowUs();x.flags=rxArmed?1:0;
 uint8_t b[5]{};regRead(0,0xFFFF,b,4);x.dev=le32(b);x.valid=1;
 regRead(0x19,0xFFFF,b,5);x.state=le40(b);x.valid|=2;
 regRead(0x0f,0xFFFF,b,5);x.status=le40(b);x.valid|=4;x.end=nowUs();
 x.loops=ctrlLoopCount;x.listens=ctrlListenCount;x.polls=ctrlPollCount;x.done=ctrlPollDone;
 x.last_poll=ctrlLastPoll;x.max_poll_gap=ctrlPollMaxGap;x.max_loop_gap=ctrlLoopMaxGap;
}
struct CtrlCheckRow{const char* stage;uint64_t begin,end,a,one,b,status;uint32_t id0,id1,pmsc0,pmsc1,syscfg,sysctrl;uint8_t valid,flags;bool low_stable,low_equal;};
static CtrlCheckRow ctrlChecks[16]{};static unsigned ctrlCheckN=0;
static bool ctrlCheckFault(const char* why){ctrlFault=true;active=false;remaining=0;idleRadio();event("{\"event\":\"CONTROL_RX_DIAG_FAULT\",\"boot\":%lu,\"role\":\"anchor\",\"source\":\"%s\",\"latched\":true}",(unsigned long)bootId,why);return false;}
static bool ctrlStageCheckpoint(const char* stage){
 if(!ctrlRecording)return true;if(ctrlFault)return false;if(ctrlCheckN>=16)return ctrlCheckFault("CHECKPOINT_OVERFLOW");
 auto& x=ctrlChecks[ctrlCheckN++];x.stage=stage;x.begin=nowUs();x.flags=rxArmed?1:0;
 uint8_t id[4]{},a[5]{},o[5]{},b[5]{},status[5]{};
 regRead(0,0xFFFF,id,4);x.id0=le32(id);x.valid=1;
 if(x.id0!=0xDECA0130UL){x.end=nowUs();return ctrlCheckFault("CHECK_ID_BEFORE");}
 regRead(0x19,0xFFFF,a,5);x.a=le40(a);x.valid|=2;
 if(x.a==MASK40){x.end=nowUs();return ctrlCheckFault("CHECK_STATE_A_FF");}
 for(unsigned k=0;k<5;k++){regRead(0x19,k,o+k,1);x.one=le40(o);x.valid|=4;if(k==2 && (x.one&0xffffff)==0xffffff){x.end=nowUs();return ctrlCheckFault("CHECK_SINGLE_LOW_FF");}}
 regRead(0x19,0xFFFF,b,5);x.b=le40(b);x.valid|=8;
 if(x.b==MASK40){x.end=nowUs();return ctrlCheckFault("CHECK_STATE_B_FF");}
 x.low_stable=((x.a&0xffffff)==(x.b&0xffffff));x.low_equal=((x.a&0xffffff)==(x.one&0xffffff));
 if(x.low_stable&&!x.low_equal){x.end=nowUs();return ctrlCheckFault("CHECK_STABLE_LOW_MISMATCH");}
 regRead(0x0f,0xFFFF,status,5);x.status=le40(status);x.valid|=16;
 if(x.status==MASK40){x.end=nowUs();return ctrlCheckFault("CHECK_STATUS_FF");}
 uint8_t c[4]{};
 regRead(0x36,0,c,4);x.pmsc0=le32(c);
 regRead(0x36,4,c,4);x.pmsc1=le32(c);
 regRead(0x04,0xFFFF,c,4);x.syscfg=le32(c);
 regRead(0x0d,0xFFFF,c,4);x.sysctrl=le32(c);
 x.valid|=64;
 regRead(0,0xFFFF,id,4);x.id1=le32(id);x.valid|=32;x.end=nowUs();
 if(x.id1!=0xDECA0130UL)return ctrlCheckFault("CHECK_ID_AFTER");return true;
}
struct CtrlPhyRow{uint64_t decision_begin,decision_end,id_begin,id_end,status;uint32_t dev;uint8_t raw[5],id[4];bool captured;};
static CtrlPhyRow ctrlPhy{};
struct CtrlPhyRead {uint64_t begin,end;uint8_t reg,raw[5],len;uint16_t off;};
static CtrlPhyRead ctrlPhyReads[9]{};static unsigned ctrlPhyReadN=0;
static void ctrlPhyRead(uint8_t reg,uint16_t off,uint8_t len){
 auto& r=ctrlPhyReads[ctrlPhyReadN++];r.reg=reg;r.off=off;r.len=len;r.begin=nowUs();
 regRead(reg,off,r.raw,len);r.end=nowUs();
}

static bool ctrlReceiveFailed(){
 if(!ctrlRecording||!ctrlWindow)return DW1000Ng::isReceiveFailed();
 if(ctrlFault)return false;
 uint64_t begin=nowUs();bool failed=DW1000Ng::isReceiveFailed();uint64_t end=nowUs();
 if(failed&&!ctrlPhy.captured){ctrlPhy.captured=true;ctrlPhy.decision_begin=begin;ctrlPhy.decision_end=end;
  DW1000Ng::copyLastSystemStatus(ctrlPhy.raw);ctrlPhy.status=le40(ctrlPhy.raw);
  ctrlPhyRead(0,0xFFFF,4);
  memcpy(ctrlPhy.id,ctrlPhyReads[0].raw,4);ctrlPhy.dev=le32(ctrlPhy.id);ctrlPhy.id_begin=ctrlPhyReads[0].begin;ctrlPhy.id_end=ctrlPhyReads[0].end;
  ctrlPhyRead(0x0F,0xFFFF,5);
  for(unsigned i=0;i<5;i++)ctrlPhyRead(0x0F,i,1);
  ctrlPhyRead(0x0F,0xFFFF,5);
  ctrlPhyRead(0,0xFFFF,4);
 }return failed;
}
static bool ctrlIntegrityStop(uint8_t reasons){if(!ctrlRecording||!ctrlWindow)return false;ctrlFault=true;active=false;remaining=0;idleRadio();event("{\"event\":\"CONTROL_RX_DIAG_FAULT\",\"role\":\"anchor\",\"boot\":%lu,\"source\":\"watchdog\",\"reasons\":%u,\"latched\":true}",(unsigned long)bootId,unsigned(reasons));return true;}
static void ctrlNote(const char* why,const AirPacket* p=nullptr,const RxObs* o=nullptr,uint32_t point=0){
 if(!ctrlRecording)return;if(strcmp(why,"ROUTE_RX_ARMED")==0){ctrlWindow=true;ctrlNextSample=0;}if(strcmp(why,"POINT_START_DISPATCH")==0)ctrlWindow=false;if(ctrlN>=64){++ctrlDropped;return;}
 auto& r=ctrlRows[ctrlN++];r.why=why;r.us=nowUs();r.point=point;
 if(p){r.session=p->h.session;r.peer=p->h.boot;r.config=p->h.config_id;r.point=p->h.txn;r.type=p->h.type;}
 if(o){r.status=o->status;r.hw=o->hw;r.bytes=o->bytes;if(o->bytes<=52)memcpy(r.raw,o->raw_air,o->bytes);}
 if(ctrlWindow&&(strcmp(why,"RX_PHY_FAILED")==0||strcmp(why,"RX_PHY_TIMEOUT")==0)){ctrlFault=true;active=false;remaining=0;idleRadio();event("{\"event\":\"CONTROL_RX_DIAG_FAULT\",\"role\":\"anchor\",\"boot\":%lu,\"source\":\"%s\",\"latched\":true}",(unsigned long)bootId,why);}
}
static void ack(uint32_t req,bool ok,const char* op,const char* why);
static bool ctrlCommand(uint32_t req,const char* op,char*& cur){
 if(strcmp(op,"CTRLMEM")==0){event("{\"event\":\"ACK\",\"request\":%lu,\"op\":\"CTRLMEM\",\"ok\":true,\"allocated\":%s,\"bytes\":%u,\"rows\":%u,\"recording\":%s,\"fault_latched\":%s,\"heap\":%lu,\"min_heap\":%lu,\"largest_internal_8bit\":%lu}",(unsigned long)req,ctrlRxRows?"true":"false",ctrlRxRows?unsigned(24*sizeof(CtrlRxRow)):0,ctrlRxN,ctrlRecording?"true":"false",ctrlFault?"true":"false",(unsigned long)ESP.getFreeHeap(),(unsigned long)ESP.getMinFreeHeap(),(unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));return true;}

 if(strcmp(op,"PMSCSTEP")==0){if(active||ctrlFault){ack(req,false,op,"STOP_REQUIRED");return true;}DW1000Ng::clearReceiveFailedStatus();bool ok=!DW1000Ng::resetTraceFault();if(!ok)ctrlCheckFault("PMSC_RELEASE_READBACK");ack(req,ok,op,ok?"OK":"PMSC_RELEASE_READBACK");return true;}
 if(strcmp(op,"CTRLDIAG")==0){if(active||ctrlFault){ack(req,false,op,ctrlFault?"DIAG_FAULT_LATCHED":"STOP_FIRST");return true;}if(!ctrlRxRows){ctrlRxRows=new(std::nothrow) CtrlRxRow[24]{};if(!ctrlRxRows){ack(req,false,op,"DIAG_ALLOC_FAILED");return true;}}ctrlPhy=CtrlPhyRow{};ctrlPhyReadN=0;memset(ctrlPhyReads,0,sizeof(ctrlPhyReads));ctrlCheckN=0;memset(ctrlChecks,0,sizeof(ctrlChecks));ctrlRxN=0;ctrlWatch=false;ctrlWindow=false;ctrlNextSample=0;ctrlLoopCount=ctrlListenCount=ctrlPollCount=ctrlPollDone=ctrlLastLoop=ctrlLoopMaxGap=ctrlLastPoll=ctrlPollMaxGap=0;ctrlN=0;ctrlDropped=0;ctrlRecording=true;ack(req,true,op,"OK");return true;}
 if(strcmp(op,"CTRLPOINTPRE")==0){if(!active||!ctrlWindow||!ctrlRecording){ack(req,false,op,"CONTROL_WAIT_REQUIRED");return true;}bool ok=ctrlStageCheckpoint("BEFORE_POINT_START");ctrlWatch=ok;ctrlWatchStart=nowUs();ctrlWatchIndex=0;ack(req,ok,op,ok?"OK":"DIAG_FAULT_LATCHED");return true;}
 if(strcmp(op,"CTRLTRACE")==0){
  if(active){ack(req,false,op,"STOP_FIRST");return true;}
  ctrlRecording=false;ctrlWatch=false;ctrlWindow=false;
  for(unsigned i=0;i<DW1000Ng::resetTraceCount();++i){delay(15);DW1000Ng::ResetReleaseTrace r{};DW1000Ng::resetTraceGet(i,r);char raw[9]{},id[9]{};for(unsigned j=0;j<4;++j){snprintf(raw+j*2,3,"%02x",r.pmsc[j]);snprintf(id+j*2,3,"%02x",r.id[j]);}
   event("{\"event\":\"PMSC_RELEASE\",\"boot\":%lu,\"seq\":%u,\"source\":%u,\"write_begin_us\":%lu,\"write_end_us\":%lu,\"read_end_us\":%lu,\"pmsc_raw\":\"%s\",\"high_byte\":%u,\"id_raw\":\"%s\",\"fault_latched\":%s,\"dropped\":%u}",(unsigned long)bootId,i,r.source,(unsigned long)r.write_begin,(unsigned long)r.write_end,(unsigned long)r.read_end,raw,r.high,id,DW1000Ng::resetTraceFault()?"true":"false",DW1000Ng::resetTraceDropped());
  }

  for(unsigned i=0;i<ctrlN;i++){delay(15);const auto& r=ctrlRows[i];char raw[105]{};for(unsigned k=0;k<r.bytes&&k<52;k++)snprintf(raw+k*2,3,"%02x",r.raw[k]);
   event("{\"event\":\"CTRL_DIAG\",\"candidate\":\"R1L5_PMSC_TRACE\",\"role\":\"anchor\",\"boot\":%lu,\"seq\":%u,\"reason\":\"%s\",\"us\":%llu,\"session\":%lu,\"peer_boot\":%lu,\"config_id\":%lu,\"point\":%lu,\"type\":%u,\"bytes\":%u,\"status_hex\":\"%010llx\",\"hw\":%llu,\"raw_hex\":\"%s\"}",(unsigned long)bootId,i,r.why,(unsigned long long)r.us,(unsigned long)r.session,(unsigned long)r.peer,(unsigned long)r.config,(unsigned long)r.point,r.type,r.bytes,(unsigned long long)r.status,(unsigned long long)r.hw,raw);
  }
  for(unsigned i=0;i<ctrlRxN;i++){delay(15);const auto& x=ctrlRxRows[i];event("{\"event\":\"CONTROL_RX_STATE\",\"boot\":%lu,\"seq\":%u,\"begin_us\":%llu,\"end_us\":%llu,\"dev_id\":%lu,\"valid_mask\":%u,\"state_hex\":\"%010llx\",\"status_hex\":\"%010llx\",\"flags\":%u,\"loops\":%llu,\"listens\":%llu,\"polls\":%llu,\"poll_done\":%llu,\"last_poll_us\":%llu,\"max_poll_gap_us\":%llu,\"max_loop_gap_us\":%llu}",(unsigned long)bootId,i,(unsigned long long)x.begin,(unsigned long long)x.end,(unsigned long)x.dev,x.valid,(unsigned long long)x.state,(unsigned long long)x.status,x.flags,(unsigned long long)x.loops,(unsigned long long)x.listens,(unsigned long long)x.polls,(unsigned long long)x.done,(unsigned long long)x.last_poll,(unsigned long long)x.max_poll_gap,(unsigned long long)x.max_loop_gap);}
  for(unsigned i=0;i<ctrlCheckN;i++){delay(15);const auto& x=ctrlChecks[i];event("{\"event\":\"RX_CHECKPOINT\",\"boot\":%lu,\"seq\":%u,\"stage\":\"%s\",\"begin_us\":%llu,\"end_us\":%llu,\"id_before\":%lu,\"id_after\":%lu,\"state_block_a\":\"%010llx\",\"state_single\":\"%010llx\",\"state_block_b\":\"%010llx\",\"pmsc_ctrl0\":\"%08lx\",\"pmsc_ctrl1\":\"%08lx\",\"sys_cfg\":\"%08lx\",\"sys_ctrl\":\"%08lx\",\"status_hex\":\"%010llx\",\"valid_mask\":%u,\"rx_armed_before\":%s,\"low24_bracket_stable\":%s,\"low24_block_single_equal\":%s}",(unsigned long)bootId,i,x.stage,(unsigned long long)x.begin,(unsigned long long)x.end,(unsigned long)x.id0,(unsigned long)x.id1,(unsigned long long)x.a,(unsigned long long)x.one,(unsigned long long)x.b,(unsigned long)x.pmsc0,(unsigned long)x.pmsc1,(unsigned long)x.syscfg,(unsigned long)x.sysctrl,(unsigned long long)x.status,x.valid,(x.flags&1)?"true":"false",x.low_stable?"true":"false",x.low_equal?"true":"false");}
  if(ctrlPhy.captured){char raw[11]{},id[9]{};for(unsigned i=0;i<5;i++)snprintf(raw+i*2,3,"%02x",ctrlPhy.raw[i]);for(unsigned i=0;i<4;i++)snprintf(id+i*2,3,"%02x",ctrlPhy.id[i]);
   event("{\"event\":\"RX_PHY_DECISION\",\"boot\":%lu,\"status_raw_hex\":\"%s\",\"status_le_hex\":\"%010llx\",\"dev_raw_hex\":\"%s\",\"dev_id\":%lu,\"decision_begin_us\":%llu,\"decision_end_us\":%llu,\"id_begin_us\":%llu,\"id_end_us\":%llu,\"source\":\"same_cached_bytes_used_by_vendor_isReceiveFailed\",\"id_phase\":\"RX_on_before_TRXOFF\"}",(unsigned long)bootId,raw,(unsigned long long)ctrlPhy.status,id,(unsigned long)ctrlPhy.dev,(unsigned long long)ctrlPhy.decision_begin,(unsigned long long)ctrlPhy.decision_end,(unsigned long long)ctrlPhy.id_begin,(unsigned long long)ctrlPhy.id_end);
  }
  for(unsigned i=0;i<ctrlPhyReadN;i++){delay(15);const auto& r=ctrlPhyReads[i];char raw[11]{};for(unsigned k=0;k<r.len;k++)snprintf(raw+k*2,3,"%02x",r.raw[k]);
   event("{\"event\":\"RX_PHY_RECHECK\",\"boot\":%lu,\"seq\":%u,\"reg\":%u,\"offset\":%u,\"len\":%u,\"raw_hex\":\"%s\",\"begin_us\":%llu,\"end_us\":%llu,\"phase\":\"before_TRXOFF_and_status_clear\"}",(unsigned long)bootId,i,r.reg,r.off,r.len,raw,(unsigned long long)r.begin,(unsigned long long)r.end);
  }
  event("{\"event\":\"CONTROL_POLL_SUMMARY\",\"boot\":%lu,\"loops\":%llu,\"listens\":%llu,\"polls\":%llu,\"poll_done\":%llu,\"last_poll_us\":%llu,\"max_poll_gap_us\":%llu,\"max_loop_gap_us\":%llu,\"samples\":%u,\"latched\":%s}",(unsigned long)bootId,(unsigned long long)ctrlLoopCount,(unsigned long long)ctrlListenCount,(unsigned long long)ctrlPollCount,(unsigned long long)ctrlPollDone,(unsigned long long)ctrlLastPoll,(unsigned long long)ctrlPollMaxGap,(unsigned long long)ctrlLoopMaxGap,ctrlRxN,ctrlFault?"true":"false");
  // Same loop owns sampling and commands; recording/watch are disabled before dumping.
  delete[] ctrlRxRows;ctrlRxRows=nullptr;ctrlRxN=0;
  event("{\"event\":\"ACK\",\"request\":%lu,\"op\":\"CTRLTRACE\",\"ok\":true,\"role\":\"anchor\",\"boot\":%lu,\"count\":%u,\"dropped\":%lu}",(unsigned long)req,(unsigned long)bootId,ctrlN,(unsigned long)ctrlDropped);return true;
 }return false;
}
